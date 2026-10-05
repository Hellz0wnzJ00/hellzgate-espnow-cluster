#include <stddef.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "csv.h"
#include "gnss.h"
#include "hg_record.h"
#include "selftest.h"
#include "esp_heap_caps.h"
#include "tally.h"

static const char *tag = "selftest";

static int failed;

static void check(int ok, const char *what)
{
    if (ok) {
        ESP_LOGI(tag, "ok   %s", what);
    } else {
        ESP_LOGE(tag, "FAIL %s", what);
        failed++;
    }
}

// Allocate the large tally fixture in PSRAM rather than on the task stack.
static tally *tp;
#define t (*tp)

static void make(hg_record_t *r, uint8_t last, uint8_t band, uint8_t node,
                 uint8_t type, uint8_t flags, const char *ssid)
{
    memset(r, 0, sizeof *r);
    r->bssid[0] = 0xaa;
    r->bssid[5] = last;
    r->rssi = -60;
    r->channel = 6;
    r->band = band;
    r->type = type;
    r->node_id = node;
    r->flags = flags;
    if (ssid)
        strncpy(r->ssid, ssid, sizeof r->ssid - 1);
    hg_record_seal(r);
}

// counts the commas that are not inside quotes, 14 columns means 13
static int commas_outside_quotes(const char *s)
{
    int n = 0, inside = 0;
    for (; *s; s++) {
        if (*s == '"') inside = !inside;
        else if (*s == ',' && !inside) n++;
    }
    return n;
}

static void test_layout(void)
{
    check(sizeof(hg_record_t) == 46, "record is 46 bytes on target");
    check(offsetof(hg_record_t, ssid) == 12, "ssid sits at byte 12");
    check(offsetof(hg_record_t, crc8) == 45, "crc sits at byte 45");
}

static void test_crc(void)
{
    // the standard check value for crc8 with poly 0x07 and init 0x00
    check(hg_crc8((const uint8_t *)"123456789", 9) == 0xf4, "crc8 check vector");

    hg_record_t r;
    make(&r, 0x01, HG_BAND_2G4, 0, HG_TYPE_AP, 0, "hello");
    check(hg_record_valid(&r), "sealed record validates");

    r.ssid[0] ^= 0x01;
    check(!hg_record_valid(&r), "flipped payload bit is caught");

    r.ssid[0] ^= 0x01;
    r.crc8 ^= 0x80;
    check(!hg_record_valid(&r), "flipped crc bit is caught");
}

static void test_tally(void)
{
    tp = heap_caps_malloc(sizeof *tp, MALLOC_CAP_SPIRAM);

    if (tp == NULL) {
        check(0, "psram for the tally selftest");
        return;
    }

    hg_record_t r;

    tally_reset(&t);

    // same device twice, once per band, plus a second device
    make(&r, 0x01, HG_BAND_2G4, 0, HG_TYPE_AP, 0, "one");
    tally_add(&t, &r);
    tally_add(&t, &r);
    make(&r, 0x01, HG_BAND_5G, 1, HG_TYPE_AP, 0, "one");
    tally_add(&t, &r);
    make(&r, 0x02, HG_BAND_5G, 1, HG_TYPE_AP, 0, "two");
    tally_add(&t, &r);

    check(t.counts.total == 4, "total counts every observation");
    check(t.counts.unique == 2, "unique de dupes by address");
    check(t.counts.unique_band[HG_BAND_2G4] == 1, "one unique on 2g4");
    check(t.counts.unique_band[HG_BAND_5G] == 2, "two unique on 5g");
    check(t.counts.unique_node[0] == 1 && t.counts.unique_node[1] == 2, "per node uniques split");

    // a rotating ble address counts but is not identifiable
    make(&r, 0x03, HG_BAND_2G4, 0, HG_TYPE_BLE, HG_FLAG_BLE_RANDOM, NULL);
    tally_add(&t, &r);
    check(t.counts.unique == 3 && t.counts.identifiable_unique == 2, "random ble is not identifiable");
    check(t.counts.unique_type[HG_TYPE_AP] == 2 && t.counts.unique_type[HG_TYPE_BLE] == 1,
          "uniques split by type");

    // a ble device with a fixed public address is countable like anything else
    make(&r, 0x06, HG_BAND_2G4, 0, HG_TYPE_BLE, 0, "watch");
    tally_add(&t, &r);
    check(t.counts.unique == 4 && t.counts.identifiable_unique == 3, "public ble is identifiable");

    // a corrupt record is dropped, not counted
    make(&r, 0x04, HG_BAND_2G4, 0, HG_TYPE_AP, 0, "bad");
    r.crc8 ^= 0xff;
    tally_add(&t, &r);
    check(t.counts.total == 6 && t.counts.bad_crc == 1, "bad crc dropped");

    // and one that survived the crc but carries nonsense, this is the trust boundary
    make(&r, 0x05, 7, 0, HG_TYPE_AP, 0, "wide");
    r.band = 7;
    hg_record_seal(&r);
    tally_add(&t, &r);
    check(t.counts.bad_field == 1, "out of range band dropped");

    heap_caps_free(tp);
    tp = NULL;
}

static void test_csv(void)
{
    char buf[512];
    hg_record_t r;

    int n = csv_header(buf, sizeof buf);
    check(n > 0 && strncmp(buf, "WigleWifi", 9) == 0, "csv header written");

    // an ssid that would shift the columns if the escaping was wrong
    make(&r, 0x01, HG_BAND_2G4, 0, HG_TYPE_AP, 0, "a,b\"c");
    // Synthetic location/time fixture, not a recorded field observation.
    csv_fix fix = { .lat = 51.5, .lon = -0.12, .alt_m = 30, .accuracy_m = 5,
                    .first_seen = "2026-08-10 19:00:00", .have_fix = 1 };

    n = csv_row(buf, sizeof buf, &r, &fix);
    check(n > 0 && n < (int)sizeof buf, "csv row fits");
    check(commas_outside_quotes(buf) == 13, "hostile ssid does not shift columns");
    check(strstr(buf, "\"\"") != NULL, "quote in ssid is doubled");

    // sizing contract, asking with no buffer returns the same length
    check(csv_row(NULL, 0, &r, &fix) == n, "csv row sizes without a buffer");

    // the time formatter has to produce wigle format for any unix time
    char when[24];
    csv_time(when, sizeof when, 1577836800LL);
    check(strcmp(when, "2020-01-01 00:00:00") == 0, "timestamp format");

    csv_time(when, sizeof when, 1577836800LL + 86399);
    check(strcmp(when, "2020-01-01 23:59:59") == 0, "timestamp rolls the clock");

    // and a short buffer must still terminate
    char small[8];
    memset(small, 'x', sizeof small);
    csv_row(small, sizeof small, &r, &fix);
    check(small[sizeof small - 1] == '\0', "short buffer stays terminated");
}

static void test_psram(void)
{
    size_t want = 64 * 1024;
    uint8_t *p = heap_caps_malloc(want, MALLOC_CAP_SPIRAM);
    check(p != NULL, "psram allocates");
    if (p == NULL)
        return;

    for (size_t i = 0; i < want; i++)
        p[i] = (uint8_t)(i * 31u);

    int same = 1;
    for (size_t i = 0; i < want; i++)
        if (p[i] != (uint8_t)(i * 31u)) { same = 0; break; }

    check(same, "psram reads back what was written");
    heap_caps_free(p);

    ESP_LOGI(tag, "psram free %u of %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
}

// canned sentences through the nmea parser, position, altitude, satellites,
// the date, a fix dropping back to searching and a bad checksum
static void test_gnss(void)
{
    check(gnss_selftest() == 0, "gnss parses gga and rmc");
}

int selftest_run(void)
{
    failed = 0;

    test_layout();
    test_crc();
    test_tally();
    test_csv();
    test_psram();
    test_gnss();

    if (failed)
        ESP_LOGE(tag, "%d checks failed", failed);
    else
        ESP_LOGI(tag, "all checks passed");

    return failed;
}
