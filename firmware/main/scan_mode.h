#ifndef HG_SCAN_MODE_H
#define HG_SCAN_MODE_H

#include <stdint.h>

#define HG_SCAN_WIFI  0
#define HG_SCAN_MIX   1
#define HG_SCAN_UNKNOWN 255
#define HG_SCAN_MODE_BYTES 5

typedef struct {
    uint32_t revision;
    uint8_t mode;
} hg_scan_mode;

void scan_mode_init(void);
int scan_mode_set(uint8_t mode);
hg_scan_mode scan_mode_wanted(void);
hg_scan_mode scan_mode_applied(void);
void scan_mode_note(hg_scan_mode mode);
void scan_mode_receive(const uint8_t *wire);
void scan_mode_pack(uint8_t *wire, hg_scan_mode mode);
hg_scan_mode scan_mode_unpack(const uint8_t *wire);
void scan_mode_seen(uint8_t id, hg_scan_mode mode);
hg_scan_mode scan_mode_node(uint8_t id);

#endif
