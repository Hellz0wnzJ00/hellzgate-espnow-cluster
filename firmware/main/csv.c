#include <stdio.h>
#include <string.h>
#include <time.h>

#include "csv.h"

// the clock and the fix come from the receiver, which only exists on target.
// the host tests build the escaping and the formatting without them
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "gnss.h"
#endif

// Do not attach fixes older than ten seconds to new observations.
#define FIX_STALE_MS 10000

// bump this if the column list below ever changes, the app and wigle both
// key off it. 1.6 adds Frequency, RCOIs and MfgrId over 1.4, so the version
// string and the column list have to move together or wigle rejects the file.
#define CSV_VERSION "WigleWifi-1.6"

static const char *csv_columns =
    "MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,"
    "CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,"
    "RCOIs,MfgrId,Type";

// wigle wants frequency as well as channel. the maps are fixed so we work it
// out instead of leaving it blank
static unsigned freq_mhz(const hg_record_t *r)
{
    if (r->channel == 0)
        return 0;   // ble, the controller never told us which advertising channel

    if (r->band == HG_BAND_2G4) {
        if (r->channel == 14)
            return 2484;   // japan only and spaced differently to the rest
        if (r->channel <= 13)
            return 2407u + r->channel * 5u;
        return 0;
    }

    return 5000u + r->channel * 5u;
}

// writes only while there is room but always keeps counting, that is what
// lets the caller ask for the size first
static void put(char *out, size_t out_size, int *pos, char c)
{
    if ((size_t)*pos < out_size)
        out[*pos] = c;

    (*pos)++;
}

static void put_str(char *out, size_t out_size, int *pos, const char *s)
{
    for (; *s != '\0'; s++)
        put(out, out_size, pos, *s);
}

static void terminate(char *out, size_t out_size, int pos)
{
    if (out_size == 0)
        return;

    out[(size_t)pos < out_size ? (size_t)pos : out_size - 1] = '\0';
}

// strnlen is not in plain c11 so here it is
static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0;

    while (n < max && s[n] != '\0')
        n++;

    return n;
}

static int needs_quotes(const char *in, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = in[i];

        if (c == ',' || c == '"' || c == '\n' || c == '\r')
            return 1;
    }
    return 0;
}

// how many bytes of valid utf-8 start here, or 0 if this byte does not begin a
// well formed sequence. overlong forms and surrogates are rejected too
static size_t utf8_run(const char *in, size_t left)
{
    unsigned char a = (unsigned char)in[0];
    size_t need;
    unsigned long cp;

    if (a >= 0xc2 && a <= 0xdf)      { need = 2; cp = a & 0x1fu; }
    else if (a >= 0xe0 && a <= 0xef) { need = 3; cp = a & 0x0fu; }
    else if (a >= 0xf0 && a <= 0xf4) { need = 4; cp = a & 0x07u; }
    else return 0;

    if (left < need)
        return 0;

    for (size_t i = 1; i < need; i++) {
        unsigned char b = (unsigned char)in[i];
        if (b < 0x80 || b > 0xbf)
            return 0;
        cp = (cp << 6) | (b & 0x3fu);
    }

    if (need == 3 && cp < 0x800)      return 0;
    if (need == 4 && cp < 0x10000)    return 0;
    if (cp > 0x10ffff)                return 0;
    if (cp >= 0xd800 && cp <= 0xdfff) return 0;

    return need;
}

static void put_backslash(char *out, size_t out_size, int *pos)
{
    put(out, out_size, pos, 0x5c);
}

static void put_hex_escape(char *out, size_t out_size, int *pos, unsigned char c)
{
    static const char hex[] = "0123456789abcdef";

    put_backslash(out, out_size, pos);
    put(out, out_size, pos, 'x');
    put(out, out_size, pos, hex[c >> 4]);
    put(out, out_size, pos, hex[c & 0x0f]);
}

static void append_escaped(char *out, size_t out_size, int *pos,
                           const char *in, size_t len)
{
    int quote = needs_quotes(in, len);

    if (quote)
        put(out, out_size, pos, '"');

    for (size_t i = 0; i < len; ) {
        unsigned char c = (unsigned char)in[i];

        // a quote inside a quoted field is written twice, that is the rule
        if (c == '"') {
            put(out, out_size, pos, '"');
            put(out, out_size, pos, '"');
            i++;
            continue;
        }

        // a real backslash is doubled, so a reader can tell an ssid that
        // literally contains backslash x f f from a byte we escaped
        if (c == 0x5c) {
            put_backslash(out, out_size, pos);
            put_backslash(out, out_size, pos);
            i++;
            continue;
        }

        if (c < 0x80) {
            put(out, out_size, pos, in[i]);
            i++;
            continue;
        }

        // an ssid is an arbitrary run of bytes and nothing says it is utf-8. a
        // raw byte that is not valid utf-8 makes the whole file undecodable, so
        // it goes out as a backslash x escape instead, which keeps the byte and
        // keeps the file readable
        size_t run = utf8_run(in + i, len - i);

        if (run == 0) {
            put_hex_escape(out, out_size, pos, c);
            i++;
            continue;
        }

        for (size_t k = 0; k < run; k++)
            put(out, out_size, pos, in[i + k]);

        i += run;
    }

    if (quote)
        put(out, out_size, pos, '"');
}

int csv_field(char *out, size_t out_size, const char *in, size_t in_len)
{
    int pos = 0;

    append_escaped(out, out_size, &pos, in, in_len);
    terminate(out, out_size, pos);

    return pos;
}

int csv_time(char *out, size_t out_size, int64_t unix_sec)
{
    time_t t = (time_t)unix_sec;
    struct tm *g = gmtime(&t);

    if (g == NULL) {
        if (out_size > 0)
            out[0] = '\0';
        return 0;
    }

    return snprintf(out, out_size, "%04d-%02d-%02d %02d:%02d:%02d",
                    g->tm_year + 1900, g->tm_mon + 1, g->tm_mday,
                    g->tm_hour, g->tm_min, g->tm_sec);
}

#ifdef ESP_PLATFORM

int64_t csv_now(void)
{
    int64_t gps = gnss_unix();
    if (gps > 0)
        return gps;

    return HG_TIME_BASE + esp_timer_get_time() / 1000000;
}

void csv_current_fix(csv_fix *out, const char *first_seen)
{
    gnss_fix g;
    gnss_read(&g);

    memset(out, 0, sizeof *out);
    out->first_seen = first_seen;

    if (!g.have_fix || g.age_ms > FIX_STALE_MS)
        return;

    out->have_fix = 1;
    out->lat = g.lat;
    out->lon = g.lon;
    out->alt_m = g.alt_m;
    out->accuracy_m = g.accuracy_m;
}

#endif

int csv_header(char *out, size_t out_size)
{
    int pos = 0;

    // WiGLE-compatible metadata. Release/model strings below are hard-coded.
    put_str(out, out_size, &pos,
            CSV_VERSION ",appRelease=1.0.0,model=HellzGate C5,release=1.0.0,"
            "device=HellzGate,display=HellzGate,board=esp32c5,brand=HellzGate,"
            "star=Sol,body=3,subBody=0");
    put(out, out_size, &pos, '\n');

    put_str(out, out_size, &pos, csv_columns);
    put(out, out_size, &pos, '\n');

    terminate(out, out_size, pos);
    return pos;
}

// BLE has its own row type; other record types use WIFI.
static const char *type_name(uint8_t type)
{
    switch (type) {
    case HG_TYPE_BLE:
        return "BLE";
    default:
        return "WIFI";
    }
}

int csv_row(char *out, size_t out_size, const hg_record_t *r, const csv_fix *fix)
{
    int pos = 0;
    char tmp[64];

    snprintf(tmp, sizeof tmp, "%02X:%02X:%02X:%02X:%02X:%02X",
             r->bssid[0], r->bssid[1], r->bssid[2],
             r->bssid[3], r->bssid[4], r->bssid[5]);
    put_str(out, out_size, &pos, tmp);
    put(out, out_size, &pos, ',');

    // this is the field with characters we do not control
    append_escaped(out, out_size, &pos, r->ssid,
                   bounded_len(r->ssid, sizeof r->ssid));
    put(out, out_size, &pos, ',');

    // authmode stays empty, the record carries no security field yet
    put(out, out_size, &pos, ',');

    if (fix != NULL && fix->first_seen != NULL)
        append_escaped(out, out_size, &pos, fix->first_seen,
                       strlen(fix->first_seen));
    put(out, out_size, &pos, ',');

    unsigned f = freq_mhz(r);
    if (f != 0)
        snprintf(tmp, sizeof tmp, "%u,%u,%d", (unsigned)r->channel, f, (int)r->rssi);
    else
        // an unknown frequency is left blank, not written as zero
        snprintf(tmp, sizeof tmp, "%u,,%d", (unsigned)r->channel, (int)r->rssi);

    put_str(out, out_size, &pos, tmp);
    put(out, out_size, &pos, ',');

    // no fix means zeros, we do not put a location in that we do not have
    double lat = (fix != NULL && fix->have_fix) ? fix->lat : 0.0;
    double lon = (fix != NULL && fix->have_fix) ? fix->lon : 0.0;
    double alt = (fix != NULL && fix->have_fix) ? fix->alt_m : 0.0;
    double acc = (fix != NULL && fix->have_fix) ? fix->accuracy_m : 0.0;

    // clamp before formatting. %.1f of a wild altitude runs to hundreds of
    // characters and would be cut inside tmp without anyone noticing
    if (alt < -100000.0 || alt > 100000.0) alt = 0.0;
    if (acc < 0.0 || acc > 100000.0) acc = 0.0;

    int fixlen = snprintf(tmp, sizeof tmp, "%.6f,%.6f,%.1f,%.1f",
                          lat, lon, alt, acc);
    if (fixlen < 0 || fixlen >= (int)sizeof tmp)
        snprintf(tmp, sizeof tmp, "0.000000,0.000000,0.0,0.0");

    put_str(out, out_size, &pos, tmp);
    put(out, out_size, &pos, ',');

    // rcois and mfgrid stay empty, the record does not carry either
    put(out, out_size, &pos, ',');
    put(out, out_size, &pos, ',');

    put_str(out, out_size, &pos, type_name(r->type));
    put(out, out_size, &pos, '\n');

    terminate(out, out_size, pos);
    return pos;
}
