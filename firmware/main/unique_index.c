#include <string.h>
#include "unique_index.h"

// next page, entry count, then 31 sixteen-byte entries; checksum at the end.
// explicit bytes keep the on-card format independent of compiler packing.
#define ENTRIES 31
#define LAST_PAGE (2147483647u / UNIQUE_PAGE_BYTES)
_Static_assert((UNIQUE_BUCKETS & (UNIQUE_BUCKETS - 1)) == 0,
               "bucket count must be a power of two");

static uint32_t get32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t n)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(n >> (8 * i));
}

static uint32_t hash(const uint8_t *p, unsigned n)
{
    uint32_t h = 2166136261u;
    while (n--) { h ^= *p++; h *= 16777619u; }
    return h;
}

static int save(unique_index *u, unique_page *p)
{
    if (!p->dirty) return 1;
    put32(p->data + 508, hash(p->data, 508));
    if (!u->io.write(u->io.ctx, (p->id - 1) * UNIQUE_PAGE_BYTES, p->data)) {
        u->failed = 1;
        return 0;
    }
    p->dirty = 0;
    return 1;
}

static unique_page *page(unique_index *u, uint32_t id, int fresh)
{
    if (!id || id > u->pages) { u->failed = 1; return NULL; }
    unique_page *p = &u->cache[id % UNIQUE_CACHE_PAGES];
    if (p->id == id) return p;
    if (!save(u, p)) return NULL;
    p->id = 0;
    if (fresh) {
        memset(p->data, 0, sizeof p->data);
    } else if (!u->io.read(u->io.ctx, (id - 1) * UNIQUE_PAGE_BYTES, p->data) ||
               get32(p->data + 508) != hash(p->data, 508) ||
               p->data[4] > ENTRIES || get32(p->data) >= id) {
        // chains only point backwards, so corruption cannot make a loop.
        u->failed = 1;
        return NULL;
    }
    p->id = id;
    return p;
}

void unique_index_init(unique_index *u, unique_io io)
{
    memset(u, 0, sizeof *u);
    u->io = io;
}

int unique_index_touch(void *ctx, const hg_record_t *r, tally_seen *before)
{
    unique_index *u = ctx;
    if (u->failed) return 0;
    uint8_t key[7];
    memcpy(key, r->bssid, 6);
    key[6] = r->type;
    uint32_t bucket = hash(key, sizeof key) & (UNIQUE_BUCKETS - 1);
    uint32_t head = u->heads[bucket];
    unique_page *p;
    uint8_t *e = NULL;

    for (uint32_t id = head; id; id = get32(p->data)) {
        p = page(u, id, 0);
        if (!p) return 0;
        for (unsigned i = 0; i < p->data[4]; i++) {
            uint8_t *candidate = p->data + 8 + i * 16;
            if (!memcmp(candidate, key, sizeof key)) {
                e = candidate;
                break;
            }
        }
        if (e) break;
    }

    memset(before, 0, sizeof *before);
    if (e) {
        memcpy(before->addr, e, 6);
        before->type = e[6];
        before->used = 1;
        before->bands = e[7];
        before->nodes = get32(e + 8);
    } else {
        p = head ? page(u, head, 0) : NULL;
        if (head && !p) return 0;
        if (!p || p->data[4] == ENTRIES) {
            if (u->pages == LAST_PAGE) { u->failed = 1; return 0; }
            p = page(u, ++u->pages, 1);
            if (!p) return 0;
            put32(p->data, head);
            u->heads[bucket] = p->id;
        }
        e = p->data + 8 + p->data[4]++ * 16;
        memcpy(e, key, sizeof key);
        p->dirty = 1;
    }

    uint8_t bands = before->bands;
    if (r->type != HG_TYPE_BLE) bands |= (uint8_t)(1u << r->band);
    uint32_t nodes = before->nodes | (1u << r->node_id);
    if (e[7] != bands || get32(e + 8) != nodes) {
        e[7] = bands;
        put32(e + 8, nodes);
        p->dirty = 1;
    }
    return 1;
}

int unique_index_flush(unique_index *u)
{
    if (u->failed) return 0;
    for (unsigned i = 0; i < UNIQUE_CACHE_PAGES; i++)
        if (!save(u, &u->cache[i])) return 0;
    if (!u->io.flush(u->io.ctx)) { u->failed = 1; return 0; }
    return 1;
}
