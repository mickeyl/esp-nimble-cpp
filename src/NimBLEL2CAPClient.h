//
// (C) Dr. Michael 'Mickey' Lauer <mickey@vanille-media.de>
//
#ifndef NIMBLEL2CAPCLIENT_H
#define NIMBLEL2CAPCLIENT_H
#pragma once

#include "inttypes.h"
#include "host/ble_l2cap.h"
#include "os/os_mbuf.h"
#include "os/os_mempool.h"

#include <vector>

class NimBLEL2CAPClientCallbacks;
class NimBLEClient;

#define L2CAP_BUF_BLOCK_SIZE            (250)
#define L2CAP_BUF_SIZE_MTUS_PER_CHANNEL (3)

class NimBLEL2CAPClient {
public:

    static NimBLEL2CAPClient* createClient(const uint16_t psm, const uint16_t mtu, NimBLEL2CAPClientCallbacks* callbacks);
    bool connect(NimBLEClient* pClient);
    bool isConnected() { return channel != nullptr; }
    bool write(const std::vector<uint8_t>& data);

private:
    NimBLEL2CAPClient(const uint16_t psm, const uint16_t mtu, NimBLEL2CAPClientCallbacks* callbacks);
    ~NimBLEL2CAPClient();

    int handleConnectionEvent(struct ble_l2cap_event* event);
    int handleTxUnstalledEvent(struct ble_l2cap_event* event);
    int handleDisconnectionEvent(struct ble_l2cap_event* event);

    static int handleL2capEvent(struct ble_l2cap_event *event, void *arg);


private:
    static constexpr const char* LOG_TAG = "NimBLEL2CAPClient";
    void* coc_memory;
    struct os_mempool coc_mempool;
    struct os_mbuf_pool coc_mbuf_pool;

    const uint16_t psm;
    const uint16_t mtu;
    struct ble_l2cap_chan* channel = nullptr; // channel handle
    uint8_t* receiveBuffer; // MTU buffer

    NimBLEL2CAPClientCallbacks* callbacks;
};

class NimBLEL2CAPClientCallbacks {
public:
    NimBLEL2CAPClientCallbacks() = default;
    virtual ~NimBLEL2CAPClientCallbacks() = default;
    virtual void onConnect(NimBLEL2CAPClient* pClient) = 0;
    virtual void onDisconnect(NimBLEL2CAPClient* pClient) = 0;
    virtual void onMTUChange(NimBLEL2CAPClient* pClient, uint16_t mtu) = 0;
    virtual void onRead(NimBLEL2CAPClient* pClient, std::vector<uint8_t>& data) = 0;
};

#endif