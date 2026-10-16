#pragma once
#include "Host.h"
struct HostTxQueue { std::atomic<bool> pending{false}; };
struct ble_hs_conn { HostTxQueue bhc_tx_q; std::atomic<uint16_t> bhc_outstanding_pkts{0}; };
inline ble_hs_conn hostConnection;
inline std::function<void()> hostBeforeConnectionLookup;
inline ble_hs_conn* ble_hs_conn_find(uint16_t) {
    if (hostBeforeConnectionLookup) hostBeforeConnectionLookup();
    return &hostConnection;
}
#define STAILQ_EMPTY(q) (!(q)->pending.load())
