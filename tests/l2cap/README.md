# L2CAP host tests

These tests compile the production `NimBLEL2CAPChannel.cpp` with small replacements for NimBLE and FreeRTOS. Separate host, callback and writer threads exercise send errors, buffer ownership, early credit notifications, disconnects, whole-message serialization, timeout cleanup, reconnect and callback queue overflow.

```sh
make test-l2cap
make test-l2cap SANITIZER=address
make test-l2cap SANITIZER=thread
```

Clang, CMake and a C++17 compiler are required. Address builds also enable undefined-behavior checks. The deferred-callback configuration and an inline-callback build are both compiled.

The fake radio does not validate ESP32 scheduling, RF behavior, controller credits or memory-pool sizing. Real-device transfer and disconnect tests remain necessary. Timeouts deliberately exercise the production two-second deadline. The lifecycle cases repeatedly create and destroy channels and verify that the shared callback task and queue are released after the last channel.

## ESP-IDF TX diagnostics

`CONFIG_NIMBLE_CPP_L2CAP_TX_DIAGNOSTICS` (default off) reads private channel
fields from the pinned ESP-IDF NimBLE host, on the host task only. At connection
it logs peer TX MPS, local RX MPS, both SDU MTUs and initial TX credits. Every 32
submitted SDUs and at disconnect it reports bytes, sampled credit min/max,
ESTALLED counts, send errors, host queue delay, ble_l2cap_send duration and time
until TX_UNSTALLED (or an aborted stall at disconnect). A batch can span response
sizes. Credit samples are before/after submissions, not a trace of every update.
Early synchronous completion may have zero measured stall duration.

These are host submission timings, not controller completion or RF delivery.
Logging itself adds overhead. The option requires the pinned IDF private header
layout; it is intended for controlled debug comparisons, not production builds.

`CONFIG_NIMBLE_CPP_L2CAP_ALIGN_TX_SDUS` (default off) is an A/B experiment.
It uses the largest TX SDU width no greater than the negotiated width whose
size plus the two SDU-length bytes is a multiple of the peer MPS. A smaller
SDU that already fits in one peer PDU is unchanged. For macOS MTU/MPS 1251 this
changes TX chunks from 1251 to 1249 bytes; for MTU 672/MPS 23 it chooses 665.
The byte stream and negotiated MTU remain unchanged. This can reduce credit-based
PDUs, but a latency/throughput benefit needs measurement on each peer. BLETx logs
the effective width. Geometry tests cover the entire uint16 MTU range against
representative MPS values, edge cases and the 4096-byte response decomposition.
