# L2CAP CoC Performance Measurement Plan

Status: draft for discussion. Nothing in here is implemented yet.

## 1. Goal

Measure and compare L2CAP connection-oriented channel (CoC) performance of
esp-nimble-cpp on the ESP32-S31 (primary) and ESP32-S3 (regression guard), with
these peers in order of priority:

1. iPhone (CoreBluetooth), because of ECOS.
2. ESP32 peer (ESP <-> ESP), because it must work well too and because it is the
   only fully scriptable pair.
3. macOS (CoreBluetooth CLI), as a convenient but not equivalent stand-in for the
   iPhone.

The result must let us answer, with numbers and repeatable procedure:

- Does a library change (TX SDU alignment, deferred callbacks, buffer pools,
  serialization, teardown work) help, hurt or do nothing, per peer and per target?
- Did a change regress the S3 while improving the S31?
- Where does the time go for the ECOS workload: small request, large response?

## 2. Existing assets and what they are good for

| Asset | Location | Useful for | Limits |
| --- | --- | --- | --- |
| Ping-pong examples (server + client) | `examples/L2CAP/` | ESP <-> ESP and ESP <-> BLE-Testing-iOS echo RTT and goodput | Human-readable printf only, one traffic pattern, S3 sdkconfig, server assumes one SDU equals one frame |
| BLE-Testing-iOS | `~/Documents/late/BLE-Testing-iOS` | iPhone echo peer, same frame format as the examples | iOS 18.2 app without git, no export, metrics only on screen, needs manual target name change |
| core-bluetooth-tool | `~/Documents/late/core-bluetooth-tool` | macOS streaming client and sink | One-way streaming only, never drains replies, no JSON |
| ECOS diagnostic benchmark | `tpe/adapter/ecos-master/testing/ble_diagnostics/` plus `DiagnosticBenchmark.swift` | The real workload on the real S31 firmware: 8/32-byte request, 32..4096-byte response, JSON output with firmware dispatch and prepare times | Needs the full product firmware, S31 debug build only, one peer type per run, cannot A/B library Kconfig knobs quickly, no ESP <-> ESP |
| TX diagnostics (`CONFIG_NIMBLE_CPP_L2CAP_TX_DIAGNOSTICS`) | `src/NimBLEL2CAPChannel.cpp` | Credits, stalls, host submission timing per 32 SDUs | Log lines, not yet machine-parsed |
| ESPenlaub hwtest | `~/Documents/late/ESPenlaub/examples/hwtest` | Console command pattern, IDF console wrapper | No BLE at all, S3-only configs, 8 MB flash, 30+ peripheral drivers that must compile on a preview target, no automation |

Measurements already recorded for the S31 in the ECOS board README
(`boards/s31coreboard/README.md`): roughly 45 KiB/s with 128-byte MSYS-1 blocks
and about 50 KiB/s with 256-byte blocks, 4096-byte ping around 161 ms, 15 ms
interval after one retry, 2M PHY, 251-byte DLE. These are the numbers to beat.

## 3. Decision: build on hwtest or standalone?

**Standalone, inside esp-nimble-cpp.** Reasons:

- hwtest contributes nothing we need. It has no BLE code, no stats helpers, no
  machine-readable output and no automated flow. We would borrow only the
  `esp_console` wrapper, which is a few dozen lines.
- hwtest costs a lot. Every peripheral driver has to compile for the S31
  preview target, the partition table needs 8 MB flash, and full rebuilds are
  heavy. A measurement firmware should build in under a minute and flash on a
  bare devkit.
- Measurement code for the library belongs next to the library. It is also the
  natural home for an upstream-able example later, which hwtest never will be.
- The ECOS benchmark stays the acceptance test for the product. The library
  bench is for fast iteration and regression. Both are needed; neither replaces
  the other.

What we do take from ESPenlaub: the console registration style, nothing else.
If we later want a TCP console for unattended runs, `TCPService` is there.

## 4. Architecture

### 4.1 Roles: orchestrator and reflector

The device under test (DUT) orchestrates every run. Peers are reflectors that
follow a small control protocol carried over the L2CAP channel itself. All
results are collected on the DUT and emitted over serial. Consequences:

- The iPhone app needs no export, no UI for parameters and no scripting. It
  only has to reflect correctly.
- macOS CLI, iOS app and ESP peer implement the same reflector; the DUT firmware
  does not care which one it talks to.
- One host-side runner drives the DUT over serial for all peers.

The reflector also runs on the DUT side, so ESP <-> ESP works with the same
firmware in both roles, selected at runtime.

### 4.2 Wire protocol

Stream-based framing on all sides. CoreBluetooth exposes the channel as a byte
stream and does not promise that one write becomes one SDU, so the existing
"one SDU equals one frame" assumption in the server example must go.

Frame: `[magic u8 = 0xB7][type u8][len u16 BE][payload]`, payload up to 4996
bytes, so the largest frame fits the 5000-byte CoreBluetooth and ECOS wire
limit.

Types:

| Type | Direction | Purpose |
| --- | --- | --- |
| `0x01 HELLO` | DUT -> peer | Protocol version, DUT identity (target, firmware hash). Peer answers with `0x81` carrying its identity (OS and version, device model). |
| `0x02 CONFIG` | DUT -> peer | Pattern, request size, response size, count. Peer answers `0x82` ack. |
| `0x03 PING` | DUT -> peer | `[seq u32][token u32][pattern bytes]`. Peer answers `0x83` with the same seq and token and a response of the configured size. Covers echo (response size equals request size) and asymmetric request/response. |
| `0x04 STREAM` | DUT -> peer | `[seq u32][pattern]`. Peer does not answer per frame. |
| `0x05 STREAM_END` | DUT -> peer | Peer answers `0x85` with received frame count, byte count, first and last receive timestamps in its own clock. |
| `0x06 SINK` | DUT -> peer | Asks the peer to stream `count` frames of `size` to the DUT. Peer streams `0x04` frames and finishes with `0x05`. |
| `0x7F ABORT` | either | Stop everything. |

Response frames carry the peer's receive-to-send turnaround in microseconds,
so the DUT can subtract reflector processing from RTT. Peer clocks are never
compared with the DUT clock; only durations measured on one side are used.

### 4.3 Traffic patterns

| Pattern | What it measures | Maps to |
| --- | --- | --- |
| `echo` | RTT and goodput, one request in flight, sizes 64..4096 | Existing ping-pong examples, BLE-Testing-iOS |
| `request` | Asymmetric: 8 or 32-byte request, 32..4096-byte response | ECOS diagnostics |
| `stream-tx` | DUT sends as fast as credits allow | ECOS firmware update download direction from the phone's point of view, flow control, stalls |
| `stream-rx` | Peer sends as fast as it can | Phone uploads, RX buffer pools, deferred callback queue |
| `duplex` | `stream-tx` and `stream-rx` at once | Worst case for the write mutex and host task |
| `lifecycle` | Connect, run a short echo, disconnect, repeat N times | Teardown and reconnect bugs, heap leaks |

### 4.4 Metrics per run

- RTT: median, p95, min, max, mean, in microseconds from `esp_timer`.
- Goodput (payload bytes per second) and link throughput (both directions).
- Time to first byte of the response, where the pattern has a response.
- TX diagnostics when enabled: stall count, stall time, credit min/max, host
  queue delay, `ble_l2cap_send` duration. The existing `BLETx` log lines are
  parsed by the runner; no change to the diagnostics format is needed at first.
- Heap: free at start, minimum ever, free at end.
- Errors: sequence, pattern, length, timeouts, disconnects, write failures.
- Negotiated parameters: SDU MTU, local MPS, peer MPS when available, connection
  interval, PHY, DLE. These are read from the existing channel and GAP logs.

### 4.5 Output format

The DUT prints one JSON object per line prefixed with `BENCH ` for every event
the runner needs: `hello`, `connected`, `params`, `run-start`, `run-result`,
`txstats`, `heap`, `run-end`, `error`. Everything else stays human-readable.
This keeps the serial console usable by hand and trivially parseable.

The runner stores each run as `results/<date>-<dut>-<peer>-<label>.json` with:

- DUT: target, esp-nimble-cpp commit, sdkconfig hash and the relevant
  `CONFIG_` values (buffer pools, deferred callbacks, alignment, diagnostics,
  CPU frequency, debug or release).
- Peer: identity from `HELLO`, plus free-text notes such as "iPhone 15 Pro, iOS
  26.1, 1 m, line of sight".
- Every sample, not only aggregates, so later questions can be answered without
  re-running.

A `compare` command renders a markdown table of two or more result files with
medians, p95 and percent change, which is what ends up in commit messages and
in the ECOS board README.

## 5. Components to build

### 5.1 DUT firmware: `examples/L2CAP/L2CAP_Bench`

- One IDF project, builds for `esp32s3` and `esp32s31` with per-target
  `sdkconfig.defaults.<target>`. Possibly `esp32c6` later.
- Runtime role selection through the IDF console: `role server|client`,
  `peer <name>`, `run <pattern> [options]`, `params interval=<ms> phy=<1m|2m>`,
  `status`, `disconnect`, `reboot`. No NVS state, so a reboot is a clean slate.
- Reflector implementation shared with the client in one C++ class, so ESP <->
  ESP uses the same code in both directions.
- GATT PSM discovery as in the existing examples, same service UUIDs, so
  BLE-Testing-iOS keeps working until it is converted.
- Connection parameter handling: request interval and PHY, log what the peer
  accepted, retry on transaction collision as the ECOS firmware already does.
- Reuse the existing ping-pong examples' stats code where it fits; the examples
  themselves stay as they are for now and are retired once the bench covers
  them.

### 5.2 Host runner: `tools/l2cap-bench/` (Python 3, stdlib plus pyserial)

- `bench.py run --port <dev> --peer <name> --pattern echo --sizes 64,256,1024,4096 --count 64 --label align-on`
- `bench.py matrix --config matrix.toml` to run a list of patterns and sizes in
  one session and store one file.
- `bench.py compare a.json b.json [c.json]` for the markdown table.
- `bench.py esp-pair --dut <port> --peer <port>` for ESP <-> ESP, where the
  runner also puts the second board into reflector role.
- Captures the full serial log next to the JSON for later forensics.

### 5.3 macOS reflector: `core-bluetooth-tool l2cap-reflect`

- New command next to `l2cap-client` and `l2cap-server`, publishes the GATT
  service with the PSM characteristic and reflects per section 4.2.
- Also gets JSON output of its own view (bytes, frames, turnaround), useful when
  a run looks odd on the DUT side.

### 5.4 iOS reflector: BLE-Testing-iOS

- Add a reflector mode. The reflector code should live in a small SwiftPM
  package (`L2CAPBenchKit` or similar, in core-bluetooth-tool's repo) so the
  CLI and the app share it. BLE-Testing-iOS has no git yet; put it under git
  first.
- Keep the existing echo UI for a while; it is useful for a quick look.
- Show live counters only. Everything that matters is collected on the DUT.

### 5.5 Library instrumentation

- `logTxStats` output stays, but gets a parse-stable format. Consider emitting
  it as the same `BENCH ` JSON line when the bench firmware is the consumer.
- No other library changes for measurement. Anything else is a candidate under
  test, not infrastructure.

## 6. Measurement matrix

Targets: S31 (every run), S3 (every run that changes default behavior).

Peers: iPhone, ESP32-S3 reflector, macOS. The macOS numbers are recorded but
decisions about ECOS are made on iPhone numbers.

Knobs, one at a time against a fixed baseline, three repeats each, same
placement and distance, WiFi and Ethernet off on the DUT:

| Knob | Values | Hypothesis |
| --- | --- | --- |
| `ALIGN_TX_SDUS` | off, on | One PDU and one credit less per full SDU; visible in `stream-tx` and large `request` responses to iPhone (MTU equals MPS 1251) |
| `DEFERRED_READ_CALLBACKS` | off, on | ECOS data says no peak gain; check `stream-rx` and `duplex` for latency and drops |
| `MSYS_1_BLOCK_SIZE` | 128, 256, 512 | Local RX MPS 120, 248, 504; affects `stream-rx` fragmentation, already showed 11 percent in ECOS |
| `MSYS_1/2_BLOCK_COUNT`, `ACL_BUF_COUNT` | current, plus 50 percent | Stall count under `stream-tx` |
| `L2CAP_SDU_BUFFER_COUNT` | 2, 5, 8 | `duplex` and `stream-rx` drops |
| Connection interval | 7.5, 15, 30 ms | Peer accepts or not; RTT floor |
| PHY | 1M, 2M | Sanity check that 2M is negotiated and matters |
| Build | debug, release, with and without TX diagnostics | Cost of logging itself |
| CPU frequency | default, max | Whether the host task is CPU bound on the S31 |

Expected first deliverable from the matrix: a baseline table for S31 and S3
with the iPhone and with an ESP peer, before any knob is touched.

## 7. Phases

1. **Firmware and runner skeleton, ESP <-> ESP.** Bench firmware with `echo`
   and `request` patterns, reflector role, `BENCH ` JSON lines, runner with
   `run` and `compare`. Verified on two S3 boards, then S31 against S3.
   This is the point where most of the design risk is retired.
2. **macOS reflector.** `l2cap-reflect` in core-bluetooth-tool. First numbers
   against a CoreBluetooth stack.
3. **Streaming and duplex patterns, lifecycle pattern.** Plus heap and TX
   diagnostics capture in the runner.
4. **iPhone reflector.** Shared Swift package, BLE-Testing-iOS under git with
   reflector mode. Baseline matrix for S31 and S3 against the iPhone.
5. **Knob matrix.** Run section 6, record in `results/`, write findings into
   this file and into the ECOS board README where they affect the product.
6. **Acceptance.** Bump the esp-nimble-cpp submodule in ECOS (currently at
   `3f46607`, before the upstream merge) and re-run
   `testing/ble_diagnostics/benchmark.py` with the chosen configuration.

Phases 1 and 3 are pure esp-nimble-cpp work. Phases 2 and 4 touch other repos.

## 8. Open questions

- **Which iPhone and iOS version is the reference?** ECOS customers decide;
  note the device in every result file.
- **Is 2M PHY negotiated with the iPhone on the S31?** The ECOS log says yes.
  The bench must log it per run rather than assume it.
- **Peer MPS from the public API.** Not available; the alignment knob needs the
  private header. Decide later whether that is acceptable for production or
  whether to ask upstream for `peer_coc_mps` in `ble_l2cap_chan_info`.
- **Hardware at hand.** No ESP board was attached while writing this. We need
  at least two S3 boards and one S31 board on USB at the same time for ESP <->
  ESP and the S31 run, and the runner must address them by port.
- **S31 as IDF preview target.** IDF v6.1 at `~/Documents/late/misc/esp-idf`
  builds it today. Pin the IDF commit in the result metadata, because the S31
  controller library can change between IDF updates and shift all numbers.
- **Should the bench firmware eventually replace the two ping-pong examples
  upstream?** Probably yes, as a single example with a role switch. Not a
  question for now.
