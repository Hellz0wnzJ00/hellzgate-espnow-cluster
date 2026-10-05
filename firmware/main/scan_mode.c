// the links only copy requests. the ble host applies them in its own task,
// and reports that exact revision once the controller has accepted it.

#include "freertos/FreeRTOS.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "scan_mode.h"

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static hg_scan_mode wanted = { .mode = HG_SCAN_MIX };
static hg_scan_mode applied = { .mode = HG_SCAN_UNKNOWN };
static hg_scan_mode reported[CONFIG_HG_MAX_NODES];
static int64_t heard[CONFIG_HG_MAX_NODES];

void scan_mode_seen(uint8_t id, hg_scan_mode mode)
{
    if (id >= CONFIG_HG_MAX_NODES)
        return;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_SAFE(&lock);
    reported[id] = mode;
    heard[id] = now;
    portEXIT_CRITICAL_SAFE(&lock);
}

hg_scan_mode scan_mode_node(uint8_t id)
{
    hg_scan_mode mode = { .mode = HG_SCAN_UNKNOWN };
    if (id >= CONFIG_HG_MAX_NODES)
        return mode;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_SAFE(&lock);
    if (heard[id] != 0 && now - heard[id] <
        (int64_t)CONFIG_HG_HEARTBEAT_MS * CONFIG_HG_HEARTBEAT_MISSES * 1000)
        mode = reported[id];
    portEXIT_CRITICAL_SAFE(&lock);
    return mode;
}

void scan_mode_init(void)
{
#ifdef CONFIG_HG_ROLE_MASTER
    wanted.revision = esp_random() | 1;
#endif
}

int scan_mode_set(uint8_t mode)
{
    if (mode > HG_SCAN_MIX)
        return 0;
    portENTER_CRITICAL_SAFE(&lock);
    if (wanted.mode != mode) {
        wanted.mode = mode;
        if (++wanted.revision == 0)
            wanted.revision = 1;
    }
    portEXIT_CRITICAL_SAFE(&lock);
    return 1;
}

hg_scan_mode scan_mode_wanted(void)
{
    portENTER_CRITICAL_SAFE(&lock);
    hg_scan_mode mode = wanted;
    portEXIT_CRITICAL_SAFE(&lock);
    return mode;
}

hg_scan_mode scan_mode_applied(void)
{
    portENTER_CRITICAL_SAFE(&lock);
    hg_scan_mode mode = applied;
    portEXIT_CRITICAL_SAFE(&lock);
    return mode;
}

void scan_mode_note(hg_scan_mode mode)
{
    portENTER_CRITICAL_SAFE(&lock);
    applied = mode;
    portEXIT_CRITICAL_SAFE(&lock);
}

hg_scan_mode scan_mode_unpack(const uint8_t *wire)
{
    hg_scan_mode mode = {
        .revision = (uint32_t)wire[0] | ((uint32_t)wire[1] << 8) |
                    ((uint32_t)wire[2] << 16) | ((uint32_t)wire[3] << 24),
        .mode = wire[4],
    };
    return mode;
}

void scan_mode_pack(uint8_t *wire, hg_scan_mode mode)
{
    for (int i = 0; i < 4; i++)
        wire[i] = (uint8_t)(mode.revision >> (8 * i));
    wire[4] = mode.mode;
}

void scan_mode_receive(const uint8_t *wire)
{
    hg_scan_mode mode = scan_mode_unpack(wire);
    if (mode.mode > HG_SCAN_MIX || mode.revision == 0)
        return;
    portENTER_CRITICAL_SAFE(&lock);
    wanted = mode;
    portEXIT_CRITICAL_SAFE(&lock);
}
