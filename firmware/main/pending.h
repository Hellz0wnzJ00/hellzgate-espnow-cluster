// observations taken before the receiver has given us a date
//
// Before GNSS supplies UTC, the monotonic timer provides uptime only.
// Buffered rows receive estimated UTC from the GNSS-to-uptime offset. This
// timestamps master reception, not the original observation time at a scanner.

#ifndef PENDING_H
#define PENDING_H

#include <stdint.h>

#include "csv.h"
#include "hg_record.h"

// how a held row gets written once its time is known. the fix carries no
// timestamp, the caller formats that from unix_sec
typedef void (*pending_emit)(const hg_record_t *r, const csv_fix *fix,
                             int64_t unix_sec);

// returns 0 if it was held, and non zero if the buffer is full. the caller then
// writes the row rather than lose it
int pending_add(const hg_record_t *r, const csv_fix *fix, int64_t at_us);

// rows held and not yet written
uint32_t pending_count(void);

// True after the configured boot-relative wait expires. If UTC remains
// unavailable, rows are exported with empty timestamps after the backlog drains.
int pending_gave_up(void);

// writes up to max held rows, oldest first, timed from when the boot happened.
// returns how many went. the master loop calls this a chunk at a time, so a
// backlog is split between collection passes. This reduces blocking but
// does not guarantee that receive queues or scanner rings cannot overflow
uint32_t pending_flush_some(int64_t boot_unix, pending_emit emit, uint32_t max);

// writes everything held. only for a run that is ending
void pending_flush(int64_t boot_unix, pending_emit emit);

// throws away anything still held. a run that ends while rows are waiting must
// not leave them to turn up in the next one. settle them first
void pending_discard(void);

#endif
