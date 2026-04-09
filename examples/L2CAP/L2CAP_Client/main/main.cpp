#include <NimBLEDevice.h>
#include <algorithm>
#include <cassert>
#include <inttypes.h>
#include <esp_hpl.hpp>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define TARGET_DEVICE_NAME   "BLE-Testing-iOS"
#define TARGET_SERVICE_UUID  "e7100001-61b3-4b3d-8b4d-7e3b51e2c001"
#define TARGET_PSM_CHAR_UUID "e7100002-61b3-4b3d-8b4d-7e3b51e2c001"
#define DEFAULT_L2CAP_PSM    192
#define L2CAP_MTU            5000
#define INITIAL_PAYLOAD_SIZE 512
#define MAX_APP_PAYLOAD_SIZE 4997
#define PAYLOAD_STEP_MS      2000
#define APP_FRAME_OVERHEAD   3
#define RESPONSE_TIMEOUT_MS  5000
#define STATUS_INTERVAL_MS   5000

const BLEAdvertisedDevice* theDevice = NULL;
BLEClient* theClient = NULL;
BLEL2CAPChannel* theChannel = NULL;
uint16_t resolvedPSM = DEFAULT_L2CAP_PSM;
bool psmResolved = false;

uint64_t bytesSent = 0;
uint64_t bytesReceived = 0;
uint64_t completedPayloadBytes = 0;
size_t currentPayloadSize = INITIAL_PAYLOAD_SIZE;
size_t maxPayloadSize = L2CAP_MTU - APP_FRAME_OVERHEAD;
uint32_t exchangesCompleted = 0;
uint64_t startTime = 0;
uint8_t nextSequenceNumber = 0;
uint64_t totalRttUs = 0;
uint64_t minRttUs = 0;
uint64_t maxRttUs = 0;
size_t responseTimeouts = 0;
size_t responseErrors = 0;
uint64_t payloadStageStartUs = 0;

SemaphoreHandle_t responseSemaphore = nullptr;
SemaphoreHandle_t stateMutex = nullptr;
bool waitingForResponse = false;
bool responseMatched = false;
uint8_t pendingSequenceNumber = 0;
size_t pendingPayloadSize = 0;
uint64_t pendingSendTimestampUs = 0;
bool reconnectRequested = false;

// Heap monitoring
size_t initialHeap = 0;
size_t lastHeap = 0;
size_t heapDecreaseCount = 0;
const size_t HEAP_LEAK_THRESHOLD = 10;  // Warn after 10 consecutive decreases

struct ClientStatusSnapshot {
    uint64_t bytesSent;
    uint64_t bytesReceived;
    uint64_t completedPayloadBytes;
    size_t currentPayloadSize;
    uint32_t exchangesCompleted;
    uint64_t startTime;
    uint64_t totalRttUs;
    uint64_t minRttUs;
    uint64_t maxRttUs;
    size_t responseTimeouts;
    size_t responseErrors;
    bool channelConnected;
};

static ClientStatusSnapshot snapshotClientStatus() {
    ClientStatusSnapshot snapshot{};
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    snapshot.bytesSent            = bytesSent;
    snapshot.bytesReceived        = bytesReceived;
    snapshot.completedPayloadBytes = completedPayloadBytes;
    snapshot.currentPayloadSize   = currentPayloadSize;
    snapshot.exchangesCompleted   = exchangesCompleted;
    snapshot.startTime            = startTime;
    snapshot.totalRttUs           = totalRttUs;
    snapshot.minRttUs             = minRttUs;
    snapshot.maxRttUs             = maxRttUs;
    snapshot.responseTimeouts     = responseTimeouts;
    snapshot.responseErrors       = responseErrors;
    snapshot.channelConnected     = theChannel && theChannel->isConnected();
    xSemaphoreGive(stateMutex);
    return snapshot;
}

class L2CAPChannelCallbacks: public BLEL2CAPChannelCallbacks {

public:
    void onConnect(NimBLEL2CAPChannel* channel, uint16_t negotiatedMTU) {
        printf("L2CAP connection established, MTU: %d\n", negotiatedMTU);
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        bytesSent = 0;
        bytesReceived = 0;
        completedPayloadBytes = 0;
        exchangesCompleted = 0;
        startTime = 0;
        nextSequenceNumber = 0;
        currentPayloadSize = INITIAL_PAYLOAD_SIZE;
        totalRttUs = 0;
        minRttUs = 0;
        maxRttUs = 0;
        responseTimeouts = 0;
        responseErrors = 0;
        payloadStageStartUs = 0;
        waitingForResponse = false;
        responseMatched = false;
        pendingSequenceNumber = 0;
        pendingPayloadSize = 0;
        pendingSendTimestampUs = 0;
        if (negotiatedMTU > APP_FRAME_OVERHEAD) {
            maxPayloadSize = std::min(static_cast<size_t>(MAX_APP_PAYLOAD_SIZE),
                                      static_cast<size_t>(negotiatedMTU - APP_FRAME_OVERHEAD));
            if (currentPayloadSize > maxPayloadSize) {
                currentPayloadSize = maxPayloadSize;
            }
        } else {
            maxPayloadSize = std::min(static_cast<size_t>(MAX_APP_PAYLOAD_SIZE),
                                      static_cast<size_t>(L2CAP_MTU - APP_FRAME_OVERHEAD));
            printf("L2CAP reported no usable MTU, keeping fallback payload limit %zu bytes\n", maxPayloadSize);
        }
        xSemaphoreGive(stateMutex);
        printf("Ping-pong payload range: initial=%u, cap=%zu (app cap=%u)\n",
               INITIAL_PAYLOAD_SIZE, maxPayloadSize, MAX_APP_PAYLOAD_SIZE);
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        payloadStageStartUs = esp_timer_get_time();
        xSemaphoreGive(stateMutex);
    }

    void onRead(NimBLEL2CAPChannel* channel, std::vector<uint8_t>& data) {
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        bytesReceived += data.size();
        bool valid = true;

        if (!waitingForResponse) {
            responseErrors++;
            printf("Unexpected response SDU of %zu bytes\n", data.size());
            xSemaphoreGive(stateMutex);
            return;
        }

        if (data.size() < APP_FRAME_OVERHEAD) {
            valid = false;
            printf("Malformed response SDU: %zu bytes\n", data.size());
        } else {
            const uint8_t seqno = data[0];
            const uint16_t payloadLen = (static_cast<uint16_t>(data[1]) << 8) | data[2];
            const size_t expectedFrameSize = static_cast<size_t>(payloadLen) + APP_FRAME_OVERHEAD;

            if (seqno != pendingSequenceNumber) {
                valid = false;
                printf("Response sequence mismatch, got %u expected %u\n", seqno, pendingSequenceNumber);
            } else if (payloadLen != pendingPayloadSize || expectedFrameSize != data.size()) {
                valid = false;
                printf("Response length mismatch, got payload=%u frame=%zu expected payload=%zu frame=%zu\n",
                       payloadLen, data.size(), pendingPayloadSize, pendingPayloadSize + APP_FRAME_OVERHEAD);
            } else {
                for (size_t i = 0; i < pendingPayloadSize; ++i) {
                    if (data[APP_FRAME_OVERHEAD + i] != (i & 0xFF)) {
                        valid = false;
                        printf("Response payload mismatch at index %zu\n", i);
                        break;
                    }
                }
            }
        }

        if (!valid) {
            responseErrors++;
            responseMatched = false;
        } else {
            const uint64_t rttUs = esp_timer_get_time() - pendingSendTimestampUs;
            responseMatched = true;
            totalRttUs += rttUs;
            minRttUs = minRttUs == 0 ? rttUs : std::min(minRttUs, rttUs);
            maxRttUs = std::max(maxRttUs, rttUs);
        }

        waitingForResponse = false;
        xSemaphoreGive(stateMutex);
        xSemaphoreGive(responseSemaphore);
    }
    void onDisconnect(NimBLEL2CAPChannel* channel) {
        printf("L2CAP disconnected\n");
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        bytesSent = 0;
        bytesReceived = 0;
        completedPayloadBytes = 0;
        exchangesCompleted = 0;
        startTime = 0;
        nextSequenceNumber = 0;
        currentPayloadSize = INITIAL_PAYLOAD_SIZE;
        totalRttUs = 0;
        minRttUs = 0;
        maxRttUs = 0;
        responseTimeouts = 0;
        responseErrors = 0;
        payloadStageStartUs = 0;
        waitingForResponse = false;
        responseMatched = false;
        pendingSequenceNumber = 0;
        pendingPayloadSize = 0;
        pendingSendTimestampUs = 0;
        xSemaphoreGive(stateMutex);
    }
};

class MyClientCallbacks: public BLEClientCallbacks {

    void onConnect(BLEClient* pClient) {
        printf("GAP connected\n");
        pClient->setDataLen(251);
    }

    void onDisconnect(BLEClient* pClient, int reason) {
        printf("GAP disconnected (reason: %d)\n", reason);
        reconnectRequested = false;
        theDevice = NULL;
        delete theChannel;
        theChannel = NULL;
        resolvedPSM = DEFAULT_L2CAP_PSM;
        psmResolved = false;
        BLEDevice::deleteClient(pClient);
        theClient = NULL;
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        BLEDevice::getScan()->start(0, false);
    }
};

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {

    void onResult(const BLEAdvertisedDevice* advertisedDevice) {
        if (theDevice) { return; }
        printf("BLE Advertised Device found: %s\n", advertisedDevice->toString().c_str());

        const bool nameMatch = advertisedDevice->haveName() && advertisedDevice->getName() == TARGET_DEVICE_NAME;
        const bool serviceMatch = advertisedDevice->isAdvertisingService(NimBLEUUID(TARGET_SERVICE_UUID));

        if (nameMatch || serviceMatch) {
            printf("Found target device (nameMatch=%d, serviceMatch=%d)\n", nameMatch, serviceMatch);
            BLEDevice::getScan()->stop();
            theDevice = advertisedDevice;
        }
    }
};

void statusTask(void *pvParameters) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(STATUS_INTERVAL_MS));
        const auto snapshot = snapshotClientStatus();
        if (snapshot.startTime > 0 && snapshot.exchangesCompleted > 0 && snapshot.channelConnected) {
            uint64_t currentTime = esp_timer_get_time();
            double elapsedSeconds = (currentTime - snapshot.startTime) / 1000000.0;
            double linkBytesPerSecond = 0.0;
            double goodputBytesPerSecond = 0.0;
            if (elapsedSeconds > 0.0) {
                linkBytesPerSecond = (snapshot.bytesSent + snapshot.bytesReceived) / elapsedSeconds;
                goodputBytesPerSecond = snapshot.completedPayloadBytes / elapsedSeconds;
            }
            const double avgRttMs = snapshot.exchangesCompleted > 0 ? (snapshot.totalRttUs / 1000.0) / snapshot.exchangesCompleted : 0.0;

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
            printf("Exchanges completed: %lu\n", (unsigned long)snapshot.exchangesCompleted);
            printf("Total request bytes: %" PRIu64 "\n", snapshot.bytesSent);
            printf("Total response bytes: %" PRIu64 "\n", snapshot.bytesReceived);
            printf("Payload: %zu bytes, Elapsed: %.1f s\n", snapshot.currentPayloadSize, elapsedSeconds);
            printf("Goodput: %.2f KB/s, Link: %.2f KB/s (%.2f Mbps)\n",
                   goodputBytesPerSecond / 1024.0,
                   linkBytesPerSecond / 1024.0,
                   (linkBytesPerSecond * 8) / 1000000.0);
            printf("RTT avg/min/max: %.2f / %.2f / %.2f ms\n",
                   avgRttMs, snapshot.minRttUs / 1000.0, snapshot.maxRttUs / 1000.0);
            printf("Response faults: errors=%zu timeouts=%zu\n", snapshot.responseErrors, snapshot.responseTimeouts);
            printf("Heap: %zu free (min: %zu), delta=%zd\n",
                   currentHeap, minHeap, initialHeap > 0 ? (ssize_t)(initialHeap - currentHeap) : 0);
            printf("==================\n\n");
        }
    }
}

static void resetConnectionState(const char* reason, bool restartScan = true) {
    printf("%s\n", reason);
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    waitingForResponse = false;
    responseMatched = false;
    pendingPayloadSize = 0;
    pendingSequenceNumber = 0;
    pendingSendTimestampUs = 0;
    reconnectRequested = false;
    xSemaphoreGive(stateMutex);

    if (theChannel) {
        delete theChannel;
        theChannel = NULL;
    }

    if (theClient) {
        if (theClient->isConnected()) {
            theClient->disconnect();
        }
        BLEDevice::deleteClient(theClient);
        theClient = NULL;
    }

    resolvedPSM = DEFAULT_L2CAP_PSM;
    psmResolved = false;
    theDevice = NULL;

    if (restartScan) {
        BLEDevice::getScan()->start(0, false);
    }
}

static void abortCurrentSession(const char* reason) {
    printf("%s\n", reason);
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    waitingForResponse = false;
    responseMatched = false;
    pendingPayloadSize = 0;
    pendingSequenceNumber = 0;
    pendingSendTimestampUs = 0;
    reconnectRequested = true;
    xSemaphoreGive(stateMutex);

    if (theClient && theClient->isConnected()) {
        theClient->disconnect();
    }
}

void connectTask(void *pvParameters) {
    while (true) {
        if (reconnectRequested) {
            if (!theClient || !theClient->isConnected()) {
                resetConnectionState("forcing reconnect after session abort");
            } else {
                vTaskDelay(200 / portTICK_PERIOD_MS);
            }
            continue;
        }

        if (theClient && !theClient->isConnected()) {
            resetConnectionState("detected dead GAP client, restarting scan");
            vTaskDelay(200 / portTICK_PERIOD_MS);
            continue;
        }

        if (!theDevice) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        if (!theClient) {
            theClient = BLEDevice::createClient();
            theClient->setConnectionParams(6, 6, 0, 42);

            auto callbacks = new MyClientCallbacks();
            theClient->setClientCallbacks(callbacks);

            auto success = theClient->connect(theDevice);
            if (!success) {
                printf("Error: Could not connect to device\n");
                BLEDevice::deleteClient(theClient);
                theClient = NULL;
                theDevice = NULL;
                BLEDevice::getScan()->start(0, false);
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                continue;
            }
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            continue;
        }

        if (!psmResolved) {
            NimBLERemoteService* service = theClient->getService(NimBLEUUID(TARGET_SERVICE_UUID));
            if (!service) {
                printf("failed to discover remote PSM service, disconnecting\n");
                theClient->disconnect();
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                continue;
            }

            NimBLERemoteCharacteristic* characteristic = service->getCharacteristic(NimBLEUUID(TARGET_PSM_CHAR_UUID));
            if (!characteristic) {
                printf("failed to find remote PSM characteristic, disconnecting\n");
                theClient->disconnect();
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                continue;
            }

            NimBLEAttValue value = characteristic->readValue();
            if (value.size() < 2) {
                printf("failed to read remote PSM value (size=%zu), disconnecting\n", value.size());
                theClient->disconnect();
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                continue;
            }

            resolvedPSM = static_cast<uint16_t>(value[0]) | (static_cast<uint16_t>(value[1]) << 8);
            psmResolved = true;
            printf("Resolved remote PSM over GATT: %u\n", resolvedPSM);
        }

        if (!theChannel) {
            theChannel = BLEL2CAPChannel::connect(theClient, resolvedPSM, L2CAP_MTU, new L2CAPChannelCallbacks());
            if (!theChannel) {
                printf("failed to open l2cap channel on PSM %u\n", resolvedPSM);
                theClient->disconnect();
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                continue;
            }

            vTaskDelay(500 / portTICK_PERIOD_MS);
            continue;
        }

        if (!theChannel->isConnected()) {
            resetConnectionState("l2cap channel not connected, restarting scan");
            vTaskDelay(200 / portTICK_PERIOD_MS);
            continue;
        }

        while (theChannel->isConnected()) {
            size_t payloadSize = 0;
            uint8_t sequenceNumber = 0;
            xSemaphoreTake(stateMutex, portMAX_DELAY);
            payloadSize = currentPayloadSize;
            sequenceNumber = nextSequenceNumber;
            xSemaphoreGive(stateMutex);

            // Create framed packet: [seqno 8bit] [16bit payload length] [payload]
            std::vector<uint8_t> packet;
            packet.reserve(3 + payloadSize);

            // Add sequence number (8 bits)
            packet.push_back(sequenceNumber);

            // Add payload length (16 bits, big endian - network byte order)
            uint16_t payloadLen = payloadSize;
            packet.push_back((payloadLen >> 8) & 0xFF);  // High byte first
            packet.push_back(payloadLen & 0xFF);         // Low byte second

            // Add payload
            for (size_t i = 0; i < payloadSize; i++) {
                packet.push_back(i & 0xFF);
            }

            xSemaphoreTake(responseSemaphore, 0);
            xSemaphoreTake(stateMutex, portMAX_DELAY);
            pendingSequenceNumber = sequenceNumber;
            pendingPayloadSize = payloadSize;
            waitingForResponse = true;
            responseMatched = false;
            pendingSendTimestampUs = esp_timer_get_time();
            xSemaphoreGive(stateMutex);

            if (theChannel->write(packet)) {
                xSemaphoreTake(stateMutex, portMAX_DELAY);
                if (startTime == 0) {
                    startTime = pendingSendTimestampUs;
                }
                bytesSent += packet.size();
                xSemaphoreGive(stateMutex);

                if (xSemaphoreTake(responseSemaphore, pdMS_TO_TICKS(RESPONSE_TIMEOUT_MS)) != pdTRUE) {
                    xSemaphoreTake(stateMutex, portMAX_DELAY);
                    responseTimeouts++;
                    xSemaphoreGive(stateMutex);
                    abortCurrentSession("response timeout, disconnecting");
                    break;
                }

                xSemaphoreTake(stateMutex, portMAX_DELAY);
                const bool matchedResponse = responseMatched;
                xSemaphoreGive(stateMutex);
                if (!matchedResponse) {
                    abortCurrentSession("response validation failed, disconnecting");
                    break;
                }

                xSemaphoreTake(stateMutex, portMAX_DELAY);
                exchangesCompleted++;
                completedPayloadBytes += pendingPayloadSize;
                nextSequenceNumber++;
                xSemaphoreGive(stateMutex);

                const uint64_t nowUs = esp_timer_get_time();
                bool payloadIncreased = false;
                size_t newPayloadSize = 0;
                xSemaphoreTake(stateMutex, portMAX_DELAY);
                if (payloadStageStartUs == 0) {
                    payloadStageStartUs = nowUs;
                }
                if (currentPayloadSize < maxPayloadSize &&
                    nowUs - payloadStageStartUs >= (PAYLOAD_STEP_MS * 1000ULL)) {
                    newPayloadSize = std::min(maxPayloadSize, currentPayloadSize * 2);
                    if (newPayloadSize != currentPayloadSize) {
                        currentPayloadSize = newPayloadSize;
                        payloadStageStartUs = nowUs;
                        payloadIncreased = true;
                    }
                }
                xSemaphoreGive(stateMutex);
                if (payloadIncreased) {
                    printf("\n=== Increasing payload size to %zu bytes after %.1f seconds ===\n",
                           newPayloadSize, PAYLOAD_STEP_MS / 1000.0);
                }
            } else {
                abortCurrentSession("failed to send, disconnecting");
                break;
            }
        }

        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

extern "C"
void app_main(void) {
    // Install high performance logging before any output
    esp_hpl::HighPerformanceLogger::init();

    printf("Starting L2CAP client example\n");

    responseSemaphore = xSemaphoreCreateBinary();
    assert(responseSemaphore != nullptr);
    stateMutex = xSemaphoreCreateMutex();
    assert(stateMutex != nullptr);

    xTaskCreate(connectTask, "connectTask", 5000, NULL, 1, NULL);
    xTaskCreate(statusTask, "statusTask", 3000, NULL, 1, NULL);

    BLEDevice::init("L2CAP-Client");
    BLEDevice::setMTU(BLE_ATT_MTU_MAX);

    auto scan = BLEDevice::getScan();
    auto callbacks = new MyAdvertisedDeviceCallbacks();
    scan->setScanCallbacks(callbacks);
    scan->setInterval(1349);
    scan->setWindow(449);
    scan->setActiveScan(true);
    scan->start(0, false);

    // Main task just waits
    while (true) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
