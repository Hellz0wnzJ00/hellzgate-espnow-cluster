#ifndef UNIQUE_TRACKER_H
#define UNIQUE_TRACKER_H

#include "tally.h"

typedef struct {
    uint32_t pending;
    uint32_t dropped;
    uint32_t errors;
    uint32_t high_water;
    int sd;
    int incomplete;
} unique_status;

void unique_tracker_start(void);
// validates and queues without waiting for disk. valid rows still reach CSV
// if this queue is full; the status then marks uniques as a lower bound.
int unique_tracker_add(const hg_record_t *r);
void unique_tracker_snapshot(tally_counts *out, unique_status *status);

#endif
