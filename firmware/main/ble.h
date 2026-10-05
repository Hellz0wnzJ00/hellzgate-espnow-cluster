// passive ble observer
// same deal as the wifi scan, it fills the locked record and hands it to a
// sink. it never advertises, connects or asks anything for a name.

#ifndef BLE_H
#define BLE_H

#include <stdint.h>

#include "hg_record.h"

typedef void (*ble_sink)(const hg_record_t *r);

// starts the host and the scan, records start arriving on their own
void ble_start(uint8_t node_id, ble_sink sink);

// how many advertising reports we have seen since boot
uint32_t ble_seen(void);
void ble_service(void);

#endif
