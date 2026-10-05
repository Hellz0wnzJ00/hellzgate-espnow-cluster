// include the actual worker so the host can schedule batches deterministically.
#include <assert.h>
#include <stdio.h>
#include "../main/unique_tracker.c"

int64_t hg_test_now_us;
int hg_test_verbose;
int storage_ready(void) { return 0; }
void storage_hold(void) { }
void storage_release(void) { }

int main(void)
{
    unique_tracker_start();
    hg_record_t r = {0};
    r.type = HG_TYPE_AP;
    hg_record_seal(&r);
    for (unsigned i = 0; i < QUEUE_SIZE + 3; i++)
        assert(unique_tracker_add(&r)); // queue loss does not reject CSV
    tally_counts c;
    unique_status s;
    unique_tracker_snapshot(&c, &s);
    assert(c.total == QUEUE_SIZE + 3 && c.unique == 0);
    assert(s.pending == QUEUE_SIZE && s.dropped == 3 && s.incomplete);
    for (unsigned i = 0; i < QUEUE_SIZE / 32; i++) process_some();
    unique_tracker_snapshot(&c, &s);
    assert(c.unique == 1 && s.pending == 0 && s.incomplete);
    r.crc8 ^= 1;
    assert(!unique_tracker_add(&r));
    r.node_id = TALLY_NODES;
    hg_record_seal(&r);
    assert(!unique_tracker_add(&r));
    unique_tracker_snapshot(&c, &s);
    assert(c.bad_crc == 1 && c.bad_field == 1 && c.total == QUEUE_SIZE + 3);

    // no SD: the fallback still counts repeats after filling, but advertises
    // that unseen addresses beyond RAM capacity cannot be counted exactly.
    for (unsigned i = 1; i <= TALLY_MAX_DEVICES; i++) {
        r.node_id = 0;
        r.bssid[0] = i;
        r.bssid[1] = i >> 8;
        hg_record_seal(&r);
        assert(unique_tracker_add(&r));
        process_some();
    }
    unique_tracker_snapshot(&c, &s);
    assert(!s.sd && s.incomplete && c.unique == TALLY_MAX_DEVICES && c.table_full == 1);
    // exercise the actual stdio adapter, including a short read at EOF.
    FILE *f = fopen("scan-modes-2026-10-04/host-tests/unique-io.tmp", "w+b");
    assert(f);
    uint8_t block[512], copy[512];
    for (unsigned i = 0; i < sizeof block; i++) block[i] = (uint8_t)i;
    assert(write_page(f, 512, block));
    assert(flush_file(f));
    assert(read_page(f, 512, copy) && !memcmp(block, copy, sizeof block));
    assert(!read_page(f, 1024, copy));
    assert(fclose(f) == 0);
    assert(remove("scan-modes-2026-10-04/host-tests/unique-io.tmp") == 0);
    free(table); free(queue);
    puts("tracker queue overflow, backlog, validation, CSV acceptance and RAM fallback tests ok");
    return 0;
}
