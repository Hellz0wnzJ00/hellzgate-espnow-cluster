// honest counts for the master
// totals, uniques, and the identifiable unique that rotating ble cannot inflate

#ifndef TALLY_H
#define TALLY_H

#include <stdint.h>

// picks up the cluster size on target, the host tests build without it
#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

#include "hg_record.h"

// RAM slots; an optional exact SD index holds addresses beyond this cache
// must stay a power of two, the lookup masks against it
#ifndef TALLY_MAX_DEVICES
#ifdef CONFIG_HG_TALLY_MAX_DEVICES
#define TALLY_MAX_DEVICES CONFIG_HG_TALLY_MAX_DEVICES
#else
#define TALLY_MAX_DEVICES 2048
#endif
#endif

#define TALLY_BANDS 2

// ap, ble, client. the record enum owns these values, this is just the width
#define TALLY_TYPES 3

// Match the supplied 20-node builds when compiling without sdkconfig.h.
// The node bitmask supports at most 32 IDs.
#ifndef TALLY_NODES
#ifdef CONFIG_HG_MAX_NODES
#define TALLY_NODES CONFIG_HG_MAX_NODES
#else
#define TALLY_NODES 20
#endif
#endif

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(TALLY_NODES <= 32, "node bitmask is 32 bits wide");
#endif

typedef struct {
    uint8_t  addr[6];
    uint8_t  type;    // part of the key, an ap and a ble device can share an address
    uint8_t  used;
    uint8_t  bands;   // bit per band, a device on both counts on both
    uint32_t nodes;   // bit per node that has seen it
} tally_seen;

typedef struct {
    uint32_t total;
    uint32_t unique;
    uint32_t identifiable_unique;

    uint32_t bad_crc;     // dropped, crc did not match
    uint32_t bad_field;   // dropped, band or node id out of range
    uint32_t table_full;  // observations whose uniqueness could not be checked

    uint32_t total_type[TALLY_TYPES];
    uint32_t unique_type[TALLY_TYPES];  // first sighting decides the type

    uint32_t total_band[TALLY_BANDS];
    uint32_t unique_band[TALLY_BANDS];
    uint32_t total_node[TALLY_NODES];
    uint32_t unique_node[TALLY_NODES];

} tally_counts;

// optional overflow store; updates its entry and returns the previous value.
typedef int (*tally_overflow)(void *, const hg_record_t *, tally_seen *);

typedef struct {
    tally_counts counts;
    tally_seen seen[TALLY_MAX_DEVICES];
    uint32_t   seen_count;
    tally_overflow overflow;
    void *overflow_ctx;
} tally;

void tally_reset(tally *t);

// returns 1 when the record was counted, 0 when it was dropped
int tally_add(tally *t, const hg_record_t *r);

#endif
