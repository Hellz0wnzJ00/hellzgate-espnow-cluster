// counting tests, built with a small table so the full up path is easy to hit

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "tally.h"

static tally t;

static hg_record_t make(uint8_t last, uint8_t type, uint8_t band,
                        uint8_t node, uint8_t flags)
{
    hg_record_t r;
    memset(&r, 0, sizeof r);

    uint8_t mac[6] = { 0xaa, 0xbb, 0xcc, 0x00, 0x00, last };
    memcpy(r.bssid, mac, sizeof mac);

    r.rssi    = -60;
    r.type    = type;
    r.band    = band;
    r.node_id = node;
    r.flags   = flags;

    hg_record_seal(&r);
    return r;
}

static hg_record_t ap(uint8_t last, uint8_t band, uint8_t node)
{
    return make(last, HG_TYPE_AP, band, node, 0);
}

static void counts_a_single_ap(void)
{
    tally_reset(&t);
    hg_record_t r = ap(1, HG_BAND_2G4, 0);

    assert(tally_add(&t, &r) == 1);
    assert(t.counts.total == 1);
    assert(t.counts.unique == 1);
    assert(t.counts.identifiable_unique == 1);
    assert(t.counts.total_band[HG_BAND_2G4] == 1);
    assert(t.counts.unique_node[0] == 1);
}

// seeing the same box again is more observations but not more devices
static void repeats_do_not_inflate_unique(void)
{
    tally_reset(&t);
    hg_record_t r = ap(1, HG_BAND_2G4, 0);

    for (int i = 0; i < 5; i++)
        tally_add(&t, &r);

    assert(t.counts.total == 5);
    assert(t.counts.unique == 1);
}

// rotating BLE addresses must not inflate the identifiable count
static void rotating_ble_never_counts_as_identifiable(void)
{
    tally_reset(&t);

    for (uint8_t i = 0; i < 10; i++) {
        hg_record_t r = make(i, HG_TYPE_BLE, HG_BAND_2G4, 0, HG_FLAG_BLE_RANDOM);
        tally_add(&t, &r);
    }

    assert(t.counts.total == 10);
    assert(t.counts.unique == 10);
    assert(t.counts.identifiable_unique == 0);
}

// a resolvable address is still random so it stays out of identifiable too
static void resolvable_ble_is_not_identifiable(void)
{
    tally_reset(&t);
    hg_record_t r = make(1, HG_TYPE_BLE, HG_BAND_2G4, 0,
                         HG_FLAG_BLE_RANDOM | HG_FLAG_BLE_RESOLVABLE);

    tally_add(&t, &r);
    assert(t.counts.unique == 1);
    assert(t.counts.identifiable_unique == 0);
}

// a ble device on a fixed address is a real countable device
static void public_ble_is_identifiable(void)
{
    tally_reset(&t);
    hg_record_t r = make(1, HG_TYPE_BLE, HG_BAND_2G4, 0, 0);

    tally_add(&t, &r);
    assert(t.counts.identifiable_unique == 1);
}

// two nodes hearing one ap is still one device, and both nodes get credit
static void same_device_from_two_nodes(void)
{
    tally_reset(&t);
    hg_record_t a = ap(7, HG_BAND_5G, 2);
    hg_record_t b = ap(7, HG_BAND_5G, 5);

    tally_add(&t, &a);
    tally_add(&t, &b);

    assert(t.counts.total == 2);
    assert(t.counts.unique == 1);
    assert(t.counts.unique_node[2] == 1);
    assert(t.counts.unique_node[5] == 1);
    assert(t.counts.total_node[2] == 1);
    assert(t.counts.total_node[5] == 1);
}

// one address showing up on both bands counts once overall and once per band
static void same_device_on_both_bands(void)
{
    tally_reset(&t);
    hg_record_t a = ap(9, HG_BAND_2G4, 0);
    hg_record_t b = ap(9, HG_BAND_5G, 0);

    tally_add(&t, &a);
    tally_add(&t, &b);

    assert(t.counts.unique == 1);
    assert(t.counts.unique_band[HG_BAND_2G4] == 1);
    assert(t.counts.unique_band[HG_BAND_5G] == 1);
    assert(t.counts.total_band[HG_BAND_2G4] == 1);
    assert(t.counts.total_band[HG_BAND_5G] == 1);
}

static void bad_crc_is_dropped(void)
{
    tally_reset(&t);
    hg_record_t r = ap(1, HG_BAND_2G4, 0);
    r.rssi = -10;   // touched after sealing so the crc no longer matches

    assert(tally_add(&t, &r) == 0);
    assert(t.counts.bad_crc == 1);
    assert(t.counts.total == 0);
    assert(t.counts.unique == 0);
}

// crc only proves it arrived intact, a bad node id would index off the end
static void out_of_range_fields_are_dropped(void)
{
    tally_reset(&t);

    hg_record_t bad_node = ap(1, HG_BAND_2G4, TALLY_NODES);
    hg_record_seal(&bad_node);
    assert(tally_add(&t, &bad_node) == 0);

    hg_record_t bad_band = ap(2, 7, 0);
    hg_record_seal(&bad_band);
    assert(tally_add(&t, &bad_band) == 0);

    assert(t.counts.bad_field == 2);
    assert(t.counts.total == 0);
}

// once the table fills the totals must stay honest even though uniques stall
static void full_table_keeps_totals_honest(void)
{
    tally_reset(&t);

    for (uint8_t i = 0; i < TALLY_MAX_DEVICES + 8; i++) {
        hg_record_t r = ap(i, HG_BAND_2G4, 0);
        assert(tally_add(&t, &r) == 1);
    }

    assert(t.counts.total == TALLY_MAX_DEVICES + 8);
    assert(t.counts.unique == TALLY_MAX_DEVICES);
    assert(t.counts.table_full == 8);
}

static void reset_clears_everything(void)
{
    tally_reset(&t);
    hg_record_t r = ap(1, HG_BAND_2G4, 0);
    tally_add(&t, &r);

    tally_reset(&t);
    assert(t.counts.total == 0);
    assert(t.counts.unique == 0);
    assert(t.seen_count == 0);
    assert(t.counts.unique_node[0] == 0);
}

int main(void)
{
    counts_a_single_ap();
    repeats_do_not_inflate_unique();
    rotating_ble_never_counts_as_identifiable();
    resolvable_ble_is_not_identifiable();
    public_ble_is_identifiable();
    same_device_from_two_nodes();
    same_device_on_both_bands();
    bad_crc_is_dropped();
    out_of_range_fields_are_dropped();
    full_table_keeps_totals_honest();
    reset_clears_everything();

    printf("tally tests ok\n");
    return 0;
}
