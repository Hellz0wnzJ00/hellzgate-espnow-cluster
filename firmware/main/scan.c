#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "scan.h"

static const char *tag = "scan";

static uint8_t   my_node_id;
static scan_sink emit;

// Maximum AP records read per channel scan. Results beyond this fixed-size
// buffer are not exported; this limit is not a guarantee for dense RF areas.
#define MAX_AP 96

// we used to set the passive dwell to 120 ms. with bluetooth enabled idf logs
// "should use default passive scan time parameter" and picks its own value to
// fit around the coexistence slots, so setting it achieved nothing except the
// warning. left to the driver now

// experimental order from distinct wifi addresses in the shared October 3
// trip window. the first six are revisited between groups of quieter channels.
// each scanner covers the entire list, starting at a different place, so an
// empty slot does not lose any channels. the driver still enforces the region.
static const uint8_t channels[] = {
    6, 1, 11, 149, 44, 157, 36, 153, 48, 9, 40, 161, 3, 2,
    6, 1, 11, 149, 44, 157, 7, 4, 10, 8, 100, 5, 52, 132,
    6, 1, 11, 149, 44, 157, 116, 165, 60, 140, 144, 108, 56, 64,
    6, 1, 11, 149, 44, 157, 128, 104, 136, 120, 112, 12, 124, 13
};

static int cur_idx;
static int sweep_steps;
static int64_t sweep_start_us;
static uint32_t sweep_ms;
static uint32_t sweep_scan_us;   // chunk time, including any in-chunk reporting yields
static uint32_t worst_away_ms;   // longest we were ever away from home
static int64_t  last_home_us;

void radio_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());

    // espnow lives on one 2.4 channel, park there so peers can hear us
    ESP_ERROR_CHECK(esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY));
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_HG_ESPNOW_CHANNEL,
                                         WIFI_SECOND_CHAN_NONE));
}

static int  (*yield_due)(void);
static void (*yield_run)(void);

void scan_set_yield(int (*due)(void), void (*run)(void))
{
    yield_due = due;
    yield_run = run;
}

void scan_init(uint8_t node_id, scan_sink sink)
{
    my_node_id = node_id;
    emit = sink;
    cur_idx = (node_id * 5u) % sizeof channels;
    sweep_start_us = esp_timer_get_time();
}

static void record_from_ap(const wifi_ap_record_t *ap, uint8_t band)
{
    hg_record_t r;
    memset(&r, 0, sizeof r);

    memcpy(r.bssid, ap->bssid, sizeof r.bssid);
    r.rssi    = ap->rssi;
    r.channel = ap->primary;
    r.band    = band;
    r.type    = HG_TYPE_AP;
    r.node_id = my_node_id;

    // both sides are 33 bytes but copy bounded and terminate anyway, an ssid
    // is attacker controlled and we are about to hand it to the csv writer
    memcpy(r.ssid, ap->ssid, sizeof r.ssid - 1);
    r.ssid[sizeof r.ssid - 1] = '\0';

    hg_record_seal(&r);

    if (emit != NULL)
        emit(&r);
}

// Passive Wi-Fi scan: no probe requests. ESP-NOW reporting still transmits.
static int scan_channel(uint8_t ch, uint8_t band)
{
    static wifi_ap_record_t found[MAX_AP];

    wifi_scan_config_t cfg = {
        .channel = ch,
        .scan_type = WIFI_SCAN_TYPE_PASSIVE,
    };

    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        // a channel the region does not allow just gets skipped, not fatal
        ESP_LOGW(tag, "channel %u refused, %s", ch, esp_err_to_name(err));
        return 0;
    }

    uint16_t n = MAX_AP;
    err = esp_wifi_scan_get_ap_records(&n, found);
    if (err != ESP_OK) {
        ESP_LOGW(tag, "reading results failed, %s", esp_err_to_name(err));
        return 0;
    }

    for (uint16_t i = 0; i < n; i++)
        record_from_ap(&found[i], band);

    return (int)n;
}

// if this fails the node is stuck off channel and the master calls it dead,
// so it has to say something
static void go_home(void)
{
    // liveness depends on how long we were away, which is what this measures.
    // a chunk can get interrupted part way so its length is a different number
    int64_t now = esp_timer_get_time();
    uint32_t away_ms = (uint32_t)((now - last_home_us) / 1000);
    if (away_ms > worst_away_ms)
        worst_away_ms = away_ms;

    esp_err_t err = esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
    if (err != ESP_OK)
        ESP_LOGW(tag, "could not get back to 2g4, %s", esp_err_to_name(err));

    err = esp_wifi_set_channel(CONFIG_HG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK)
        ESP_LOGW(tag, "could not get back to channel %d, %s",
                 CONFIG_HG_ESPNOW_CHANNEL, esp_err_to_name(err));

    uint8_t ch; wifi_second_chan_t sec;
    if (esp_wifi_get_channel(&ch, &sec) == ESP_OK && ch != CONFIG_HG_ESPNOW_CHANNEL)
        ESP_LOGW(tag, "home is channel %d but we are sat on %u",
                 CONFIG_HG_ESPNOW_CHANNEL, ch);

    // the channel change is not instant. sending into it right away got a
    // third of the frames unacknowledged, so let the radio settle first
    vTaskDelay(pdMS_TO_TICKS(10));

    last_home_us = esp_timer_get_time();
}

int scan_step(void)
{
    int64_t t0 = esp_timer_get_time();
    int done = 0;

    for (int i = 0; i < CONFIG_HG_SCAN_CHUNK; i++) {
        uint8_t ch = channels[cur_idx];
        int two = ch <= 14;
        esp_err_t err = esp_wifi_set_band_mode(two ? WIFI_BAND_MODE_2G_ONLY
                                                  : WIFI_BAND_MODE_5G_ONLY);
        if (err == ESP_OK)
            scan_channel(ch, two ? HG_BAND_2G4 : HG_BAND_5G);
        else
            ESP_LOGW(tag, "channel %u band refused, %s", ch, esp_err_to_name(err));

        cur_idx = (cur_idx + 1) % sizeof channels;
        sweep_steps++;

        if (yield_due != NULL && yield_due()) {
            go_home();
            yield_run();
        }

        if (sweep_steps == sizeof channels) {
            sweep_steps = 0;
            done = 1;
            break;
        }
    }

    sweep_scan_us += (uint32_t)(esp_timer_get_time() - t0);
    if (done) {
        sweep_ms = (uint32_t)((esp_timer_get_time() - sweep_start_us) / 1000);
        ESP_LOGI(tag, "sweep done in %lu ms, %lu ms scanning, longest away %lu ms",
                 (unsigned long)sweep_ms, (unsigned long)(sweep_scan_us / 1000),
                 (unsigned long)worst_away_ms);
        sweep_start_us = esp_timer_get_time();
        sweep_scan_us = 0;
        worst_away_ms = 0;
    }

    // back on the espnow channel before anyone can call us missing
    go_home();
    return done;
}

uint32_t scan_last_sweep_ms(void)
{
    return sweep_ms;
}
