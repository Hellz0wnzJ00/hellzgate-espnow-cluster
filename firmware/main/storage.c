// microsd over spi
// the card is mounted once at boot and a session is a single csv file on it.
// nothing here is allowed to stop the master, a card that is missing, full or
// pulled mid run turns writing off and the console carries on

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "csv.h"
#include "gnss.h"
#include "sdcard.h"
#include "session.h"
#include "storage.h"

static const char *tag = "sd";

#ifdef CONFIG_HG_SD

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#define MOUNT_POINT  "/sd"

// how many rows and how long between flushes. a pulled card loses whatever is
// still in the buffer, and two seconds of rows is a fair trade against wearing
// the card out with a sync per observation
#define FLUSH_ROWS   64
#define FLUSH_MS     2000

// after this many write failures in a row the session is closed rather than
// retried forever. a card that is full or gone does not come back on its own
#define ERROR_LIMIT  16

static sdmmc_card_t *card;
static int mounted;

static FILE *fp;
static char path[STORAGE_PATH_MAX];

// rows handed to the file, and rows the card has confirmed. a row is only
// saved once a flush and a sync have both come back clean. the two used to be
// one number, so a card that failed with rows still in the buffer reported
// rows it never kept
static uint32_t rows;
static uint32_t saved;
static uint32_t errors;

// a flush failed and the close after it has not counted it yet
static int flush_failed;
static uint32_t run_errors;
static uint32_t since_flush;
static int64_t flushed_us;

// the collect task writes rows while the web handler can call stop from its own
// task. without this, stop can close the file underneath a write in progress
static SemaphoreHandle_t busy;

void storage_hold(void)
{
    if (busy != NULL)
        xSemaphoreTakeRecursive(busy, portMAX_DELAY);
}

void storage_release(void)
{
    if (busy != NULL)
        xSemaphoreGiveRecursive(busy);
}

#ifdef CONFIG_HG_SD_BENCH_CHECKS

// a card holds its data line up through its own internal pull up, which is far
// stronger than the internal pull. if the pin still reads high while pulled
// down, a powered card is really sitting there. if it just follows whichever
// way we pull, nothing is connected
static void report_miso(void)
{
    int pin = CONFIG_HG_SD_MISO_GPIO;
    int cs = CONFIG_HG_SD_CS_GPIO;

    // hold the card selected while we look. a good breakout releases this line
    // when chip select is high, so a reading taken then means nothing
    gpio_reset_pin(cs);
    gpio_set_direction(cs, GPIO_MODE_OUTPUT);
    gpio_set_level(cs, 0);

    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);

    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);
    esp_rom_delay_us(1000);
    int up = gpio_get_level(pin);

    gpio_set_pull_mode(pin, GPIO_PULLDOWN_ONLY);
    esp_rom_delay_us(1000);
    int down = gpio_get_level(pin);

    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);

    if (up != down) {
        ESP_LOGE(tag, "miso on gpio%d is floating even with the card selected. nothing is on the other end, so either the slot is empty, the breakout has no 3v3 and ground, or one of the miso, ground or supply wires is not landing",
                 pin);
        gpio_reset_pin(cs);
        return;
    }

    if (up == 1) {
        // most breakouts fit their own pull up on this line, so held high says
        // the module has power and this wire lands. it does not prove a card is
        // in the slot, and it does not prove the card has enough voltage
        ESP_LOGE(tag, "miso on gpio%d is held high with the card selected, so there is a powered card on the other end and this wire is good. look at clock, mosi and cs",
                 pin);
        gpio_reset_pin(cs);
        return;
    }

    // held low is two very different faults and they need different fixes, so
    // push against it. a dead short to ground will not let go, a module output
    // sitting low is only as strong as its buffer and our drive wins. this is
    // brief contention against one gate on a failure path we are already on
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(pin, 1);
    esp_rom_delay_us(500);
    int forced = gpio_get_level(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_PULLUP_ONLY);

    if (forced == 0)
        ESP_LOGE(tag, "miso on gpio%d will not go high even when driven, that wire is shorted to ground or landed on a ground pin",
                 pin);
    else
        // the common blue breakout ties its buffer enable to ground, so that
        // chip drives this line always. low there is the card's own data line
        ESP_LOGE(tag, "miso on gpio%d is being driven low by the module, not shorted. on a buffered breakout that is the card's own data line, so the card is not seated or not answering",
                 pin);

    gpio_reset_pin(cs);
}

// clock, mosi and cs all run into the module, and a module input is high
// impedance, so driving one and reading it back should give whatever we drove.
// anything that will not follow is shorted, landed on a ground pin, or on a pin
// that is not the one on the silkscreen
static void check_drive(int pin, const char *name)
{
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);

    gpio_set_level(pin, 1);
    esp_rom_delay_us(500);
    int high = gpio_get_level(pin);

    gpio_set_level(pin, 0);
    esp_rom_delay_us(500);
    int low = gpio_get_level(pin);

    gpio_set_direction(pin, GPIO_MODE_INPUT);

    if (high == 1 && low == 0)
        ESP_LOGE(tag, "%s on gpio%d drives both ways, the pin itself is fine", name, pin);
    else if (high == 0)
        ESP_LOGE(tag, "%s on gpio%d will not go high, that wire is shorted to ground or sitting on a ground pin", name, pin);
    else
        ESP_LOGE(tag, "%s on gpio%d will not go low, something is holding it up", name, pin);
}

// jumper mosi to miso with every module unplugged and whatever we send has to
// come back byte for byte. a match proves the peripheral and both pins
static void loopback_test(spi_host_device_t host)
{
    spi_device_interface_config_t dcfg = {
        .clock_speed_hz = 1000000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
    };

    spi_device_handle_t dev;
    if (spi_bus_add_device(host, &dcfg, &dev) != ESP_OK) {
        ESP_LOGE(tag, "loopback could not take the bus, skipping it");
        return;
    }

    // a pattern with both edges and both stuck states in it, so a line held
    // high or held low cannot accidentally look like a pass
    uint8_t tx[4] = { 0xa5, 0x5a, 0x00, 0xff };
    uint8_t rx[4] = { 0 };

    spi_transaction_t t = {
        .length = sizeof tx * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    esp_err_t err = spi_device_transmit(dev, &t);
    spi_bus_remove_device(dev);

    if (err != ESP_OK) {
        ESP_LOGE(tag, "loopback transfer failed outright, %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGE(tag, "loopback sent %02x %02x %02x %02x and read %02x %02x %02x %02x",
             tx[0], tx[1], tx[2], tx[3], rx[0], rx[1], rx[2], rx[3]);

    if (memcmp(tx, rx, sizeof tx) == 0)
        ESP_LOGE(tag, "loopback matched. our spi is good, so the fault is the breakout, the card or those wires");
    else
        ESP_LOGE(tag, "loopback did not match. either the jumper from mosi to miso is not fitted, or our spi is not driving those pins at all");
}

// are these two header holes really the two pins we think they are. drive one
// and sense the other against a pull going the other way, both directions
static void probe_pair(int drive, int sense, const char *name)
{
    gpio_reset_pin(drive);
    gpio_set_direction(drive, GPIO_MODE_OUTPUT);

    gpio_reset_pin(sense);
    gpio_set_direction(sense, GPIO_MODE_INPUT);

    gpio_set_pull_mode(sense, GPIO_PULLUP_ONLY);
    gpio_set_level(drive, 0);
    esp_rom_delay_us(1000);
    int lo = gpio_get_level(sense);

    gpio_set_pull_mode(sense, GPIO_PULLDOWN_ONLY);
    gpio_set_level(drive, 1);
    esp_rom_delay_us(1000);
    int hi = gpio_get_level(sense);

    gpio_set_direction(drive, GPIO_MODE_INPUT);
    gpio_set_pull_mode(sense, GPIO_PULLUP_ONLY);

    if (lo == 0 && hi == 1)
        ESP_LOGE(tag, "%s: gpio%d and gpio%d are joined", name, drive, sense);
    else
        ESP_LOGE(tag, "%s: gpio%d and gpio%d are not joined", name, drive, sense);
}

#endif

esp_err_t storage_mount(void)
{
    if (mounted)
        return ESP_OK;

    if (busy == NULL)
        busy = xSemaphoreCreateRecursiveMutex();
    if (busy == NULL) {
        ESP_LOGE(tag, "no mutex for shared SD access");
        return ESP_ERR_NO_MEM;
    }

#ifdef CONFIG_HG_SD_OWN_DRIVER
    // Optional experimental SD driver. Fall back to the ESP-IDF driver
    // when initialization fails; the ESP-IDF driver remains the default.
    if (sdcard_mount(MOUNT_POINT) == ESP_OK) {
        mounted = 1;
        ESP_LOGI(tag, "card up on our own driver, %s, %lu mb",
                 sdcard_type(), (unsigned long)(sdcard_sectors() / 2048));
        return ESP_OK;
    }

    ESP_LOGW(tag, "own driver could not bring the card up, falling back to the idf one");
#endif

    // SDSPI_HOST_DEFAULT already picks the right host for the chip. the c5 has
    // one general purpose spi and SPI2_HOST is enum value 1 on it, so a host
    // number carried in from config was refused outright
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = CONFIG_HG_SD_FREQ_KHZ;

    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_HG_SD_MOSI_GPIO,
        .miso_io_num = CONFIG_HG_SD_MISO_GPIO,
        .sclk_io_num = CONFIG_HG_SD_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    // Enable DMA for block transfers through the ESP-IDF SD driver.
    esp_err_t err = spi_bus_initialize(host.slot, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(tag, "spi bus would not start, %s", esp_err_to_name(err));
        return err;
    }

    // Enable weak internal pull-ups. These do not replace external pull-ups
    // required by the verified board/module design.
    gpio_set_pull_mode(CONFIG_HG_SD_MISO_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(CONFIG_HG_SD_MOSI_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(CONFIG_HG_SD_SCLK_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(CONFIG_HG_SD_CS_GPIO, GPIO_PULLUP_ONLY);

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = CONFIG_HG_SD_CS_GPIO;
    slot.host_id = host.slot;

    esp_vfs_fat_mount_config_t mcfg = {
        // a card is formatted before it goes in the unit. formatting one here
        // would wipe a run someone was trying to recover
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot, &mcfg, &card);

    if (err != ESP_OK) {
        // the driver only says the mount failed, the reason sits a layer down
        // and is logged at debug. turn it up and go round once more, it costs
        // a second and it names the step that gave up
        ESP_LOGW(tag, "first mount failed with %s, retrying with the sd driver logs turned up",
                 esp_err_to_name(err));

        esp_log_level_set("sdmmc_init", ESP_LOG_DEBUG);
        esp_log_level_set("sdmmc_common", ESP_LOG_DEBUG);
        esp_log_level_set("sdmmc_sd", ESP_LOG_DEBUG);
        esp_log_level_set("sdmmc_cmd", ESP_LOG_DEBUG);
        esp_log_level_set("sdspi_host", ESP_LOG_DEBUG);

        err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot, &mcfg, &card);

        esp_log_level_set("sdmmc_init", ESP_LOG_INFO);
        esp_log_level_set("sdmmc_common", ESP_LOG_INFO);
        esp_log_level_set("sdmmc_sd", ESP_LOG_INFO);
        esp_log_level_set("sdmmc_cmd", ESP_LOG_INFO);
        esp_log_level_set("sdspi_host", ESP_LOG_INFO);
    }

    if (err != ESP_OK) {
        ESP_LOGW(tag, "no card mounted, %s. rows will only go to the console",
                 esp_err_to_name(err));

        // on any failure, not just a timeout. a miso stuck low reads 0x00, and
        // 0x00 is a valid answer to the first command, so the card looks like
        // it replied and the init falls over two commands later instead
#ifdef CONFIG_HG_SD_BENCH_CHECKS
        // first, and it has to stay first. the checks below call gpio_reset_pin
        // and take mosi off the peripheral, which makes this read all ones
        loopback_test(host.slot);

        report_miso();
        check_drive(CONFIG_HG_SD_SCLK_GPIO, "clock");
        check_drive(CONFIG_HG_SD_MOSI_GPIO, "mosi");
        check_drive(CONFIG_HG_SD_CS_GPIO, "cs");
#endif

        // the loopback shifts through the peripheral's own clock, so it never
        // touched the sck pin and never touched cs. these do. unplug every
        // module, put one jumper from miso to the hole you believe is each of
        // these in turn, and they say whether that hole is really that pin
#ifdef CONFIG_HG_SD_BENCH_CHECKS
        ESP_LOGE(tag, "the three below only mean something with every module unplugged and one jumper fitted from miso to the pin being checked");
        probe_pair(CONFIG_HG_SD_SCLK_GPIO, CONFIG_HG_SD_MISO_GPIO, "clock jumper");
        probe_pair(CONFIG_HG_SD_MOSI_GPIO, CONFIG_HG_SD_MISO_GPIO, "mosi jumper");
        probe_pair(CONFIG_HG_SD_CS_GPIO, CONFIG_HG_SD_MISO_GPIO, "cs jumper");
#endif

        // the checks above call gpio_reset_pin, which takes the pads off the
        // spi peripheral. give the bus back so the next attempt rebuilds them
        spi_bus_free(host.slot);

        return err;
    }

    mounted = 1;
    ESP_LOGI(tag, "card mounted, %llu mb", ((uint64_t)card->csd.capacity * card->csd.sector_size) >> 20);
    return ESP_OK;
}

int storage_ready(void)
{
    return mounted;
}

int storage_open_now(void)
{
    return fp != NULL;
}

static esp_err_t open_locked(const char *name, int64_t start_unix)
{
    if (!mounted)
        return ESP_ERR_INVALID_STATE;

    if (fp != NULL)
        storage_close();

    // the time in the name is when the run started, in utc, worked back from
    // the receiver. with no date at all it falls back to uptime, which still
    // gives every run its own name and says plainly that the time is not real
    int64_t stamp = start_unix;
    char when[24];

    if (stamp > 0) {
        char iso[24];
        csv_time(iso, sizeof iso, stamp);
        // 2026-09-13 21:50:30 becomes 2026-09-13_21_50_30
        snprintf(when, sizeof when, "%.10s_%.2s_%.2s_%.2s",
                 iso, iso + 11, iso + 14, iso + 17);
    } else {
        snprintf(when, sizeof when, "nofix_up%lld",
                 (long long)(esp_timer_get_time() / 1000000));
    }

    // the run name goes in the filename, so a card holding several runs can be
    // sorted out afterwards. fat has no room for most punctuation and a space
    // breaks half the tools that read these, so anything outside letters,
    // digits, dash and underscore becomes an underscore
    char label[SESSION_NAME_MAX];
    size_t w = 0;

    for (size_t i = 0; name != NULL && name[i] != '\0' && w < sizeof label - 1; i++) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_';
        label[w++] = ok ? c : '_';
    }

    label[w] = '\0';

    if (label[0] == '\0')
        strcpy(label, "run");

    snprintf(path, sizeof path, MOUNT_POINT "/HellzGate_%s_%s.csv", label, when);

    // two runs inside the same second, or a card put back in a unit whose clock
    // has not moved, would otherwise land on a file that already exists. never
    // overwrite somebody's run
    for (int n = 1; n < 100 && access(path, F_OK) == 0; n++)
        snprintf(path, sizeof path, MOUNT_POINT "/HellzGate_%s_%s_%d.csv", label, when, n);

    if (access(path, F_OK) == 0) {
        ESP_LOGE(tag, "a hundred files already named %s_%s, not opening another", label, when);
        path[0] = 0;
        errors++;
        return ESP_FAIL;
    }

    fp = fopen(path, "w");
    if (fp == NULL) {
        ESP_LOGE(tag, "could not open %s", path);
        errors++;
        path[0] = '\0';
        return ESP_FAIL;
    }

    // ask how long the header is and take exactly that. a fixed buffer has been
    // outgrown twice now, and a csv with no header is not a usable export, so a
    // header that will not fit fails the open rather than writing a broken file
    int need = csv_header(NULL, 0);
    char *header = malloc((size_t)need + 1);

    if (header == NULL) {
        ESP_LOGE(tag, "no room for a %d byte csv header, not opening %s", need, path);
        fclose(fp);
        fp = NULL;
        path[0] = '\0';
        errors++;
        return ESP_ERR_NO_MEM;
    }

    csv_header(header, (size_t)need + 1);
    fputs(header, fp);
    free(header);

    rows = 0;
    saved = 0;
    run_errors = 0;
    since_flush = 0;
    flushed_us = esp_timer_get_time();

    fflush(fp);
    ESP_LOGI(tag, "writing to %s", path);
    return ESP_OK;
}

static void close_locked(void);

static void write_locked(const char *line)
{
    if (fp == NULL)
        return;

    if (fputs(line, fp) < 0) {
        errors++;
        run_errors++;

        if (run_errors >= ERROR_LIMIT) {
            ESP_LOGE(tag, "%lu writes in a row failed, closing the session",
                     (unsigned long)run_errors);
            close_locked();
        }
        return;
    }

    run_errors = 0;
    rows++;
    since_flush++;
}

static void tick_locked(void)
{
    if (fp == NULL)
        return;

    int64_t now = esp_timer_get_time();

    if (since_flush < FLUSH_ROWS && now - flushed_us < FLUSH_MS * 1000)
        return;

    since_flush = 0;
    flushed_us = now;

    if (fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
        // counted once, by the close that follows, whether it fails as well
        // or not
        flush_failed = 1;
        ESP_LOGE(tag, "flush failed, the card may have been pulled. %lu rows are saved, the %lu after them may not be",
                 (unsigned long)saved, (unsigned long)(rows - saved));
        close_locked();
        return;
    }

    saved = rows;
}

static void close_locked(void)
{
    if (fp == NULL)
        return;

    // every step is checked. a close that fails means the last rows, and on
    // fat possibly the file length, never reached the card
    int ok = fflush(fp) == 0;
    ok = fsync(fileno(fp)) == 0 && ok;
    ok = fclose(fp) == 0 && ok;
    fp = NULL;

    if (flush_failed && ok)
        errors++;
    flush_failed = 0;

    if (ok) {
        saved = rows;
        ESP_LOGI(tag, "closed %s, %lu rows saved", path, (unsigned long)saved);
        return;
    }

    errors++;
    ESP_LOGE(tag, "closing %s failed. %lu rows are saved, the %lu after them may not be on the card",
             path, (unsigned long)saved, (unsigned long)(rows - saved));
}

esp_err_t storage_open(const char *name, int64_t start_unix)
{
    storage_hold();
    esp_err_t err = open_locked(name, start_unix);
    storage_release();
    return err;
}

void storage_write(const char *line)
{
    storage_hold();
    write_locked(line);
    storage_release();
}

void storage_tick(void)
{
    storage_hold();
    tick_locked();
    storage_release();
}

void storage_close(void)
{
    storage_hold();
    close_locked();
    storage_release();
}

void storage_stats(char *out_path, size_t path_n, uint32_t *out_rows,
                   uint32_t *out_saved, uint32_t *out_errors, uint64_t *free_bytes)
{
    // under the lock like everything else that touches path. the web task asks
    // for this while the collect task can be opening the next file
    storage_hold();

    if (path_n > 0) {
        strncpy(out_path, path, path_n - 1);
        out_path[path_n - 1] = '\0';
    }

    *out_rows = rows;
    *out_saved = saved;
    *out_errors = errors;

    storage_release();

    *free_bytes = 0;

    if (!mounted)
        return;

    uint64_t total = 0, avail = 0;
    storage_hold();
    if (esp_vfs_fat_info(MOUNT_POINT, &total, &avail) == ESP_OK)
        *free_bytes = avail;
    storage_release();
}

#else

esp_err_t storage_mount(void)
{
    ESP_LOGI(tag, "not built in, rows only go to the console");
    return ESP_ERR_NOT_SUPPORTED;
}

void storage_hold(void) { }
void storage_release(void) { }
int storage_ready(void)      { return 0; }
int storage_open_now(void)   { return 0; }

esp_err_t storage_open(const char *name, int64_t start_unix)
{
    (void)name; (void)start_unix;
    return ESP_ERR_NOT_SUPPORTED;
}

void storage_write(const char *line) { (void)line; }
void storage_tick(void) { }
void storage_close(void) { }

void storage_stats(char *out_path, size_t path_n, uint32_t *out_rows,
                   uint32_t *out_saved, uint32_t *out_errors, uint64_t *free_bytes)
{
    if (path_n > 0)
        out_path[0] = '\0';

    *out_rows = 0;
    *out_saved = 0;
    *out_errors = 0;
    *free_bytes = 0;
}

#endif
