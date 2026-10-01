# L2CAP host tests

These tests compile the production `NimBLEL2CAPChannel.cpp` with small replacements for NimBLE and FreeRTOS. Separate host, callback and writer threads exercise send errors, buffer ownership, early credit notifications, disconnects, whole-message serialization, timeout cleanup, reconnect and callback queue overflow.

```sh
make test-l2cap
make test-l2cap SANITIZER=address
make test-l2cap SANITIZER=thread
```

Clang, CMake and a C++17 compiler are required. Address builds also enable undefined-behavior checks. The deferred-callback configuration and an inline-callback build are both compiled.

The fake radio does not validate ESP32 scheduling, RF behavior, controller credits or memory-pool sizing. Real-device transfer and disconnect tests remain necessary. Timeouts deliberately exercise the production two-second deadline. The host process owns its fake task/queue threads for its lifetime.
