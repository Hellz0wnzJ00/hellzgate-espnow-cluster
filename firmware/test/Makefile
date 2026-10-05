# host tests for the pure logic bits, no idf and no boards needed
# just run make

CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -I../main

all: test_unique_million test_unique_collisions test_unique_queue test_ble_mode test_record test_tally test_csv test_pending test_espnow test_espnow_fake
	./test_unique_million
	./test_unique_collisions
	./test_unique_queue
	./test_ble_mode
	./test_record
	./test_tally
	./test_csv
	./test_pending
	./test_espnow
	./test_espnow_fake

test_record: test_record.c ../main/hg_record.h
	$(CC) $(CFLAGS) -o $@ test_record.c

# small table on purpose, makes the full up case quick to reach
test_tally: test_tally.c ../main/tally.c ../main/tally.h ../main/hg_record.h
	$(CC) $(CFLAGS) -DTALLY_MAX_DEVICES=16 -o $@ test_tally.c ../main/tally.c

test_csv: test_csv.c ../main/csv.c ../main/csv.h ../main/hg_record.h
	$(CC) $(CFLAGS) -o $@ test_csv.c ../main/csv.c

# small buffer so the full case is quick to reach
test_pending: test_pending.c ../main/pending.c ../main/pending.h
	$(CC) $(CFLAGS) -I idf_stub -DCONFIG_HG_GNSS -DCONFIG_HG_PENDING_MAX=100 		-DCONFIG_HG_PENDING_WAIT_S=180 -o $@ test_pending.c ../main/pending.c

MODE_SLAVE = -Dscan_mode_init=s_scan_mode_init -Dscan_mode_set=s_scan_mode_set -Dscan_mode_wanted=s_scan_mode_wanted -Dscan_mode_applied=s_scan_mode_applied -Dscan_mode_note=s_scan_mode_note -Dscan_mode_receive=s_scan_mode_receive -Dscan_mode_seen=s_scan_mode_seen -Dscan_mode_node=s_scan_mode_node -Dscan_mode_pack=s_scan_mode_pack -Dscan_mode_unpack=s_scan_mode_unpack

# the real espnow code as scanner and as master, over a fake radio that loses
# frames and acks. the scanner's exported names are moved out of the way
EN_COMMON = -I idf_stub -DCONFIG_HG_MAX_NODES=2 -DCONFIG_HG_RING_RECORDS=32 -DCONFIG_HG_ESPNOW_CHANNEL=1 -DCONFIG_HG_HEARTBEAT_MS=2000 -DCONFIG_HG_HEARTBEAT_MISSES=3 -DCONFIG_HG_BEACON_MS=1000
EN_NODE = $(MODE_SLAVE) -DCONFIG_HG_ROLE_NODE -Dtransport_espnow=node_transport_espnow -Dtransport_node=node_transport_node -Dtransport_enrolled=node_transport_enrolled -Dtransport_beat_due=node_transport_beat_due -Dtransport_tx_stats=node_transport_tx_stats
EN_SRC = ../main/espnow.c ../main/transport.h ../main/hg_record.h

test_espnow: test_espnow.c side_en_node.c side_en_master.c $(EN_SRC) mode_master.o mode_slave.o
	$(CC) $(CFLAGS) $(EN_COMMON) $(EN_NODE) -DCONFIG_HG_TEST_FAKE_LOST_ACK_EVERY=0 -c side_en_node.c -o en_node.o
	$(CC) $(CFLAGS) $(EN_COMMON) -DCONFIG_HG_ROLE_MASTER -c side_en_master.c -o en_master.o
	$(CC) $(CFLAGS) $(EN_COMMON) -DCONFIG_HG_TEST_FAKE_LOST_ACK_EVERY=0 -c test_espnow.c -o en_test.o
	$(CC) -o $@ en_test.o en_node.o en_master.o mode_master.o mode_slave.o

# the same with the scanner treating every third ack as lost
test_espnow_fake: test_espnow.c side_en_node.c side_en_master.c $(EN_SRC) mode_master.o mode_slave.o
	$(CC) $(CFLAGS) $(EN_COMMON) $(EN_NODE) -DCONFIG_HG_TEST_FAKE_LOST_ACK_EVERY=3 -c side_en_node.c -o enf_node.o
	$(CC) $(CFLAGS) $(EN_COMMON) -DCONFIG_HG_ROLE_MASTER -c side_en_master.c -o enf_master.o
	$(CC) $(CFLAGS) $(EN_COMMON) -DCONFIG_HG_TEST_FAKE_LOST_ACK_EVERY=3 -c test_espnow.c -o enf_test.o
	$(CC) -o $@ enf_test.o enf_node.o enf_master.o mode_master.o mode_slave.o

clean:
	rm -f test_unique_million test_unique_collisions test_unique_queue test_ble_mode test_record test_tally test_csv test_pending test_espnow test_espnow_fake *.o

.PHONY: all clean

mode_master.o: ../main/scan_mode.c ../main/scan_mode.h
	$(CC) $(CFLAGS) $(EN_COMMON) -DCONFIG_HG_ROLE_MASTER -c ../main/scan_mode.c -o $@

mode_slave.o: ../main/scan_mode.c ../main/scan_mode.h
	$(CC) $(CFLAGS) $(EN_COMMON) $(MODE_SLAVE) -DCONFIG_HG_ROLE_NODE -c ../main/scan_mode.c -o $@

test_ble_mode: test_ble_mode.c ../main/ble.c ../main/scan_mode.c
	$(CC) $(CFLAGS) $(EN_COMMON) -o $@ test_ble_mode.c ../main/ble.c ../main/scan_mode.c

test_unique_million: test_unique_index.c ../main/tally.c ../main/unique_index.c
	$(CC) $(CFLAGS) -DTALLY_MAX_DEVICES=65536 -o $@ $^

test_unique_collisions: test_unique_index.c ../main/tally.c ../main/unique_index.c
	$(CC) $(CFLAGS) -DTALLY_MAX_DEVICES=16 -DUNIQUE_BUCKETS=16 -DUNIQUE_CACHE_PAGES=1 -DSMALL_COLLISION_TEST -o $@ $^

test_unique_queue: test_unique_tracker.c ../main/tally.c ../main/unique_index.c
	$(CC) $(CFLAGS) -I idf_stub -DCONFIG_HG_ROLE_MASTER -DTALLY_MAX_DEVICES=16 -o $@ $^
