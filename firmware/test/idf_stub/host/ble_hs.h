#ifndef TEST_BLE_HS_H
#define TEST_BLE_HS_H
#include <stdint.h>
#define BLE_ADDR_PUBLIC 0
#define BLE_ADDR_PUBLIC_ID 2
#define BLE_GAP_EVENT_DISC 1
#define BLE_GAP_EVENT_DISC_COMPLETE 2
#define BLE_HS_EALREADY 2
typedef struct { uint8_t type; uint8_t val[6]; } ble_addr_t;
struct ble_gap_disc_desc { ble_addr_t addr; int8_t rssi; const uint8_t *data; uint8_t length_data; };
struct ble_gap_event { int type; struct ble_gap_disc_desc disc; };
struct ble_hs_adv_fields { const uint8_t *name; uint8_t name_len; };
struct ble_gap_disc_params { int itvl, window, filter_policy, limited, passive, filter_duplicates; };
struct test_hs_cfg { void (*sync_cb)(void); void (*reset_cb)(int); };
extern struct test_hs_cfg ble_hs_cfg;
int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *, const uint8_t *, uint8_t);
int ble_hs_id_infer_auto(int, uint8_t *);
int ble_gap_disc_active(void);
int ble_gap_disc_cancel(void);
int ble_gap_disc(uint8_t, int, const struct ble_gap_disc_params *, int (*cb)(struct ble_gap_event *, void *), void *);
#endif
