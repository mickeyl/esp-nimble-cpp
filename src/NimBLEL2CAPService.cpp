//
// (C) Dr. Michael 'Mickey' Lauer <mickey@vanille-media.de>
//
#include "NimBLEL2CAPService.h"
#include "NimBLEL2CAPServer.h"

#include "NimBLELog.h"
#include "NimBLEUtils.h"

#include "nimble/nimble_port.h"

static const char* LOG_TAG = "NimBLEL2CAPService";

NimBLEL2CAPService::NimBLEL2CAPService(uint16_t psm, NimBLEL2CAPServiceCallbacks* callbacks) {

    assert(callbacks != NULL);

    int rc = ble_l2cap_create_server(psm, APP_MTU, NimBLEL2CAPService::handleL2capEvent, this);
    if (rc != 0) {
        NIMBLE_LOGE(LOG_TAG, "L2CAP Server creation error: %d, %s", rc, NimBLEUtils::returnCodeToString(rc));
        return;
    }
    rc = os_mempool_init(&_coc_mempool, MBUFCNT, MBUFSIZE, _coc_mem, "appbuf");
    if (rc != 0) {
        NIMBLE_LOGE(LOG_TAG, "Can't init mempool: %d, %s", rc, NimBLEUtils::returnCodeToString(rc));
        return;
    }
    rc = os_mbuf_pool_init(&_coc_mbuf_pool, &_coc_mempool, MBUFSIZE, MBUFCNT);
    if (rc != 0) {
        NIMBLE_LOGE(LOG_TAG, "Can't init mbuf pool: %d, %s", rc, NimBLEUtils::returnCodeToString(rc));
        return;
    }

    receiveBuffer = (uint8_t*) malloc(APP_MTU);
    if (receiveBuffer == NULL) {
        NIMBLE_LOGE(LOG_TAG, "Can't malloc receive buffer: %d, %s", errno, NimBLEUtils::returnCodeToString(errno));
    }

    this->psm = psm;
    this->callbacks = callbacks;
    NIMBLE_LOGI(LOG_TAG, "L2CAP COC 0x%04X registered w/ L2CAP MTU %i", this->psm, APP_MTU);
}

NimBLEL2CAPService::~NimBLEL2CAPService() {
}

int NimBLEL2CAPService::handleConnectionEvent(struct ble_l2cap_event* event) {

    channel = event->connect.chan;
    struct ble_l2cap_chan_info info;
    ble_l2cap_get_chan_info(channel, &info);
    NIMBLE_LOGI(LOG_TAG, "L2CAP COC 0x%04X connected. Our MTU is %i, remote MTU is %i", psm, info.our_l2cap_mtu, info.peer_l2cap_mtu);
    return 0;
}

int NimBLEL2CAPService::handleAcceptEvent(struct ble_l2cap_event* event) {
    NIMBLE_LOGI(LOG_TAG, "L2CAP COC 0x%04X accept.", psm);
    struct os_mbuf *sdu_rx = os_mbuf_get_pkthdr(&_coc_mbuf_pool, 0);
    assert(sdu_rx != NULL);
    ble_l2cap_recv_ready(event->accept.chan, sdu_rx);
    return 0;
}

int NimBLEL2CAPService::handleDataReceivedEvent(struct ble_l2cap_event* event) {
    NIMBLE_LOGI(LOG_TAG, "L2CAP COC 0x%04X data received.", psm);

    struct os_mbuf* rxd = event->receive.sdu_rx;
    assert(rxd != NULL);

    int rx_len = (int)OS_MBUF_PKTLEN(rxd);
    assert(rx_len <= (int)APP_MTU);

    int res = os_mbuf_copydata(rxd, 0, rx_len, receiveBuffer);
    assert(res == 0);

    printf("Received %5i bytes...\n", rx_len);
    NIMBLE_LOGD(LOG_TAG, "Received: len %5i", rx_len);

    std::vector<uint8_t> incomingData(receiveBuffer, receiveBuffer + rx_len);
    callbacks->onRead(this, incomingData);

    struct os_mbuf* next = os_mbuf_get_pkthdr(&_coc_mbuf_pool, 0);
    assert(next != NULL);
    res = ble_l2cap_recv_ready(channel, next);
    assert(res == 0);

    return 0;
}

int NimBLEL2CAPService::handleTxUnstalledEvent(struct ble_l2cap_event* event) {
    NIMBLE_LOGI(LOG_TAG, "L2CAP COC 0x%04X transmit unstalled.", psm);
    return 0;
}

int NimBLEL2CAPService::handleDisconnectionEvent(struct ble_l2cap_event* event) {
    NIMBLE_LOGI(LOG_TAG, "L2CAP COC 0x%04X disconnected.", psm);
    channel = NULL;
    return 0;
}

/* STATIC */
int NimBLEL2CAPService::handleL2capEvent(struct ble_l2cap_event *event, void *arg) {

    NIMBLE_LOGD(LOG_TAG, "handleL2capEvent: handling l2cap event %d", event->type);
    NimBLEL2CAPService* self = reinterpret_cast<NimBLEL2CAPService*>(arg);

    int returnValue = 0;

    switch (event->type) {
        case BLE_L2CAP_EVENT_COC_CONNECTED: 
            returnValue = self->handleConnectionEvent(event);
            break;

        case BLE_L2CAP_EVENT_COC_DISCONNECTED:
            returnValue = self->handleDisconnectionEvent(event);
            break;

        case BLE_L2CAP_EVENT_COC_ACCEPT:
            returnValue = self->handleAcceptEvent(event);
            break;

        case BLE_L2CAP_EVENT_COC_DATA_RECEIVED:
            returnValue = self->handleDataReceivedEvent(event);
            break;

        case BLE_L2CAP_EVENT_COC_TX_UNSTALLED:
            returnValue = self->handleTxUnstalledEvent(event);
            break;

        default:
            NIMBLE_LOGW(LOG_TAG, "Unhandled l2cap event %d", event->type);
            break;
    }

    return returnValue;
}

NimBLEL2CAPServiceCallbacks::~NimBLEL2CAPServiceCallbacks() {}
