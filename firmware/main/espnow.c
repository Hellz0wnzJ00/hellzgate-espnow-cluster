// espnow implementation of the transport interface
//
// nodes push, the master listens. above this file nothing knows that, the
// interface is still stage and collect.

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_now.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "slotid.h"
#include "scan_mode.h"
#include "transport.h"

static const char *tag = "espnow";

static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

// the record is the locked 46 byte one and moves as is. this header is just
// the envelope round it, it never touches the record

enum {
    HG_FRAME_HELLO     = 1,   // node says it exists
    HG_FRAME_HELLO_ACK = 2,   // master broadcast, how a node learns its address
    HG_FRAME_BEAT      = 3,   // liveness and applied mode, no records
    HG_FRAME_RECORDS   = 4
};

typedef struct __attribute__((packed)) {
    uint16_t proto;
    uint8_t  kind;
    uint8_t  node_id;
    uint32_t seq;     // records frames only, per node, gaps are lost frames
    uint32_t boot;    // the scanner's boot number, the same in every frame
    uint8_t  count;   // records that follow
} hg_frame;

#define HG_FRAME_HDR   ((int)sizeof(hg_frame))

// the espnow frame's own version, separate from the record's. 1.03 took the
// sequence off hellos and beats and added the scanner's boot number to every
// frame. still five records a frame. a scanner and a master that disagree ignore each other, and the
// master says so once per node rather than guessing at what the fields mean
// experimental 0x8104 adds the requested mode to beacons and the applied mode
// to beats. the high bit keeps this bench build separate from production.
#define HG_ESPNOW_PROTO_VERSION  0x8104u
#define ESPNOW_MTU     250

// how many records actually fit in one frame
#define ESPNOW_BATCH   ((ESPNOW_MTU - HG_FRAME_HDR) / (int)sizeof(hg_record_t))

static uint8_t my_node_id;
static uint32_t tx_seq;

// espnow send is asynchronous. if we hand it a frame and then hop channels to
// scan, the frame goes out on the wrong channel or not at all, so every send
// waits for completion, but the bounded timeout path can return without one.
static SemaphoreHandle_t tx_done;
static volatile int tx_ok;

static uint32_t tx_frames;   // frames handed to the radio
static uint32_t tx_err;      // the call itself was refused
static uint32_t tx_fail;     // still no ack after the retry
static uint32_t tx_retry;    // needed a second go
static uint32_t tx_timeout;  // the radio never said whether it went or not
static uint32_t tx_miss;     // failures in a row, this is how we spot a dead master
static uint32_t tx_records;  // records actually handed over, not frames

// failures in a row before we give up on the master we enrolled with
#define MASTER_GONE_AFTER 6

static void on_sent(const wifi_tx_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    tx_ok = (status == ESP_NOW_SEND_SUCCESS);
    xSemaphoreGive(tx_done);
}

static int send_once(const uint8_t *mac, const void *buf, int len)
{
    // a callback from a previous send that arrived late would otherwise be
    // taken as this send finishing, and every send after it reads the wrong
    // result. Drain an already-arrived notification before starting; callbacks
    // arriving after this point are not correlated to an individual send
    xSemaphoreTake(tx_done, 0);

    esp_err_t err = esp_now_send(mac, (const uint8_t *)buf, len);
    if (err != ESP_OK) {
        tx_err++;
        return 0;
    }

    tx_frames++;

    // ble shares this radio, so the callback can take far longer than the
    // couple of ms the frame itself needs. waiting only 50 ms called perfectly
    // good sends failures and made the node retry frames the master already had
    if (xSemaphoreTake(tx_done, pdMS_TO_TICKS(500)) != pdTRUE) {
        tx_timeout++;

        // the callback for this frame is still coming. wait it out here, or the
        // next send picks it up and reads this frame's result as its own, and a
        // stale success drops records off the ring that never arrived
        xSemaphoreTake(tx_done, pdMS_TO_TICKS(500));
        return 0;
    }

    return tx_ok;
}

// returns 1 when the frame is out. the retry keeps the same sequence number
// and the master skips one it already took, so a retry is not counted twice
#if CONFIG_HG_TEST_FAKE_LOST_ACK_EVERY > 0
// test builds only. every nth records frame that did go out is treated as if
// its ack was lost, so it goes again under the same sequence. the master already has
// it and has to throw the second copy away, which is the path being tested
static uint32_t test_sends;
static uint32_t test_faked;
#endif

static int send_frame(const uint8_t *mac, const void *buf, int len)
{
    int ok = send_once(mac, buf, len);

#if CONFIG_HG_TEST_FAKE_LOST_ACK_EVERY > 0
    // records frames only. a hello or a beat carries no sequence, so a copy of
    // one is not something the master counts
    if (ok && ((const hg_frame *)buf)->kind == HG_FRAME_RECORDS &&
        ++test_sends % CONFIG_HG_TEST_FAKE_LOST_ACK_EVERY == 0) {
        test_faked++;
        ESP_LOGW(tag, "test, ack %lu treated as lost, sending the frame again",
                 (unsigned long)test_faked);
        ok = 0;
    }
#endif

    if (ok) {
        tx_miss = 0;
        return 1;
    }

    tx_retry++;

    if (send_once(mac, buf, len)) {
        tx_miss = 0;
        return 1;
    }

    tx_fail++;
    tx_miss++;
    return 0;
}

// node side

static hg_record_t ring[CONFIG_HG_RING_RECORDS];
static uint32_t ring_head, ring_tail, ring_used, ring_dropped;

// wifi scanning and ble both stage into this from their own tasks
static SemaphoreHandle_t ring_lock;

static uint8_t master_mac[6];
static int have_master;

// picked at boot and sent in every frame. the records sequence starts again
// from 1 after a reboot, and this is how the master knows it has
static uint32_t boot_id;

// a records frame that did not go through is kept exactly as it was and sent
// again, same sequence and same records, until it does. the master may already
// have it. anything else sent under that sequence in the meantime, a beat or a
// frame with more records in it, was taken for a copy and thrown away with
// whatever it held
static uint8_t retry_buf[ESPNOW_MTU];
static int retry_len;
static uint32_t retry_n;
static uint32_t retry_drops;
static int64_t last_beat_us;
static int64_t last_hello_us;

// master side

static hg_node_info nodes[HG_MAX_NODES];
static QueueHandle_t inbox;
static int64_t last_beacon_us;

#ifdef CONFIG_HG_ROLE_MASTER
// returns 0 when the frame should be ignored
static int note_frame(uint8_t id, const uint8_t *mac, const hg_frame *f)
{
    hg_node_info *n = &nodes[id];

    if (n->state != HG_NODE_UP) {
        memcpy(n->mac, mac, 6);
        n->proto = f->proto;
        n->state = HG_NODE_UP;
        ESP_LOGI(tag, "node %u up, %02x:%02x:%02x:%02x:%02x:%02x proto %04x",
                 id, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], f->proto);
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    n->last_seen_ms = now_ms;

    // a boot number we have not seen from this node. if we had one before, the
    // scanner restarted and its records started again from 1, so the old
    // sequence is forgotten, or its first frame could be taken for a copy of
    // its last one. if we had none, we are the one that restarted, and there
    // was no sequence to forget
    if (f->boot != n->boot_id) {
        scan_mode_seen(id, (hg_scan_mode){ .mode = HG_SCAN_UNKNOWN });
        if (n->boot_id != 0) {
            n->restarts++;
            n->seq_known = 0;
            ESP_LOGW(tag, "node %u restarted", id);
        }
        n->boot_id = f->boot;
    }

    // only records frames carry a sequence. a hello or a beat that reached us
    // with its ack lost used to take the number the next records frame went out
    // under, and that frame was then thrown away as a copy
    if (f->kind != HG_FRAME_RECORDS)
        return 1;

    // a retry of a frame we already took, counting it twice inflates the
    // totals. it still means the node is there, which is already noted
    if (n->seq_known && f->seq == n->seq) {
        n->dupes++;
        return 0;
    }

    // a gap in the sequence is how we know a frame went missing
    if (n->seq_known && f->seq > n->seq + 1)
        n->frames_lost += f->seq - n->seq - 1;

    n->seq = f->seq;
    n->seq_known = 1;
    n->frames++;
    return 1;
}
#endif

static void add_peer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac))
        return;

    esp_now_peer_info_t p;
    memset(&p, 0, sizeof p);
    memcpy(p.peer_addr, mac, 6);
    p.channel = 0;             // whatever channel we are on
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false;

    esp_err_t err = esp_now_add_peer(&p);
    if (err != ESP_OK)
        ESP_LOGW(tag, "add peer failed, %s", esp_err_to_name(err));
}

// Runs in the Wi-Fi task: validates headers, updates status, queues records,
// and enrolls scanners. Keep callback work bounded.
static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    // the version is read before the length. an older scanner sends a shorter
    // header, and a length check first would drop it without ever saying which
    // board needs reflashing. proto and node id sit in the same place in both
    if (len < 4)
        return;

    uint16_t proto = (uint16_t)(data[0] | (data[1] << 8));
    uint8_t node = data[3];

    if (proto != HG_ESPNOW_PROTO_VERSION) {
        static uint32_t warned;
        uint32_t bit = node < 31 ? 1u << node : 1u << 31;
        if (!(warned & bit)) {
            warned |= bit;
            ESP_LOGW(tag, "node %u sends espnow proto %04x, this build wants %04x. reflash it with the matching image",
                     node, proto, HG_ESPNOW_PROTO_VERSION);
        }
        return;
    }

    if (len < HG_FRAME_HDR)
        return;

    hg_frame f;
    memcpy(&f, data, sizeof f);

    ESP_LOGD(tag, "rx %d bytes, proto %04x kind %u node %u seq %lu",
             len, f.proto, f.kind, f.node_id, (unsigned long)f.seq);

#ifdef CONFIG_HG_ROLE_MASTER
    // a second master on the channel, not a scanner
    if (f.kind == HG_FRAME_HELLO_ACK)
        return;

    if (f.node_id >= HG_MAX_NODES) {
        ESP_LOGW(tag, "node id %u is outside the cluster", f.node_id);
        return;
    }

    // reject malformed envelopes before changing liveness or sequence state.
    int extra = f.kind == HG_FRAME_BEAT ? HG_SCAN_MODE_BYTES :
                f.kind == HG_FRAME_RECORDS ? f.count * (int)sizeof(hg_record_t) : 0;
    if ((f.kind != HG_FRAME_HELLO && f.kind != HG_FRAME_BEAT &&
         f.kind != HG_FRAME_RECORDS) || len != HG_FRAME_HDR + extra ||
        (f.kind != HG_FRAME_RECORDS && f.count != 0) ||
        (f.kind == HG_FRAME_RECORDS && (f.count == 0 || f.count > ESPNOW_BATCH)))
        return;

    if (!note_frame(f.node_id, info->src_addr, &f))
        return;

    if (f.kind == HG_FRAME_HELLO) {
        // nothing to do but note it, which note_frame already did. we do not
        // register the scanner as a peer because we never unicast to it, the
        // master only ever broadcasts. registering them would burn one of the
        // 20 espnow peer slots each, and the broadcast address takes one of
        // those too, so 20 scanners would not fit
        return;
    }

    if (f.kind == HG_FRAME_BEAT) {
        scan_mode_seen(f.node_id, scan_mode_unpack(data + HG_FRAME_HDR));
        nodes[f.node_id].heartbeats++;
        return;
    }

    if (f.kind == HG_FRAME_RECORDS) {
        int want = HG_FRAME_HDR + f.count * (int)sizeof(hg_record_t);
        if (len < want) {
            ESP_LOGW(tag, "short frame, said %u records but only %d bytes",
                     f.count, len);
            return;
        }

        for (int i = 0; i < f.count; i++) {
            const hg_record_t *r =
                (const hg_record_t *)(data + HG_FRAME_HDR + i * sizeof(hg_record_t));
            // queue full means the master is behind. count it and move on,
            // blocking in here would stall the wifi task
            if (xQueueSend(inbox, r, 0) != pdTRUE) {
                // master is behind. count the whole rest of the frame, not just
                // this one, or the number under reports what was thrown away
                nodes[f.node_id].inbox_full += f.count - i;
                break;
            }
            nodes[f.node_id].records++;
        }
    }
#else
    if (f.kind != HG_FRAME_HELLO_ACK || f.count != 0 ||
        len != HG_FRAME_HDR + HG_SCAN_MODE_BYTES ||
        (have_master && memcmp(master_mac, info->src_addr, 6) != 0))
        return;
    hg_scan_mode request = scan_mode_unpack(data + HG_FRAME_HDR);
    if (request.mode > HG_SCAN_MIX || request.revision == 0)
        return;
    scan_mode_receive(data + HG_FRAME_HDR);
    if (!have_master) {
        memcpy(master_mac, info->src_addr, 6);
        add_peer(master_mac);
        have_master = 1;
        ESP_LOGI(tag, "enrolled with master %02x:%02x:%02x:%02x:%02x:%02x",
                 master_mac[0], master_mac[1], master_mac[2],
                 master_mac[3], master_mac[4], master_mac[5]);
    }
    (void)data;
#endif
}

static esp_err_t espnow_init(void)
{
#ifdef CONFIG_HG_ROLE_NODE
    my_node_id = slotid_get();
    boot_id = esp_random() | 1;
#else
    my_node_id = 0;
    inbox = xQueueCreate(512, sizeof(hg_record_t));
    if (inbox == NULL)
        return ESP_ERR_NO_MEM;
#endif

    tx_done = xSemaphoreCreateBinary();
    ring_lock = xSemaphoreCreateMutex();
    if (tx_done == NULL || ring_lock == NULL)
        return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_sent));

    add_peer(bcast);

    ESP_LOGI(tag, "up on channel %d, %d records per frame",
             CONFIG_HG_ESPNOW_CHANNEL, ESPNOW_BATCH);
    return ESP_OK;
}

// node side. this only stages, the wire happens in service
static esp_err_t espnow_stage(const hg_record_t *r)
{
    xSemaphoreTake(ring_lock, portMAX_DELAY);

    if (ring_used == CONFIG_HG_RING_RECORDS) {
        // oldest goes first and it gets counted
        ring_tail = (ring_tail + 1) % CONFIG_HG_RING_RECORDS;
        ring_used--;
        ring_dropped++;
    }

    ring[ring_head] = *r;
    ring_head = (ring_head + 1) % CONFIG_HG_RING_RECORDS;
    ring_used++;

    xSemaphoreGive(ring_lock);
    return ESP_OK;
}

// the frame went through, so its records leave the ring. staging may have
// pushed some of the oldest out while it was waiting, and those went out in
// the frame anyway, so only what is left is released. worked out from the drop
// counter rather than ring positions, which wrap if a retry waits a long time
static void release(uint32_t n, uint32_t drops_then)
{
    xSemaphoreTake(ring_lock, portMAX_DELAY);

    uint32_t evicted = ring_dropped - drops_then;
    if (evicted < n) {
        uint32_t drop = n - evicted;
        ring_tail = (ring_tail + drop) % CONFIG_HG_RING_RECORDS;
        ring_used -= drop;
    }

    xSemaphoreGive(ring_lock);

    tx_seq++;
    tx_records += n;
}

static int send_records(void)
{
    if (retry_len > 0) {
        if (!send_frame(master_mac, retry_buf, retry_len))
            return 0;

        retry_len = 0;
        release(retry_n, retry_drops);
        return (int)retry_n;
    }

    uint8_t buf[ESPNOW_MTU];
    hg_frame *f = (hg_frame *)buf;

    // copy without consuming. records leave the ring on MAC-layer send success.
    // this is not an application-level acknowledgement from the master
    int n = 0;
    xSemaphoreTake(ring_lock, portMAX_DELAY);
    uint32_t drops_then = ring_dropped;
    uint32_t peek = ring_tail;
    while (n < ESPNOW_BATCH && (uint32_t)n < ring_used) {
        memcpy(buf + HG_FRAME_HDR + n * sizeof(hg_record_t),
               &ring[peek], sizeof(hg_record_t));
        peek = (peek + 1) % CONFIG_HG_RING_RECORDS;
        n++;
    }
    xSemaphoreGive(ring_lock);

    if (n == 0)
        return 0;

    f->proto = HG_ESPNOW_PROTO_VERSION;
    f->kind = HG_FRAME_RECORDS;
    f->node_id = my_node_id;
    // the sequence only advances on success, so a frame that never landed does
    // not leave a hole the master would report as loss
    f->seq = tx_seq + 1;
    f->boot = boot_id;
    f->count = (uint8_t)n;

    int len = HG_FRAME_HDR + n * (int)sizeof(hg_record_t);

    if (!send_frame(master_mac, buf, len)) {
        // they stay staged, and this exact frame goes again next time round
        memcpy(retry_buf, buf, len);
        retry_len = len;
        retry_n = (uint32_t)n;
        retry_drops = drops_then;
        return 0;
    }

    release((uint32_t)n, drops_then);
    return n;
}

static void espnow_service(void)
{
    int64_t now = esp_timer_get_time();

    if (!have_master) {
        // keep saying hello until someone answers, one a second is plenty
        if (now - last_hello_us < 1000 * 1000)
            return;
        last_hello_us = now;

        // a hello carries no sequence. see note_frame
        hg_frame hello = { .proto = HG_ESPNOW_PROTO_VERSION, .kind = HG_FRAME_HELLO,
                           .node_id = my_node_id, .seq = 0, .boot = boot_id,
                           .count = 0 };

        // keep the send out of the log call, at the default level the macro
        // compiles out and the send goes with it
        int ok = send_frame(bcast, &hello, sizeof hello);

        ESP_LOGD(tag, "hello, sent %d", ok);
        return;
    }

    // too many failures in a row means the master is gone. go back to saying
    // hello so we pick it up again when it reboots
    if (tx_miss >= MASTER_GONE_AFTER) {
        ESP_LOGW(tag, "master stopped answering, enrolling again");
        have_master = 0;
        tx_miss = 0;
        last_hello_us = 0;
        return;
    }

    // heartbeat first. draining records ahead of it pushed the beat out to
    // 4.3 s against a 6 s timeout, which is too close for comfort. not while a
    // records frame is waiting to go again, the retry itself tells the master
    // we are here
    if (retry_len == 0 && now - last_beat_us >= CONFIG_HG_HEARTBEAT_MS * 1000) {
        last_beat_us = now;

        // a beat carries no sequence. see note_frame
        hg_frame beat = { .proto = HG_ESPNOW_PROTO_VERSION, .kind = HG_FRAME_BEAT,
                          .node_id = my_node_id, .seq = 0, .boot = boot_id,
                          .count = 0 };
        uint8_t buf[HG_FRAME_HDR + HG_SCAN_MODE_BYTES];
        memcpy(buf, &beat, sizeof beat);
        scan_mode_pack(buf + HG_FRAME_HDR, scan_mode_applied());
        send_frame(master_mac, buf, sizeof buf);
    }

    // then drain. one frame per call could not keep up with a sweep, the ring
    // just grew and the master saw a fraction of what was found
    while ((retry_len > 0 || ring_used > 0) && send_records() > 0)
        ;
}

// master side
static uint32_t espnow_collect(hg_record_t *out, uint32_t max)
{
    uint32_t n = 0;

    if (inbox == NULL)
        return 0;

    // say who we are often enough that any node listening can enrol. sent from
    // our own task, the callback is too busy with one node's records to get a
    // reply out to another. at once a second a scanner with ble running took
    // over a minute to catch one, so the default is four times that. the frame
    // is eighteen bytes, but it is still master airtime on a shared channel, which
    // is why the interval is a setting rather than a number in here
    int64_t now_us = esp_timer_get_time();
    if (now_us - last_beacon_us >= (int64_t)CONFIG_HG_BEACON_MS * 1000) {
        last_beacon_us = now_us;

        hg_frame beacon = { .proto = HG_ESPNOW_PROTO_VERSION,
                            .kind = HG_FRAME_HELLO_ACK,
                            .node_id = 0, .seq = 0, .count = 0 };
        uint8_t buf[HG_FRAME_HDR + HG_SCAN_MODE_BYTES];
        memcpy(buf, &beacon, sizeof beacon);
        scan_mode_pack(buf + HG_FRAME_HDR, scan_mode_wanted());
        esp_now_send(bcast, buf, sizeof buf);
    }

    while (n < max && xQueueReceive(inbox, &out[n], 0) == pdTRUE)
        n++;

    // any accepted traffic refreshes liveness; mark down after the quiet timeout
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t dead_ms = CONFIG_HG_HEARTBEAT_MS * CONFIG_HG_HEARTBEAT_MISSES;

    for (uint8_t i = 0; i < HG_MAX_NODES; i++) {
        uint32_t quiet = now_ms - nodes[i].last_seen_ms;

        if (nodes[i].state == HG_NODE_UP && quiet > dead_ms) {
            nodes[i].state = HG_NODE_DOWN;
            nodes[i].downs++;
            ESP_LOGW(tag, "node %u down after %lu ms quiet", i,
                     (unsigned long)quiet);
        }
    }

    return n;
}

static const hg_transport espnow = {
    .name = "espnow",
    .max_batch = ESPNOW_BATCH,
    .init = espnow_init,
    .stage = espnow_stage,
    .service = espnow_service,
    .collect = espnow_collect,
    .ready = transport_enrolled,
    .beat_due = transport_beat_due,
    .tx_stats = transport_tx_stats,
};

const hg_transport *transport_espnow(void)
{
    return &espnow;
}

const hg_node_info *transport_node(uint8_t id)
{
    return (id < HG_MAX_NODES) ? &nodes[id] : NULL;
}

// asked between channels. answering early leaves room for the scan to finish
// the channel it is on before the beat is actually late
int transport_enrolled(void)
{
    return have_master;
}

int transport_beat_due(void)
{
    if (!have_master)
        return 0;

    int64_t margin = (int64_t)CONFIG_HG_HEARTBEAT_MS * 1000 / 2;
    return (esp_timer_get_time() - last_beat_us) >= margin;
}

void transport_tx_stats(hg_tx_stats *s)
{
    s->records = tx_records;
    s->frames  = tx_frames;
    s->refused = tx_err + tx_timeout;   // neither one reported a success
    s->retried = tx_retry;
    s->lost    = tx_fail;
    s->dropped = ring_dropped;
    s->queued  = ring_used;
}
