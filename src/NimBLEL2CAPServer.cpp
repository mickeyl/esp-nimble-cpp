//
// (C) Dr. Michael 'Mickey' Lauer <mickey@vanille-media.de>
//
#include "NimBLEL2CAPServer.h"
#include "NimBLEL2CAPService.h"

static const char* LOG_TAG = "NimBLEL2CAPServer";

NimBLEL2CAPServer::NimBLEL2CAPServer() {

    // Nothing to do here...
}

NimBLEL2CAPServer::~NimBLEL2CAPServer() {

    // Delete all services
    for (auto service : m_svcVec) {
        delete service;
    }
}

NimBLEL2CAPService* NimBLEL2CAPServer::createService(const uint16_t psm, const uint16_t mtu, NimBLEL2CAPServiceCallbacks* callbacks) {

    // Create new service and store
    auto service = new NimBLEL2CAPService(psm, mtu, callbacks);
    this->m_svcVec.push_back(service);
    return service;    
}
