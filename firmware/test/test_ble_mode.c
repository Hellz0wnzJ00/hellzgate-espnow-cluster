// real ble observer and mode state, with the controller calls replaced.
#include <assert.h>
#include <stdio.h>
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "ble.h"
#include "scan_mode.h"

int hg_test_verbose;
int64_t hg_test_now_us = 1;
struct test_hs_cfg ble_hs_cfg;
static int active, fail_start, fail_stop, starts;
static int (*gap)(struct ble_gap_event *, void *);
static struct ble_npl_event *pending;
void ble_npl_eventq_put(void *q, struct ble_npl_event *e) { (void)q; pending = e; }
esp_err_t nimble_port_init(void) { return ESP_OK; }
void nimble_port_run(void) {}
void nimble_port_freertos_init(void (*task)(void *)) { (void)task; ble_hs_cfg.sync_cb(); }
void nimble_port_freertos_deinit(void) {}
int ble_hs_util_ensure_addr(int x) { (void)x; return 0; }
int ble_hs_id_infer_auto(int x, uint8_t *a) { (void)x; *a = 0; return 0; }
int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *f, const uint8_t *d, uint8_t n) { (void)f; (void)d; (void)n; return 1; }
int ble_gap_disc_active(void) { return active; }
int ble_gap_disc_cancel(void) { if (fail_stop) return 9; active = 0; return 0; }
int ble_gap_disc(uint8_t a, int ms, const struct ble_gap_disc_params *p, int (*cb)(struct ble_gap_event *, void *), void *arg)
{
    (void)a; (void)ms; (void)arg;
    assert(p->passive == 1 && p->filter_duplicates == 1);
    gap = cb;
    if (fail_start) return 9;
    if (active) return BLE_HS_EALREADY;
    active = 1; starts++; return 0;
}
static void serve(void)
{
    ble_service();
    assert(pending != NULL);
    struct ble_npl_event *ev = pending;
    pending = NULL;
    ev->fn(ev);
}
static void request(uint32_t revision, uint8_t mode)
{
    uint8_t wire[HG_SCAN_MODE_BYTES];
    scan_mode_pack(wire, (hg_scan_mode){ .revision = revision, .mode = mode });
    scan_mode_receive(wire);
}
int main(void)
{
    ble_start(0, NULL);
    assert(active && scan_mode_applied().mode == HG_SCAN_MIX);
    request(7, HG_SCAN_WIFI);
    fail_stop = 1; serve();
    assert(scan_mode_applied().revision != 7 && active);
    fail_stop = 0; serve();
    assert(!active && scan_mode_applied().revision == 7);
    struct ble_gap_event done = { .type = BLE_GAP_EVENT_DISC_COMPLETE };
    gap(&done, NULL);
    assert(!active && starts == 1);
    request(8, HG_SCAN_MIX);
    fail_start = 1; serve();
    assert(scan_mode_applied().mode == HG_SCAN_UNKNOWN && !active);
    fail_start = 0; serve();
    assert(active && scan_mode_applied().revision == 8);
    // losing a later discovery window must also invalidate an old success.
    active = 0; fail_start = 1; gap(&done, NULL);
    assert(scan_mode_applied().mode == HG_SCAN_UNKNOWN);
    fail_start = 0; serve();
    assert(active && scan_mode_applied().revision == 8);
    request(9, 2); request(0, HG_SCAN_WIFI);
    assert(scan_mode_wanted().revision == 8);
    ble_hs_cfg.reset_cb(1); active = 0;
    assert(scan_mode_applied().mode == HG_SCAN_UNKNOWN);
    serve(); assert(!active);
    ble_hs_cfg.sync_cb();
    assert(active && scan_mode_applied().revision == 8);
    // rapid requests before the queued event runs apply only the latest one.
    request(10, HG_SCAN_WIFI); ble_service(); request(11, HG_SCAN_MIX); serve();
    assert(active && scan_mode_applied().revision == 11);
    puts("ble mode tests ok: failed starts/stops, late completion, reset, invalid and rapid requests");
    return 0;
}
