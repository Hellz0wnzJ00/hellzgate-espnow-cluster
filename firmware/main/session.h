// a run, start to stop
// this owns whether the unit is recording and what the run is called. the card
// is opened and closed underneath it, and the web page and the json endpoint
// drive it from above

#ifndef SESSION_H
#define SESSION_H

#include <stdint.h>

#include "esp_err.h"

#define SESSION_NAME_MAX 24

typedef struct {
    char name[SESSION_NAME_MAX];
    int running;
    int on_card;          // the rows are reaching a file, not just the console
    int64_t started_unix; // 0 if the run began before the receiver had a date
    uint32_t seconds;
    uint32_t records;
} session_info;

// Starts a run; an empty name becomes "run". Filename punctuation is replaced
// with underscores. Starting while running stops the previous session first;
// storage errors are reported separately and a successful close is not guaranteed.
esp_err_t session_start(const char *name);

void session_stop(void);

// opens the card file if it is not open yet. called on the first row rather
// than at the start of the run, so the filename can carry the real time
void session_ensure_file(void);

// tells the session when the boot happened in real time, once the receiver has
// said. the run's own start is worked out from that, so the filename carries
// when the run actually began and not when the first row happened to land
void session_set_start(int64_t boot_unix);

// called before the file closes so held rows land in the run they were taken
// in. without it they survive the stop and turn up in the next session
void session_on_stop(void (*settle)(void));

// one gate over the whole session, the held rows and the file. the collect task
// and the web task both reach this state, and a start or a stop has to be
// finished before a row can be taken against it. held rows are not protected by
// the storage lock, that one only covers the file
void session_hold(void);
void session_release(void);

int session_running(void);

// one record went into this run. counted here rather than in the csv layer so
// the number matches the run and not the lifetime of the board
void session_record(void);

void session_state(session_info *out);

#endif
