//
// (C) Dr. Michael 'Mickey' Lauer <mickey@vanille-media.de>
//
#include "NimBLEL2CAPServer.h"
#include "NimBLEL2CAPService.h"

static const char* LOG_TAG = "NimBLEL2CAPServer";

NimBLEL2CAPServer::NimBLEL2CAPServer() {

}

NimBLEL2CAPServer::~NimBLEL2CAPServer() {

}

NimBLEL2CAPService* NimBLEL2CAPServer::createService(uint16_t psm) {

    auto service = new NimBLEL2CAPService(psm);
    this->m_svcVec.push_back(service);
    return service;    
}

