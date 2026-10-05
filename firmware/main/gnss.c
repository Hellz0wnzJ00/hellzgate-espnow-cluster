// UART GNSS reader for NMEA GGA/RMC sentences
// one task sits on the uart, splits sentences on newline, checks the checksum
// and keeps the last good fix. everything else reads a copy of it

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/uart.h"
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "gnss.h"

static const char *tag = "gnss";

#ifdef CONFIG_HG_GNSS

// longest nmea sentence is 82 characters, the spare is for receivers that run
// over that when they are set to a high update rate
#define NMEA_MAX  128

// nominal user range error in metres. hdop is unitless, multiplying by this is
// the usual way to turn it into something a mapping tool will accept
#define UERE_M    5.0

// The selftest feeds fixed example sentences through the parser; one
// carries a date. it must never reach the system clock or every board boots
// with a plausible looking wrong time
static int in_selftest;

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

static gnss_fix last;
static int64_t last_us;

static int64_t time_unix;      // seconds at the moment time_us was taken
static int64_t time_us;
static int have_time;

static uint32_t sentences;
static uint32_t rejected;

// nmea checksum is an xor of everything between the dollar and the star,
// written after it as two hex digits
static int checksum_ok(const char *s, int len)
{
    if (len < 4 || s[0] != '$')
        return 0;

    int star = -1;
    for (int i = len - 1; i > 0; i--) {
        if (s[i] == '*') {
            star = i;
            break;
        }
    }

    if (star < 0 || star + 2 >= len)
        return 0;

    uint8_t sum = 0;
    for (int i = 1; i < star; i++)
        sum ^= (uint8_t)s[i];

    char want[3] = { s[star + 1], s[star + 2], '\0' };
    return (uint8_t)strtol(want, NULL, 16) == sum;
}

// splits the sentence in place into comma separated fields. an empty field
// comes back as an empty string, which is how nmea says a value is missing
static int split(char *s, char *field[], int max)
{
    int n = 0;
    field[n++] = s;

    for (; *s != '\0'; s++) {
        if (*s == '*') {
            *s = '\0';
            break;
        }
        if (*s == ',' && n < max) {
            *s = '\0';
            field[n++] = s + 1;
        }
    }

    return n;
}

// nmea writes degrees and minutes stuck together, ddmm.mmmm for latitude and
// dddmm.mmmm for longitude, with the hemisphere in the next field
static int parse_degrees(const char *val, const char *hemi, double *out)
{
    if (val[0] == '\0' || hemi[0] == '\0')
        return 0;

    double raw = atof(val);
    double deg = (double)(int)(raw / 100.0);
    double min = raw - deg * 100.0;

    if (min < 0.0 || min >= 60.0)
        return 0;

    double v = deg + min / 60.0;
    if (hemi[0] == 'S' || hemi[0] == 'W')
        v = -v;

    *out = v;
    return 1;
}

// days since the epoch for a civil date, no library call and no timezone in it
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;

    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

    return era * 146097 + doe - 719468;
}

// rmc carries the date, gga does not, so this is the only place time comes from
static void take_rmc(char *field[], int n)
{
    if (n < 10)
        return;

    // field 2 is the status, A is a valid fix and V means the receiver is
    // still searching. a time from V is not trustworthy
    if (field[2][0] != 'A')
        return;

    const char *hms = field[1];
    const char *dmy = field[9];

    if (strlen(hms) < 6 || strlen(dmy) != 6)
        return;

    int hh = (hms[0] - '0') * 10 + (hms[1] - '0');
    int mi = (hms[2] - '0') * 10 + (hms[3] - '0');
    int ss = (hms[4] - '0') * 10 + (hms[5] - '0');

    int dd = (dmy[0] - '0') * 10 + (dmy[1] - '0');
    int mo = (dmy[2] - '0') * 10 + (dmy[3] - '0');
    int yy = (dmy[4] - '0') * 10 + (dmy[5] - '0');

    if (hh > 23 || mi > 59 || ss > 60 || dd < 1 || dd > 31 || mo < 1 || mo > 12)
        return;

    // Interpret the two-digit RMC year as 2000-2099. This parser does not
    // correct receiver-specific GNSS week-rollover errors.
    int64_t days = days_from_civil(2000 + yy, mo, dd);
    int64_t unix_sec = days * 86400 + hh * 3600 + mi * 60 + ss;

    portENTER_CRITICAL(&lock);
    int first = !have_time;
    time_unix = unix_sec;
    time_us = esp_timer_get_time();
    have_time = 1;
    portEXIT_CRITICAL(&lock);

    // set the system clock the first time we learn the date. without this the
    // chip counts from 1970, fatfs stamps every file it creates as 1980, and
    // anything that asks the c library for the time gets nonsense
    if (first && !in_selftest) {
        struct timeval tv = { .tv_sec = (time_t)unix_sec, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ESP_LOGI(tag, "system clock set from the receiver");
    }
}

static void take_gga(char *field[], int n)
{
    if (n < 10)
        return;

    int quality = atoi(field[6]);

    if (quality == 0) {
        portENTER_CRITICAL(&lock);
        last.have_fix = 0;
        last.quality = 0;
        portEXIT_CRITICAL(&lock);
        return;
    }

    double lat, lon;
    if (!parse_degrees(field[2], field[3], &lat))
        return;
    if (!parse_degrees(field[4], field[5], &lon))
        return;

    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0)
        return;

    double hdop = atof(field[8]);
    if (hdop <= 0.0)
        hdop = 99.0;

    portENTER_CRITICAL(&lock);
    last.have_fix = 1;
    last.lat = lat;
    last.lon = lon;
    last.alt_m = atof(field[9]);
    last.accuracy_m = hdop * UERE_M;
    last.sats = (uint8_t)atoi(field[7]);
    last.quality = (uint8_t)quality;
    last_us = esp_timer_get_time();
    portEXIT_CRITICAL(&lock);
}

static void take_line(char *s, int len)
{
    if (!checksum_ok(s, len)) {
        rejected++;
        return;
    }

    sentences++;

    // Match the sentence type independently of the two-character talker ID.
    // GP identifies GPS; GN is used for combined GNSS data.
    if (len < 7)
        return;

    char *field[20];
    int n = split(s, field, 20);

    if (strcmp(field[0] + 3, "GGA") == 0)
        take_gga(field, n);
    else if (strcmp(field[0] + 3, "RMC") == 0)
        take_rmc(field, n);
}

// a copy of the sentence goes in because take_line splits it in place, and a
// string literal is not writable
static void feed(const char *s)
{
    char buf[NMEA_MAX];
    int len = (int)strlen(s);

    if (len >= NMEA_MAX)
        return;

    memcpy(buf, s, (size_t)len + 1);
    take_line(buf, len);
}

int gnss_selftest(void)
{
    int bad = 0;

    in_selftest = 1;

    memset(&last, 0, sizeof last);
    last_us = 0;
    have_time = 0;
    sentences = 0;
    rejected = 0;

    // Munich NMEA test fixture; the date below exercises the parser
    // with a two-digit year in the supported range
    feed("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47");

    gnss_fix g;
    gnss_read(&g);

    if (!g.have_fix) bad++;
    if (g.lat < 48.1172 || g.lat > 48.1174) bad++;
    if (g.lon < 11.5166 || g.lon > 11.5168) bad++;
    if (g.alt_m < 545.3 || g.alt_m > 545.5) bad++;
    if (g.sats != 8) bad++;
    if (g.accuracy_m < 4.4 || g.accuracy_m > 4.6) bad++;

    feed("$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,310826,003.1,W*6B");

    // 31 august 2026 at 12:35:19 utc. a second of slack because the clock is
    // carried forward from when the sentence landed
    int64_t t = gnss_unix();
    if (t < 1788179719LL || t > 1788179720LL) bad++;

    // quality 0 means the receiver is searching, the old position has to go
    feed("$GNGGA,000000,0000.000,N,00000.000,E,0,00,99.9,0.0,M,0.0,M,,*54");
    gnss_read(&g);
    if (g.have_fix) bad++;

    // one bit flipped in the body, the checksum has to catch it
    uint32_t was = rejected;
    feed("$GPGGA,123519,4807.038,N,01131.000,E,1,09,0.9,545.4,M,46.9,M,,*47");
    if (rejected != was + 1) bad++;
    gnss_read(&g);
    if (g.have_fix) bad++;

    memset(&last, 0, sizeof last);
    last_us = 0;
    have_time = 0;
    sentences = 0;
    rejected = 0;

    in_selftest = 0;

    return bad;
}

static void gnss_task(void *arg)
{
    (void)arg;

    char line[NMEA_MAX];
    int len = 0;

    while (1) {
        uint8_t c;
        int got = uart_read_bytes(CONFIG_HG_GNSS_UART, &c, 1, pdMS_TO_TICKS(1000));

        if (got != 1)
            continue;

        if (c == '\r')
            continue;

        if (c == '\n') {
            if (len > 0) {
                line[len] = '\0';
                take_line(line, len);
            }
            len = 0;
            continue;
        }

        // Reset the buffer on overflow. Remaining bytes before the newline
        // may be collected as a fragment and must pass the normal parser checks.
        if (len >= NMEA_MAX - 1) {
            len = 0;
            rejected++;
            continue;
        }

        line[len++] = (char)c;
    }
}

esp_err_t gnss_start(void)
{
    uart_config_t cfg = {
        .baud_rate = CONFIG_HG_GNSS_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // configure before installing. the port keeps its clock choice across a
    // software reset, and startup turns that clock off again. installing first
    // writes a register that waits on the dead clock inside a critical section
    // and the interrupt watchdog fires. the reboots after a panic usually did
    // the same until the power was cycled. configuring first turns it on
    esp_err_t err = uart_param_config(CONFIG_HG_GNSS_UART, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(tag, "uart %d would not configure, %s",
                 CONFIG_HG_GNSS_UART, esp_err_to_name(err));
        return err;
    }

    err = uart_driver_install(CONFIG_HG_GNSS_UART, 1024, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(tag, "uart %d would not install, %s",
                 CONFIG_HG_GNSS_UART, esp_err_to_name(err));
        return err;
    }

    err = uart_set_pin(CONFIG_HG_GNSS_UART, CONFIG_HG_GNSS_TX_GPIO,
                       CONFIG_HG_GNSS_RX_GPIO,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(tag, "gnss pins tx %d rx %d were refused, %s",
                 CONFIG_HG_GNSS_TX_GPIO, CONFIG_HG_GNSS_RX_GPIO,
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(tag, "reading a receiver on uart %d, rx %d, tx %d, %d baud",
             CONFIG_HG_GNSS_UART, CONFIG_HG_GNSS_RX_GPIO,
             CONFIG_HG_GNSS_TX_GPIO, CONFIG_HG_GNSS_BAUD);

    xTaskCreate(gnss_task, "gnss", 3072, NULL, 4, NULL);
    return ESP_OK;
}

void gnss_read(gnss_fix *out)
{
    portENTER_CRITICAL(&lock);
    *out = last;
    int64_t at = last_us;
    portEXIT_CRITICAL(&lock);

    out->age_ms = at == 0 ? 0 : (esp_timer_get_time() - at) / 1000;
}

int64_t gnss_unix(void)
{
    portENTER_CRITICAL(&lock);
    int64_t base = time_unix;
    int64_t at = time_us;
    int known = have_time;
    portEXIT_CRITICAL(&lock);

    if (!known)
        return 0;

    return base + (esp_timer_get_time() - at) / 1000000;
}

void gnss_stats(uint32_t *s, uint32_t *r)
{
    *s = sentences;
    *r = rejected;
}

#else

esp_err_t gnss_start(void)
{
    ESP_LOGI(tag, "not built in, rows go out with no time and no position");
    return ESP_OK;
}

void gnss_read(gnss_fix *out)
{
    memset(out, 0, sizeof *out);
}

int64_t gnss_unix(void)
{
    return 0;
}

void gnss_stats(uint32_t *s, uint32_t *r)
{
    *s = 0;
    *r = 0;
}

int gnss_selftest(void)
{
    return 0;
}

#endif
