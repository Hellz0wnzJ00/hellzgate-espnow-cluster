#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unique_index.h"

static tally t;
static unique_index u;
static unsigned char *disk;
static size_t bytes;
static int fail_read, fail_write, fail_flush;
static unsigned reads, writes;

static int rd(void *ctx, uint32_t off, uint8_t *out)
{
    (void)ctx;
    reads++;
    if (fail_read || off + 512u > bytes) return 0;
    memcpy(out, disk + off, 512);
    return 1;
}
static int wr(void *ctx, uint32_t off, const uint8_t *in)
{
    (void)ctx;
    writes++;
    if (fail_write) return 0;
    if (off + 512u > bytes) {
        size_t size = off + 512u;
        disk = realloc(disk, size);
        assert(disk);
        memset(disk + bytes, 0, size - bytes);
        bytes = size;
    }
    memcpy(disk + off, in, 512);
    return 1;
}
static int fl(void *ctx) { (void)ctx; return !fail_flush; }

static hg_record_t record(unsigned i, unsigned node, unsigned band)
{
    hg_record_t r = {0};
    unsigned addr = i / 2;
    for (int j = 0; j < 4; j++) r.bssid[j] = (uint8_t)(addr >> (8 * j));
    r.type = i & 1 ? HG_TYPE_BLE : HG_TYPE_AP;
    r.flags = (i % 4 == 3) ? HG_FLAG_BLE_RANDOM : 0;
    r.node_id = node;
    r.band = band;
    hg_record_seal(&r);
    return r;
}

static void reset(void)
{
    fail_read = fail_write = fail_flush = 0;
    unique_io io = {rd, wr, fl, NULL};
    unique_index_init(&u, io);
    tally_reset(&t);
    t.overflow = unique_index_touch;
    t.overflow_ctx = &u;
}

int main(void)
{
#ifdef SMALL_COLLISION_TEST
    const unsigned count = 2400;
#else
    const unsigned count = 1000000;
#endif
    reset();
    for (unsigned i = 0; i < count; i++) {
        hg_record_t r = record(i, 0, 0);
        assert(tally_add(&t, &r));
    }
    assert(t.counts.unique == count);
    assert(t.counts.unique_type[HG_TYPE_AP] == count / 2);
    assert(t.counts.unique_type[HG_TYPE_BLE] == count / 2);
    assert(t.counts.identifiable_unique == count * 3 / 4);
    assert(t.seen_count == TALLY_MAX_DEVICES * 3 / 4);
    assert(unique_index_flush(&u));
    memset(u.cache, 0, sizeof u.cache); // force disk reads after cache eviction
    for (unsigned i = count; i-- > 0;) {
        hg_record_t r = record(i, 1, 1);
        assert(tally_add(&t, &r));
    }
    assert(t.counts.total == count * 2);
    assert(t.counts.unique == count);
    assert(t.counts.unique_node[0] == count && t.counts.unique_node[1] == count);
    assert(t.counts.unique_band[0] == count / 2 && t.counts.unique_band[1] == count / 2);
    assert(t.counts.table_full == 0);
    assert(unique_index_flush(&u));
    printf("exact uniques %u, observations %u, disk bytes %zu, reads %u, writes %u\n",
           count, count * 2, bytes, reads, writes);

    // the same address with a third type is a distinct key.
    hg_record_t r = record(count - 2, 0, 0);
    r.type = HG_TYPE_CLIENT; hg_record_seal(&r);
    assert(tally_add(&t, &r));
    assert(t.counts.unique == count + 1);
    assert(unique_index_flush(&u));

    // a read failure must never be interpreted as an empty bucket.
    memset(u.cache, 0, sizeof u.cache);
    fail_read = 1;
    r = record(count - 1, 0, 0);
    assert(tally_add(&t, &r)); // CSV still gets a valid observation
    assert(t.counts.unique == count + 1 && t.counts.table_full == 1 && u.failed);
    fail_read = 0;
    assert(tally_add(&t, &r)); // failure stays latched, no empty restart
    assert(t.counts.unique == count + 1 && t.counts.table_full == 2);

    // a fresh boot ignores all old file contents; it is a new since-boot set.
    reset();
    tally_seen before;
    assert(unique_index_touch(&u, &r, &before) && !before.used);
    assert(unique_index_touch(&u, &r, &before) && before.used);
    fail_write = 1;
    assert(!unique_index_flush(&u) && u.failed);
    fail_write = 0;
    assert(!unique_index_touch(&u, &r, &before));

    reset();
    assert(unique_index_touch(&u, &r, &before));
    fail_flush = 1;
    assert(!unique_index_flush(&u) && u.failed);

    reset();
    assert(unique_index_touch(&u, &r, &before));
    assert(unique_index_flush(&u));
    memset(u.cache, 0, sizeof u.cache);
    disk[8] ^= 1;
    assert(!unique_index_touch(&u, &r, &before) && u.failed);
    free(disk);
    puts("unique index collision, eviction, restart and I/O failure tests ok");
    return 0;
}
