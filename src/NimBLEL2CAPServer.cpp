//
// (C) Dr. Michael 'Mickey' Lauer <mickey@vanille-media.de>
//
#include "NimBLEL2CAPServer.h"
#include "NimBLEL2CAPChannel.h"
#include "NimBLEDevice.h"

static const char* LOG_TAG = "NimBLEL2CAPServer";

NimBLEL2CAPServer::NimBLEL2CAPServer() {

    // Nothing to do here...
}

NimBLEL2CAPServer::~NimBLEL2CAPServer() {

    // Delete all services
    for (auto service: this->services) {
        delete service;
    }
}

NimBLEL2CAPChannel* NimBLEL2CAPServer::createService(const uint16_t psm, const uint16_t mtu, NimBLEL2CAPChannelCallbacks* callbacks) {

    auto service = new NimBLEL2CAPChannel(psm, mtu, callbacks);

    if (auto rc = ble_l2cap_create_server(psm, mtu, NimBLEL2CAPChannel::handleL2capEvent, service); rc != 0) {
        NIMBLE_LOGE(LOG_TAG, "Could not ble_l2cap_create_server: %d", rc);
        return nullptr;
    }

    this->services.push_back(service);
    return service;    
}
