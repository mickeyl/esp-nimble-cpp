//
// (C) Dr. Michael 'Mickey' Lauer <mickey@vanille-media.de>
//
#ifndef NIMBLEL2CAPSERVER_H
#define NIMBLEL2CAPSERVER_H

#include "inttypes.h"
#include <vector>

#include "host/ble_l2cap.h"

class NimBLEL2CAPService;

#pragma once

class NimBLEL2CAPServer {
public:
    NimBLEL2CAPServer();
    ~NimBLEL2CAPServer();

    NimBLEL2CAPService* createService(uint16_t psm);

private:
    friend class NimBLEL2CAPService;
    std::vector<NimBLEL2CAPService*> m_svcVec;

    static int handleL2capEvent(struct ble_l2cap_event *event, void *arg);

};

#endif