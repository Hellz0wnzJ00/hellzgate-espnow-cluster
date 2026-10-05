#ifndef TEST_NIMBLE_PORT_H
#define TEST_NIMBLE_PORT_H
#include "esp_err.h"
struct ble_npl_event { void (*fn)(struct ble_npl_event *); };
static inline void ble_npl_event_init(struct ble_npl_event *e, void (*fn)(struct ble_npl_event *), void *arg) { (void)arg; e->fn = fn; }
static inline void *nimble_port_get_dflt_eventq(void) { return 0; }
void ble_npl_eventq_put(void *q, struct ble_npl_event *e);
esp_err_t nimble_port_init(void);
void nimble_port_run(void);
#endif
