#include <string.h>

#include "tally.h"

_Static_assert((TALLY_MAX_DEVICES & (TALLY_MAX_DEVICES - 1)) == 0,
               "TALLY_MAX_DEVICES must be a power of two");

// fnv1a over the address and the type. the key is both, the same address can
// turn up as an access point and as a ble device and they are not one device
static uint32_t key_hash(const uint8_t *a, uint8_t type)
{
    uint32_t h = 2166136261u;

    for (int i = 0; i < 6; i++) {
        h ^= a[i];
        h *= 16777619u;
    }

    h ^= type;
    h *= 16777619u;
    return h;
}

// a rotating ble address is a different device every few minutes as far as we
// can tell, so it never counts toward identifiable unique
static int is_identifiable(const hg_record_t *r)
{
    if (r->type == HG_TYPE_BLE && (r->flags & HG_FLAG_BLE_RANDOM))
        return 0;

    return 1;
}

// existing slot for this address, or the free one it belongs in
// null when the table has no room left
static tally_seen *slot_for(tally *t, const uint8_t *addr, uint8_t type)
{
    uint32_t i = key_hash(addr, type) & (TALLY_MAX_DEVICES - 1);

    for (uint32_t probe = 0; probe < TALLY_MAX_DEVICES; probe++) {
        tally_seen *s = &t->seen[i];

        if (!s->used) {
            // leave spare slots so an overflow lookup never walks a full table.
            if (t->overflow && t->seen_count >= TALLY_MAX_DEVICES * 3 / 4)
                return NULL;
            return s;
        }
        if (s->type == type && memcmp(s->addr, addr, 6) == 0)
            return s;

        i = (i + 1) & (TALLY_MAX_DEVICES - 1);
    }
    return NULL;
}

void tally_reset(tally *t)
{
    memset(t, 0, sizeof *t);
}

int tally_add(tally *t, const hg_record_t *r)
{
    if (!hg_record_valid(r)) {
        t->counts.bad_crc++;
        return 0;
    }

    // this came off a radio link, the crc says it arrived intact but not that
    // the values make sense, so range check before indexing anything
    if (r->band >= TALLY_BANDS || r->node_id >= TALLY_NODES ||
        r->type >= TALLY_TYPES) {
        t->counts.bad_field++;
        return 0;
    }

    t->counts.total++;
    t->counts.total_node[r->node_id]++;
    t->counts.total_type[r->type]++;

    // ble sits on 2.4 physically but it does not go in the wifi band buckets,
    // or the wifi band numbers stop meaning wifi
    if (r->type != HG_TYPE_BLE)
        t->counts.total_band[r->band]++;

    tally_seen *s = slot_for(t, r->bssid, r->type);
    tally_seen previous;
    int on_card = s == NULL && t->overflow;
    if (on_card && t->overflow(t->overflow_ctx, r, &previous))
        s = &previous;
    if (s == NULL) {
        // totals stay right, we just cannot say if this one is new
        t->counts.table_full++;
        return 1;
    }

    if (!s->used) {
        s->used = 1;
        s->type = r->type;
        memcpy(s->addr, r->bssid, 6);
        if (!on_card) t->seen_count++;
        t->counts.unique++;
        t->counts.unique_type[r->type]++;

        if (is_identifiable(r))
            t->counts.identifiable_unique++;
    }

    if (r->type != HG_TYPE_BLE) {
        uint8_t band_bit = (uint8_t)(1u << r->band);
        if (!(s->bands & band_bit)) {
            s->bands |= band_bit;
            t->counts.unique_band[r->band]++;
        }
    }

    uint32_t node_bit = 1u << r->node_id;
    if (!(s->nodes & node_bit)) {
        s->nodes |= node_bit;
        t->counts.unique_node[r->node_id]++;
    }

    return 1;
}
