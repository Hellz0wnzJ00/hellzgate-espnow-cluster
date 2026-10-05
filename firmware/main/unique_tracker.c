#include "sdkconfig.h"
#include "unique_tracker.h"

#ifdef CONFIG_HG_ROLE_MASTER

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "storage.h"
#include "unique_index.h"

#define QUEUE_SIZE 8192

typedef struct {
    uint8_t addr[6];
    uint8_t type, band, node, flags;
} sighting;

static portMUX_TYPE busy = portMUX_INITIALIZER_UNLOCKED;
static tally_counts received, published;
static unique_status state;
static sighting *queue;
static uint32_t head, tail, queued;
static tally *table;
static unique_index *index_store;
static FILE *index_file;
static int running;
static const char *tag = "unique";

// these locks cover individual file operations, never a whole chain lookup.
// the CSV writer uses the same lock, including on the custom SD SPI driver.
static int read_page(void *ctx, uint32_t offset, uint8_t *out)
{
    FILE *f = ctx;
    storage_hold();
    int ok = fseek(f, (long)offset, SEEK_SET) == 0 &&
             fread(out, 1, UNIQUE_PAGE_BYTES, f) == UNIQUE_PAGE_BYTES;
    storage_release();
    return ok;
}

static int write_page(void *ctx, uint32_t offset, const uint8_t *in)
{
    FILE *f = ctx;
    storage_hold();
    int ok = fseek(f, (long)offset, SEEK_SET) == 0 &&
             fwrite(in, 1, UNIQUE_PAGE_BYTES, f) == UNIQUE_PAGE_BYTES;
    storage_release();
    return ok;
}

static int flush_file(void *ctx)
{
    storage_hold();
    int ok = fflush(ctx) == 0 && !ferror(ctx);
    storage_release();
    return ok;
}

static void publish(void)
{
    portENTER_CRITICAL(&busy);
    published = table->counts;
    if (table->counts.table_full || (index_store && index_store->failed))
        state.incomplete = 1;
    if (index_store && index_store->failed) state.errors = 1;
    portEXIT_CRITICAL(&busy);
}

static void process_some(void)
{
    for (unsigned n = 0; n < 32; n++) {
        sighting s;
        portENTER_CRITICAL(&busy);
        int have = queued != 0;
        if (have) {
            s = queue[tail];
            tail = (tail + 1) % QUEUE_SIZE;
            queued--;
        }
        portEXIT_CRITICAL(&busy);
        if (!have) break;

        hg_record_t r = {0};
        memcpy(r.bssid, s.addr, 6);
        r.type = s.type; r.band = s.band;
        r.node_id = s.node; r.flags = s.flags;
        hg_record_seal(&r);
        tally_add(table, &r);

        // pending includes the in-flight record until its counts publish.
        portENTER_CRITICAL(&busy);
        published = table->counts;
        state.pending--;
        if (table->counts.table_full) state.incomplete = 1;
        if (index_store && index_store->failed) {
            state.errors = 1;
            state.incomplete = 1;
        }
        portEXIT_CRITICAL(&busy);
    }
}

static void worker(void *arg)
{
    (void)arg;
    int64_t flushed = esp_timer_get_time();
    for (;;) {
        process_some();
        int64_t now = esp_timer_get_time();
        if (index_store && now - flushed >= 1000000) {
            unique_index_flush(index_store);
            publish();
            flushed = now;
        }
        // bound our CPU share even with a sustained backlog.
        vTaskDelay(1);
    }
}

void unique_tracker_start(void)
{
    table = heap_caps_calloc(1, sizeof *table, MALLOC_CAP_SPIRAM);
    queue = heap_caps_malloc(QUEUE_SIZE * sizeof *queue, MALLOC_CAP_SPIRAM);
    if (!table || !queue) {
        state.incomplete = 1;
        state.errors = 1;
        ESP_LOGE(tag, "no memory for tracker; CSV continues, uniques incomplete");
        return;
    }
    if (storage_ready()) {
        index_store = heap_caps_malloc(sizeof *index_store, MALLOC_CAP_SPIRAM);
        if (index_store) {
            // only our scratch file is replaced. never resume an index while
            // boot counters are zero, and never truncate a session CSV.
            storage_hold();
            index_file = fopen("/sd/hg_unique_v1.tmp", "w+b");
            if (index_file) setvbuf(index_file, NULL, _IONBF, 0);
            storage_release();
        }
        if (index_store && index_file) {
            unique_io io = {read_page, write_page, flush_file, index_file};
            unique_index_init(index_store, io);
            table->overflow = unique_index_touch;
            table->overflow_ctx = index_store;
            state.sd = 1;
        } else {
            heap_caps_free(index_store);
            index_store = NULL;
            state.errors = 1;
            ESP_LOGW(tag, "SD index unavailable; RAM table only");
        }
    }
    if (xTaskCreate(worker, "uniques", 4096, NULL, 1, NULL) != pdPASS) {
        state.errors = 1;
        state.incomplete = 1;
        ESP_LOGE(tag, "could not start tracker; CSV continues");
        return;
    }
    running = 1;
    ESP_LOGI(tag, "%s, %u queued sightings, %u bytes per RAM entry",
             state.sd ? "RAM with SD overflow" : "RAM only (bounded)",
             QUEUE_SIZE, (unsigned)sizeof(tally_seen));
}

int unique_tracker_add(const hg_record_t *r)
{
    // keep validation on the collect path so invalid records never reach CSV.
    int crc = hg_record_valid(r);
    int fields = r->type < TALLY_TYPES && r->band < TALLY_BANDS &&
                 r->node_id < TALLY_NODES;
    portENTER_CRITICAL(&busy);
    if (!crc || !fields) {
        if (!crc) received.bad_crc++;
        else received.bad_field++;
        portEXIT_CRITICAL(&busy);
        return 0;
    }
    received.total++;
    received.total_type[r->type]++;
    received.total_node[r->node_id]++;
    if (r->type != HG_TYPE_BLE) received.total_band[r->band]++;

    if (!running || queued == QUEUE_SIZE) {
        state.dropped++;
        state.incomplete = 1;
    } else {
        sighting *s = &queue[head];
        memcpy(s->addr, r->bssid, 6);
        s->type = r->type; s->band = r->band;
        s->node = r->node_id; s->flags = r->flags;
        head = (head + 1) % QUEUE_SIZE;
        queued++;
        state.pending++;
        if (state.pending > state.high_water) state.high_water = state.pending;
    }
    portEXIT_CRITICAL(&busy);
    return 1;
}

void unique_tracker_snapshot(tally_counts *out, unique_status *status)
{
    portENTER_CRITICAL(&busy);
    *out = published;
    out->total = received.total;
    out->bad_crc = received.bad_crc;
    out->bad_field = received.bad_field;
    memcpy(out->total_type, received.total_type, sizeof out->total_type);
    memcpy(out->total_band, received.total_band, sizeof out->total_band);
    memcpy(out->total_node, received.total_node, sizeof out->total_node);
    *status = state;
    portEXIT_CRITICAL(&busy);
}

#else
void unique_tracker_start(void) { }
int unique_tracker_add(const hg_record_t *r) { (void)r; return 0; }
void unique_tracker_snapshot(tally_counts *out, unique_status *status)
{
    *out = (tally_counts){0};
    *status = (unique_status){0};
}
#endif
