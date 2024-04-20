//
// (C) Dr. Michael 'Mickey' Lauer <mickey@vanille-media.de>
//
#ifndef NIMBLEL2CAPCHANNEL_H
#define NIMBLEL2CAPCHANNEL_H
#pragma once

#include "inttypes.h"
#include "host/ble_l2cap.h"
#include "os/os_mbuf.h"
#include "os/os_mempool.h"

#include <vector>
#include <atomic>

class NimBLEClient;
class NimBLEL2CAPChannelCallbacks;

/**
 * @brief Encapsulates a L2CAP channel.
 * 
 * This class is used to encapsulate a L2CAP connection oriented channel, both
 * from the "server" (which waits for the connection to be opened) and the "client"
 * (which opens the connection) point of view.
 */
class NimBLEL2CAPChannel {

    friend class NimBLEL2CAPServer;

public:
    /// @brief Open an L2CAP channel via the specified PSM and MTU.
    ///
    /// @return the channel on success, NULL otherwise.
    static NimBLEL2CAPChannel* connect(NimBLEClient* client, uint16_t psm, uint16_t mtu, NimBLEL2CAPChannelCallbacks* callbacks);

    /// @brief Write data (up to the maximum length of one MTU) to the channel.
    ///
    /// @return true on success, after the data has been sent.
    /// @return false, if the data can't be sent or is too large.
    ///
    /// NOTE: This function may block if the transmission is stalled.
    bool write(const std::vector<uint8_t>& bytes);

    /// @return whether the channel is connected.
    bool isConnected() const { return !!channel; }

protected:






    NimBLEL2CAPChannel(uint16_t psm, uint16_t mtu, NimBLEL2CAPChannelCallbacks* callbacks);
    ~NimBLEL2CAPChannel();

    int handleConnectionEvent(struct ble_l2cap_event* event);
    int handleAcceptEvent(struct ble_l2cap_event* event);
    int handleDataReceivedEvent(struct ble_l2cap_event* event);
    int handleTxUnstalledEvent(struct ble_l2cap_event* event);
    int handleDisconnectionEvent(struct ble_l2cap_event* event);

private:
    static constexpr const char* LOG_TAG = "NimBLEL2CAPChannel";

    const uint16_t psm; // PSM of the channel
    const uint16_t mtu; // The requested MTU of the channel, might be larger than negotiated MTU
    struct ble_l2cap_chan* channel = nullptr;
    NimBLEL2CAPChannelCallbacks* callbacks;
    uint8_t* receiveBuffer = nullptr; // buffers a full MTU

    // NimBLE memory pool
    void* _coc_memory = nullptr;
    struct os_mempool _coc_mempool;
    struct os_mbuf_pool _coc_mbuf_pool;

    // Runtime handling
    std::atomic<bool> stalled = false;
    SemaphoreHandle_t stalledSemaphore = nullptr;

    // Allocate / deallocate NimBLE memory pool
    bool setupMemPool();
    void teardownMemPool();

    // L2CAP event handler
    static int handleL2capEvent(struct ble_l2cap_event* event, void *arg);
};

/**
 * @brief Callbacks base class for the L2CAP channel.
 */
class NimBLEL2CAPChannelCallbacks {

public:
    NimBLEL2CAPChannelCallbacks() = default;
    virtual ~NimBLEL2CAPChannelCallbacks() = default;

    /// Called when the client attempts to open a channel on the server.
    /// You can choose to accept or deny the connection.
    /// Default implementation returns true.
    virtual bool shouldAcceptConnection(NimBLEL2CAPChannel* channel) { return true; }
    /// Called after a connection has been made.
    /// Default implementation does nothing.
    virtual void onConnect(NimBLEL2CAPChannel* channel, uint16_t negotiatedMTU) {};
    /// Called when data has been read from the channel.
    /// Default implementation does nothing.
    virtual void onRead(NimBLEL2CAPChannel* channel, std::vector<uint8_t>& data) {};
    /// Called after the channel has been disconnected.
    /// Default implementation does nothing.
    virtual void onDisconnect(NimBLEL2CAPChannel* channel) {};
};

#endif