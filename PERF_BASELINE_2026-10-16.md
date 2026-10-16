# L2CAP CoC first measured baseline — 2026-10-16

This is the first macOS and S3↔S31 bench checkpoint for the
[measurement plan](PERF_MEASUREMENT_PLAN.md). It does not complete the iPhone,
streaming, duplex or product-firmware acceptance phases. No library defaults
are changed by this work.

## Setup and reproducibility

- ESP32-S3 revision 0.2 at 240 MHz and ESP32-S31 coreboard at 320 MHz.
- Mac13,2, macOS 27.0.1 (26A434). Xcode 27.1 RC is selected with
  `DEVELOPER_DIR` for each build; the machine's global selection is untouched.
- S3 to Mac approximately 1 m, one glass pane, no wall. Fixed placement during
  the comparisons. The S31 is on the same bench; its distance was not separately
  measured. A Bluetooth Jabra headset is connected to the Mac; concurrent audio
  and other host activity are not controlled.
- Library base `79315b7` (runtime merge `d8a7ec2`), ESP-IDF
  `fff9895c82d744c7237be8847347bdd1b07c6643`, IDF NimBLE
  `139cada0ae932957fa06ba37d17e3c9c2c95c773`.
- Performance build, Wi-Fi/Ethernet uninitialized, deferred callbacks and TX
  diagnostics disabled. MSYS-1: 24 × 256; MSYS-2: 24 × 320; ACL buffers: 24;
  SDU buffers: 5. Alignment is off unless explicitly labelled on.
- Each baseline case: 8 warmup exchanges followed by 64 measured exchanges,
  three repetitions. Separate small smoke tests are labelled as such.
- The ESP orchestrates and times the traffic in both GAP roles. In product GAP
  roles, Apple is Central and opens CoC to the ESP Peripheral. This does not
  reproduce the product's application-level request initiator; ECOS acceptance
  remains necessary.

All request/response sizes in the tables are relative to the ESP DUT.
**8→4096 loads Apple→ESP; 4096→32 loads ESP→Apple.** Echo loads both. Values
exclude the four-byte application framing header but include the sequence,
token and response turnaround fields.

Raw JSON, console/peer logs, firmware binaries/ELFs, configs, source snapshots
and checksums live outside Git at `~/Documents/late/measurements/ble/2026-10-16`
(`~/Dokumente/late/measurements/ble/2026-10-16` on the Linux host). ELF hashes in
JSON distinguish the uncommitted bench-source variants; a Git base alone is
insufficient to reconstruct these builds. All final controlled A/B and ESP-pair
firmware hashes have matching archived ELFs. Some early exploratory and initial
GAP-role captures predate artifact preservation: their original binaries are not
retained. Keep them as historical observations, not exact rebuild baselines.
Local artifacts retain radio/device identifiers; this report omits them.

## Measurement integrity and setup findings

1. A CLI started over SSH reported CoreBluetooth unauthorized, without a consent
   dialog or a Settings entry. Packaging a stable `.app` identity with Bluetooth
   usage descriptions, signing it and launching it through LaunchServices in
   the logged-in GUI session resolved that path. Use
   `core-bluetooth-tool/tools/run-l2cap-reflector-app.sh`; no TCC database edits
   are required.
2. GAP roles matter. The first reverse-role series negotiated CoC MTU 2048;
   Apple-Central/ESP-Peripheral negotiated MTU 1251. Keep these series separate.
3. The first S31 interval update collided (`status=554`, different transaction
   collision), leaving 30 ms. After adding the already-used ECOS delayed retry
   to the bench setup: `attempt=1, request_rc=0, prior_status=554,
   completion_status=0, actual_interval_ms=15`. This is a harness setup fix,
   not a library performance optimization. It runs outside the timed loop and
   outside GAP callbacks. The original 30-ms files remain valid controls.
4. One earlier S31 capture lost characters through IDF's USB stdout VFS and
   failed JSON parsing. Preserve it as failed evidence. Bench JSON now uses
   blocking USB-driver writes after timing stops, and the runner rejects
   missing, duplicate or reordered samples, including warmups.
5. Controlled A/B runs use `--require-params`. Per-run parameters are captured
   before and after traffic. Comparison keys include GAP role, interval, PHY,
   MTU and observed DLE; mismatches cannot produce a purported same-link gain.

RTT ends at complete-response parsing, using only the DUT clock. Adjusted RTT
subtracts peer-local turnaround; it is still a round trip. `first_callback_us`
is whole-SDU delivery, not physical first-byte latency. Goodput counts useful
pattern bytes in both directions, excludes framing/metadata and is
single-request-at-a-time goodput, not streaming or physical BLE throughput.
Table RTT and p95 are medians of the three per-run values, not a pooled p95.

## Results

### Apple Central / ESP Peripheral

All rows: actual interval 15 ms, PHY 2M/2M, DLE 251/251, CoC MTU 1251.

| DUT | ESP request → response | Median RTT ms | p95 ms | Goodput KiB/s |
|---|---|---:|---:|---:|
| S3 | 64 → 64 | 29.851 | 29.878 | 3.49 |
| S3 | 4096 → 4096 | 164.213 | 194.212 | 48.09 |
| S3 | 8 → 4096 | 105.806 | 163.073 | 33.90 |
| S3 | 4096 → 32 | 89.710 | 134.621 | 39.26 |
| S31 | 64 → 64 | 29.849 | 44.789 | 3.38 |
| S31 | 4096 → 4096 | 209.554 | 254.539 | 38.82 |
| S31 | 8 → 4096 | 90.995 | 134.694 | 38.50 |
| S31 | 4096 → 32 | 119.757 | 164.811 | 32.54 |

Source files: `apple-central-*-product-ab-{echo,tx4096}-baseline.json`,
`apple-central-s31-request8-baseline-15ms.json`,
`apple-central-s3-request8-final-15ms.json`. Chip rows were measured sequentially.
Differences combine controller, host timing, RF conditions and CPU; they do not
isolate a chip or library effect.

### TX SDU alignment, off/on/off

Same source and configuration, except `CONFIG_NIMBLE_CPP_L2CAP_ALIGN_TX_SDUS`.
Config diffs were checked for both targets. Each row is three repeats ×64,
plus 8 warmups per repeat. Actual link parameters match those above.

| DUT | ESP request → response | Phase | Median RTT ms | p95 ms | Goodput KiB/s |
|---|---|---|---:|---:|---:|
| S3 | 4096 → 4096 | baseline | 164.213 | 194.212 | 48.09 |
| S3 | 4096 → 4096 | align-on | 161.387 | 192.816 | 51.13 |
| S3 | 4096 → 4096 | return-off | 164.197 | 195.550 | 47.94 |
| S3 | 4096 → 32 | baseline | 89.710 | 134.621 | 39.26 |
| S3 | 4096 → 32 | align-on | 89.189 | 104.661 | 47.29 |
| S3 | 4096 → 32 | return-off | 89.649 | 134.735 | 38.04 |
| S31 | 4096 → 4096 | baseline | 209.554 | 254.539 | 38.82 |
| S31 | 4096 → 4096 | align-on | 194.588 | 225.800 | 41.12 |
| S31 | 4096 → 4096 | return-off | 209.554 | 270.761 | 37.87 |
| S31 | 4096 → 32 | baseline | 119.757 | 164.811 | 32.54 |
| S31 | 4096 → 32 | align-on | 104.816 | 149.787 | 36.34 |
| S31 | 4096 → 32 | return-off | 119.524 | 150.229 | 33.56 |

### ESP Central / ESP reflector

Alignment off on both boards. Actual interval 15 ms and PHY 2M/2M are
required. Negotiated CoC MTU and DLE are recorded in the files.

| DUT → reflector | ESP request → response | Median RTT ms | p95 ms | Goodput KiB/s |
|---|---|---:|---:|---:|
| S3 → S31 | 64 → 64 | 29.854 | 29.872 | 3.49 |
| S3 → S31 | 4096 → 4096 | 88.773 | 103.766 | 84.83 |
| S3 → S31 | 8 → 32 | 29.869 | 29.882 | 0.65 |
| S3 → S31 | 8 → 4096 | 58.984 | 72.193 | 65.12 |
| S3 → S31 | 4096 → 32 | 59.635 | 74.631 | 65.09 |
| S31 → S3 | 64 → 64 | 29.856 | 29.910 | 3.46 |
| S31 → S3 | 4096 → 4096 | 89.380 | 104.383 | 85.29 |
| S31 → S3 | 8 → 32 | 29.865 | 29.907 | 0.64 |
| S31 → S3 | 8 → 4096 | 59.456 | 72.677 | 64.86 |
| S31 → S3 | 4096 → 32 | 59.784 | 74.785 | 64.60 |

These are S3↔S31 results, not S3↔S3. Changing the DUT also changes GAP and
application roles; do not infer a directional chip limitation from them alone.

## Interpretation

The controlled macOS off/on/off results justify continued testing of alignment,
without changing its experimental default. On S31, the 4-KiB echo median moves
from about 210 ms to 195 ms and returns to 210 ms; large ESP→Mac transfers also
show a modest gain. On S3, the large-transfer median barely moves, while p95 and
goodput improve in the on phase and mostly return in the off phase. Judge all
three metrics together; median alone would hide the S3 tail/throughput result.

In the large-frame macOS baseline, peer turnaround medians are about 75 µs
(echo) or 15 µs (4096→32), compared with tens to hundreds of milliseconds RTT.
The reported peer application turnaround is therefore a small part of these
measurements; this does not identify independent one-way radio delays.

These are sequential comparisons in one uncontrolled host/radio environment.
Earlier reverse-GAP-role comparisons did not show a consistent improvement,
and one apparent S31 gain persisted after returning to off. That is why this
checkpoint keeps both GAP roles and return controls, and why iPhone plus ECOS
acceptance remain the decision gates. Peer MPS/credit diagnostics are needed to
attribute the observed gain to the expected segmentation mechanism.

The clearest setup finding is the S31 interval retry: initial 30-ms control
4-KiB echo median 418 ms, corrected 15-ms series about 180 ms (separate runs).
This is not a library-speed comparison. Never benchmark requested parameters
as if they were accepted. Later 15-ms series vary too; compare local off/on/off
controls instead of choosing the best historical run.

## Capture audit and validation

The completed checkpoint has 50 successful measurement files, 333 measured
runs, 20,976 measured exchanges and 2,624 warmup exchanges. The final controlled
matrix alone contains 135 runs / 8,640 measured exchanges, with matching
archived firmware and accepted link parameters before/after every run. Every
successful file was audited for exact sample count/order and zero callback
faults. Both application endpoints validate payload length, sequence/token
and deterministic data. This is short-run acceptance, not an endurance claim.

One historical file is intentionally excluded:
`mac-s31-request32-baseline.json`, the USB JSON-corruption capture described
above. The successful replacement and the failed partial evidence are retained.
The private `audit.json` also identifies exploratory files without original
ELFs; none of those is used for the final controlled A/B or ESP-pair claims.

Validation: 14 native C++ tests, 5 Python evidence/comparison tests and 4 Swift
codec tests pass. Both S3/S31 performance images build with alignment off/on;
the macOS release reflector builds and runs through LaunchServices.

The Mac reflector is stopped after capture; the S3 keeps the alignment-off
bench image. The S31 is restored to the previously committed LIS2DW12 `hwtest`
image after the measurements. Physical double-tap acceptance remains deferred
to the operator; no iPhone or production ECOS acceptance is claimed here.

## Next gates

1. Add `stream-tx`, `stream-rx`, duplex and repeated connect/run/disconnect
   automation, with heap trends and TX credit/stall diagnostics.
2. Build the iPhone reflector using `L2CAPBenchKit`, record model/iOS and repeat
   both chips in product GAP roles. macOS results cannot substitute for iPhone.
3. Vary one library setting at a time with matched actual link parameters and
   off/on/off controls. Promote no default based on a single favorable run.
4. Re-run the ECOS diagnostic benchmark and real CAN-response lifecycle checks
   before updating the product's submodule/configuration. S3↔S3 awaits a second
   S3; the available ESP pair here is S3↔S31.


## Review interpretation — 2026-10-16

The alignment percentages above are exploratory observations from sequential
macOS echo/request runs. RTT changes are quantized by the 15-ms connection
interval; off/off p95 itself changes by an interval in some cases, and the
reported goodput derives from the same round-trip timings. These are not
independent measurements establishing a universal percentage gain. Streaming,
iPhone alignment A/B, host-task CPU and observed PDU/LL counts remain open.
See the review follow-up in PERF_STREAMING_2026-10-16.md for implementation
changes, the SDU-boundary contract and required counterprobes.
