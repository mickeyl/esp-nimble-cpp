#pragma once
#include <cstdint>

// Advisory host-mbuf model for the pinned allocator: every CoC PDU starts in
// MSYS1 with a packet header and leading space; append extends the same pool.
// Excludes RX/ATT, other channels, controller ownership and ACL re-fragmentation.
// Zero means the supplied geometry cannot be modelled, not zero resource use.
constexpr uint32_t nimbleL2capTxPoolBlocks(uint16_t sdu, uint16_t mps,
                                        uint32_t block, uint32_t mbufHeader,
                                        uint32_t packetHeader, uint32_t leading) {
    if (!sdu || !mps || block <= mbufHeader + packetHeader + leading) return 0;
    const uint32_t first = block - mbufHeader - packetHeader - leading;
    const uint32_t next = block - mbufHeader;
    const uint32_t full = (uint32_t(sdu) + 2) / mps;
    const uint32_t tail = (uint32_t(sdu) + 2) % mps;
    const uint32_t fullBlocks = 1 + (mps > first ? (mps - first + next - 1) / next : 0);
    const uint32_t tailBlocks = !tail ? 0 : 1 + (tail > first ? (tail - first + next - 1) / next : 0);
    return full * fullBlocks + tailBlocks;
}

// Add a deliberately simple one-block-per-outstanding-packet allowance. This
// is a warning heuristic, not a proven upper bound or a controller-pool budget.
constexpr bool nimbleL2capTxPoolTight(uint32_t sduBlocks, uint32_t window, uint32_t count) {
    return sduBlocks && window && uint64_t(sduBlocks) + window - 1 >= count;
}
