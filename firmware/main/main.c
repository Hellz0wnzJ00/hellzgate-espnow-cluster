// hellzgate c5, entry point
// build the master and scanner roles separately using the supplied configurations

#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "ble.h"
#include "csv.h"
#include "fan.h"
#include "gnss.h"
#include "hg_record.h"
#include "pending.h"
#include "scan.h"
#include "scan_mode.h"
#include "screen.h"
#include "selftest.h"
#include "session.h"
#include "slotid.h"
#include "storage.h"
#include "tally.h"
#include "unique_tracker.h"
#include "transport.h"
#include "web.h"

static const char *tag = "hellzgate";

static const hg_transport *link;


#ifdef CONFIG_HG_ROLE_MASTER



// rows go out with printf and not through the logger. a log line would put a
// level, a timestamp and a tag in front of every row and the file would not
// load. The card receives the same row text while its file is open.
// Serial capture also includes diagnostic logs unless CSV-only mode is enabled.
static void csv_out(const char *s)
{
    fputs(s, stdout);
    storage_write(s);
}

// unix_sec of 0 means the receiver has never given us a date. the column is
// left empty rather than filled from a made up base, because a wrong date that
// looks real is worse in an export than an obviously missing one
// rows that went out with the timestamp empty, whatever the reason. no date
// before the wait ran out, a full hold buffer, or a run stopped before a date
static uint32_t untimed;

static void emit_row(const hg_record_t *r, const csv_fix *fix_in, int64_t unix_sec)
{
    char when[24];

    if (unix_sec > 0) {
        csv_time(when, sizeof when, unix_sec);
    } else {
        when[0] = 0;
        untimed++;
    }

    csv_fix fix = *fix_in;
    fix.first_seen = when;

    // every observation gets a row, no cap. check the length it needed, a row
    // written short would be a broken line in the middle of the file
    char line[256];
    int n = csv_row(line, sizeof line, r, &fix);

    if (n >= (int)sizeof line) {
        ESP_LOGE(tag, "csv row needed %d bytes, row dropped", n);
        return;
    }

    // opens the file on the first row rather than at the start of the run, so
    // the name can carry the real time
    session_ensure_file();

    csv_out(line);
    session_record();
}

// called when a run stops. anything still waiting for a date goes into this
// run with an empty timestamp rather than surviving into the next one
static void settle_pending(void)
{
    int64_t now = gnss_unix();
    int64_t boot = now > 0 ? now - esp_timer_get_time() / 1000000 : 0;

    // flush does nothing when nothing is held, and discard always runs so
    // nothing held survives into the next run
    pending_flush(boot, emit_row);
    pending_discard();
}

static void took(const hg_record_t *r)
{
    // a record the tally rejected failed its crc or carried a field out of
    // range. it does not go in the export either, or the csv carries data the
    // validation already said was not real
    if (!unique_tracker_add(r))
        return;

    // the gate covers the held rows and the session transition together. stop
    // runs on the web task and reaches the same buffer this is about to write
    session_hold();

    // nothing is recorded while stopped. the counts still move because they are
    // since boot, but a row taken now must not reach the next run
    if (!session_running()) {
        session_release();
        return;
    }

    csv_fix fix;
    csv_current_fix(&fix, NULL);

    int64_t now = gnss_unix();

    // held rather than written in two cases. before the date, so the row can
    // get a real time once it arrives instead of a made up one. and while a
    // backlog is still going out, so the csv stays in time order. the backlog
    // is written a chunk at a time from the master loop, never in one go here
    if (pending_count() || (now <= 0 && !pending_gave_up())) {
        if (pending_add(r, &fix, esp_timer_get_time()) == 0) {
            session_release();
            return;
        }

        // no room to hold it. it goes out now, with a time if there is one,
        // and ahead of the backlog if one is still draining
    }

    emit_row(r, &fix, now);

    session_release();
}

// the held rows go out a chunk at a time between collection passes. writing
// thousands in one go held the collect path up for seconds, the inbox filled
// and the scanners overflowed. the offset between the monotonic timer and real
// time is the same for every held row, so their times are measured rather than
// guessed. with no date at all they go out with an empty timestamp once the
// wait runs out, never with an invented one
#define SETTLE_CHUNK 64

static void settle_some(void)
{
    // worked out once for a whole backlog. the receiver gives whole seconds
    // and the timer is cut to whole seconds, so working it out again for every
    // chunk could move the base by a second partway through
    static int64_t boot;
    static int have_boot;

    session_hold();

    if (session_running() && pending_count()) {
        int64_t now = gnss_unix();

        if (!have_boot && (now > 0 || pending_gave_up())) {
            boot = now > 0 ? now - esp_timer_get_time() / 1000000 : 0;
            have_boot = 1;
            session_set_start(boot);
        }

        if (have_boot)
            pending_flush_some(boot, emit_row, SETTLE_CHUNK);
    }

    if (pending_count() == 0)
        have_boot = 0;

    session_release();
}

static void report_session(void)
{
    session_info s;
    session_state(&s);

    if (!s.running)
        return;

    ESP_LOGI(tag, "run %s, %lu seconds, %lu records, %s",
             s.name, (unsigned long)s.seconds, (unsigned long)s.records,
             s.on_card ? "on the card" : "console only");

    // rows still held, waiting for a date or waiting their turn to be written,
    // and every row since boot that went out with an empty timestamp
    uint32_t held = pending_count();

    if (held || untimed)
        ESP_LOGI(tag, "%lu held rows still to write, %lu rows since boot went out without a timestamp",
                 (unsigned long)held, (unsigned long)untimed);
}

static void report_gnss(void)
{
    gnss_fix g;
    gnss_read(&g);

    uint32_t sentences, rejected;
    gnss_stats(&sentences, &rejected);

    if (sentences == 0 && rejected == 0)
        return;

    if (!g.have_fix) {
        ESP_LOGI(tag, "gnss has no fix, %lu sentences, %lu rejected",
                 (unsigned long)sentences, (unsigned long)rejected);
        return;
    }

    ESP_LOGI(tag, "gnss %.6f %.6f, %.1f m up, %u sats, quality %u, within %.0f m, %lld ms old",
             g.lat, g.lon, g.alt_m, (unsigned)g.sats, (unsigned)g.quality,
             g.accuracy_m, (long long)g.age_ms);
}

static void report_storage(void)
{
    if (!storage_ready())
        return;

    char path[STORAGE_PATH_MAX];
    uint32_t rows, saved, errors;
    uint64_t free_bytes;

    storage_stats(path, sizeof path, &rows, &saved, &errors, &free_bytes);

    ESP_LOGI(tag, "card %s, %lu rows written, %lu saved, %lu errors, %llu mb free",
             path[0] != '\0' ? path : "idle", (unsigned long)rows,
             (unsigned long)saved, (unsigned long)errors,
             (unsigned long long)(free_bytes >> 20));
}

static void report_nodes(void)
{
    for (uint8_t i = 0; i < HG_MAX_NODES; i++) {
        const hg_node_info *n = transport_node(i);
        if (n->state == HG_NODE_UNSEEN)
            continue;

        ESP_LOGI(tag, "node %u %s, received %lu records in %lu frames, lost %lu, dupes %lu, master full %lu, beats %lu, downs %lu, restarts %lu",
                 i, n->state == HG_NODE_UP ? "up" : "down",
                 (unsigned long)n->records, (unsigned long)n->frames,
                 (unsigned long)n->frames_lost, (unsigned long)n->dupes,
                 (unsigned long)n->inbox_full, (unsigned long)n->heartbeats,
                 (unsigned long)n->downs, (unsigned long)n->restarts);
    }

}

#ifdef CONFIG_HG_SCREEN

// eight lines of twenty one characters is not much, so this is the handful of
// numbers someone standing over the unit in a field actually needs. everything
// else is on the page and in the log
static void draw_status(void)
{
    unique_status status;
    tally_counts snapshot;
    unique_tracker_snapshot(&snapshot, &status);
    const tally_counts *counts = &snapshot;
    session_info run;
    session_state(&run);

    gnss_fix g;
    gnss_read(&g);

    char when[24];
    csv_time(when, sizeof when, csv_now());

    char line[SCREEN_COLS + 1];

    // the date is on the page, the panel only has room for the clock
    snprintf(line, sizeof line, "hellzgate   %.8s", when + 11);
    screen_line(0, line);

    snprintf(line, sizeof line, "run %-12.12s %s",
             run.name, run.running ? (run.on_card ? "rec" : "con") : "off");
    screen_line(1, line);

    snprintf(line, sizeof line, "total %lu%s", (unsigned long)counts->total,
             status.incomplete ? " !" : (status.pending ? " ~" : ""));
    screen_line(2, line);

    snprintf(line, sizeof line, "uniq %lu id %lu",
             (unsigned long)counts->unique,
             (unsigned long)counts->identifiable_unique);
    screen_line(3, line);

    snprintf(line, sizeof line, "ap %lu ble %lu",
             (unsigned long)counts->unique_type[HG_TYPE_AP],
             (unsigned long)counts->unique_type[HG_TYPE_BLE]);
    screen_line(4, line);

    if (g.have_fix)
        snprintf(line, sizeof line, "gps %.4f %.4f", g.lat, g.lon);
    else
        snprintf(line, sizeof line, "gps searching");
    screen_line(5, line);

    // no card fitted and a card that will not mount look the same from here,
    // both mean nothing is being written and the console is the only copy
    snprintf(line, sizeof line, "%u sats %.0fm card %s",
             (unsigned)g.sats, g.have_fix ? g.accuracy_m : 0.0,
             storage_ready() ? "ok" : "no");
    screen_line(6, line);

    uint32_t up = 0, lost = 0;

    for (uint8_t i = 0; i < HG_MAX_NODES; i++) {
        const hg_node_info *n = transport_node(i);
        if (n->state == HG_NODE_UP)
            up++;
        lost += n->frames_lost;

    }

    snprintf(line, sizeof line, "nodes %lu up lost %lu",
             (unsigned long)up, (unsigned long)lost);
    screen_line(7, line);

    screen_flush();
}

static void screen_task(void *arg)
{
    (void)arg;

    while (1) {
        // a panel powered after the master, or one that lost power, is picked
        // up here without a reset
        screen_retry();

        if (screen_ready())
            draw_status();

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#endif

static void run_master(void)
{
    // ask how long the header is before writing it. it grew past a fixed 128
    // byte buffer once already and got silently cut in half
    int need = csv_header(NULL, 0);
    char *header = malloc((size_t)need + 1);

    if (header != NULL) {
        csv_header(header, (size_t)need + 1);
        // console only. the card gets its own header when the session opens,
        // sending it through csv_out would write it into the file twice
        fputs(header, stdout);
        free(header);
    } else {
        ESP_LOGE(tag, "no room for the csv header, %d bytes", need);
    }

#ifdef CONFIG_HG_CSV_ONLY
    // everything else goes quiet so the console is nothing but the csv. without
    // this the rows are correct but the stream has log lines through it and has
    // to be filtered before it will load
    esp_log_level_set("*", ESP_LOG_NONE);
#endif

    // 1472 bytes, too much to sit on the task stack next to the csv buffers
    static hg_record_t batch[32];
    int ticks = 0;

    while (1) {
        // drain until it is empty, one pass per tick let the inbox back up
        uint32_t n;
        do {
            n = link->collect(batch, 32);
            for (uint32_t i = 0; i < n; i++)
                took(&batch[i]);
        } while (n == 32);


        // once a second is enough noise on the console
        if (++ticks >= 50) {
            ticks = 0;
            tally_counts snapshot;
            unique_status status;
            unique_tracker_snapshot(&snapshot, &status);
            const tally_counts *counts = &snapshot;
            ESP_LOGI(tag, "unique tracker: %s, pending %lu, dropped %lu, incomplete %d",
                     status.sd ? "sd overflow" : "ram only",
                     (unsigned long)status.pending, (unsigned long)status.dropped, status.incomplete);
            ESP_LOGI(tag, "total %lu, unique %lu, identifiable %lu, ap %lu, ble %lu, 2g4 %lu, 5g %lu",
                     (unsigned long)counts->total,
                     (unsigned long)counts->unique,
                     (unsigned long)counts->identifiable_unique,
                     (unsigned long)counts->unique_type[HG_TYPE_AP],
                     (unsigned long)counts->unique_type[HG_TYPE_BLE],
                     (unsigned long)counts->unique_band[HG_BAND_2G4],
                     (unsigned long)counts->unique_band[HG_BAND_5G]);

            // wifi totals per band, and everything that got thrown away. these
            // were counted and never printed, so bad crc 0 on its own did not
            // mean nothing was dropped
            ESP_LOGI(tag, "total 2g4 %lu, total 5g %lu, dropped bad crc %lu, bad field %lu, table full %lu",
                     (unsigned long)counts->total_band[HG_BAND_2G4],
                     (unsigned long)counts->total_band[HG_BAND_5G],
                     (unsigned long)counts->bad_crc,
                     (unsigned long)counts->bad_field,
                     (unsigned long)counts->table_full);
            report_session();
            report_gnss();
            report_storage();
            report_nodes();
        }

        settle_some();

        // decides for itself whether enough rows or enough time have gone by
        storage_tick();

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

#else

// a node numbered at or past the cluster size has every frame rejected by the
// master and its whole stream disappears. fail the build instead
_Static_assert(CONFIG_HG_NODE_ID < CONFIG_HG_MAX_NODES,
               "HG_NODE_ID must be below HG_MAX_NODES");

static uint32_t staged;

// everything either radio finds goes to the transport. send only stages it,
// the master collects
static void found(const hg_record_t *r)
{
    link->stage(r);


    staged++;
}

static uint32_t sweeps;
static int64_t last_report_us;
static int ble_running;

// printed from the scan loop and from the yield both. the scan blocks for a
// whole chunk at a time, so from the loop alone the line only landed every few
// seconds and the node log finished well before the master's
static void report_stats(void)
{
    int64_t now = esp_timer_get_time();
    if (now - last_report_us < 1000 * 1000)
        return;

    last_report_us = now;

    hg_tx_stats tx;
    link->tx_stats(&tx);

    ESP_LOGI(tag, "sweep %lu, generated %lu, ble %lu, sent %lu in %lu frames, refused %lu, retried %lu, lost %lu, overflow %lu, queued %lu",
             (unsigned long)sweeps, (unsigned long)staged,
             (unsigned long)ble_seen(),
             (unsigned long)tx.records, (unsigned long)tx.frames,
             (unsigned long)tx.refused, (unsigned long)tx.retried,
             (unsigned long)tx.lost, (unsigned long)tx.dropped,
             (unsigned long)tx.queued);
}

static void serve_link(void)
{
    link->service();
    report_stats();
}

// wifi sweeping and the link share one task because they share the radio
// channel. side by side the scanner could hop mid send and the frame would go
// out on the wrong channel. ble has its own controller and its own nimble task
static void radio_task(void *arg)
{
    (void)arg;
    scan_init(slotid_get(), found);
    scan_set_yield(link->beat_due, serve_link);

    while (1) {
        link->service();

        // nothing starts until the transport can deliver. on espnow that means
        // a master has answered. scanning before then would hop us off the
        // channel and we would miss the reply, and either radio would fill the
        // ring with records that have nowhere to go. ble is the worse of the
        // two, it scans continuously and starves the wifi side enough that the
        // master's beacon can be missed for minutes. wait for enrollment first
        if (!link->ready()) {
            int64_t now = esp_timer_get_time();
            if (now - last_report_us >= 1000 * 1000) {
                last_report_us = now;
                if (!slotid_known())
                    ESP_LOGE(tag, "not scanning, this board never worked out its slot");
                else
                    ESP_LOGI(tag, "%s not ready yet, still waiting for a master", link->name);
            }

            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // first time through after enrolling, bring ble up
        if (!ble_running) {
            ble_running = 1;
            ble_start(slotid_get(), found);
        }

        ble_service();

        if (scan_step())
            sweeps++;

        report_stats();
        link->service();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void run_node(void)
{
    xTaskCreate(radio_task, "radio", 4096, NULL, 5, NULL);

    // app_main has nothing left to do, the tasks own the work now
    vTaskDelete(NULL);
}

#endif

#ifdef CONFIG_HG_PIN_FINDER

// touch a wire from 3v3 to a header hole and this says which gpio it is.
// every pin is pulled down and watched, so the one reading high is the one
// connected to the test wire. this identifies unlabelled header pins
// without relying on an assumed header layout
static void find_pins(void)
{
    // the console, the usb pins and anything the flash or psram owns are left
    // out on purpose. touching those takes the chip down mid sentence
    static const int pins[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
        23, 24, 25, 26, 27, 28
    };

    const size_t count = sizeof pins / sizeof pins[0];

    for (size_t i = 0; i < count; i++) {
        gpio_reset_pin(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(pins[i], GPIO_PULLDOWN_ONLY);
    }

    ESP_LOGI(tag, "pin finder. nothing else is running.");
    ESP_LOGI(tag, "put one wire in 3v3 and touch the other end to a header hole");

    while (1) {
        char list[96];
        int n = 0;

        for (size_t i = 0; i < count; i++) {
            if (!gpio_get_level(pins[i]))
                continue;

            int wrote = snprintf(list + n, sizeof list - n, "%s%d",
                                 n ? ", " : "", pins[i]);
            if (wrote < 0 || (size_t)wrote >= sizeof list - n)
                break;

            n += wrote;
        }

        if (n)
            ESP_LOGI(tag, "3v3 is touching gpio %s", list);
        else
            ESP_LOGI(tag, "nothing touching");

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

#endif

void app_main(void)
{
#ifdef CONFIG_HG_PIN_FINDER
    find_pins();
#endif

#ifdef CONFIG_HG_ROLE_MASTER
    ESP_LOGI(tag, "starting as master, proto %04x", HG_PROTOCOL_VERSION);
#else
    slotid_init();
    ESP_LOGI(tag, "starting as node %u, slot %u, proto %04x",
             slotid_get(), slotid_get() + 1, HG_PROTOCOL_VERSION);
#endif

    scan_mode_init();

    // if this ever prints anything but 46 the packing broke
    ESP_LOGI(tag, "record is %d bytes", (int)sizeof(hg_record_t));

    if (selftest_run() != 0)
        ESP_LOGE(tag, "selftest failed, do not trust anything below this line");

    radio_init();


#ifdef CONFIG_HG_REQUIRE_ID
    // a board that does not know its id would send as the built in one, and
    // over espnow that id belongs to another scanner. the master would take
    // its frames for that scanner's and throw them away as copies. so it sends
    // nothing until it has a real id
    while (!slotid_known()) {
        ESP_LOGE(tag, "no node id, not sending. write one with tools/slot_nvs.py and reset");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
#endif

    link = transport_espnow();
    ESP_ERROR_CHECK(link->init());



#ifdef CONFIG_HG_SCREEN
    screen_start();
#endif

#ifdef CONFIG_HG_ROLE_MASTER
    // the receiver comes up first so it has the whole boot to find the sky.
    // the session filename carries gps time when it has any by then
    gnss_start();

    // no card is a warning and not a stop, the console still carries the rows.
    // a run opens itself at boot as well as from the page, so a unit switched
    // on in the field is recording rather than waiting to be told
    // held rows belong to the run they were taken in, so stopping settles them
    // into the current file instead of leaving them for the next session
    session_on_stop(settle_pending);

    fan_init();

    storage_mount();
    unique_tracker_start();
    session_start("run");

    // readers take a consistent snapshot; disk counting runs off the collect path.
    web_start();

#ifdef CONFIG_HG_SCREEN
    // Display refresh runs separately from record collection.
    xTaskCreate(screen_task, "screen", 3072, NULL, 3, NULL);
#endif

    run_master();
#else
    run_node();
#endif
}
