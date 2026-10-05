// local page and json for the master
// the unit brings up its own access point, so there is no router to configure
// and nothing here ever reaches the internet. join the network, open the page,
// look at the counts and start or stop a run

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "web.h"
#include "unique_tracker.h"

static const char *tag = "web";

#ifdef CONFIG_HG_WEB

#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "csv.h"
#include "fan.h"
#include "gnss.h"
#include "session.h"
#include "storage.h"
#include "transport.h"
#include "scan_mode.h"

// Status buffer includes per-node data and spare capacity. Overflow returns an error.
#define JSON_MAX (1536 + HG_MAX_NODES * 448)

// a buffer that knows when it is full. printf into a fixed array and the tail
// just disappears, and half a json document still parses as far as it goes
typedef struct {
    char  *buf;
    size_t size;
    size_t n;
    int    over;
} sink;

static void put(sink *s, const char *fmt, ...)
{
    if (s->over)
        return;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s->buf + s->n, s->size - s->n, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= s->size - s->n) {
        s->over = 1;
        return;
    }

    s->n += (size_t)n;
}

// a run name is whatever someone typed, so a quote or a backslash in it would
// break the document open. control characters go as well, they are not worth
// spelling out properly for a label
static void put_str(sink *s, const char *in)
{
    put(s, "\"");

    for (; in != NULL && *in != '\0'; in++) {
        if (*in == '"' || *in == '\\')
            put(s, "\\%c", *in);
        else if ((unsigned char)*in >= 0x20)
            put(s, "%c", *in);
    }

    put(s, "\"");
}

static const char *state_name(hg_node_state st)
{
    if (st == HG_NODE_UP)
        return "up";
    return "down";
}

static void put_node(sink *s, int *first, const char *link,
                     uint8_t id, const hg_node_info *n)
{
    if (n->state == HG_NODE_UNSEEN)
        return;

    // overflow keeps the meaning it always had, both kinds added together, so
    // anything already reading it sees no change. the two parts sit beside it
    put(s, "%s{\"id\":%u,\"link\":\"%s\",\"state\":\"%s\","
           "\"records\":%lu,\"frames\":%lu,\"lost\":%lu,\"dupes\":%lu,"
           "\"overflow\":%lu,\"master_full\":%lu,\"node_overflow\":%lu,"
           "\"wrong_id\":%lu,\"reframes\":%lu,"
           "\"downs\":%lu,\"restarts\":%lu,\"last_seen_ms\":%lu",
        *first ? "" : ",", (unsigned)id, link, state_name(n->state),
        (unsigned long)n->records, (unsigned long)n->frames,
        (unsigned long)n->frames_lost, (unsigned long)n->dupes,
        (unsigned long)(n->inbox_full + n->node_overflow),
        (unsigned long)n->inbox_full, (unsigned long)n->node_overflow,
        (unsigned long)n->wrong_id, (unsigned long)n->reframes,
        (unsigned long)n->downs, (unsigned long)n->restarts,
        (unsigned long)n->last_seen_ms);

    hg_scan_mode mode = scan_mode_node(id);
    hg_scan_mode want = scan_mode_wanted();
    put(s, ",\"scan_mode\":%u,\"scan_applied\":%d}", mode.mode,
        n->state == HG_NODE_UP && mode.revision == want.revision && mode.mode == want.mode);
    *first = 0;
}

// Collect status in one response. Subsystems are read sequentially, so this
// is not an atomic snapshot of every counter at one instant.
static void build_status(sink *s)
{
    session_info run;
    session_state(&run);

    gnss_fix g;
    gnss_read(&g);

    char path[STORAGE_PATH_MAX];
    uint32_t rows, saved, errors;
    uint64_t free_bytes;
    storage_stats(path, sizeof path, &rows, &saved, &errors, &free_bytes);

    char when[24];
    csv_time(when, sizeof when, csv_now());

    put(s, "{\"uptime_s\":%lld,\"time\":\"%s\",\"hotspot\":%d,\"fan\":%d,",
        (long long)(esp_timer_get_time() / 1000000), when,
        web_ap_on(), fan_on());

    hg_scan_mode mode = scan_mode_wanted();
    put(s, "\"scan_mode\":%u,", mode.mode);

    put(s, "\"session\":{\"name\":");
    put_str(s, run.name);
    put(s, ",\"running\":%d,\"on_card\":%d,\"seconds\":%lu,\"records\":%lu},",
        run.running, run.on_card,
        (unsigned long)run.seconds, (unsigned long)run.records);

    put(s, "\"gnss\":{\"fix\":%d", g.have_fix);
    if (g.have_fix)
        put(s, ",\"lat\":%.6f,\"lon\":%.6f,\"alt_m\":%.1f,\"accuracy_m\":%.1f",
            g.lat, g.lon, g.alt_m, g.accuracy_m);
    put(s, ",\"sats\":%u,\"quality\":%u,\"age_ms\":%lld},",
        (unsigned)g.sats, (unsigned)g.quality, (long long)g.age_ms);

    put(s, "\"card\":{\"mounted\":%d,\"file\":", storage_ready());
    if (path[0] != '\0')
        put_str(s, path);
    else
        put(s, "null");
    put(s, ",\"rows\":%lu,\"saved\":%lu,\"errors\":%lu,\"free_mb\":%llu},",
        (unsigned long)rows, (unsigned long)saved, (unsigned long)errors,
        (unsigned long long)(free_bytes >> 20));

    tally_counts snapshot;
    unique_status status;
    unique_tracker_snapshot(&snapshot, &status);
    const tally_counts *counts = &snapshot;
    put(s, "\"unique_tracker\":{\"backend\":\"%s\",\"pending\":%lu,"
           "\"dropped\":%lu,\"errors\":%lu,\"high_water\":%lu,\"incomplete\":%d},",
        status.sd ? "ram+sd" : "ram",
        (unsigned long)status.pending, (unsigned long)status.dropped,
        (unsigned long)status.errors, (unsigned long)status.high_water, status.incomplete);
    put(s, "\"counts\":{\"scope\":\"since_boot\",\"total\":%lu,\"unique\":%lu,\"identifiable\":%lu,"
           "\"ap\":%lu,\"ble\":%lu,\"client\":%lu,"
           "\"g2_4\":%lu,\"g5\":%lu,"
           "\"bad_crc\":%lu,\"bad_field\":%lu,\"table_full\":%lu},",
        (unsigned long)counts->total,
        (unsigned long)counts->unique,
        (unsigned long)counts->identifiable_unique,
        (unsigned long)counts->unique_type[HG_TYPE_AP],
        (unsigned long)counts->unique_type[HG_TYPE_BLE],
        (unsigned long)counts->unique_type[HG_TYPE_CLIENT],
        (unsigned long)counts->unique_band[HG_BAND_2G4],
        (unsigned long)counts->unique_band[HG_BAND_5G],
        (unsigned long)counts->bad_crc,
        (unsigned long)counts->bad_field,
        (unsigned long)counts->table_full);

    int first = 1;
    put(s, "\"nodes\":[");

    for (uint8_t i = 0; i < HG_MAX_NODES; i++)
        put_node(s, &first, "espnow", i, transport_node(i));


    put(s, "]}");
}

static esp_err_t status_get(httpd_req_t *req)
{
    char *buf = malloc(JSON_MAX);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }

    sink s = { .buf = buf, .size = JSON_MAX };
    build_status(&s);

    if (s.over) {
        ESP_LOGE(tag, "status did not fit in %d bytes", JSON_MAX);
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status too long");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, (ssize_t)s.n);
    free(buf);
    return ESP_OK;
}

// what comes back from a form field is percent encoded, so a run called north
// loop would arrive as north%20loop and end up in the filename that way
static void undo_percent(char *s)
{
    char *out = s;

    for (; *s != '\0'; s++) {
        if (*s == '+') {
            *out++ = ' ';
        } else if (*s == '%' && s[1] != '\0' && s[2] != '\0') {
            char hex[3] = { s[1], s[2], '\0' };
            char *end;
            long v = strtol(hex, &end, 16);
            if (*end != '\0')
                continue;
            *out++ = (char)v;
            s += 2;
        } else {
            *out++ = *s;
        }
    }

    *out = '\0';
}

static esp_err_t start_post(httpd_req_t *req)
{
    char name[SESSION_NAME_MAX] = "";
    char query[96];

    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK)
        httpd_query_key_value(query, "name", name, sizeof name);

    undo_percent(name);
    session_start(name);

    return status_get(req);
}

static esp_err_t stop_post(httpd_req_t *req)
{
    session_stop();
    return status_get(req);
}

// one page, no build step and nothing fetched from anywhere. it polls the same
// json endpoint anything else would use, so if the page works the endpoint does
static const char page[] =
"<!doctype html><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>hellzgate</title>"
"<style>"
"body{font:15px system-ui,sans-serif;margin:0;padding:16px;background:#111;color:#eee}"
"h1{font-size:18px;margin:0 0 12px}"
"section{background:#1c1c1c;border-radius:8px;padding:12px;margin-bottom:12px}"
"h2{font-size:13px;text-transform:uppercase;color:#888;margin:0 0 8px}"
"div.r{display:flex;justify-content:space-between;padding:2px 0}"
"span.v{font-variant-numeric:tabular-nums}"
"button{font:inherit;padding:8px 16px;margin-right:8px;border:0;border-radius:6px;background:#2d6;color:#111}"
"button.s{background:#d54;color:#fff}"
"input{font:inherit;padding:8px;border:0;border-radius:6px;background:#333;color:#eee;width:9em}"
"</style>"
"<h1>hellzgate</h1>"
"<section><h2>run</h2><div id=run></div><p>"
"<input id=name placeholder='run name'>"
"<button onclick='go(\"start\")'>start</button>"
"<button class=s onclick='go(\"stop\")'>stop</button></section>"
"<section><h2>board</h2><p>"
"<button id=hs onclick='flip(\"hotspot\")'>hotspot</button>"
"<button id=fn onclick='flip(\"fan\")'>fan</button>"
"<button id=sm onclick='setscan()'>Wi-Fi + BLE</button>"
"<span id=scanstate></span></section>"
"<section><h2>counts since boot</h2><div id=unique_status></div><div id=counts></div></section>"
"<section><h2>position</h2><div id=gnss></div></section>"
"<section><h2>card</h2><div id=card></div></section>"
"<section><h2>nodes</h2><div id=nodes></div></section>"
"<script>"
"function esc(v){let e=document.createElement('span');e.textContent=String(v);return e.innerHTML;}"
"function rows(o){return Object.keys(o).map(k=>"
"'<div class=r><span>'+esc(k)+'</span><span class=v>'+esc(o[k])+'</span></div>').join('')}"
"var scanmode=1;\n"
"function draw(d){"
"document.getElementById('run').innerHTML=rows({"
"name:d.session.name,state:d.session.running?'recording':'stopped',"
"writing:d.session.on_card?'card':'console only',"
"seconds:d.session.seconds,records:d.session.records,clock:d.time});"
"document.getElementById('counts').innerHTML=rows(d.counts);"
"let u=d.unique_tracker;document.getElementById('unique_status').textContent="
"(u.incomplete?'INCOMPLETE - unique counts are a lower bound':u.pending?'Catching up - '+u.pending+' observations pending':'Unique count caught up')"
"+' | '+u.backend+' | dropped '+u.dropped+' | errors '+u.errors;"
"var g=d.gnss;document.getElementById('gnss').innerHTML=rows(g.fix?{"
"latitude:g.lat,longitude:g.lon,altitude:g.alt_m+' m',"
"accuracy:g.accuracy_m+' m',satellites:g.sats,age:g.age_ms+' ms'}:"
"{fix:'searching',satellites:g.sats});"
"document.getElementById('card').innerHTML=rows({"
"mounted:d.card.mounted?'yes':'no',file:d.card.file||'none',"
"rows:d.card.rows,saved:d.card.saved,errors:d.card.errors,free:d.card.free_mb+' mb'});"
"document.getElementById('nodes').innerHTML=d.nodes.length?d.nodes.map(n=>"
"'<div class=r><span>'+n.link+' '+n.id+' '+n.state+'</span><span class=v>'"
"+n.records+' records, '+n.lost+' lost'+'</span></div>').join(''):"
"'<div class=r><span>nothing has reported yet</span></div>';"
"document.getElementById('hs').className=d.hotspot?'':'s';"
"document.getElementById('fn').className=d.fan?'':'s';"
"scanmode=d.scan_mode;document.getElementById('sm').textContent=scanmode?'Wi-Fi + BLE':'Wi-Fi Only';"
"var up=d.nodes.filter(n=>n.state=='up'),ok=up.filter(n=>n.scan_applied).length;"
"document.getElementById('scanstate').textContent=up.length?ok+'/'+up.length+' online links applied':'waiting for scanners';}"
"function setscan(){var b=document.getElementById('sm');b.disabled=true;"
"fetch('/api/scan?mode='+(scanmode?'wifi':'mixed'),{method:'POST'})"
".then(r=>{if(!r.ok)throw Error('request failed');return r.json()}).then(draw)"
".catch(()=>{document.getElementById('scanstate').textContent='request failed'})"
".finally(()=>{b.disabled=false})}"
"function tick(){fetch('/api/status').then(r=>r.json()).then(draw).catch(()=>{})}"
"function flip(what){fetch('/api/'+what,{method:'POST'})"
".then(r=>r.json()).then(draw)}"
"function go(what){fetch('/api/'+what+'?name='+encodeURIComponent("
"document.getElementById('name').value),{method:'POST'})"
".then(r=>r.json()).then(draw)}"
"tick();setInterval(tick,2000);"
"</script>";

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static int ap_up;

// the access point and espnow are the same radio. dropping the access point
// leaves the station side alone, so scanning and logging carry on either way
void web_ap_set(int on)
{
    esp_err_t err = esp_wifi_set_mode(on ? WIFI_MODE_APSTA : WIFI_MODE_STA);

    if (err != ESP_OK) {
        ESP_LOGE(tag, "could not turn the access point %s, %s",
                 on ? "on" : "off", esp_err_to_name(err));
        return;
    }

    ap_up = on ? 1 : 0;

    if (ap_up)
        ESP_LOGI(tag, "access point %s is up, page at http://192.168.4.1/",
                 CONFIG_HG_WEB_SSID);
    else
        ESP_LOGI(tag, "access point is off, scanning and logging carry on");
}

int web_ap_on(void)
{
    return ap_up;
}

#if CONFIG_HG_WEB_BUTTON_GPIO >= 0

// held for a moment, not tapped, so a knock does not put the radio up in the
// field. the pin is a strapping pin only while the chip is resetting
static void button_task(void *arg)
{
    (void)arg;

    const int pin = CONFIG_HG_WEB_BUTTON_GPIO;

    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);

    int held = 0;

    while (1) {
        if (gpio_get_level(pin) == 0) {
            held++;
            if (held == 6) {
                web_ap_set(!ap_up);
                // wait for the release so one hold is one toggle
                while (gpio_get_level(pin) == 0)
                    vTaskDelay(pdMS_TO_TICKS(50));
                held = 0;
            }
        } else {
            held = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

#endif

static void start_ap(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.ap.ssid, CONFIG_HG_WEB_SSID, sizeof cfg.ap.ssid);
    strncpy((char *)cfg.ap.password, CONFIG_HG_WEB_PASS, sizeof cfg.ap.password);

    cfg.ap.ssid_len = (uint8_t)strlen(CONFIG_HG_WEB_SSID);
    cfg.ap.max_connection = 4;

    // espnow and the access point are the same radio, so they have to sit on
    // the same channel. put the ap anywhere else and the cluster goes deaf
    cfg.ap.channel = CONFIG_HG_ESPNOW_CHANNEL;

    // wpa2 needs eight characters. a shorter one is treated as no password
    // rather than quietly refusing to come up at all
    cfg.ap.authmode = strlen(CONFIG_HG_WEB_PASS) >= 8
                      ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    // configured either way, brought up only if it is wanted at boot. the
    // config has to go on while the mode allows an access point, so it goes up
    // and comes straight back down when the unit is meant to start quiet
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ap_up = 1;

    ESP_LOGI(tag, "access point %s on channel %d, %s",
             CONFIG_HG_WEB_SSID, CONFIG_HG_ESPNOW_CHANNEL,
             cfg.ap.authmode == WIFI_AUTH_OPEN ? "open" : "wpa2");

#if !CONFIG_HG_WEB_AP_AT_BOOT
    web_ap_set(0);
#endif
}

static esp_err_t hotspot_post(httpd_req_t *req)
{
    char query[32];
    char want[8] = "";

    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK)
        httpd_query_key_value(query, "on", want, sizeof want);

    // no argument flips it, which is what the page's button sends
    web_ap_set(want[0] ? (want[0] == '1') : !web_ap_on());

    return status_get(req);
}

static esp_err_t scan_post(httpd_req_t *req)
{
    char query[32], want[8];
    if (httpd_req_get_url_query_str(req, query, sizeof query) != ESP_OK ||
        httpd_query_key_value(query, "mode", want, sizeof want) != ESP_OK ||
        (strcmp(want, "wifi") != 0 && strcmp(want, "mixed") != 0)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be wifi or mixed");
        return ESP_FAIL;
    }
    scan_mode_set(strcmp(want, "mixed") == 0 ? HG_SCAN_MIX : HG_SCAN_WIFI);
    return status_get(req);
}

static esp_err_t fan_post(httpd_req_t *req)
{
    char query[32];
    char want[8] = "";

    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK)
        httpd_query_key_value(query, "on", want, sizeof want);

    fan_set(want[0] ? (want[0] == '1') : !fan_on());

    return status_get(req);
}

esp_err_t web_start(void)
{

    start_ap();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();

    // the default 4096 is not enough. stopping a run settles the held rows and
    // writes them from this task, and that path goes down through fatfs and the
    // sd driver, which are deep
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);

    if (err != ESP_OK) {
        ESP_LOGE(tag, "the server would not start, %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = page_get },
        { .uri = "/api/status",  .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/start",   .method = HTTP_POST, .handler = start_post },
        { .uri = "/api/stop",    .method = HTTP_POST, .handler = stop_post },
        { .uri = "/api/hotspot", .method = HTTP_POST, .handler = hotspot_post },
        { .uri = "/api/scan",    .method = HTTP_POST, .handler = scan_post },
        { .uri = "/api/fan",     .method = HTTP_POST, .handler = fan_post },
    };

    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++)
        httpd_register_uri_handler(server, &uris[i]);

#if CONFIG_HG_WEB_BUTTON_GPIO >= 0
    xTaskCreate(button_task, "webbtn", 2560, NULL, 3, NULL);
    ESP_LOGI(tag, "hold the button on gpio%d for about a third of a second to turn the access point on or off",
             CONFIG_HG_WEB_BUTTON_GPIO);
#endif

    ESP_LOGI(tag, "page is at http://192.168.4.1/ and the json at /api/status");
    return ESP_OK;
}

#else

esp_err_t web_start(void)
{

    ESP_LOGI(tag, "not built in");
    return ESP_ERR_NOT_SUPPORTED;
}

void web_ap_set(int on) { (void)on; }
int web_ap_on(void)     { return 0; }

#endif
