// UART GNSS input. GGA supplies position/fix data; RMC supplies UTC date/time.
// Other receiver sentences are ignored. Match pins and baud to the hardware.

#ifndef GNSS_H
#define GNSS_H

#include <stdint.h>

#include "esp_err.h"

typedef struct {
    int have_fix;
    double lat;
    double lon;
    double alt_m;
    double accuracy_m;   // worked out from hdop, the receiver does not report it
    uint8_t sats;
    uint8_t quality;     // gga fix quality, 0 none, 1 gps, 2 dgps
    int64_t age_ms;      // how long ago that fix was parsed
} gnss_fix;

// starts the uart and the reader task. safe to call when gnss is built out, it
// returns ok and every read after it says no fix
esp_err_t gnss_start(void);

// snapshot of the last fix, callable from any task
void gnss_read(gnss_fix *out);

// unix seconds from the receiver clock, carried forward with the system timer
// between sentences. zero until it has given us a date at least once
int64_t gnss_unix(void);

// how many sentences came in and how many were thrown away, for the status page
void gnss_stats(uint32_t *sentences, uint32_t *rejected);

// runs canned sentences through the parser and returns how many checks failed.
// call it before gnss_start, it writes to the same state the reader task uses
int gnss_selftest(void);

#endif
