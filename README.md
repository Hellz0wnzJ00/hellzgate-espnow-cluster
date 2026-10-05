# HellzGate ESP-NOW Cluster Firmware â€” Open-Source Beta

> Some people spoon, we fork. Have fun and be safe! - Hellz

**BETA - experimental source for testing and forks, not a final or production release.**

**Master + scanner/node firmware for ESP32-C5.**

An early community test build for passive Wi-Fi/BLE observation collection.
Scanner nodes report to a master over ESP-NOW. The FullGate master supports
GNSS, microSD CSV logging, a local status page, a display and fan control.
This is not a production release or the frozen mobile-app integration API.

This source is also intended for forks using XIAO ESP32-C5 boards and custom
hardware. The FullGate configurations are board-specific examples, not universal
pin assignments. See the adaptation guidance below before using them elsewhere.

**HellzGate Project by Hellz (Sean Clossey).**

## October 5 beta update — SD uniques and scan modes

Thanks to **HotBy73** for flagging the unique-count plateau. Wi-Fi and BLE
shared a 65,536-entry RAM table. Once full, new address/type pairs could not
increase the unique count, although total observations and CSV logging could
continue. This was a table-capacity limit, not a 16-bit counter limit.

- Exact SD overflow tracking keeps the first 49,152 distinct keys in RAM and
  stores later keys in an SD index. Full addresses and types are compared, so
  hash collisions do not merge different devices. This adds overflow storage;
  it does not periodically empty the RAM table or reset the unique count.
- The web page now offers **Wi-Fi Only** and **Wi-Fi + BLE** beside hotspot/fan.
  Mixed mode is the boot default; the selection is not saved across master
  restarts. The page reports whether online scanners applied the request.
  Previously queued BLE rows can drain after a mode change.
- The 56-step scan pattern revisits channels **6, 1, 11, 149, 44 and 157** four
  times while covering all 38 original channels. Scanner starting positions
  are staggered. Priorities came from a sampled trip, not universal channel
  popularity; higher capture yield has not been measured. Region enforcement
  and passive scanning remain unchanged.

**Upgrade the master and every scanner together.** ESP-NOW protocol **0x8104**
requires matching images; previous public images cannot join this version.
The observation record and CSV layout are unchanged. This is source-only beta
firmware; use the build configurations below for your actual hardware.

### What has and has not been tested

The corresponding FullGate ESP-NOW M1/S1 bench build stopped BLE in Wi-Fi-only
mode and resumed BLE in mixed mode while Wi-Fi and SD recording continued.
The GPS fix, timestamped coordinates and zero reported storage/link losses
were observed in the short test. The public source is a separate ESP-NOW-only
port; see VALIDATION.md for its build and host-test checks.

**The hardware run has not yet crossed the old 65,536-unique limit.** Desktop
stress tests passed 1,000,000 distinct keys and 2,000,000 observations, but
that does not establish physical SD throughput or reliability under sustained
multi-scanner load. Community confirmation is welcome; this is still beta.

### Counting limits and community checks

- A usable SD card is required for overflow. Without it, the RAM-only fallback
  remains limited to 65,536 keys and reports incomplete counts after overflow.
- The page shows pending work, dropped counting work, index errors and an
  incomplete warning. Pending counts may catch up; incomplete counts remain
  a lower bound for the rest of that boot. Valid rows still reach the CSV path
  when counting fails. CSV and the index share the SD card, so slow media can
  affect both. The queue holds 8,192 sightings and uses PSRAM.
- `/sd/hg_unique_v1.tmp` is disposable index data, recreated at master boot.
  Do not upload it as a capture log. Counts are since boot, including while
  recording is paused; starting/stopping a session does not reset them.
  This is not reboot persistence or power-loss recovery.
- Storage and counters are finite: an index offset guard stops below 2 GiB;
  counters remain 32-bit. The default table/index/cache/queue consume roughly
  1.4 MiB of PSRAM. This removes the old working-SD table limit, not every limit.
- To check the fix, collect more than 65,536 distinct address/type pairs with
  `unique_tracker.backend` showing `ram+sd`, then let `pending` reach zero.
  Compare against an independent deduplication of all rows from that boot;
  report `incomplete`, `dropped`, `errors`, `high_water` and storage/link losses.
  Include SD model, firmware revision and scanner count. Do not publicly share
  raw captures containing locations or device addresses.
- WDGW territory captures are a different metric from unique address counts.
  This correction does not itself prove increased territory captures.

## Package

Only source, build configurations and tests are included. No project logs,
field captures, delivery correspondence, PCB files or prebuilt binaries are bundled.
The wired scanner transport is excluded. Display I2C remains because it drives
the screen; it does not carry scanner records. MIT license: see LICENSE.
ESP-IDF and its dependencies retain their separate licenses.

## License and credit

Released under the [MIT License](LICENSE), copyright (c) 2026 Sean Clossey.
You may use, modify, fork, share and sell the software under those terms.
Keep the copyright notice and license in copies or substantial portions.

If you build on HellzGate ESP-NOW Cluster Firmware, please credit the **HellzGate Project by
Hellz (Sean Clossey)** and link back to this repository. Attribution helps
others find the original project and keeps the community growing. Suggested credit:

> Based on the HellzGate Project by Hellz (Sean Clossey).
> https://github.com/Hellz0wnzJ00/hellzgate-espnow-cluster

The README credit and link are appreciated, not additional license conditions.
No credit on a device screen, app interface or boot screen is required.

## Educational and authorized use

HellzGate ESP-NOW Cluster Firmware is intended for education, research and authorized testing.
Use it only with equipment and in environments where you have permission;
obtain authorization before testing systems belonging to others. You are
responsible for your use, applicable laws and regulations, and respecting
others' privacy. The project does not endorse unauthorized access,
interference or other misuse. This is responsible-use guidance, not an
additional restriction on the rights granted by the MIT License.

The software is experimental and provided "AS IS", without warranty. Use it
at your own risk. To the extent permitted by applicable law, Hellz (Sean Clossey),
as the owner of the HellzGate Project, disclaims liability for claims, damages
or other liability arising from the software or its use, as set out in
[LICENSE](LICENSE).

## Build

Use ESP-IDF 5.5.3 targeting ESP32-C5. The three public configurations are
validated separately from the bench source; current results are in VALIDATION.md.
Run these commands inside `firmware/`. Use separate build directories/configs.

FullGate master:
```
idf.py -B build_master "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.base;sdkconfig.xiao;sdkconfig.fullgate;sdkconfig.fullgate_espnow" "-DSDKCONFIG=build_master/sdkconfig" build
```

Scanner in a hardware slot with ID straps:
```
idf.py -B build_scanner "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.xiao;sdkconfig.fgnode_espnow" "-DSDKCONFIG=build_scanner/sdkconfig" build
```

Standalone scanner with an NVS ID (slots 1-20):
```
idf.py -B build_nvs "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.xiao;sdkconfig.fgnode_espnow;sdkconfig.fgnode_espnow_nvs" "-DSDKCONFIG=build_nvs/sdkconfig" build
```

All supplied builds select a capacity of 20 scanners; that is a configuration limit, not
proof of a validated 20-scanner field deployment. Every scanner needs a unique
ID. Slots are one-based; internal node IDs are zero-based. The four hardware
strap pins can encode slots 1-15, with zero reserved for an invalid slot; increasing
the cluster capacity does not create more physical slots. The existing backplane
has nine scanner slots. The NVS build accepts slots 1-20; use unused IDs, normally
10-20 when the nine hardware slots are populated. Never duplicate an ID between
a strapped scanner and an NVS scanner.

Provisioning example (generates a file without contacting hardware):
```
python tools/slot_nvs.py 10
```
The tool needs `esp_idf_nvs_partition_gen`. Its optional `--port PORT` writes
the whole NVS partition, replacing its existing contents. Check board, port,
partition layout and ID before use. Provision after flashing the matching image.

Flash only the matching build and verified board/port. For the master:
```
idf.py -B build_master "-DSDKCONFIG=build_master/sdkconfig" -p PORT flash monitor
```
Use the corresponding build directory and SDKCONFIG for each scanner.
The FullGate overlays assume the existing FullGate pin layout and an 8 MB
XIAO ESP32-C5 target. Do not apply these pin assignments to arbitrary boards.
The September 30 FullGate PCB BOM specifies an onboard ATGM336H-5N31 GNSS
module (U8); the earlier Base bench setup used a NEO-6M module. The reader
parses NMEA GGA/RMC over UART. That shared format does not prove compatibility
with the assembled production PCB: verify receiver configuration and wiring.
Review configuration values before building. Use a new build directory when
changing overlays; defaults do not replace values in an existing sdkconfig.

## Use

### Adapting a fork to other hardware

The ESP-NOW transport does not require the FullGate PCB. The supplied master
command enables FullGate peripherals, however. For a bare master, use
`sdkconfig.defaults;sdkconfig.xiao` in a fresh build directory, run `menuconfig`,
and select the master role. Leave GNSS, SD, display, fan, pin finder and SD bench
checks disabled unless the corresponding hardware is connected and its pin map
has been verified. Match flash size, partition layout and PSRAM settings to the
actual module. This bare-master configuration has not been hardware-tested here.
The current master requires PSRAM for its tally, and startup selftests also
allocate PSRAM. Disabling peripherals alone does not make this a no-PSRAM build.

A standalone scanner can use the NVS-ID build above without backplane straps.
Assign a unique ID before use; the supplied scanner will not send without one.
Do not use the hardware-slot build on a board without the matching strap wiring.

Without SD enabled, records go to the console rather than a card. Without GNSS,
exports have no GNSS timestamp or position. Forks can adapt the output path, but
must test their own hardware, storage and timing behavior.

### FullGate configuration

The FullGate master starts logging automatically when powered. Hold its configured
button to enable the hotspot. Join `hellzgate` (default password `hellzgate`) and
open `http://192.168.4.1/`. Set a different hotspot password in menuconfig.
Use Stop and allow the save to finish before disconnecting power or removing SD.
Counts are since boot, not per session. GNSS startup may produce empty timestamps
and zero coordinates. Check exported data before uploading it anywhere.

## Known limitations and useful testing

- The device's own hotspot is not filtered from observations yet.
- Radio send success is MAC-layer acknowledgement, not confirmation that records
  reached the master's application queue or SD card. Queue saturation can drop data.
- Receive validation order and delayed send-callback handling need additional tests.
- ESP-NOW enrollment is unauthenticated and unencrypted. Use an isolated test setup;
  matching clusters on the same channel can interfere. The local HTTP controls
  have no separate application authentication.
- Anyone connected to the hotspot can read GPS/session status and operate its
  controls. The public default password is not private. Serial output and SD
  files contain observed SSIDs, device addresses and positions. No automatic
  internet upload or remote analytics is implemented in the packaged code.
- Active SD wiring diagnostics require explicitly enabling `HG_SD_BENCH_CHECKS`.
  Keep it disabled for normal use, including boards with no card attached. The
  bench option drives GPIOs and requires disconnected modules and verified jumpers.
- The optional raw SD driver has unresolved token-buffer/capacity handling;
  keep `HG_SD_OWN_DRIVER` disabled. The supplied builds use ESP-IDF SDSPI.
- Current-build 20-scanner field validation remains outstanding. The host harness
  uses one scanner/master pair and synchronous callbacks.
- Power loss can discard buffered records, particularly before a GNSS fix.
- Observation time and position are assigned at master reception; the wire
  record has no scanner capture timestamp. Reporting delays affect that mapping.
- The status/display clock uses a placeholder date before GNSS time is known;
  it is not a real UTC date. Exported observations leave unknown times empty.
- A full pending-time buffer can emit newer rows before older buffered rows.
  Chronological CSV order is therefore not guaranteed under saturation.
- SSIDs are untrusted input. CSV quoting preserves columns, but does not disable
  spreadsheet formulas or terminal control sequences. Import exports as text.

Useful reports include build configuration, scanner count, uptime, queue/drop
counters, reset/recovery behavior and whether Stop completed. Remove SSIDs,
MAC addresses, coordinates, credentials and personal paths before sharing logs.
Use only equipment and testing environments where you have permission.

## Host tests

With GCC and Make available, from `firmware/test/`:
```
make
```
This runs record, tally, CSV, pending-buffer and ESP-NOW tests, including simulated
lost acknowledgements. It does not test RF hardware, real callback scheduling or
the maximum node count.
