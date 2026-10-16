# L2CAP benchmark: first baseline

Standalone IDF firmware for ESP32-S3 and ESP32-S31. It supports `echo`,
asymmetric `request`, `stream-tx`, `stream-rx` and `duplex`, as DUT or ESP
reflector. The serial runner automates reconnect cycles; iPhone and product
acceptance remain later steps in `../../../PERF_MEASUREMENT_PLAN.md`.

Use the same IDF checkout and sdkconfig for every comparison. Wi-Fi and Ethernet
are never initialized by this firmware. CPU frequency, buffer counts, ELF hash,
library/IDF commits and sdkconfig hash appear in the `hello` JSON event.

```sh
source ~/Documents/late/misc/esp-idf/export.sh
IDF_TARGET=esp32s3 idf.py -B build-esp32s3 \
  -D SDKCONFIG=build-esp32s3/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32s3' build
IDF_TARGET=esp32s3 idf.py -B build-esp32s3 \
  -D SDKCONFIG=build-esp32s3/sdkconfig -p /dev/serial/by-id/<board> flash
```

Use `esp32s31`, `idf.py --preview` and a separate `build-esp32s31` directory for S31. The console is
USB Serial/JTAG, 115200 baud. Commands are LF-terminated, without terminal cursor
negotiation. `status` reports firmware identity and actual negotiated parameters.

```text
params 15 2
connect auto
run echo 64 64 64
run request 8 4096 64
disconnect
```

This example makes the ESP the GAP Central. For ECOS product roles, start the
macOS reflector with `--connect L2CAP-Bench-ESP`, then use the ESP as Peripheral:

```text
params 15 2
listen
await 30
run echo 64 64 64
run request 8 4096 64
disconnect
```

The ESP orchestrates both variants. GAP role and application role are independent:
`listen` accepts a channel and runs the DUT, whereas `role server` accepts a
channel and reflects requests. `role client` returns to Central/DUT mode without
a reboot. `await` waits for the channel and performs HELLO. `disconnect` stops
advertising and closes both CoC and GAP, preventing automatic reconnection during
the next setup. GAP role is included in JSON; accepted parameters are captured
before every run.

Link setup retries an unaccepted interval after 1 s, at most three times, with
request return codes and asynchronous GAP status in `interval-update` events.
This handles the S31's known initial transaction collision without blocking a
GAP callback. The peer still chooses the accepted parameters; the runner prints
them and `--require-params` rejects mismatches for controlled A/B runs.

`params <interval_ms> <1|2>` selects the requested connection interval/PHY before
connecting. The `params` event reports the accepted interval and actual TX/RX PHY;
`tx_octets`/`rx_octets` report an observed DLE change, when the controller emits
one. `requested_tx_octets` is a request, not proof of negotiation. Public
channel APIs currently provide the negotiated MTU through the connect callback,
but not peer MPS. A failed request ends the run and disconnects before retrying,
so a late response cannot contaminate the next run.

`connect auto` requires exactly one peer advertising the benchmark service.
Use `connect <name-or-BLE-address>` when several peers are present. macOS may
advertise the service without a local name; select its BLE address in that case.

For an ESP peer, flash the same image to a second board, then run `role server`.
It advertises `L2CAP-Bench-ESP`. A reboot also restores the default client role.
Responses are written on a worker task, never from a NimBLE read callback.

On mond, `core-bluetooth-tool` provides `l2cap-reflect`. Launch it with
`bash tools/run-l2cap-reflector-app.sh` from that repository so LaunchServices
can show Bluetooth consent in the logged-in GUI session. The app logs to
`/tmp/l2cap-reflect-app.log`. Its shared `L2CAPBenchKit` library is intended for
the future iPhone reflector as well.

The JSON evidence uses blocking USB-driver writes outside the timed loop.
The runner checks both sample count and sequence, including warmup samples,
so incomplete console captures fail rather than becoming comparison data.

The runner is `../../../tools/l2cap-bench/bench.py`; see its README for repeated
runs, raw data and comparison output.

## Base messages (current HELLO version 2)

All multibyte fields are big endian, except the GATT PSM characteristic, which is
little endian. The stream parser accepts split/coalesced frames; it never assumes
an L2CAP SDU or CoreBluetooth stream read is one application frame.

Frame: `B7 type length:u16 payload`, maximum payload 4996 bytes (5000 total).

| Type | Payload |
|---|---|
| `01 HELLO` / `81 reply` | Version byte `02`, followed by a UTF-8 identity string. |
| `02 CONFIG` | Pattern (`01` echo, `02` request), request size `u16`, response size `u16`, count `u32`. |
| `82 reply` | One status byte, `00` for accepted. |
| `03 PING` | Sequence `u32`, token `u32`, deterministic pattern bytes. |
| `83 reply` | Same sequence/token, reflector turnaround `u32` microseconds, deterministic pattern bytes. |
| `7F ABORT` | Optional UTF-8 error description. |

Sizes include the message payload, excluding the four-byte frame header. Requests
must be at least 8 bytes; responses at least 12. Echo uses equal sizes. Sequence
starts at zero per CONFIG. The token stays fixed for a run. Pattern byte `i` is
`(sequence + token + i) & 0xFF`, starting at offset 8 in PING and 12 in its reply.
Both sides verify lengths, sequence and data; a stream error rejects the channel.

## Timing interpretation

RTT starts before the DUT's L2CAP write and ends when its parser has the complete
reply. `first_callback_us` measures delivery of the first response SDU to the
application callback, not the physical first byte over the air. `tx_submit_us`
is the blocking write duration; it does not prove remote consumption.

Reflector turnaround runs from complete-request parsing to the first response
write attempt (macOS) or L2CAP write submission (ESP). It includes application
queueing and response preparation. Adjusted RTT subtracts that duration from RTT;
it remains a round trip and cannot identify independent one-way delays. It does
not remove CoreBluetooth/controller buffering after the peer's write call.

Goodput counts deterministic application bytes, excluding sequence, token,
turnaround and framing. `framed_bytes_per_s` includes our frame/metadata bytes;
it is not BLE physical throughput. All samples stay in memory during the timed
run and are printed afterwards. Single-request-at-a-time goodput is separate
from the streaming measurements below. Minimum heap is the lifetime minimum.

## Streaming and lifecycle (bench protocol version 2)

The current firmware and reflector use HELLO version 2. Use matching peers;
version-1 binaries from the first baseline remain in its private artifact set.
The frame header and echo/request layouts are unchanged. New CONFIG patterns:
`03 stream-tx`, `04 stream-rx`, `05 duplex`, with count limited to 1..4096.
Sizes are relative to the ESP DUT: TX uses request size, RX uses response size.

```text
run stream-tx 4096 4096 128
run stream-rx 4096 4096 128
run duplex 4096 4096 128
```

The stream extension is:

| Type | Payload / purpose |
|---|---|
| `06 START` / `86 ready` | Token `u32`, echoed only after the receiver is armed. Peer TX starts after READY. |
| `04 DATA` | Sequence `u32`, token `u32`, pattern bytes as in PING. No per-frame ACK. |
| `05 END` | Token `u32`, announced count `u32`. |
| `85 receipt` | Token `u32`, received frames `u32`, payload bytes `u64`, first/last complete-frame receive times `u64` each, validation errors `u32` (36 bytes total). |
| `07 FINISH` / `87 finished` | Token `u32`. Peer confirms only after every enabled direction has ended and every sent stream has a matching receipt. |

All integers are big endian. Both peers validate every DATA length, sequence,
token and pattern, then validate END and the final receipt/fence. A direction
that is disabled sends no DATA/END and needs no receipt. A token is new per run.
Duplicate END/receipt, data after END, gaps, altered data and stale tokens fail.

ESP DATA is validated in the read callback, with counters protected by a mutex;
blocking writes remain on the DUT or reflector worker. Control messages alone
use the bounded message queue. This prevents a busy stream writer from starving
its own receiver during duplex. The Mac reads and writes independently on its
run loop, preparing at most one generated DATA frame beyond pending controls.

Streaming results contain per-frame DUT TX submission times and DUT RX arrival
times, emitted after timing stops. `goodput_bytes_per_s` counts useful pattern
bytes in both directions over START through confirmed FINISH, including control
round trips. `*_arrival_goodput_bytes_per_s` measures `(count - 1) * (size - 8)`
over first-to-last arrival on that receiver's own clock. It excludes the first
frame and is absent when there is no measurable arrival interval. Neither
metric is physical BLE throughput. Summary bytes include the eight-byte stream
metadata; goodput excludes it. RX timestamps can repeat when several frames
complete in the same callback.

The benchmark's ten-second no-progress watchdog is refreshed by valid received
frames. It detects a stuck test; it is unrelated to vehicle P2/P2* or ECOS
protocol response timing. The library's existing bounded credit/write waits
still apply.

`--cycles N` in the serial runner repeats connect/run/disconnect. After teardown
the ESP allows 100 ms for FreeRTOS task reclamation, then reports free/internal
heap, largest internal block and task count. The host retains every cycle and
summarizes its trend, excluding up to five initial cycles. These are measured
trends, not a hard pass/fail leak threshold or proof of long-term stability.

### Experimental HCI backlog control

Unpaced 4-KiB TX/duplex exposed `BLE_HS_ENOMEM` with the baseline pools. Peer CoC
credits alone do not bound the native HCI ACL backlog; the pinned NimBLE code
can exhaust host mbufs while copying a later SDU, after earlier bytes have been
submitted. Such an SDU must never be blindly retried.

`sdkconfig.backpressure` enables `CONFIG_NIMBLE_CPP_L2CAP_HCI_BACKPRESSURE`.
It checks the native HCI queue and outstanding ACL packet count on the host
before SDU submission. The configurable `HCI_TX_WINDOW` defaults to 8; this is
a threshold for starting another SDU, not a hard total packet cap because one
SDU can add several ACL packets. If backed up or at that threshold,
that SDU is not submitted; a worker waits one RTOS tick and checks again, within
the existing send deadline. The untouched mbuf is retained. After a blocked
host preflight, the worker waits one tick and queues another host check,
without allocating or copying again. The host event and completion-semaphore
cost remain. The worker must not take the native global mutex: its lock function
waits indefinitely and could defeat the existing send deadline. One tick is
1 ms at 1000 Hz and 10 ms at 100 Hz; when ready there is no deliberate wait.
There is no idle poller.

Host-callback writes bypass the preflight and retain native send semantics,
including native resource/credit failures. Sustained TX must use a worker.
Connection-time pool warnings are advisory: they model one full SDU's host
mbufs plus a simple outstanding-packet allowance, not sufficient RX/ATT,
multi-channel or controller capacity. The S31 explicitly reports its opaque
controller-owned backend instead. Window 8 was measured with MSYS1 24x256,
MSYS2 24x320 and ACL count 24; 12-block pools are not validated. Too small a
window can leave the controller idle, and too large a window can exhaust pools.

Actual send failures still abort the channel and are never retried. The option
uses the pinned IDF private connection layout and stays off by default, pending
broader tests and production acceptance. The earlier candidate passed the
iPhone matrix and S3 hour run; the review follow-up needs new hardware runs.
The initial queue-only candidate worked on S3 but still hit ENOMEM on S31;
the separate outstanding window addresses that additional path. It is not a general guarantee for
arbitrary pool sizes or concurrent channels.

Run `idf.py reconfigure` when a library update adds Kconfig options to an
existing build directory.

Add `sdkconfig.txstats` to compile TX diagnostics and enable the BLETx tag at
INFO while other logs stay at ERROR. The runner parses geometry, credits,
queue/send timing and credit-stall records into `txstats` events. Windows reset
per 32 SDUs or at disconnect, so they may straddle benchmark runs; use one case
per connection for attribution. Diagnostic builds are separate from timing
baselines. Both raw lines and parsed records are retained.

Backpressure diagnostics add per-connection cumulative attempted-SDU, wait and
host-bypass counts plus maximum waits for one SDU. These counters do not reset
with the 32-SDU timing windows. A wait is one readiness-poll delay, not a native
send retry. Compare start/end snapshots; final disconnect prints the totals.
They do not measure host-task CPU utilization or RF packet counts.

### Experimental TX alignment contract

`sdkconfig.align` aligns TX chunk sizes to peer MPS including the SDU length.
It preserves application bytes but changes SDU boundaries: at MTU/MPS 1251,
1250- and 1251-byte writes become 1249+1 and 1249+2. Peers that treat SDUs as
messages (such as a SOCK_SEQPACKET consumer) cannot assume one write is one
SDU. onConnect still reports negotiated MTU. Alignment is only suitable for
a byte-stream/framed-stream consumer and can add SDUs and host round trips.
The existing macOS echo A/B suggests a benefit; streaming/iPhone A/B, CPU and
observed PDU/LL counts remain unmeasured. See the review follow-up in
`PERF_STREAMING_2026-10-16.md` for the measurement gates.
