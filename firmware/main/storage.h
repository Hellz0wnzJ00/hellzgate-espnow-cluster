// microsd session files
// The card holds one CSV per run. The master also writes rows to the serial
// console, which remains available when no card file is open.

#ifndef STORAGE_H
#define STORAGE_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// mounts the card. an absent or unreadable card is not fatal, it comes back
// with an error and everything below still answers
esp_err_t storage_mount(void);

int storage_ready(void);

// opens a new file and writes the wigle header into it. name is what the user
// called the run, anything awkward in it is replaced
// start_unix is when the run began, in real time, or 0 if the receiver never
// gave us a date. the filename carries it
esp_err_t storage_open(const char *name, int64_t start_unix);

// appends one already formatted csv row. does nothing when no session is open
void storage_write(const char *line);

// flushes to the card if it is time. call it from the master loop, it decides
void storage_tick(void);

void storage_close(void);

int storage_open_now(void);

// what the status page and the log line need
// the longest a file path gets. mount point, the run name, a full timestamp and
// a duplicate counter, with room to spare
#define STORAGE_PATH_MAX 80

// the path is copied out rather than pointed at. the collect task rewrites the
// one inside storage every time a file opens
// rows is what went into the file, saved is what the card has confirmed with a
// clean flush and sync. on a clean stop the two are equal
void storage_stats(char *path, size_t path_n, uint32_t *rows, uint32_t *saved,
                   uint32_t *errors, uint64_t *free_bytes);

// serialize scratch-index I/O with CSV I/O, including the custom SPI driver.
void storage_hold(void);
void storage_release(void);

#endif
