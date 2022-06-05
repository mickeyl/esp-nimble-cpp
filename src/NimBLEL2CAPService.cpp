//
// (C) Dr. Michael 'Mickey' Lauer <mickey@vanille-media.de>
//
#include "NimBLEL2CAPService.h"
#include "NimBLEL2CAPServer.h"

#include "NimBLELog.h"
#include "NimBLEUtils.h"

#include "nimble/nimble_port.h"

static const char* LOG_TAG = "NimBLEL2CAPService";

NimBLEL2CAPService::NimBLEL2CAPService(uint16_t psm) {

    int rc = ble_l2cap_create_server(psm, 1024, NimBLEL2CAPServer::handleL2capEvent, NULL);
    if (rc != 0) {
        NIMBLE_LOGE(LOG_TAG, "L2CAP Server creation error: %d, %s", rc, NimBLEUtils::returnCodeToString(rc));
    }
}

NimBLEL2CAPService::~NimBLEL2CAPService() {

}