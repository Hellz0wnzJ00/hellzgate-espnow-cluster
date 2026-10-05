#include <string.h>

#include "esp_log.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

#include "ble.h"
#include "scan_mode.h"

static const char *tag = "ble";

static uint8_t  my_node_id;
static ble_sink emit;
static uint32_t seen;
static struct ble_npl_event mode_event;
static int initialized;
static int synced;

// how long one scan run lasts before we restart it. the controller drops
// repeats within a run, so this is also the window a device is counted once in
#define WINDOW_MS 10000

static void start_scan(void);

// ble hands addresses back least significant byte first, everyone writes them
// the other way round, so flip it before it goes in the record
static void flip(uint8_t *out, const uint8_t *in)
{
    for (int i = 0; i < 6; i++)
        out[i] = in[5 - i];
}

static uint8_t flags_for(const ble_addr_t *a)
{
    uint8_t f = 0;

    if (a->type != BLE_ADDR_PUBLIC && a->type != BLE_ADDR_PUBLIC_ID)
        f |= HG_FLAG_BLE_RANDOM;

    // top two bits of the top byte say which kind of random it is,
    // 01 is a resolvable private address, the ones that rotate
    if ((f & HG_FLAG_BLE_RANDOM) && (a->val[5] & 0xc0) == 0x40)
        f |= HG_FLAG_BLE_RESOLVABLE;

    return f;
}

static void report(const struct ble_gap_disc_desc *d)
{
    hg_record_t r;
    memset(&r, 0, sizeof r);

    flip(r.bssid, d->addr.val);
    r.rssi    = d->rssi;
    r.channel = 0;            // the controller does not tell us which of 37 38 39
    r.band    = HG_BAND_2G4;  // ble is 2.4 only
    r.type    = HG_TYPE_BLE;
    r.node_id = my_node_id;
    r.flags   = flags_for(&d->addr);

    // the name is optional and attacker controlled, bound it and terminate it
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0 &&
        f.name != NULL && f.name_len > 0) {
        size_t n = f.name_len;
        if (n > sizeof r.ssid - 1)
            n = sizeof r.ssid - 1;
        memcpy(r.ssid, f.name, n);
        r.ssid[n] = '\0';
    }

    hg_record_seal(&r);
    seen++;

    if (emit != NULL)
        emit(&r);
}

static int on_gap(struct ble_gap_event *ev, void *arg)
{
    (void)arg;

    switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
        if (scan_mode_wanted().mode == HG_SCAN_MIX)
            report(&ev->disc);
        break;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        // the run ended, go again so the repeat filter starts fresh
        start_scan();
        break;

    default:
        break;
    }

    return 0;
}

static void start_scan(void)
{
    if (!synced)
        return;

    hg_scan_mode want = scan_mode_wanted();
    if (want.mode == HG_SCAN_WIFI) {
        int rc = ble_gap_disc_active() ? ble_gap_disc_cancel() : 0;
        if (rc == 0 || rc == BLE_HS_EALREADY)
            scan_mode_note(want);
        else {
            scan_mode_note((hg_scan_mode){ .mode = HG_SCAN_UNKNOWN });
            ESP_LOGW(tag, "scan stop failed, rc %d", rc);
        }
        return;
    }

    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        scan_mode_note((hg_scan_mode){ .mode = HG_SCAN_UNKNOWN });
        ESP_LOGE(tag, "no usable address, scan not started");
        return;
    }

    struct ble_gap_disc_params p = {
        .itvl = 0,               // controller default
        .window = 0,
        .filter_policy = 0,
        .limited = 0,
        .passive = 1,            // never send a scan request
        .filter_duplicates = 1,  // one report per device per run
    };

    int rc = ble_gap_disc(own_addr_type, WINDOW_MS, &p, on_gap, NULL);
    if (rc == 0 || rc == BLE_HS_EALREADY)
        scan_mode_note(want);
    else {
        scan_mode_note((hg_scan_mode){ .mode = HG_SCAN_UNKNOWN });
        ESP_LOGE(tag, "scan start failed, rc %d", rc);
    }
}

static void on_sync(void)
{
    synced = 1;
    ble_hs_util_ensure_addr(0);
    ESP_LOGI(tag, "observer up, passive, %d ms window", WINDOW_MS);
    start_scan();
}

static void on_reset(int reason)
{
    synced = 0;
    scan_mode_note((hg_scan_mode){ .mode = HG_SCAN_UNKNOWN });
    ESP_LOGW(tag, "host reset, reason %d", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void apply_mode(struct ble_npl_event *ev)
{
    (void)ev;
    start_scan();
}

// queueing the same event twice is harmless. only the host touches discovery,
// so a completed window cannot restart ble behind a wifi-only request.
void ble_service(void)
{
    if (initialized)
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &mode_event);
}

void ble_start(uint8_t node_id, ble_sink sink)
{
    my_node_id = node_id;
    emit = sink;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(tag, "nimble init failed, %s", esp_err_to_name(err));
        return;
    }

    ble_npl_event_init(&mode_event, apply_mode, NULL);
    initialized = 1;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    nimble_port_freertos_init(host_task);
}

uint32_t ble_seen(void)
{
    return seen;
}
