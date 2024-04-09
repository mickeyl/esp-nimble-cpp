//
// (C) Dr. Michael 'Mickey' Lauer <mickey@vanille-media.de>
//
#ifndef NIMBLEL2CAPSERVER_H
#define NIMBLEL2CAPSERVER_H
#pragma once

#include "inttypes.h"
#include <vector>

class NimBLEL2CAPChannel;
class NimBLEL2CAPChannelCallbacks;


/**
 * @brief L2CAP server class.
 * 
 * Encapsulates a L2CAP server that can hold multiple services. Every service is represented by a channel object
 * and an assorted set of callbacks.
 */
class NimBLEL2CAPServer {
public:
    /// @brief Register a new L2CAP service instance.
    /// @param psm The port multiplexor service number.
    /// @param mtu The maximum transmission unit.
    /// @param callbacks The callbacks for this service.
    /// @return the newly created object, if the server registration was successful.
    NimBLEL2CAPChannel* createService(const uint16_t psm, const uint16_t mtu, NimBLEL2CAPChannelCallbacks* callbacks);

private:
    NimBLEL2CAPServer();
    ~NimBLEL2CAPServer();
    std::vector<NimBLEL2CAPChannel*> services;

    friend class NimBLEL2CAPChannel;
    friend class NimBLEDevice;
};

#endif