//
// (C) Dr. Michael 'Mickey' Lauer <mickey@vanille-media.de>
//
#include "NimBLEL2CAPClient.h"

#include "NimBLEDevice.h"
#include "nimble/nimble_port.h"

// Round-up integer division
#define CEIL_DIVIDE(a, b) (((a) + (b) - 1) / (b))
#define ROUND_DIVIDE(a, b) (((a) + (b) / 2) / (b))

NimBLEL2CAPClient* NimBLEL2CAPClient::createClient(const uint16_t psm, const uint16_t mtu, NimBLEL2CAPClientCallbacks* callbacks) {
    
    NimBLEL2CAPClient* client = new NimBLEL2CAPClient(psm, mtu, callbacks);
    return client;
}

NimBLEL2CAPClient::NimBLEL2CAPClient(const uint16_t psm, const uint16_t mtu, NimBLEL2CAPClientCallbacks* callbacks)
: psm(psm), mtu(mtu), callbacks(callbacks) {

    assert(callbacks != NULL); // fail here, if no callbacks are given

    const size_t buf_blocks = CEIL_DIVIDE(mtu, L2CAP_BUF_BLOCK_SIZE) * L2CAP_BUF_SIZE_MTUS_PER_CHANNEL;
    NIMBLE_LOGD(LOG_TAG, "Computed number of buf_blocks = %d", buf_blocks);

    int rc;

    coc_memory = malloc(OS_MEMPOOL_SIZE(buf_blocks, L2CAP_BUF_BLOCK_SIZE) * sizeof(os_membuf_t));
    if (coc_memory == 0) {
        NIMBLE_LOGE(LOG_TAG, "Can't allocate _coc_memory: %d", errno);
        return;
    }

    rc = os_mempool_init(&coc_mempool, buf_blocks, L2CAP_BUF_BLOCK_SIZE, coc_memory, "appbuf");
    if (rc != 0) {
        NIMBLE_LOGE(LOG_TAG, "Can't os_mempool_init: %d", rc);
        return;
    }
    rc = os_mbuf_pool_init(&coc_mbuf_pool, &coc_mempool, L2CAP_BUF_BLOCK_SIZE, buf_blocks);
    if (rc != 0) {
        NIMBLE_LOGE(LOG_TAG, "Can't os_mbuf_pool_init: %d", rc);
        return;
    }

    receiveBuffer = (uint8_t*) malloc(mtu);
    if (receiveBuffer == NULL) {
        NIMBLE_LOGE(LOG_TAG, "Can't malloc receive buffer: %d, %s", errno, strerror(errno));
    }

    NIMBLE_LOGI(LOG_TAG, "L2CAP COC 0x%04X registered w/ L2CAP MTU %i", this->psm, this->mtu);
}

bool NimBLEL2CAPClient::connect(NimBLEClient* pClient) {

    assert(pClient->isConnected()); // fail here, if client is not connected

    auto sdu_rx = os_mbuf_get_pkthdr(&this->coc_mbuf_pool, 0);
    auto rc = ble_l2cap_connect(pClient->getConnId(), this->psm, this->mtu, sdu_rx, NimBLEL2CAPClient::handleL2capEvent, this);
    if (rc != 0) {
        NIMBLE_LOGE(LOG_TAG, "ble_l2cap_connect failed: %d", rc);
    }
    return rc;
}

bool NimBLEL2CAPClient::write(const std::vector<uint8_t>& data) {

    if (!isConnected()) { return false; }

    auto connectionHandle = ble_l2cap_get_conn_handle(this->channel);
    if (!connectionHandle) { return false; }

    struct ble_l2cap_chan_info info;
    if (ble_l2cap_get_chan_info(this->channel, &info) < 0) { return false; }
    size_t mtu = this->mtu;

    auto sdu_tx = os_mbuf_get_pkthdr(&this->coc_mbuf_pool, 0);
    if (!sdu_tx) {
        NIMBLE_LOGE(LOG_TAG, "os_mbuf_get_pkthdr failed");
        return false;
    }

    auto iter = data.begin();
    while (iter != data.end()) {
        auto chunk = std::min(static_cast<size_t>(std::distance(iter, data.end())), mtu);
        if (auto res = os_mbuf_append(sdu_tx, &(*iter), chunk); res != 0) {
            NIMBLE_LOGE(LOG_TAG, "Can't os_mbuf_append: %d", res);
            return false;
        }
        int res = 0;
        do {
            res = ble_l2cap_send(channel, sdu_tx);
            if (res == BLE_HS_ENOMEM) {
                // we don't have enough space in the TX buffer, try again after a short delay
                vTaskDelay(10 / portTICK_PERIOD_MS);
            }
        } while (res != 0 && res != BLE_HS_ESTALLED && res != BLE_HS_ENOMEM);
        iter += chunk;
        NIMBLE_LOGD(LOG_TAG, "L2CAP COC 0x%04X sent %d bytes.", this->psm, chunk);
    }
    return true;
}

int NimBLEL2CAPClient::handleConnectionEvent(struct ble_l2cap_event* event) {

    this->channel = event->connect.chan;
    struct ble_l2cap_chan_info info;
    ble_l2cap_get_chan_info(this->channel, &info);
    NIMBLE_LOGI(LOG_TAG, "L2CAP COC 0x%04X connected. Our MTU is %i, remote MTU is %i.", psm, info.our_l2cap_mtu, info.peer_l2cap_mtu);
    callbacks->onConnect(this);
    return 0;
}

int NimBLEL2CAPClient::handleTxUnstalledEvent(struct ble_l2cap_event* event) {
    NIMBLE_LOGI(LOG_TAG, "L2CAP COC 0x%04X transmit unstalled.", psm);
    return 0;
}

int NimBLEL2CAPClient::handleDisconnectionEvent(struct ble_l2cap_event* event) {
    NIMBLE_LOGI(LOG_TAG, "L2CAP COC 0x%04X disconnected.", psm);
    channel = NULL;
    callbacks->onDisconnect(this);
    return 0;
}

// L2CAP event handler
int NimBLEL2CAPClient::handleL2capEvent(struct ble_l2cap_event *event, void *arg) {

    NIMBLE_LOGD(LOG_TAG, "handleL2capEvent: handling l2cap event %d", event->type);
    NimBLEL2CAPClient* self = reinterpret_cast<NimBLEL2CAPClient*>(arg);

    int returnValue = 0;

    switch (event->type) {

        case BLE_L2CAP_EVENT_COC_CONNECTED: 
            returnValue = self->handleConnectionEvent(event);
            break;

        case BLE_L2CAP_EVENT_COC_DISCONNECTED:
            returnValue = self->handleDisconnectionEvent(event);
            break;

#if false
        case BLE_L2CAP_EVENT_COC_ACCEPT:
            returnValue = self->handleAcceptEvent(event);
            break;

        case BLE_L2CAP_EVENT_COC_DATA_RECEIVED:
            returnValue = self->handleDataReceivedEvent(event);
            break;
#endif
        case BLE_L2CAP_EVENT_COC_TX_UNSTALLED:
            returnValue = self->handleTxUnstalledEvent(event);
            break;

        default:
            NIMBLE_LOGW(LOG_TAG, "Unhandled l2cap event %d", event->type);
            break;
    }

    return returnValue;
}
