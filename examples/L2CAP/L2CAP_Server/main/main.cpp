#include <NimBLEDevice.h>
#include <inttypes.h>
#include <esp_hpl.hpp>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#define DEVICE_NAME          "BLE-Testing-ESP"
#define TARGET_SERVICE_UUID  "e7100001-61b3-4b3d-8b4d-7e3b51e2c001"
#define TARGET_PSM_CHAR_UUID "e7100002-61b3-4b3d-8b4d-7e3b51e2c001"
#define L2CAP_PSM            192
#define L2CAP_MTU            5000
#define RESPONSE_QUEUE_DEPTH 8
#define STATUS_INTERVAL_MS   5000

static QueueHandle_t responseQueue = nullptr;
static SemaphoreHandle_t stateMutex = nullptr;

struct QueuedResponse {
    NimBLEL2CAPChannel* channel;
    std::vector<uint8_t>* data;
};

// Heap monitoring
size_t initialHeap = 0;
size_t lastHeap = 0;
size_t heapDecreaseCount = 0;
const size_t HEAP_LEAK_THRESHOLD = 10;  // Warn after 10 consecutive decreases

class GATTCallbacks: public BLEServerCallbacks {

public:
    void onConnect(BLEServer* pServer, BLEConnInfo& info) {
        /// Booster #1
        pServer->setDataLen(info.getConnHandle(), 251);
        /// Booster #2 (especially for Apple devices)
        BLEDevice::getServer()->updateConnParams(info.getConnHandle(), 12, 12, 0, 200);
    }
};

class L2CAPChannelCallbacks: public BLEL2CAPChannelCallbacks {

public:
    bool connected = false;
    NimBLEL2CAPChannel* currentChannel = nullptr;
    uint64_t totalRequestBytes = 0;
    uint64_t totalResponseBytes = 0;
    size_t totalRequestsReceived = 0;
    size_t totalResponsesSent = 0;
    uint64_t totalPayloadBytes = 0;
    uint8_t expectedSequenceNumber = 0;
    size_t sequenceErrors = 0;
    size_t frameErrors = 0;
    size_t responseWriteErrors = 0;
    size_t responseQueueDrops = 0;
    uint64_t startTime = 0;

public:
    struct Snapshot {
        bool connected;
        uint64_t totalRequestBytes;
        uint64_t totalResponseBytes;
        size_t totalRequestsReceived;
        size_t totalResponsesSent;
        uint64_t totalPayloadBytes;
        size_t sequenceErrors;
        size_t responseWriteErrors;
        size_t responseQueueDrops;
        uint64_t startTime;
    };

    Snapshot snapshot() const {
        Snapshot snapshot{};
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        snapshot.connected           = connected;
        snapshot.totalRequestBytes   = totalRequestBytes;
        snapshot.totalResponseBytes  = totalResponseBytes;
        snapshot.totalRequestsReceived = totalRequestsReceived;
        snapshot.totalResponsesSent  = totalResponsesSent;
        snapshot.totalPayloadBytes   = totalPayloadBytes;
        snapshot.sequenceErrors      = sequenceErrors;
        snapshot.responseWriteErrors = responseWriteErrors;
        snapshot.responseQueueDrops  = responseQueueDrops;
        snapshot.startTime           = startTime;
        xSemaphoreGive(stateMutex);
        return snapshot;
    }

    void onConnect(NimBLEL2CAPChannel* channel, uint16_t negotiatedMTU) {
        printf("L2CAP connection established on PSM %d, MTU %d\n", L2CAP_PSM, negotiatedMTU);
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        connected = true;
        currentChannel = channel;
        totalRequestBytes = 0;
        totalResponseBytes = 0;
        totalRequestsReceived = 0;
        totalResponsesSent = 0;
        totalPayloadBytes = 0;
        expectedSequenceNumber = 0;
        sequenceErrors = 0;
        frameErrors = 0;
        responseWriteErrors = 0;
        responseQueueDrops = 0;
        startTime = esp_timer_get_time();
        xSemaphoreGive(stateMutex);
    }

    void onRead(NimBLEL2CAPChannel* channel, std::vector<uint8_t>& data) {
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        totalRequestBytes += data.size();
        if (data.size() < 3) {
            frameErrors++;
            printf("Malformed SDU: %zu bytes, expected at least 3-byte application header\n", data.size());
            xSemaphoreGive(stateMutex);
            return;
        }

        const uint8_t seqno = data[0];
        const uint16_t payloadLen = (static_cast<uint16_t>(data[1]) << 8) | data[2];
        const size_t expectedFrameSize = static_cast<size_t>(payloadLen) + 3;

        if (expectedFrameSize != data.size()) {
            frameErrors++;
            printf("Malformed SDU: declared payload=%u, actual payload=%zu (frame=%zu)\n",
                   payloadLen, data.size() - 3, data.size());
            xSemaphoreGive(stateMutex);
            return;
        }

        totalRequestsReceived++;
        totalPayloadBytes += payloadLen;

        if (seqno != expectedSequenceNumber) {
            sequenceErrors++;
            printf("Request %zu: sequence error, got %u expected %u (payload=%u)\n",
                   totalRequestsReceived, seqno, expectedSequenceNumber, payloadLen);
        }
        expectedSequenceNumber = static_cast<uint8_t>(seqno + 1);
        const size_t requestCount = totalRequestsReceived;
        xSemaphoreGive(stateMutex);

        auto* responseData = new std::vector<uint8_t>(data);
        QueuedResponse response{channel, responseData};
        if (xQueueSend(responseQueue, &response, 0) != pdTRUE) {
            delete responseData;
            xSemaphoreTake(stateMutex, portMAX_DELAY);
            responseQueueDrops++;
            xSemaphoreGive(stateMutex);
            printf("Response queue full for request %zu\n", requestCount);
            return;
        }
    }

    void onDisconnect(NimBLEL2CAPChannel* channel) {
        printf("\nL2CAP disconnected\n");
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        double elapsedSeconds = startTime > 0 ? (esp_timer_get_time() - startTime) / 1000000.0 : 0.0;
        double linkBytesPerSecond =
            elapsedSeconds > 0 ? (totalRequestBytes + totalResponseBytes) / elapsedSeconds : 0.0;

        printf("Final statistics:\n");
        printf("  Requests received: %zu\n", totalRequestsReceived);
        printf("  Responses sent: %zu\n", totalResponsesSent);
        printf("  Request bytes: %" PRIu64 "\n", totalRequestBytes);
        printf("  Response bytes: %" PRIu64 "\n", totalResponseBytes);
        printf("  Payload bytes: %" PRIu64 "\n", totalPayloadBytes);
        printf("  Sequence errors: %zu\n", sequenceErrors);
        printf("  Frame errors: %zu\n", frameErrors);
        printf("  Response write errors: %zu\n", responseWriteErrors);
        printf("  Response queue drops: %zu\n", responseQueueDrops);
        printf("  Link bandwidth: %.2f KB/s (%.2f Mbps)\n",
               linkBytesPerSecond / 1024.0, (linkBytesPerSecond * 8) / 1000000.0);

        // Reset state for the next connection
        currentChannel = nullptr;
        totalRequestBytes = 0;
        totalResponseBytes = 0;
        totalRequestsReceived = 0;
        totalResponsesSent = 0;
        totalPayloadBytes = 0;
        expectedSequenceNumber = 0;
        sequenceErrors = 0;
        frameErrors = 0;
        responseWriteErrors = 0;
        responseQueueDrops = 0;
        startTime = 0;
        connected = false;
        xSemaphoreGive(stateMutex);

        QueuedResponse pending;
        while (xQueueReceive(responseQueue, &pending, 0) == pdTRUE) {
            delete pending.data;
        }

        // Restart advertising so another client can connect
        BLEDevice::startAdvertising();
    }
};

static void responseTask(void* pvParameters) {
    auto* callbacks = static_cast<L2CAPChannelCallbacks*>(pvParameters);
    QueuedResponse response;

    while (true) {
        if (xQueueReceive(responseQueue, &response, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        xSemaphoreTake(stateMutex, portMAX_DELAY);
        const bool canSend = callbacks->connected &&
                             callbacks->currentChannel == response.channel &&
                             response.channel != nullptr &&
                             response.channel->isConnected();
        const size_t requestCount = callbacks->totalRequestsReceived;
        xSemaphoreGive(stateMutex);

        if (!canSend) {
            delete response.data;
            continue;
        }

        if (!response.channel->write(*response.data)) {
            xSemaphoreTake(stateMutex, portMAX_DELAY);
            callbacks->responseWriteErrors++;
            xSemaphoreGive(stateMutex);
            printf("Failed to echo queued response for request %zu\n", requestCount);
            delete response.data;
            continue;
        }

        xSemaphoreTake(stateMutex, portMAX_DELAY);
        callbacks->totalResponsesSent++;
        callbacks->totalResponseBytes += response.data->size();
        xSemaphoreGive(stateMutex);
        delete response.data;
    }
}

extern "C"
void app_main(void) {
    // Install high performance logging before any other output
    esp_hpl::HighPerformanceLogger::init();

    printf("Starting L2CAP server example [%lu free] [%lu min]\n", esp_get_free_heap_size(), esp_get_minimum_free_heap_size());

    BLEDevice::init(DEVICE_NAME);
    BLEDevice::setMTU(BLE_ATT_MTU_MAX);

    auto cocServer = BLEDevice::createL2CAPServer();
    auto l2capChannelCallbacks = new L2CAPChannelCallbacks();
    responseQueue = xQueueCreate(RESPONSE_QUEUE_DEPTH, sizeof(QueuedResponse));
    stateMutex = xSemaphoreCreateMutex();
    assert(responseQueue != nullptr);
    assert(stateMutex != nullptr);
    xTaskCreate(responseTask, "responseTask", 6144, l2capChannelCallbacks, 1, nullptr);
    auto channel = cocServer->createService(L2CAP_PSM, L2CAP_MTU, l2capChannelCallbacks);
    (void)channel;  // prevent unused warning

    auto server = BLEDevice::createServer();
    server->setCallbacks(new GATTCallbacks());
    auto gattService = server->createService(TARGET_SERVICE_UUID);
    auto psmCharacteristic = gattService->createCharacteristic(TARGET_PSM_CHAR_UUID, NIMBLE_PROPERTY::READ);
    uint8_t psmValue[2] = {
        static_cast<uint8_t>(L2CAP_PSM & 0xFF),
        static_cast<uint8_t>((L2CAP_PSM >> 8) & 0xFF)
    };
    psmCharacteristic->setValue(psmValue, sizeof(psmValue));

    auto advertising = BLEDevice::getAdvertising();
    NimBLEAdvertisementData scanData;
    scanData.setName(DEVICE_NAME);
    advertising->setScanResponseData(scanData);
    advertising->addServiceUUID(TARGET_SERVICE_UUID);

    BLEDevice::startAdvertising();
    printf("Server waiting for connection requests [%lu free] [%lu min]\n", esp_get_free_heap_size(), esp_get_minimum_free_heap_size());

    // Status reporting loop
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(STATUS_INTERVAL_MS));
        const auto snapshot = l2capChannelCallbacks->snapshot();
        if (snapshot.connected && snapshot.totalRequestsReceived > 0) {
            uint64_t currentTime = esp_timer_get_time();
            double elapsedSeconds = (currentTime - snapshot.startTime) / 1000000.0;

            if (elapsedSeconds > 0) {
                double requestsPerSecond = snapshot.totalRequestsReceived / elapsedSeconds;
                double linkBytesPerSecond =
                    (snapshot.totalRequestBytes + snapshot.totalResponseBytes) / elapsedSeconds;

                // Heap monitoring
                size_t currentHeap = esp_get_free_heap_size();
                size_t minHeap = esp_get_minimum_free_heap_size();

                // Track heap for leak detection
                if (initialHeap == 0) {
                    initialHeap = currentHeap;
                    lastHeap = currentHeap;
                }

                // Check for consistent heap decrease
                if (currentHeap < lastHeap) {
                    heapDecreaseCount++;
                    if (heapDecreaseCount >= HEAP_LEAK_THRESHOLD) {
                        printf("\nWARNING: possible memory leak detected\n");
                        printf("Heap has decreased %zu times in a row\n", heapDecreaseCount);
                        printf("Initial heap: %zu, Current heap: %zu, Lost: %zu bytes\n",
                               initialHeap, currentHeap, initialHeap - currentHeap);
                    }
                } else if (currentHeap >= lastHeap) {
                    heapDecreaseCount = 0;  // Reset counter if heap stabilizes or increases
                }
                lastHeap = currentHeap;

                printf("\n=== STATUS UPDATE ===\n");
                printf("Requests: %zu, Responses: %zu (%.1f rps)\n",
                       snapshot.totalRequestsReceived, snapshot.totalResponsesSent, requestsPerSecond);
                printf("Request bytes: %" PRIu64 ", Response bytes: %" PRIu64 "\n",
                       snapshot.totalRequestBytes, snapshot.totalResponseBytes);
                printf("Payload bytes: %" PRIu64 "\n", snapshot.totalPayloadBytes);
                printf("Link bandwidth: %.2f KB/s (%.2f Mbps)\n",
                       linkBytesPerSecond / 1024.0, (linkBytesPerSecond * 8) / 1000000.0);
                printf("Errors: seq=%zu write=%zu queue=%zu\n",
                       snapshot.sequenceErrors, snapshot.responseWriteErrors, snapshot.responseQueueDrops);
                printf("Heap: %zu free (min: %zu), delta=%zd\n",
                       currentHeap, minHeap, initialHeap > 0 ? (ssize_t)(initialHeap - currentHeap) : 0);
                printf("==================\n");
            }
        }
    }
}
