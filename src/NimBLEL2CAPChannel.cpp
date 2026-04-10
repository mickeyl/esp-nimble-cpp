//
// (C) Dr. Michael 'Mickey' Lauer <mickey@vanille-media.de>
//

#include "NimBLEL2CAPChannel.h"
#if CONFIG_BT_NIMBLE_ENABLED && MYNEWT_VAL(BLE_L2CAP_COC_MAX_NUM)

# include "NimBLEClient.h"
# include "NimBLELog.h"
# include "NimBLEUtils.h"
# include "freertos/queue.h"
# include "freertos/task.h"

# ifdef USING_NIMBLE_ARDUINO_HEADERS
#  include "nimble/nimble/host/include/host/ble_gap.h"
# else
#  include "host/ble_gap.h"
# endif

// Allocate one full SDU per mbuf, matching NimBLE's own CoC examples.
# define L2CAP_SDU_BUFFER_COUNT CONFIG_NIMBLE_CPP_L2CAP_SDU_BUFFER_COUNT
// Retry
constexpr uint32_t RetryTimeout = 50;
constexpr int      RetryCounter = 3;

static void logPoolSanityWarning(const char* tag, uint16_t psm, const ble_l2cap_chan_info& info) {
    const uint16_t negotiatedCocMtu = info.peer_coc_mtu < info.our_coc_mtu ? info.peer_coc_mtu : info.our_coc_mtu;
    const uint16_t effectiveL2capMtu = info.peer_l2cap_mtu < info.our_l2cap_mtu ? info.peer_l2cap_mtu : info.our_l2cap_mtu;

    if (negotiatedCocMtu == 0 || effectiveL2capMtu == 0) {
        return;
    }

    const uint32_t fragmentsPerSdu = (negotiatedCocMtu + effectiveL2capMtu - 1) / effectiveL2capMtu;
    const uint32_t msys1BlockCount = MYNEWT_VAL(MSYS_1_BLOCK_COUNT);
    const uint32_t msys1BlockSize = MYNEWT_VAL(MSYS_1_BLOCK_SIZE);
    const uint32_t cocMps = MYNEWT_VAL(BLE_L2CAP_COC_MPS);
    const uint32_t aclFromLlCount = MYNEWT_VAL(BLE_TRANSPORT_ACL_FROM_LL_COUNT);
    const uint32_t localSduBufferCount = L2CAP_SDU_BUFFER_COUNT;

    NIMBLE_LOGI(tag,
                "L2CAP COC 0x%04X path geometry: negotiated_coc=%u effective_l2cap=%u fragments_per_sdu=%lu mps=%lu local_sdu_bufs=%lu msys1=%lux%lu acl_from_ll=%lu",
                psm,
                negotiatedCocMtu,
                effectiveL2capMtu,
                (unsigned long)fragmentsPerSdu,
                (unsigned long)cocMps,
                (unsigned long)localSduBufferCount,
                (unsigned long)msys1BlockCount,
                (unsigned long)msys1BlockSize,
                (unsigned long)aclFromLlCount);

    if (effectiveL2capMtu < negotiatedCocMtu &&
        (fragmentsPerSdu >= msys1BlockCount || fragmentsPerSdu * 2 >= msys1BlockCount || fragmentsPerSdu >= aclFromLlCount)) {
        NIMBLE_LOGW(tag,
                    "L2CAP COC 0x%04X large-SDU risk: negotiated MTU %u requires %lu fragments at L2CAP MTU %u. Current pools (MSYS_1=%lux%lu, ACL_FROM_LL=%lu) may cause ENOMEM/timeouts under load.",
                    psm,
                    negotiatedCocMtu,
                    (unsigned long)fragmentsPerSdu,
                    effectiveL2capMtu,
                    (unsigned long)msys1BlockCount,
                    (unsigned long)msys1BlockSize,
                    (unsigned long)aclFromLlCount);
    }

    if (localSduBufferCount < 3) {
        NIMBLE_LOGW(tag,
                    "L2CAP COC 0x%04X local SDU buffer count is %lu. Very small per-channel SDU pools reduce heap use but can make large MTUs fragile when send/receive work overlaps or callbacks lag.",
                    psm,
                    (unsigned long)localSduBufferCount);
    }
}

#if CONFIG_NIMBLE_CPP_L2CAP_DEFERRED_READ_CALLBACKS
struct DeferredReadItem {
    NimBLEL2CAPChannel*     channel;
    std::vector<uint8_t>*   data;
};

static QueueHandle_t     s_l2capDeferredReadQueue = nullptr;
static TaskHandle_t      s_l2capDeferredReadTask  = nullptr;
static SemaphoreHandle_t s_l2capDeferredInitLock  = nullptr;

void deferredReadWorker(void*) {
    DeferredReadItem item{};
    while (true) {
        if (xQueueReceive(s_l2capDeferredReadQueue, &item, portMAX_DELAY) == pdTRUE) {
            if (item.channel != nullptr) {
                item.channel->m_pendingDeferredReads.fetch_sub(1);
            }
            if (item.channel != nullptr && item.data != nullptr && item.channel->callbacks != nullptr) {
                item.channel->callbacks->onRead(item.channel, *item.data);
            }
            delete item.data;
        }
    }
}

bool ensureDeferredReadWorker() {
    if (s_l2capDeferredReadQueue != nullptr && s_l2capDeferredReadTask != nullptr) {
        return true;
    }

    if (s_l2capDeferredInitLock == nullptr) {
        s_l2capDeferredInitLock = xSemaphoreCreateMutex();
        if (s_l2capDeferredInitLock == nullptr) {
            return false;
        }
    }

    if (xSemaphoreTake(s_l2capDeferredInitLock, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    if (s_l2capDeferredReadQueue == nullptr) {
        s_l2capDeferredReadQueue = xQueueCreate(CONFIG_NIMBLE_CPP_L2CAP_CALLBACK_QUEUE_LENGTH, sizeof(DeferredReadItem));
    }

    if (s_l2capDeferredReadQueue != nullptr && s_l2capDeferredReadTask == nullptr) {
        BaseType_t rc = xTaskCreate(
            deferredReadWorker,
            "nimble_l2cap_cb",
            CONFIG_NIMBLE_CPP_L2CAP_CALLBACK_TASK_STACK_SIZE,
            nullptr,
            CONFIG_NIMBLE_CPP_L2CAP_CALLBACK_TASK_PRIORITY,
            &s_l2capDeferredReadTask);
        if (rc != pdPASS) {
            s_l2capDeferredReadTask = nullptr;
        }
    }

    const bool ok = s_l2capDeferredReadQueue != nullptr && s_l2capDeferredReadTask != nullptr;
    xSemaphoreGive(s_l2capDeferredInitLock);
    return ok;
}
#endif

NimBLEL2CAPChannel::NimBLEL2CAPChannel(uint16_t psm, uint16_t mtu, NimBLEL2CAPChannelCallbacks* callbacks)
    : psm(psm), mtu(mtu), callbacks(callbacks) {
    assert(mtu);            // fail here, if MTU is too little
    assert(callbacks);      // fail here, if no callbacks are given
    assert(setupMemPool()); // fail here, if the memory pool could not be setup
#if CONFIG_NIMBLE_CPP_L2CAP_DEFERRED_READ_CALLBACKS
    assert(ensureDeferredReadWorker());
#endif
    m_unstallSem = xSemaphoreCreateBinary();
    assert(m_unstallSem);

    NIMBLE_LOGI(LOG_TAG, "L2CAP COC 0x%04X initialized w/ L2CAP MTU %i", this->psm, this->mtu);
};

NimBLEL2CAPChannel::~NimBLEL2CAPChannel() {
    while (m_pendingDeferredReads.load() > 0) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    teardownMemPool();

    NIMBLE_LOGI(LOG_TAG, "L2CAP COC 0x%04X shutdown and freed.", this->psm);
}

bool NimBLEL2CAPChannel::setupMemPool() {
    const size_t buf_blocks = L2CAP_SDU_BUFFER_COUNT;
    NIMBLE_LOGD(LOG_TAG, "Allocating %d L2CAP SDU buffers of %d bytes", buf_blocks, mtu);

    memset(&_coc_mempool, 0, sizeof(_coc_mempool));
    memset(&_coc_mbuf_pool, 0, sizeof(_coc_mbuf_pool));

    _coc_memory = malloc(OS_MEMPOOL_SIZE(buf_blocks, mtu) * sizeof(os_membuf_t));
    if (_coc_memory == 0) {
        NIMBLE_LOGE(LOG_TAG, "Can't allocate _coc_memory: %d", errno);
        return false;
    }

    auto rc = os_mempool_init(&_coc_mempool, buf_blocks, mtu, _coc_memory, "appbuf");
    if (rc != 0) {
        NIMBLE_LOGE(LOG_TAG, "Can't os_mempool_init: %d", rc);
        return false;
    }

    auto rc2 = os_mbuf_pool_init(&_coc_mbuf_pool, &_coc_mempool, mtu, buf_blocks);
    if (rc2 != 0) {
        NIMBLE_LOGE(LOG_TAG, "Can't os_mbuf_pool_init: %d", rc2);
        return false;
    }

    this->receiveBuffer = (uint8_t*)malloc(mtu);
    if (!this->receiveBuffer) {
        NIMBLE_LOGE(LOG_TAG, "Can't malloc receive buffer: %d, %s", errno, strerror(errno));
        return false;
    }

    return true;
}

void NimBLEL2CAPChannel::teardownMemPool() {
    if (m_unstallSem) {
        vSemaphoreDelete(m_unstallSem);
        m_unstallSem = nullptr;
    }
    if (this->callbacks) {
        delete this->callbacks;
        this->callbacks = nullptr;
    }
    if (this->receiveBuffer) {
        free(this->receiveBuffer);
        this->receiveBuffer = nullptr;
    }
    if (_coc_mempool.name) {
        os_mempool_unregister(&_coc_mempool);
        memset(&_coc_mempool, 0, sizeof(_coc_mempool));
        memset(&_coc_mbuf_pool, 0, sizeof(_coc_mbuf_pool));
    }
    if (_coc_memory) {
        free(_coc_memory);
        _coc_memory = nullptr;
    }
}

int NimBLEL2CAPChannel::writeFragment(std::vector<uint8_t>::const_iterator begin, std::vector<uint8_t>::const_iterator end) {
    auto toSend = end - begin;

    if (stalled) {
        NIMBLE_LOGW(LOG_TAG, "L2CAP COC 0x%04X waiting for TX_UNSTALLED.", this->psm);
        xSemaphoreTake(m_unstallSem, 0);
        m_unstallStatus.store(BLE_HS_EUNKNOWN);
        xSemaphoreTake(m_unstallSem, portMAX_DELAY);
        const int unstallStatus = m_unstallStatus.load();
        stalled                 = false;
        NIMBLE_LOGI(LOG_TAG, "L2CAP COC 0x%04X TX_UNSTALLED status=%d.", this->psm, unstallStatus);
        if (unstallStatus != 0) {
            NIMBLE_LOGE(LOG_TAG, "Pending L2CAP SDU completion failed: %d", unstallStatus);
            return unstallStatus;
        }
    }

    struct ble_l2cap_chan_info info;
    ble_l2cap_get_chan_info(channel, &info);
    // Take the minimum of our and peer MTU
    auto mtu = info.peer_coc_mtu < info.our_coc_mtu ? info.peer_coc_mtu : info.our_coc_mtu;

    if (toSend > mtu) {
        return -BLE_HS_EBADDATA;
    }

    auto retries = RetryCounter;

    while (retries--) {
        auto txd = os_mbuf_get_pkthdr(&_coc_mbuf_pool, 0);
        if (!txd) {
            NIMBLE_LOGE(LOG_TAG, "Can't os_mbuf_get_pkthdr.");
            return -BLE_HS_ENOMEM;
        }
        auto append = os_mbuf_append(txd, &(*begin), toSend);
        if (append != 0) {
            NIMBLE_LOGE(LOG_TAG, "Can't os_mbuf_append: %d", append);
            os_mbuf_free_chain(txd);
            return append;
        }

        auto res = ble_l2cap_send(channel, txd);
        switch (res) {
            case 0:
                NIMBLE_LOGD(LOG_TAG, "L2CAP COC 0x%04X sent %d bytes.", this->psm, toSend);
                return 0;

            case BLE_HS_ESTALLED:
                stalled = true;
                NIMBLE_LOGW(LOG_TAG, "L2CAP COC 0x%04X send stalled; waiting for TX_UNSTALLED.", this->psm);
                xSemaphoreTake(m_unstallSem, 0);
                m_unstallStatus.store(BLE_HS_EUNKNOWN);
                xSemaphoreTake(m_unstallSem, portMAX_DELAY);
                stalled = false;
                if (m_unstallStatus.load() != 0) {
                    NIMBLE_LOGE(LOG_TAG, "L2CAP COC 0x%04X stalled send failed with status %d.",
                                this->psm, m_unstallStatus.load());
                    return m_unstallStatus.load();
                }
                return 0;

            case BLE_HS_ENOMEM:
            case BLE_HS_EAGAIN:
                /* This error path is consumed by NimBLE; retrying the same SDU can duplicate partial data. */
                NIMBLE_LOGE(LOG_TAG, "L2CAP COC 0x%04X send failed with consumed error %d; dropping channel state.", this->psm, res);
                return res;

            case BLE_HS_EBUSY:
                /* Channel busy; txd not consumed */
                NIMBLE_LOGD(LOG_TAG, "ble_l2cap_send returned %d (busy). Retrying shortly...", res);
                os_mbuf_free_chain(txd);
                ble_npl_time_delay(ble_npl_time_ms_to_ticks32(RetryTimeout));
                continue;

            default:
                NIMBLE_LOGE(LOG_TAG, "ble_l2cap_send failed: %d", res);
                os_mbuf_free_chain(txd);
                return res;
        }
    }
    NIMBLE_LOGE(LOG_TAG, "Retries exhausted, dropping %d bytes to send.", toSend);
    return -BLE_HS_EREJECT;
}

# if MYNEWT_VAL(BLE_ROLE_CENTRAL)
NimBLEL2CAPChannel* NimBLEL2CAPChannel::connect(NimBLEClient*                client,
                                                uint16_t                     psm,
                                                uint16_t                     mtu,
                                                NimBLEL2CAPChannelCallbacks* callbacks) {
    if (!client->isConnected()) {
        NIMBLE_LOGE(
            LOG_TAG,
            "Client is not connected. Before connecting via L2CAP, a GAP connection must have been established");
        return nullptr;
    };

    auto channel = new NimBLEL2CAPChannel(psm, mtu, callbacks);

    auto sdu_rx = os_mbuf_get_pkthdr(&channel->_coc_mbuf_pool, 0);
    if (!sdu_rx) {
        NIMBLE_LOGE(LOG_TAG, "Can't allocate SDU buffer: %d, %s", errno, strerror(errno));
        delete channel;
        return nullptr;
    }
    auto rc = ble_l2cap_connect(client->getConnHandle(), psm, mtu, sdu_rx, NimBLEL2CAPChannel::handleL2capEvent, channel);
    if (rc != 0) {
        NIMBLE_LOGE(LOG_TAG, "ble_l2cap_connect failed: %d", rc);
        os_mbuf_free_chain(sdu_rx);
        delete channel;
        return nullptr;
    }
    return channel;
}
# endif // MYNEWT_VAL(BLE_ROLE_CENTRAL)

bool NimBLEL2CAPChannel::write(const std::vector<uint8_t>& bytes) {
    if (!this->channel) {
        NIMBLE_LOGW(LOG_TAG, "L2CAP Channel not open");
        return false;
    }

    struct ble_l2cap_chan_info info;
    ble_l2cap_get_chan_info(channel, &info);
    auto mtu = info.peer_coc_mtu < info.our_coc_mtu ? info.peer_coc_mtu : info.our_coc_mtu;

    auto start = bytes.begin();
    while (start != bytes.end()) {
        auto end = start + mtu < bytes.end() ? start + mtu : bytes.end();
        int rc = writeFragment(start, end);
        if (rc < 0) {
            this->disconnect();
            return false;
        }
        start = end;
    }
    return true;
}

bool NimBLEL2CAPChannel::disconnect() {
    if (!this->channel) {
        NIMBLE_LOGW(LOG_TAG, "L2CAP Channel not open");
        return false;
    }

    int rc = ble_l2cap_disconnect(this->channel);
    if (rc != 0 && rc != BLE_HS_ENOTCONN && rc != BLE_HS_EALREADY) {
        NIMBLE_LOGE(LOG_TAG, "ble_l2cap_disconnect failed: rc=%d %s", rc, NimBLEUtils::returnCodeToString(rc));
        return false;
    }

    return true;
}

uint16_t NimBLEL2CAPChannel::getConnHandle() const {
    if (!this->channel) {
        return BLE_HS_CONN_HANDLE_NONE;
    }
    return ble_l2cap_get_conn_handle(this->channel);
}

// private
int NimBLEL2CAPChannel::handleConnectionEvent(struct ble_l2cap_event* event) {
    channel = event->connect.chan;
    xSemaphoreTake(m_unstallSem, 0);
    m_unstallStatus.store(0);
    stalled = false;
    struct ble_l2cap_chan_info info;
    int rc = ble_l2cap_get_chan_info(channel, &info);
    if (rc != 0) {
        NIMBLE_LOGE(LOG_TAG, "L2CAP COC 0x%04X connected but ble_l2cap_get_chan_info failed: %d", psm, rc);
        callbacks->onConnect(this, mtu);
        return 0;
    }
    NIMBLE_LOGI(LOG_TAG,
                "L2CAP COC 0x%04X connected. scid=0x%04X dcid=0x%04X local_l2cap=%d local_coc=%d peer_l2cap=%d peer_coc=%d.",
                psm,
                info.scid,
                info.dcid,
                info.our_l2cap_mtu,
                info.our_coc_mtu,
                info.peer_l2cap_mtu,
                info.peer_coc_mtu);
    logPoolSanityWarning(LOG_TAG, psm, info);
    if (info.our_coc_mtu > 0 && info.peer_coc_mtu > 0 && info.our_coc_mtu > info.peer_coc_mtu) {
        NIMBLE_LOGW(LOG_TAG, "L2CAP COC 0x%04X connected, but local MTU is bigger than remote MTU.", psm);
    }

    uint16_t negotiatedMTU = 0;
    if (info.our_coc_mtu > 0 && info.peer_coc_mtu > 0) {
        negotiatedMTU = info.peer_coc_mtu < info.our_coc_mtu ? info.peer_coc_mtu : info.our_coc_mtu;
    } else if (info.our_l2cap_mtu > 0 && info.peer_l2cap_mtu > 0) {
        negotiatedMTU = info.peer_l2cap_mtu < info.our_l2cap_mtu ? info.peer_l2cap_mtu : info.our_l2cap_mtu;
        NIMBLE_LOGW(LOG_TAG,
                    "L2CAP COC 0x%04X connected with zero CoC MTU info; falling back to L2CAP MTU %u for callback.",
                    psm,
                    negotiatedMTU);
    } else {
        negotiatedMTU = mtu;
        NIMBLE_LOGW(LOG_TAG,
                    "L2CAP COC 0x%04X connected with no peer MTU info; falling back to configured MTU %u for callback.",
                    psm,
                    negotiatedMTU);
    }

    callbacks->onConnect(this, negotiatedMTU);
    return 0;
}

int NimBLEL2CAPChannel::handleAcceptEvent(struct ble_l2cap_event* event) {
    NIMBLE_LOGI(LOG_TAG, "L2CAP COC 0x%04X accept.", psm);
    if (!callbacks->shouldAcceptConnection(this)) {
        NIMBLE_LOGI(LOG_TAG, "L2CAP COC 0x%04X refused by delegate.", psm);
        return -1;
    }

    struct os_mbuf* sdu_rx = os_mbuf_get_pkthdr(&_coc_mbuf_pool, 0);
    if (!sdu_rx) {
        NIMBLE_LOGE(LOG_TAG, "L2CAP COC 0x%04X could not allocate receive SDU.", psm);
        return BLE_HS_ENOMEM;
    }

    int rc = ble_l2cap_recv_ready(event->accept.chan, sdu_rx);
    if (rc != 0) {
        NIMBLE_LOGE(LOG_TAG, "L2CAP COC 0x%04X ble_l2cap_recv_ready failed during accept: %d", psm, rc);
        os_mbuf_free_chain(sdu_rx);
        return rc;
    }

    return 0;
}

int NimBLEL2CAPChannel::handleDataReceivedEvent(struct ble_l2cap_event* event) {
    NIMBLE_LOGD(LOG_TAG, "L2CAP COC 0x%04X data received.", psm);

    struct os_mbuf* rxd = event->receive.sdu_rx;
    assert(rxd != NULL);

    int rx_len = (int)OS_MBUF_PKTLEN(rxd);
    assert(rx_len <= (int)mtu);

    int res = os_mbuf_copydata(rxd, 0, rx_len, receiveBuffer);
    assert(res == 0);

    NIMBLE_LOGD(LOG_TAG, "L2CAP COC 0x%04X received %d bytes.", psm, rx_len);

    res = os_mbuf_free_chain(rxd);
    assert(res == 0);

    std::vector<uint8_t> incomingData(receiveBuffer, receiveBuffer + rx_len);

    struct os_mbuf* next = os_mbuf_get_pkthdr(&_coc_mbuf_pool, 0);
    assert(next != NULL);

    res = ble_l2cap_recv_ready(channel, next);
    assert(res == 0);

#if CONFIG_NIMBLE_CPP_L2CAP_DEFERRED_READ_CALLBACKS
    auto deferredData = new std::vector<uint8_t>(std::move(incomingData));
    DeferredReadItem item{this, deferredData};
    m_pendingDeferredReads.fetch_add(1);
    if (xQueueSend(s_l2capDeferredReadQueue, &item, 0) != pdTRUE) {
        m_pendingDeferredReads.fetch_sub(1);
        NIMBLE_LOGW(LOG_TAG, "L2CAP COC 0x%04X deferred callback queue full; falling back to inline onRead.", psm);
        callbacks->onRead(this, *deferredData);
        delete deferredData;
    }
#else
    callbacks->onRead(this, incomingData);
#endif

    return 0;
}

int NimBLEL2CAPChannel::handleTxUnstalledEvent(struct ble_l2cap_event* event) {
    m_unstallStatus.store(event->tx_unstalled.status);
    xSemaphoreGive(m_unstallSem);
    NIMBLE_LOGI(LOG_TAG, "L2CAP COC 0x%04X transmit unstalled (status=%d).", psm, event->tx_unstalled.status);
    return 0;
}

int NimBLEL2CAPChannel::handleDisconnectionEvent(struct ble_l2cap_event* event) {
    NIMBLE_LOGI(LOG_TAG, "L2CAP COC 0x%04X disconnected.", psm);
    xSemaphoreTake(m_unstallSem, 0);
    m_unstallStatus.store(0);
    stalled = false;
    channel = NULL;
    callbacks->onDisconnect(this);
    return 0;
}

/* STATIC */
int NimBLEL2CAPChannel::handleL2capEvent(struct ble_l2cap_event* event, void* arg) {
    NIMBLE_LOGD(LOG_TAG, "handleL2capEvent: handling l2cap event %d", event->type);
    NimBLEL2CAPChannel* self = reinterpret_cast<NimBLEL2CAPChannel*>(arg);

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

#endif // #if CONFIG_BT_NIMBLE_ENABLED && MYNEWT_VAL(BLE_L2CAP_COC_MAX_NUM)
