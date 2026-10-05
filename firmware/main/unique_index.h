// exact overflow index. one owner, one scratch file, no saved state across boots
#ifndef UNIQUE_INDEX_H
#define UNIQUE_INDEX_H

#include <stdint.h>
#include "tally.h"

#define UNIQUE_PAGE_BYTES 512
#ifndef UNIQUE_BUCKETS
#define UNIQUE_BUCKETS 65536
#endif
#ifndef UNIQUE_CACHE_PAGES
#define UNIQUE_CACHE_PAGES 128
#endif

// callbacks transfer a whole page and return 1 only on success. flush must
// check buffered writes too. offsets never exceed signed 32 bit file offsets.
typedef struct {
    int (*read)(void *, uint32_t, uint8_t *);
    int (*write)(void *, uint32_t, const uint8_t *);
    int (*flush)(void *);
    void *ctx;
} unique_io;

typedef struct {
    uint32_t id;
    uint8_t dirty;
    uint8_t data[UNIQUE_PAGE_BYTES];
} unique_page;

typedef struct {
    uint32_t heads[UNIQUE_BUCKETS];
    unique_page cache[UNIQUE_CACHE_PAGES];
    unique_io io;
    uint32_t pages;
    int failed;
} unique_index;

void unique_index_init(unique_index *u, unique_io io);
// returns the entry BEFORE this sighting, then remembers its band and node.
// 1 success, 0 failure. never substitutes a hash for the actual address.
int unique_index_touch(void *ctx, const hg_record_t *r, tally_seen *before);
int unique_index_flush(unique_index *u);

#endif
