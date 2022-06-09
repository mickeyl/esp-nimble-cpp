//
// (C) Dr. Michael 'Mickey' Lauer <mickey@vanille-media.de>
//
#ifndef NIMBLEL2CAPSERVICE_H
#define NIMBLEL2CAPSERVICE_H

#pragma once

#include "inttypes.h"
#include "host/ble_l2cap.h"
#include "os/os_mbuf.h"
#include "os/os_mempool.h"
#undef max
#undef min

#define APP_MTU 5000
#define APP_BUF_CHUNKSIZE 500
#define APP_BUF_NUM 10
#define MBUFSIZE_OVHD       (sizeof(struct os_mbuf) + \
                             sizeof(struct os_mbuf_pkthdr))
#define MBUFS_PER_MTU       (APP_MTU / APP_BUF_CHUNKSIZE)
#define MBUFSIZE            (APP_BUF_CHUNKSIZE + MBUFSIZE_OVHD)
#define MBUFCNT             (APP_BUF_NUM * MBUFS_PER_MTU)

class NimBLEL2CAPService {
public:
    NimBLEL2CAPService(uint16_t psm);
    ~NimBLEL2CAPService();

protected:
    int handleConnectionEvent(struct ble_l2cap_event *event);
    int handleAcceptEvent(struct ble_l2cap_event *event);
    int handleDataReceivedEvent(struct ble_l2cap_event *event);
    int handleTxUnstalledEvent(struct ble_l2cap_event *event);
    int handleDisconnectionEvent(struct ble_l2cap_event *event);

private:
    uint16_t psm; // protocol service multiplexer
    struct ble_l2cap_chan* channel; // channel handle

    os_membuf_t _coc_mem[OS_MEMPOOL_SIZE(MBUFCNT, MBUFSIZE)];
    struct os_mempool _coc_mempool;
    struct os_mbuf_pool _coc_mbuf_pool;

    static int handleL2capEvent(struct ble_l2cap_event *event, void *arg);

};

#endif