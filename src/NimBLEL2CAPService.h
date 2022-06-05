//
// (C) Dr. Michael 'Mickey' Lauer <mickey@vanille-media.de>
//
#ifndef NIMBLEL2CAPSERVICE_H
#define NIMBLEL2CAPSERVICE_H

#pragma once

#include "inttypes.h"

class NimBLEL2CAPService {
public:
    NimBLEL2CAPService(uint16_t psm);
    ~NimBLEL2CAPService();

private:
    uint16_t psm; // protocol service multiplexer
};

#endif