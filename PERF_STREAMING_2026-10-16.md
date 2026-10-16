# L2CAP streaming, duplex and reconnect checkpoint — 2026-10-16

This continues [the first baseline](PERF_BASELINE_2026-10-16.md) on the same
S3, S31 coreboard and Mac13,2/macOS 27.0.1 bench. The production ECOS pin and
all library defaults remain unchanged. The current bench protocol is version 2;
echo/request wire layouts remain the same, while stream modes add end-to-end
receipts and a final completion fence.

After the operator authorized the final Mac app, its repeated matrix and
forced-disconnect/recovery tests passed on both chips. There are now 210
explicit reconnect stress cycles. The final archived peer is identified below;
iPhone, long endurance and production acceptance were open at that checkpoint.
The later resource/iPhone checkpoint at the end records subsequent acceptance
and the completed hour-scale test.

## Finding: CoC credits do not bound native HCI backlog

The unpaced large duplex smoke failed reproducibly with the baseline 24×256 /
24×320 host pools. Small duplex and Mac→ESP streaming worked. The instrumented
S3 failure records:

```text
NimBLEL2CAPChannel: TX failed rc=6 offset=2502 length=1251 total=4100
pattern=duplex, success=false, tx_frames=1, rx_frames=0, callback_faults=0
```

`rc=6` is `BLE_HS_ENOMEM`. At least one application frame and part of another
had already been submitted. The pinned `ble_l2cap_tx` converts HCI EAGAIN to a
queued packet and returns success; `ble_l2cap_coc_continue_tx` can then run out
of mbufs while continuing to consume peer credits. Its error path frees the
remaining SDU. A blind retry could duplicate earlier bytes and corrupt the
framed application stream. The diagnostic Mac connection advertised **65,535 initial CoC
credits**, with peer MPS 1251 and local MPS 248. Across 518 logged outgoing SDUs
the successful candidate recorded zero CoC-credit stalls and zero send errors:
peer credits are not the limiting local resource here.

The library continues to abort on an actual send
error, including ENOMEM and EAGAIN; no partial SDU retry was added.

The opt-in `CONFIG_NIMBLE_CPP_L2CAP_HCI_BACKPRESSURE` checks the connection's
native HCI ACL queue and outstanding ACL packet count on the NimBLE host,
under its lock, before calling
`ble_l2cap_send`. The initial queue-only candidate passed the S3 matrix but
still failed on S31 (`rc=6`, offset 1251 of 4100). The revised candidate also
waits when outstanding ACL packets reach the explicit resource threshold
`CONFIG_NIMBLE_CPP_L2CAP_HCI_TX_WINDOW` (8 in these tests). One SDU may add
several packets, so the threshold governs starting a new SDU rather than
asserting a hard total packet cap. If blocked, that SDU is not submitted. Only a worker
writer can wait one RTOS tick and check readiness again, within the pre-existing
send deadline. When both readiness checks are clear there is no deliberate wait; host callbacks
never wait and idle connections have no polling task. The internal readiness
result is distinct from native send errors, so it cannot accidentally turn a
partially submitted SDU into a retry.

This is an experimental candidate using the pinned private IDF connection
layout, not a default change or a universal guarantee for arbitrary buffer
sizes, other radios or multiple channels. It requires enough resources for one
SDU. iPhone acceptance is recorded below; production ECOS acceptance remains
open. Unit tests cover
backlog/drain, deadline, disconnect, host-callback nonblocking behavior and the
unchanged rule that actual send errors abort after one attempt.

## Measurement method

- Actual 15-ms interval, 2M/2M PHY and 251/251-byte DLE are required for the
  controlled runs. CoC MTU is 1251 with Apple Central and 5000 for the ESP pair.
- Alignment, deferred callbacks and TX diagnostics are off for timed runs.
  The HCI-backpressure candidate is explicitly labelled in each dataset.
- Streams validate every frame's length, sequence, per-run token and pattern.
  There are no per-frame application ACKs. END carries the announced count;
  the receiver's receipt carries count, bytes, errors and local first/last
  timestamps. FINISH succeeds only after both enabled directions and receipts
  complete. The host also requires every per-frame TX/RX evidence record.
- ESP stream receive validation runs in its read callback; no writes or serial
  output run there. Reflector writes have a separate task. macOS generates
  output under stream backpressure while continuing to receive. All per-frame
  DUT samples are printed after timing stops.
- Wall goodput is useful pattern bytes across both enabled directions divided
  by START-through-FINISH duration, including control round trips. Arrival
  goodput excludes the first frame and uses the receiver's own first-to-last
  interval. No clocks are synchronized or compared between peers.
- Reconnect cycles sample heap after closing both CoC and GAP plus 100 ms of
  RTOS cleanup. Reports retain every cycle and exclude five initial cycles
  from trend summaries. Small byte-level allocator fluctuations are reported,
  not silently treated as zero. Task count and largest internal block are
  checked alongside free heap; these short tests cannot prove day-long safety.
- TX diagnostic windows are parsed from BLETx records. These windows cover
  32 SDUs or a disconnect and can cross run boundaries; diagnostic passes are
  separate from timing baselines.

Raw JSON/logs, peer logs, ELFs/binaries/configs, source snapshots and checksums
are kept outside Git in `~/Dokumente/late/measurements/ble/2026-10-16-phase2`
on the Linux host. Failed attempts remain evidence and are excluded from
performance comparisons. Data below are measured, not product release gates.

## Results

### Final archived Mac peer: repeated matrix

The previously blocked final peer now passes with macOS Bluetooth authorization
`allowedAlways` (3), manager state `poweredOn` (5), and automatic scanning.
Its executable SHA-256 matches the archived binary before and after the runs:

```text
d4c03055c22b3dabc3cc2bb0b9fc12dedfd820362ee713de2633c18675cdf71a
```

Each row uses three 128-frame repetitions at 4096-byte DATA payloads and
8 warmups per repetition. DUT firmware is the archived scan-cache-cleanup
window-8 image. Actual 15-ms interval, 2M/2M PHY, 251/251 DLE and MTU 1251
are required. These are sequential repetitions with uncontrolled ambient radio
traffic; differences from the earlier peer do not isolate a code-change effect.

| DUT | Pattern | Earlier wall KiB/s | Final-peer wall median KiB/s | Final-peer wall range | Final TX arrival KiB/s | Final RX arrival KiB/s |
|---|---|---:|---:|---:|---:|---:|
| S3 | stream-tx | 52.34 | 51.31 | 50.47–56.13 | 51.93 | — |
| S3 | stream-rx | 57.74 | 66.93 | 57.17–67.07 | — | 67.45 |
| S3 | duplex | 56.40 | 55.13 | 54.60–55.67 | 27.66 | 28.42 |
| S31 | stream-tx | 51.16 | 47.72 | 47.45–49.88 | 48.14 | — |
| S31 | stream-rx | 57.45 | 57.16 | 51.70–58.74 | — | 57.42 |
| S31 | duplex | 54.03 | 51.42 | 48.67–53.48 | 33.90 | 25.76 |

Echo uses three 64-exchange repetitions per size, plus warmups:

| DUT | Echo bytes | Earlier median ms | Final-peer median ms | Final-peer p95 ms |
|---|---:|---:|---:|---:|
| S3 | 64 | 29.84 | 29.84 | 29.88 |
| S3 | 4096 | 164.19 | 164.08 | 195.58 |
| S31 | 64 | 29.88 | 29.86 | 44.86 |
| S31 | 4096 | 179.59 | 179.51 | 224.43 |

Twenty additional full CoC/GAP cycles per chip use the final Mac peer. Ranges
below exclude the first five cycles, as for the earlier lifecycle captures.

| DUT | Cycles | Free heap | Internal heap | Largest internal block | Tasks |
|---|---:|---:|---:|---:|---:|
| S3 | 20 | 214152–214152 | 213948–213948 | 155648–155648 | 10–10 |
| S31 | 20 | 318404–318420 | 318200–318224 | 270336–270336 | 10–10 |

These short samples do not establish day-long leak-free operation.

### Initial streaming matrix, HCI window 8

Every row is the median of three 128-frame runs at 4096-byte payloads;
each repetition has 8 warmups. Wall goodput is summed across enabled directions.
Arrival goodput uses that receiver's clock and excludes the first frame.

| DUT / peer | Pattern | Wall KiB/s | ESP TX arrival KiB/s | ESP RX arrival KiB/s |
|---|---|---:|---:|---:|
| S3 / apple | stream-tx | 52.34 | 52.73 | — |
| S3 / apple | stream-rx | 57.74 | — | 58.22 |
| S3 / apple | duplex | 56.40 | 28.41 | 29.29 |
| S31 / apple | stream-tx | 51.16 | 51.52 | — |
| S31 / apple | stream-rx | 57.45 | — | 57.88 |
| S31 / apple | duplex | 54.03 | 35.58 | 27.07 |
| S3 / s31 | stream-tx | 93.86 | 95.37 | — |
| S3 / s31 | stream-rx | 79.78 | — | 80.22 |
| S3 / s31 | duplex | 97.20 | 48.96 | 48.86 |
| S31 / s3 | stream-tx | 80.55 | 81.38 | — |
| S31 / s3 | stream-rx | 95.97 | — | 96.82 |
| S31 / s3 | duplex | 106.14 | 53.57 | 53.55 |

ESP-pair results use the available S3 and S31, not two S3s. Chip/GAP roles and
radio conditions differ; these rows do not isolate a hardware-architecture effect.

### Low-load control, same source with window enabled/disabled

Three 64-exchange repetitions per row; actual interval 15 ms and PHY 2M/2M.
Both sets used the same active Mac peer before the later CLI-start hardening.

| DUT | Echo bytes | Window-8 median ms | Disabled median ms | Window-8 p95 ms | Disabled p95 ms |
|---|---:|---:|---:|---:|---:|
| S3 | 64 | 29.84 | 29.86 | 29.91 | 44.86 |
| S3 | 4096 | 164.19 | 167.03 | 206.36 | 209.19 |
| S31 | 64 | 29.88 | 29.87 | 44.81 | 44.83 |
| S31 | 4096 | 179.59 | 179.56 | 210.89 | 210.88 |

Small-echo medians remain about 30 ms. Sequential large-frame variation is
not sufficient to infer a sub-millisecond overhead; there is no deliberate
preflight wait when the queue and outstanding-packet checks are clear.

### Initial reconnect stress and heap after teardown

First five cycles excluded from trend ranges. Each cycle has small and 4-KiB
traffic plus full CoC/GAP teardown. Values are bytes unless stated otherwise.

| Series | Cycles | Free heap range | Internal heap range | Largest internal block range | Tasks |
|---|---:|---:|---:|---:|---:|
| apple-s3-lifecycle-backpressure | 30 | 214400–214400 | 214204–214204 | 155648–155648 | 10–10 |
| apple-s3-lifecycle-hci-window8 | 25 | 214400–214400 | 214204–214204 | 163840–163840 | 10–10 |
| apple-s31-lifecycle-baseline | 30 | 318484–318508 | 318276–318308 | 270336–270336 | 10–10 |
| apple-s31-lifecycle-hci-window8 | 25 | 318412–318420 | 318188–318224 | 270336–270336 | 10–10 |
| esp-s3-to-s31-lifecycle-cache-cleaned-window8 | 10 | 258488–258488 | 258284–258284 | 212992–212992 | 9–9 |
| esp-s3-to-s31-lifecycle-hci-window8 | 20 | 212272–212752 | 212076–212552 | 108544–108544 | 10–10 |
| esp-s31-to-s3-lifecycle-cache-cleaned-window8 | 10 | 318228–318232 | 318016–318020 | 233472–262144 | 10–10 |
| esp-s31-to-s3-lifecycle-hci-window8 | 20 | 314956–315472 | 314720–315248 | 262144–262144 | 10–10 |

The first 150 stress cycles retained scan results: task counts and largest
free blocks were stable within each series, with bounded free-heap variation
in ESP-central roles. Nearby advertisers were still represented in that heap.

The final 20 cycles clear the scan cache after client deletion. The settled
S3 free/internal heap is constant; S31 varies by four bytes. S31 largest-block
size fluctuates between 233472 and 262144 bytes without decreasing free heap;
the short capture does not establish the allocator cause. Task counts are
stable within each final series. Compare settled cycles within one build/role,
not absolute heaps across images. These 170 cycles do not prove day-long
leak-free operation. The retained parser buffer can also produce a one-time
roughly 8-KiB high-water allocation after the first large RX.

### Longer continuous duplex

The S31 DUT and S3 peer completed 4096 DATA frames in **each** direction:
8192 frames / 33,554,432 payload bytes (32 MiB), excluding framing.
Elapsed START-through-FINISH: 298.74 s; wall goodput: 109.47 KiB/s.
Every frame and receipt passed; callback faults were zero. This is a roughly
five-minute continuous run, not an hour/day-scale soak.

### Forced disconnects

| DUT / ESP peer | Host injection → runner exit | Recovery |
|---|---:|---|
| S3 / S31 | 0.365 s | Fresh 64×4-KiB duplex passed |
| S31 / S3 | 0.265 s | Fresh 64×4-KiB duplex passed |

Neither ESP showed a crash/reboot in these captures. The injection scripts
verify failed runs are reported as failed, then start a fresh connection;
they do not hide the error with an in-run retry.

## Mac restart: initial block and completed recovery

The first intentionally terminated Mac peer made the ESP fail its active stream
without a visible ESP panic/reboot, but its first recovery attempt timed out.
That earlier attempt remains a failed file, not a passed recovery test.

The prepared final binary initially reported `CBManager.authorization=0`
(`notDetermined`) and manager state `unknown`, without a state callback. This
was not an observed denial. After the operator granted access, the manager
reported `poweredOn` and started scanning. Relaunches reported authorization 3
(`allowedAlways`). The synchronous CLI bootstrap enters on the real main thread;
the reflector uses AppKit's event loop and explicit object lifetimes. Its
parse/help/error checks, main-thread precondition and subsequent real BLE runs
all pass; the earlier capture does not isolate the startup change from consent.

The final test sends 4096-byte duplex DATA, terminates only the verified bench
app with SIGINT during that transfer, then restarts the exact app using
`L2CAP_BENCH_REUSE_APP=1`. Each ESP must report failure with partial TX/RX and
no FINISH confirmation; a fresh connection must complete 64 frames each way.

| DUT | Interrupted TX submitted / RX validated | Host injection → runner exit | Fresh 64×4-KiB duplex |
|---|---:|---:|---|
| S3 | 31 / 33 | 1.187 s | Passed |
| S31 | 39 / 19 | 1.294 s | Passed |

Both failed runs are explicitly unsuccessful, with no callback fault or observed
ESP panic/reboot. Both recoveries pass and the Mac binary SHA-256 is unchanged
afterward. The two relaunches needed no further consent. No benchmark Mac app
is left running after the tests.

Injection durations use the Linux host's monotonic clock, from before the SSH
stop command through runner exit; they include SSH and teardown. They are not
radio-disconnect latency or a one-way measurement. The earlier ESP-peer table
also times the host's close command through runner exit. These process/peer-close
tests do not substitute for RF-loss or controller-reset fault injection.

An ad-hoc designated requirement contains a binary hash; rebuilding can require
a new Bluetooth decision. Reuse mode avoids rebuilding or resigning between
faults. Development-certificate signing previously returned
`errSecInternalComponent` over SSH; no Keychain access policy or TCC database
was changed. The private scripts and raw failed attempts are retained.

## Validation and evidence scope

Twenty-four C++ tests, nine Python evidence/runner tests and six Swift codec tests
pass. New checks cover stream gaps, duplicates, altered tokens/data, malformed
receipts, complete evidence, a reboot during a long command, HCI queue/window
backpressure, disconnect/deadline behavior and host-callback nonblocking.
Actual native send errors still abort on their first attempt.

The final evidence audit validates 604 successful measured runs across 48 files:
33,712 streaming DATA frames / 88,184,320 DATA-payload bytes, 2,928 echo/request
exchanges and 173 separately validated warmup runs. Eleven failed/incomplete files
remain in the private evidence: baseline/candidate resource failures, intentional
disconnects and the Mac authorization-blocked recovery. They are excluded from
the successful totals. `audit.json` records the per-file verdicts and ELF mapping.
The final Mac log independently matches 156 stream completions, including
warmups, to DUT results using DUT identity, run token and CONFIG geometry/count.
Tokens are per-run checks, not globally unique identifiers; matching token alone
across a cumulative multi-device log would be insufficient. The contextual
checks are retained in `final-peer-audit.json`.

The TX diagnostic pass is DUT-side. Continuous ESP reflector-console logging
is not implemented; the runner refuses a TX-diagnostic reflector build with
`--peer-port`, preventing missing log data or USB logging backpressure from
silently affecting a run. Timed peer builds keep TX diagnostics off.

All recorded successful native firmware ELF hashes have matching archived
ELFs/configs. Native candidate snapshots and the final Mac source/binary are
retained. The original active Mac performance-peer binary was not saved before
the later startup hardening; the initial Apple tables describe that earlier
observed peer. The final-peer matrix above supplies the repeated Apple reference
with an archived binary and contextual peer evidence. The ESP-pair results and
native artifacts also remain identified.

Enabling error logging also exposed `NimBLEServer: subscribe event;
attr_handle=8, not found` around some disconnects. This is retained as a logging
follow-up; it did not coincide with a failed data receipt in successful runs.
It is not silently counted as a transport-validation failure or declared fixed.

## Remaining gates

The iPhone reference, production ECOS/CAN/radio-lifecycle acceptance, additional
pool/PHY/deferred-callback variations and hour/day-scale soak remain open.
No product submodule pin or library default is promoted by this checkpoint.

## Bench handoff and next acceptance

The S31 was restored to ESPenlaub's LIS2DW12 hwtest image after these runs.
Flash verification passed and the console exposes the `lis2dw12` command.
No physical double-tap acceptance was repeated. The S3 retains the optional
window-8 benchmark image. Both serial ports are released; no runner is left
holding a board.

Next are the iPhone peer, hour/day-scale streaming and reconnect soak,
RF-loss/controller-reset injection, then the production
ECOS CAN-answer acceptance across radio transitions. The experimental window
must not become a product default based on these bench results alone. Pool-size,
alignment/deferred-callback streaming sweeps and a continuous diagnostic
ESP-peer console reader remain separate measurement work.

## SoC interpretation and proposed upstream work

For the prioritized Mac Central / ESP Peripheral path, the final three-run
medians favor S3: about 7.5% more ESP→Mac streaming goodput, 17.1% more
Mac→ESP goodput and 7.2% more summed duplex goodput. Large echo median latency
is about 8.6% lower on S3; small echo is effectively tied at 29.84/29.86 ms.
The earlier Apple matrix also favors S3 streaming, though by smaller margins.
This identifies the better-performing board in this bench, not a general SoC
ranking. We have one board of each type, sequential measurements, different CPU
frequencies (240/320 MHz), unnormalized antennas/placement and uncontrolled
ambient host/radio activity. Three repetitions are not a statistical population
or a CPU/controller profile. Both chips pass payload validation and recovery.

For ESP↔ESP, the S31-central duplex run is faster (106.14 versus 97.20 KiB/s),
but changing DUT also switches both peers' GAP roles. Unidirectional S3-source
rows reach about 94–96 KiB/s versus about 80 KiB/s with S31 as source. A single
mixed pair cannot isolate a chip effect. No iPhone performance decision follows
from these Mac and mixed-pair results; iPhone remains the next Apple reference.

### Current branch readiness (read-only GitHub check, 2026-10-16)

The current [upstream master](https://github.com/h2zero/esp-nimble-cpp/commit/9c775c7ddac9571fcee650fc9ffc6dd539e2b950)
is `9c775c7`, already an ancestor of our published fork master `79315b7` via
`d8a7ec2`. Both exact commits have 65 successful check runs. The
[existing build workflow](https://github.com/h2zero/esp-nimble-cpp/blob/9c775c7ddac9571fcee650fc9ffc6dd539e2b950/.github/workflows/build.yml)
builds ordinary and extended GATT examples on IDF 5.4/5.5; it does not build the
CoC benchmark or run the new native L2CAP tests. Thus green master CI alone does
not validate this CoC delta. The latest benchmark/HCI-backpressure changes are
still uncommitted locally and are not on the published master.

No open upstream PR authored by `mickeyl` was found. The earlier
[disconnect/error API PR #396](https://github.com/h2zero/esp-nimble-cpp/pull/396)
is merged. [Open PR #419](https://github.com/h2zero/esp-nimble-cpp/pull/419)
also addresses blocking on the host task; its approach should be coordinated
with lifecycle work rather than creating competing host-task conventions.

The table below records the proposal at the analysis checkpoint. It was
authorized afterward; the submission checkpoint below records the resulting PRs.

| Order | Proposed scope | Readiness and required work |
|---|---|---|
| 1 | `build: enable ESP32-S31 and guard broadcaster-only state` | Extract the small target/role-guard delta from `9a3aac9`; retain S3 regression builds and add a separate pinned preview-IDF S31 build, rather than inserting S31 into an unsupported stable-IDF matrix. Ready for preparation. |
| 2 | `fix(l2cap): report send errors and avoid retrying partial SDUs` | A concrete current-upstream defect: `write()` checks `< 0`, while several `writeFragment()` error paths return positive NimBLE errors. Its ENOMEM/EAGAIN retry can replay the beginning of a partly sent SDU. Submit the smallest correctness fix with error/ownership tests and clean abort semantics. Do not mix experimental queue tuning into it. |
| 3 | `fix(l2cap): serialize writers and drain disconnect/deinit work` | Extract the interdependent lifecycle and pool changes with their native tests, clear callback ownership/threading contract, and a worker-driven CoC example. The full-SDU pool layout/count changes memory use; disconnect callbacks moving to a worker are observable behavior. Discuss optional callback offload separately. Add actual CoC-enabled IDF 5.4/5.5 builds with inline/deferred variants before calling the extracted branch merge-ready. The existing green GATT CI does not cover this. Coordinate host-task handling with #419. |
| 4 | `test: add standalone CoC benchmark and serial evidence runner` | Submit the portable example, default config fragments, codec/evidence tests and runner after their library prerequisites. Link the separate Apple peer. Exclude private captures, device identities, full generated sdkconfigs and machine-specific paths from the upstream payload. |

HCI backlog/window preflight, peer-MPS alignment and native TX diagnostics remain
separate experimental follow-ups. They depend on private pinned host layouts;
upstream needs a supported readiness/geometry interface or explicit version
support before a generic enablement is defensible. First retain the ENOMEM
reproducer and validate iPhone plus production radio/CAN transitions. The new
window fixes the measured resource failure, but is not a universal scheduling
contract and is not promoted to a default here.

## Authorized upstream submission checkpoint

The approved split is published as:

- [#447: S31 target and broadcaster guards](https://github.com/h2zero/esp-nimble-cpp/pull/447), ready for review.
- [#448: send errors and partial-SDU replay](https://github.com/h2zero/esp-nimble-cpp/pull/448), ready for review. Native ownership was checked against the actual NimBLE revisions in IDF 5.4.3, 5.5.1 and 6.1; the PR body links the pinned source paths.
- [#449: write/lifecycle hardening](https://github.com/h2zero/esp-nimble-cpp/pull/449), stacked draft pending #448.
- [#450: benchmark and evidence runner](https://github.com/h2zero/esp-nimble-cpp/pull/450), stacked draft pending #447/#448/#449.

All pushes use SSH. The separate Apple peer/shared Swift codecs are published at
[core-bluetooth-tool 6124827](https://github.com/mickeyl/core-bluetooth-tool/commit/6124827).
The original experimental worktree/master is preserved; none of its HCI-window,
MPS-alignment or private TX-diagnostic implementations is in these PRs.

The lifecycle extraction adds full-SDU mbuf header headroom. Its 16 native
tests pass normally and with AddressSanitizer/UBSan; actual CoC-enabled builds
pass on S3 with IDF 5.4.3/5.5.1, each inline/deferred, and 6.1. The benchmark
adds a codec test (17 C++ tests total) and nine Python tests; both target firmware
builds pass. This extraction checkpoint is distinct from the earlier experimental
performance matrices.

Final clean-image HIL: 10 successful captures, 42 measured runs, 608 echo/request
exchanges and 128 streaming DATA frames, across 16 completed connections. Apple
Central checks run on both chips; the mixed ESP pair passes bounded 4-KiB
echo/request traffic in both GAP-role directions. Preliminary S3 captures were
retained separately and repeated against the final archived ELF; only final
clean-image captures contribute to these totals.

A full 5000-byte SDU (4996-byte payload plus framing) fails on S3→S31 in the
default pool profile. Temporary error-only instrumentation reports
`TX failure rc=6 offset=0 length=5000 total=5000` (ENOMEM). It does not establish
which pool first limits this geometry. The diagnostic patch/ELF and both failed
captures are retained separately; the patch is excluded from the PRs and final
clean images. This maximum-SDU case is explicitly not an acceptance pass or
claimed fixed. Apple uses MTU 1251, so a 4-KiB application frame is segmented
into smaller SDUs and passed its checks.

Evidence is in `~/Dokumente/late/measurements/ble/2026-10-16-upstream-prs` on
the Linux host, including clean firmware, the temporary diagnostic variant,
source archives, native/build/HIL logs and checksums. GitHub CI is tracked
separately from the completed local tests; the live PR checks are authoritative.
The S31 is restored to LIS2DW12 hwtest afterward, with physical tap acceptance
still deferred to the operator.

## Experimental resource and iPhone checkpoint — 16:53 CEST

Performance work is retained on `experiment/l2cap-performance`; no additional
upstream PR was created. Its tested runtime source is `957015c`. The isolated
resource investigation is on `experiment/l2cap-resource-limits` (`0d34dba`),
based on the submitted benchmark. Product pins and defaults remain unchanged.
The experimental branch also carries the submitted full-SDU header headroom,
ownership tests and Release-mode test scheduling correction. Its 28 native
tests pass in Release and with AddressSanitizer/UBSan; both target images build
against the unchanged pinned IDF 6.1 revision.

### Maximum-SDU attribution

An allocation-failure-only S3 probe identifies `msys_1`, 24 blocks × 256 bytes,
as the exhausted pool for the first 5000-byte SDU. MSYS2 remains free. Raising
only MSYS2 to 48 blocks still fails; raising only MSYS1 to 48 passes the maximum
request/small-reply control. Keeping 24 MSYS1 blocks and changing their size to
288 bytes also passes, adding only 768 bytes of backing storage. Returning to
the baseline reproduces the original failure. The 288-byte profile also passes
without the allocation probe: three connections, 192 measured echo exchanges
plus 12 warmups, split between 4-KiB and maximum-size payloads. Earlier diagnostic
and inverse-GAP-role checks are retained separately.

This profile is specific to the unchanged peer's 248-byte MPS. The SDK also
derives the local advertised MPS from MSYS1 block size (248 → 280 here), so it
is not a universal recommendation for arbitrary or symmetrically modified
peers. The native packet allocator starts in MSYS1 and extends from the same
pool rather than falling back to MSYS2. See the resource branch's benchmark
README for the headroom calculation, configuration fragments and probe contract.

Resource evidence contains ten captures: six successful captures with 364
measured exchanges, and four retained resource-failure reproductions. All
referenced application ELF hashes resolve to archived images. The S31's
controller-owned `r_os_*` backend does not expose the same per-pool registry;
the probe declares this limitation rather than treating missing events as a
resource-pressure pass.

### iPhone acceptance

The new foreground iOS app and the macOS command use the same extracted
`L2CAPBenchApple` CoreBluetooth reflector and `L2CAPBenchKit` codecs. The Apple
source is `5643062` on `experiment/ble-iphone-peer` in core-bluetooth-tool.
Unsigned and signed iOS Release builds pass; all six codec tests pass on macOS.
The app is installed on an iPhone 16 Pro, iOS 27.0.1 (24A446). It records complete
peer logs outside Git, limits its on-screen log, and stops when backgrounded.

The operator reports approximately 0.5 m distance with no wall or glass;
positions were to remain fixed. The iPhone is Central and each ESP is Peripheral.
All 336 negotiated and before/after-run parameter records in the completed
matrix match 15 ms, 2M/2M PHY, 251/251-byte DLE and SDU MTU 1251.

With the clean baseline, both chips pass echo, 8→4096 requests and RX-only
streaming, but fail TX-only streaming and duplex. Returning to window-off
error-logging images reproduces `ENOMEM` on both chips:

```text
S3:  TX failure rc=6 offset=2502 length=1251 total=4100
     pool=msys_1 blocks=24 block_size=256 free_after_failure=0
S31: TX failure rc=6 offset=1251 length=1251 total=4100
     exact native pool not observable through the S3 allocator hook
```

With HCI window 8, both chips pass all three 128-frame repeats in each streaming
pattern, echo/request controls and 25 reconnect cycles each. Alignment, deferred
callbacks and TX diagnostics remain off. These results do not include the
288-byte pool profile; both candidate configurations retain the baseline pools.

| iPhone peer, ESP DUT | S3 | S31 |
|---|---:|---:|
| 64-byte echo median RTT, ms | 29.857 | 29.854 |
| 4096-byte echo median RTT, ms | 164.236 | 178.899 |
| 8→4096 request median RTT, ms | 90.801 | 90.344 |
| ESP→iPhone streaming, KiB/s | 50.03 | 51.77 |
| iPhone→ESP streaming, KiB/s | 55.22 | 58.54 |
| Duplex summed goodput, KiB/s | 61.27 | 60.46 |

Rows are medians across three run medians/rates. Streaming rows use wall goodput
including control round trips, and duplex sums both directions. The iPhone
streaming results are close, with different directions favoring different chips;
large echo latency favors this S3. One board per chip and an uncontrolled radio
environment do not establish a general silicon winner. The earlier Mac setup
used a different distance/obstruction and is a separate benchmark.

After excluding five initial reconnect cycles, S3 free heap is constant at
213888 bytes. S31 free heap stays within 317860–317904 bytes. Both keep ten tasks
and a constant largest internal block. This is bounded short-run evidence,
not day-scale leak acceptance.

The completed matrix contains 24 captures: 18 successful and six retained
baseline/window-off resource-failure reproductions. Successful captures contain
1152 measured echo/request exchanges, 144 warmups, 4128 validated streaming
DATA frames and 66 completed connections. Each captured firmware identity maps
to an archived ELF. Raw peer/device identities and signed apps stay outside Git.

### Completed hour-scale test — 17:50 CEST

The independent `ble-m16-s3-soak-20261016.service` passed from 16:49:51 to
17:50:04 CEST: 3613.07 seconds elapsed, exit status 0. The S3 and iPhone kept
one connection for all 27 duplex batches, each with 1024 DATA frames in each
direction. These are repeated batches with control/sample-dump gaps, not a
claim of an uninterrupted hour of DATA transmission.

All 55,296 DATA frames passed sequence, per-run token, length and pattern
validation, with zero receiver errors on either peer, zero callback faults,
and matching receiver receipts and FINISH confirmation for every batch. The
archive contains every TX/RX sample in sequence. There were 216 MiB of DATA
bodies including eight metadata bytes per frame (215.578125 MiB of useful
pattern bytes), excluding framing. All 55 negotiated/before/after parameter
records retain 15 ms, 2M/2M PHY, 251/251-byte DLE and MTU 1251.

Median per-batch summed useful wall goodput is 61.60 KiB/s, with a range of
55.93–64.03 KiB/s; aggregate useful goodput over the 3589.44 measured seconds
is 61.50 KiB/s. The first five batches have a median of 62.52 KiB/s, the last
five 60.73 KiB/s. This variation does not establish a monotonic degradation
or its cause in an uncontrolled radio environment.

Every one of the 27 after-run heap samples reports 213908 free bytes,
213712 internal free bytes, a 143360-byte largest internal block, and ten
tasks. The low-water mark settles at 188068 bytes after the third batch.
This is no visible progressive heap loss during this test; it does not prove
all allocations are reclaimed across arbitrary reconnect or error paths.

The supervisor checked the exact archived firmware hash and full negotiated
parameter tuple, stopped on first failure without retry, and had a separate
600-second no-serial-evidence watchdog. The final disconnect succeeded. The
peer logs were exported and only the bench app's iPhone process was terminated
to release its auto-lock override. Durable status is `soak-state.json` in the
private iPhone evidence directory. This soak is additional to, and excluded
from, the 24-capture matrix totals. There is no S31 hour-scale result yet.

The S31 is restored to the LIS2DW12 hwtest image; flash hashes and console help
were checked. The serial helper must answer the terminal cursor-position query
before issuing a command. No physical tap test was repeated. S3 retains window-8
benchmark firmware; the completed service has released its serial port.

Private evidence directories on the Linux host:
`~/Dokumente/late/measurements/ble/2026-10-16-resource-limits` and
`~/Dokumente/late/measurements/ble/2026-10-16-iphone`. Remaining gates are S31
hour-scale and day-scale endurance, multiple connections/channels, RF-loss and
controller-reset cases, and actual production ECOS/CAN-answer lifecycle checks.
