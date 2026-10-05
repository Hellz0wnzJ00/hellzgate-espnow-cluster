# Current beta verification — October 5, 2026

This update was ported into the existing public ESP-NOW repository. Private
wired-scanner source, configuration and tests were excluded. The existing
public display driver is retained; it does not transport scanner records.
Previous public comment corrections and HTML escaping are preserved.

## Current checks

- Public host tests exercise the exact disk index with 1,000,000 distinct keys
  and 2,000,000 observations, collisions/cache eviction and I/O failures.
  Queue loss/backlog, validation, RAM-only fallback, BLE mode changes and
  ESP-NOW mode acknowledgements/retries are covered.
- Public web checks exercise both mode requests, pending/applied state,
  request failure and unique-count pending/incomplete messages. All 38 channels
  remain covered at every supported offset.
- All three public configurations (master, strapped scanner and NVS scanner)
  built with ESP-IDF 5.5.3. All executed host tests passed, including CSV;
  the earlier bench-workspace CSV execution block did not recur here.
- Final source checks exclude private wired-scanner symbols, files, settings,
  tests, bench logs, device/location data and machine paths. The existing
  public display driver remains. No public-source image has yet been flashed.
- The GCC Makefile was updated to include the new index/queue/mode tests;
  this run used the Windows MSVC host harness, not GNU make.

## Hardware evidence and open limits

The related FullGate ESP-NOW bench source was tested with one M1/S1 pair:
Wi-Fi-only produced 243 Wi-Fi rows and no BLE in approximately 50 seconds;
mixed resumption produced 144 Wi-Fi and 354 BLE complete rows. A GPS fix with
8 satellites and timestamped coordinates was observed. Final status reported
3,797 rows written/saved, no SD errors, no link sequence loss/duplicates,
no master-full events and no dropped/incomplete counting work.

These are serial/status observations, not a read-back audit of the physical
CSV. That bench source includes the private transport but ran ESP-NOW; this
public port removes the wired transport and requires its own hardware check.
The hardware unique-count overflow threshold was not reached. Long-run SD
throughput, multiple scanners, restart behavior in each mode, and improved
field capture yield remain unverified. Community beta testing is needed.

## Earlier release history (not a claim about the current tree)

# Review status - 2 October 2026

This experimental source package builds successfully in all three 20-node
configurations. A limited master/one-scanner bench test predates the final
capacity-default adjustment and SD diagnostic guard. Those final changes are
build-verified; their hardware verification remains pending.

## File coverage and privacy

Every line of all 73 candidate files was read, including production code,
headers, test fixtures/stubs, scripts, configuration, comments and documentation.
Follow-up scans checked names, paths, URLs, credentials, encoded content and
control characters. No private credentials, contributor correspondence,
personal computer paths, field captures or outside-contributor identities were
found in this candidate. Sean Clossey's owner/MIT attribution is intentional.
Example coordinates, dates and addresses in tests are fixed synthetic fixtures.

No automatic external upload or analytics endpoint was found. Runtime data is
not confidential by design: read the ESP-NOW, hotspot, serial and CSV limitations
in README. Complete file coverage does not guarantee absence of vulnerabilities.

## Corrections and static checks

Removed stale wired-transport notes and empty tests. Comments were revised,
but a follow-up review found further stale wording; coverage is not proof that
every statement or code path is correct. Restored two meaningful disabled configuration values
that the earlier cleanup had incorrectly treated as comments: the FullGate
SSD1306 selection and the standalone scanner's disabled hardware straps.

All 17 CMake sources and their 35-file production include closure resolve.
All 78 project include references, 80 ordinary API declarations and 45 HG
configuration references checked resolve. External SDK headers were located;
SDK implementation and transitive dependencies are not included in this review
or licensed by this project's MIT file.

## Full builds

On 2 October 2026, the FullGate master, hardware-slot scanner and NVS-ID scanner
each compiled, linked and generated an ESP32-C5 firmware image successfully with
ESP-IDF 5.5.3 after repair of the local Python environment. The starting firmware
references 5.5.1; that SDK version was not re-tested. Generated binaries remain
outside this source package.

The refreshed generated configurations confirm the intended roles and capacity
20 for the master and both scanner variants, disabled straps for NVS, SSD1306
selection for the master, and 8 MB flash. The optional raw SD driver, bench
diagnostics and pin finder are disabled. All three refreshed builds completed
without compiler or CMake warnings.

Active SD mount-failure GPIO diagnostic definitions and calls are now gated by
the disabled-by-default bench option. Normal mounting and recording are unchanged.
All three variants were rebuilt successfully after this correction. Disassembly
of the default master's storage object confirms that the pin-driving diagnostic
functions and calls are absent. A deliberate SD-failure hardware test was not run.

## Two-board bench test and capacity adjustment

The master and one hardware-slot scanner were flashed and tested together on
2 October 2026. Target selftests passed, the scanner read slot 1, ESP-NOW records
arrived, and the master mounted the SD card. A controlled scanner reset produced
one down event and one restart, followed by resumed records without a master
restart. Hotspot connection, web Stop/save, and starting another session worked.
The two files closed with 818 and 298 rows reported saved, with zero reported SD
errors. Reading the actual files from the card remains pending.

The GNSS antenna was added during the test. The first session exercised no-fix
logging; a GNSS fix was observed later. This was a short two-board bench check,
not a field test or a 20-scanner throughput test.

After that test, the remaining 9-node Kconfig default and tally fallback were
changed to 20, and the hardware-slot scanner overlay now explicitly selects 20.
The master and NVS scanner overlays already selected 20. Four strap pins still
encode only slots 1-15; standalone scanners use provisioned NVS IDs. All three
variants were rebuilt and their generated settings checked after this adjustment.
The revised scanner image has not yet been tested on hardware.

## Verification limits

Only the limited hardware checks above have been performed. These checks preceded
the public beta release; they do not establish production readiness.
Compilation alone does not establish radio or storage reliability.
Ten cross-compiler syntax checks passed without diagnostics; host tests were not
executed. The local page passed JavaScript syntax checking and the provisioning
script passed Python parsing; neither contacted hardware. Actual HG Kconfig
parsing confirmed master/strap-scanner/NVS-scanner roles, ID limits, display
selection and strap/NVS settings. All retained overlay values match the starting
firmware apart from intentional scanner-bus/shared-display-bus removals and the
hardware-slot scanner capacity adjustment described above.
These checks and successful builds do not replace hardware verification.

Other experimental reliability/privacy limitations are documented in README.
The final source changes do not yet have a repeat hardware test, and the actual
SD files have not yet been read back. These limits remain open for community
testing; successful compilation is not a claim of production readiness.

## Follow-up source review - October 2, 2026

All 73 tracked files were reviewed again, including shared headers and test
stubs. Stale GNSS descriptions and misleading comments were corrected; no
executable firmware behavior was changed. All three variants built successfully
from this working tree with ESP-IDF 5.5.3. Ten host-test translation units passed
cross-compiler syntax checking, and the provisioning script parsed successfully.
Host tests were not executed, and no new hardware validation was performed.
Receive validation, delayed send callbacks, SD failure handling and malformed
GNSS input still need focused regression tests and potential behavior changes.
This review does not establish production readiness or replace the limitations
above.
