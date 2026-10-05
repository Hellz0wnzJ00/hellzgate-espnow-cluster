// espnow end to end on a pc. the real code built once as a scanner and once as
// the master, joined by a fake radio that can lose a frame, or deliver it and
// lose the ack. the scanner then cannot tell whether the master has the frame,
// and every record still has to reach the master exactly once, or be one the
// scanner counted as pushed out of its ring
//
// built twice. plain, and with CONFIG_HG_TEST_FAKE_LOST_ACK_EVERY set on the
// scanner, which runs the same check the bench test does with that switch on

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "esp_now.h"
#include "hg_record.h"
#include "transport.h"
#include "scan_mode.h"

hg_scan_mode s_scan_mode_wanted(void);
void s_scan_mode_note(hg_scan_mode mode);

int hg_test_verbose = 0;
int64_t hg_test_now_us = 0;
int hg_test_line_level = 1;

uint8_t slotid_get(void) { return 0; }
int slotid_known(void) { return 1; }

esp_err_t n_init(void);
esp_err_t n_stage(const hg_record_t *r);
void n_service(void);
int n_enrolled(void);
uint32_t n_ring_used(void);
uint32_t n_dropped(void);
int n_retry_pending(void);
uint32_t n_test_faked(void);
void n_reboot(void);
void m_reboot(void);

esp_err_t m_init(void);
uint32_t m_collect(hg_record_t *out, uint32_t max);
const hg_node_info *m_node(uint8_t id);

// ---- the fake radio ---------------------------------------------------------

enum { SCANNER, MASTER };
static int side;

static esp_now_recv_cb_t recv_cb[2];
static esp_now_send_cb_t send_cb[2];

static uint8_t mac_of[2][6] = {
    { 0x10, 0, 0, 0, 0, 1 },
    { 0x10, 0, 0, 0, 0, 2 },
};

// what happens to the scanner's next sends. anything not queued here goes
// through and is acknowledged
enum { OK, NO_ACK, LOST };
static int fates[256];
static int nfates;

static void fate(int f, int times)
{
    for (int i = 0; i < times; i++)
        fates[nfates++] = f;
}

static int next_fate(void)
{
    if (nfates == 0)
        return OK;
    int f = fates[0];
    memmove(fates, fates + 1, (size_t)(nfates - 1) * sizeof fates[0]);
    nfates--;
    return f;
}

esp_err_t esp_now_init(void) { return ESP_OK; }
esp_err_t esp_now_register_recv_cb(esp_now_recv_cb_t cb) { recv_cb[side] = cb; return ESP_OK; }
esp_err_t esp_now_register_send_cb(esp_now_send_cb_t cb) { send_cb[side] = cb; return ESP_OK; }
bool esp_now_is_peer_exist(const uint8_t *mac) { (void)mac; return false; }
esp_err_t esp_now_add_peer(const esp_now_peer_info_t *p) { (void)p; return ESP_OK; }

static uint32_t radio_frames;

// broadcasts from the scanner never arrive. a hello is a broadcast, so the
// master only hears a rebooted scanner's new boot number from a beat or a
// records frame
static int drop_broadcast;
static int drop_beacon;

esp_err_t esp_now_send(const uint8_t *mac, const uint8_t *data, size_t len)
{
    int from = side, to = 1 - side;
    int f = from == SCANNER ? next_fate() : OK;

    if (from == SCANNER && drop_broadcast && mac[0] == 0xff)
        f = LOST;
    if (from == MASTER && drop_beacon)
        f = LOST;

    if (from == SCANNER)
        radio_frames++;

    if (f != LOST && recv_cb[to] != NULL) {
        esp_now_recv_info_t info = { .src_addr = mac_of[from], .des_addr = mac_of[to] };
        side = to;
        recv_cb[to](&info, data, (int)len);
        side = from;
    }

    if (send_cb[from] != NULL)
        send_cb[from](NULL, f == OK ? ESP_NOW_SEND_SUCCESS : ESP_NOW_SEND_FAIL);

    return ESP_OK;
}

// ---- the checks ---------------------------------------------------------------

static uint32_t next_counter;
static uint8_t got[4096];

static hg_record_t make(uint32_t c)
{
    hg_record_t r;
    memset(&r, 0, sizeof r);
    r.bssid[0] = (uint8_t)c;
    r.bssid[1] = (uint8_t)(c >> 8);
    r.bssid[3] = 0x5a;
    r.rssi = -50;
    r.channel = 1;
    hg_record_seal(&r);
    return r;
}

static void stage(int n)
{
    for (int i = 0; i < n; i++) {
        hg_record_t r = make(next_counter++);
        side = SCANNER;
        n_stage(&r);
    }
}

static void collect(void)
{
    hg_record_t out[64];
    uint32_t n;

    side = MASTER;
    while ((n = m_collect(out, 64)) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            uint32_t c = out[i].bssid[0] | (out[i].bssid[1] << 8);
            assert(c < next_counter);
            got[c]++;
            // the one thing that must never happen, a record taken twice
            assert(got[c] == 1);
        }
    }
}

// one turn of the scanner's loop and the master's
static void turn(int ms)
{
    hg_test_now_us += (int64_t)ms * 1000;
    side = SCANNER;
    n_service();
    collect();
}

static void settle(void)
{
    for (int i = 0; i < 200 && (n_ring_used() > 0 || n_retry_pending()); i++)
        turn(20);
    turn(20);
    assert(n_ring_used() == 0 && !n_retry_pending());
}

static void check(const char *what, uint32_t from, uint32_t to)
{
    for (uint32_t c = from; c < to; c++) {
        if (got[c] != 1) {
            printf("FAIL %s, record %lu arrived %d times\n", what, (unsigned long)c, got[c]);
            assert(0);
        }
    }
    printf("  %-48s ok, %lu delivered once\n", what, (unsigned long)(to - from));
}

int main(void)
{
    side = MASTER;
    assert(m_init() == ESP_OK);
    side = SCANNER;
    assert(n_init() == ESP_OK);

    // the scanner says hello until it hears the master's beacon
    for (int i = 0; i < 100 && !n_enrolled(); i++) {
        turn(100);
        side = MASTER;
        collect();
    }
    assert(n_enrolled());

    const hg_node_info *node = m_node(0);
    uint32_t from, dupes;

    // the scanner sends one records frame, sequence 1, and reboots. its first
    // frame after the reboot is sequence 1 again and must not be taken for a
    // copy of the one before
    from = next_counter;
    stage(3);
    settle();
    n_reboot();
    drop_broadcast = 1;
    for (int i = 0; i < 100 && !n_enrolled(); i++) {
        turn(100);
        side = MASTER;
        collect();
    }
    assert(n_enrolled());
    // and the first beat after the reboot is lost as well, so the first thing
    // the master hears from the new boot is the records frame itself
    fate(LOST, 2);
    stage(3);
    settle();
    drop_broadcast = 0;
    assert(node->restarts == 1);
    check("reboot after a single frame", from, next_counter);

    // in rounds that fit the ring, so nothing here is pushed out
    from = next_counter;
    for (int round = 0; round < 4; round++) {
        stage(20);
        settle();
    }
    assert(n_dropped() == 0);
    check("plain traffic", from, next_counter);

    // regression case: the master takes the frame but the ack is lost on
    // both tries. more records arrive before the scanner goes again. the next
    // frame must be the same frame, not a bigger one under the same sequence
    from = next_counter;
    dupes = node->dupes;
    stage(4);
    fate(NO_ACK, 2);
    turn(20);
    stage(12);
    settle();
    assert(node->dupes > dupes);
    check("acks lost, master had it, more records waiting", from, next_counter);

    // the frame itself never arrived, both tries
    from = next_counter;
    stage(4);
    fate(LOST, 2);
    turn(20);
    stage(12);
    settle();
    check("frame lost twice, then more records", from, next_counter);

    // the frame never arrives and a beat falls due while it waits. a beat
    // under that sequence would be taken, and the frame after it thrown away
    // as a copy of the beat
    from = next_counter;
    stage(4);
    fate(LOST, 2);
    turn(20);
    turn(CONFIG_HG_HEARTBEAT_MS + 500);
    stage(6);
    settle();
    check("beat due while a lost frame waits", from, next_counter);

    // the master goes quiet long enough that the scanner gives up on it with a
    // frame the master never got still waiting. a hello reaches the master
    // before the scanner is enrolled again. the hello must not use up the
    // frame's sequence, or the frame is thrown away as a copy when it goes
    from = next_counter;
    stage(4);
    fate(LOST, 2);
    turn(20);
    for (int i = 0; i < 10; i++) {
        fate(LOST, 2);
        turn(20);
    }
    assert(!n_enrolled());
    nfates = 0;
    turn(1100);
    for (int i = 0; i < 100 && !n_enrolled(); i++) {
        turn(100);
        side = MASTER;
        collect();
    }
    assert(n_enrolled());
    settle();
    check("master lost, hello heard, then found again", from, next_counter);

    // a beat reaches the master and both its acks are lost. the records after
    // it must not go out under a number the beat has used up
    from = next_counter;
    fate(NO_ACK, 2);
    turn(CONFIG_HG_HEARTBEAT_MS + 100);
    assert(nfates == 0);
    stage(4);
    settle();
    check("beat heard, both its acks lost", from, next_counter);

    // the master reboots and the scanner does not notice. a frame reaches the
    // new master with its acks lost, so often that the scanner gives up and
    // enrols again. its hellos bring the boot number the master has only just
    // learned, and the frame going again must still be taken for a copy
    from = next_counter;
    side = MASTER;
    m_reboot();
    dupes = node->dupes;
    uint32_t restarts = node->restarts;
    stage(3);
    fate(NO_ACK, 12);
    for (int i = 0; i < 400 && (n_ring_used() > 0 || n_retry_pending() || !n_enrolled()); i++)
        turn(20);
    settle();
    assert(node->restarts == restarts);
    check("master rebooted, acks lost, scanner enrolled again", from, next_counter);

    // the ring overflows while a frame waits, far enough to wrap round past a
    // multiple of its size. the records still in the ring at the end must all
    // arrive, none released unsent
    from = next_counter;
    uint32_t dropped = n_dropped();
    stage(4);
    fate(NO_ACK, 2);
    turn(20);
    stage(CONFIG_HG_RING_RECORDS * 2 + (CONFIG_HG_RING_RECORDS - 4) + 1);
    uint32_t newest = next_counter - CONFIG_HG_RING_RECORDS;
    settle();
    assert(n_dropped() > dropped);
    check("ring wrapped while a retry waited, newest kept", newest, next_counter);
    for (uint32_t c = from; c < newest; c++)
        assert(got[c] <= 1);

#if CONFIG_HG_TEST_FAKE_LOST_ACK_EVERY > 0
    // the bench test. every nth ack treated as lost, and each one has to come
    // back as a copy the master throws away
    from = next_counter;
    dupes = node->dupes;
    uint32_t faked = n_test_faked();
    for (int i = 0; i < 20; i++) {
        stage(5);
        settle();
    }
    assert(n_test_faked() > faked);
    assert(node->dupes - dupes == n_test_faked() - faked);
    check("test switch, acks treated as lost", from, next_counter);
#endif

    // none of that is loss as far as the master can tell, and it never was
    assert(node->frames_lost == 0);

    for (int mode = HG_SCAN_WIFI; mode <= HG_SCAN_MIX; mode++) {
        assert(scan_mode_set((uint8_t)mode));
        hg_scan_mode want = scan_mode_wanted();
        drop_beacon = 1;
        turn(2100);
        assert(s_scan_mode_wanted().revision != want.revision);
        drop_beacon = 0;
        turn(1100);
        assert(s_scan_mode_wanted().revision == want.revision);
        assert(scan_mode_node(0).revision != want.revision);
        s_scan_mode_note(s_scan_mode_wanted());
        turn(2100);
        assert(scan_mode_node(0).revision == want.revision);
        assert(scan_mode_node(0).mode == mode);
    }
    hg_test_now_us += 7000000;
    assert(scan_mode_node(0).mode == HG_SCAN_UNKNOWN);
    puts("  radio mode retry, acknowledgement and expiry ok");

    printf("espnow tests ok, %lu records, %lu frames\n",
           (unsigned long)next_counter, (unsigned long)radio_frames);
    return 0;
}
