#pragma once
#include <cstdint>

// Each SDU carries two length bytes inside its first credit-based PDU.
// Fill whole peer-MPS units where possible, without increasing the SDU MTU.
// Keep a smaller SDU unchanged if it already fits in one peer PDU.
constexpr uint16_t nimbleL2capTxSduWidth(uint16_t mtu, uint16_t peerMps) {
    if (peerMps <= 2 || uint32_t(mtu) + 2 < peerMps) { return mtu; }
    const uint32_t units = (uint32_t(mtu) + 2) / peerMps;
    return uint16_t(units * peerMps - 2);
}
