#include <NimBLEDevice.h>

// See the following for generating UUIDs:
// https://www.uuidgenerator.net/

// The remote service we wish to connect to.
static BLEUUID serviceUUID("dcbc7255-1e9e-49a0-a360-b0430b6c6905");
// The characteristic of the remote service we are interested in.
static BLEUUID    charUUID("371a55c8-f251-4ad2-90b3-c7c195b049be");

#define L2CAP_CHANNEL        150
#define L2CAP_MTU            5000

BLEAdvertisedDevice* theDevice = NULL;
BLEClient* theClient = NULL;
BLEL2CAPClient* theL2CAPClient = NULL;

size_t bytesSent = 0;
size_t bytesReceived = 0;

class L2CAPClientCallbacks: public BLEL2CAPClientCallbacks {

public:
    void onConnect(NimBLEL2CAPClient* pClient) {
        printf("L2CAP connection established\n");
    }

    void onMTUChange(NimBLEL2CAPClient* pClient, uint16_t mtu) {
        printf("L2CAP MTU changed to %d\n", mtu);
    }

    void onRead(NimBLEL2CAPClient* pClient, std::vector<uint8_t>& data) {
        printf("L2CAP read %d bytes", data.size());
    }
    void onDisconnect(NimBLEL2CAPClient* pClient) {
        printf("L2CAP disconnected\n");
    }
};

class MyClientCallbacks: public BLEClientCallbacks {

    void onConnect(BLEClient* pclient) {
        printf("GAP connected\n");

        auto callbacks = new L2CAPClientCallbacks();
        theL2CAPClient = BLEL2CAPClient::createClient(L2CAP_CHANNEL, L2CAP_MTU, callbacks);
        theL2CAPClient->connect(pclient);
    }

    void onDisconnect(BLEClient* pclient, int reason) {
        printf("GAP disconnected (reason: %d)\n", reason);
    }
};

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {

    void onResult(BLEAdvertisedDevice* advertisedDevice) {
        printf("BLE Advertised Device found: %s\n", advertisedDevice->toString().c_str());

        if (!advertisedDevice->haveServiceUUID()) { return; }
        if (!advertisedDevice->isAdvertisingService(serviceUUID)) { return; }

        printf("Found the device we're interested in!\n");
        BLEDevice::getScan()->stop();

        // Hand over the device to the other task
        theDevice = advertisedDevice;
    }
};

void connectTask(void *pvParameters) {

    while (true) {
        
        if (!theDevice) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        if (!theClient) {
            theClient = BLEDevice::createClient();
            auto callbacks = new MyClientCallbacks();
            theClient->setClientCallbacks(callbacks);

            auto success = theClient->connect(theDevice);
            if (!success) {
                printf("Error: Could not connect to device\n");
                break;
            }
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;            
        }

        if (!theL2CAPClient) {
            printf("l2cap client not initialized\n");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        if (!theL2CAPClient->isConnected()) {
            printf("l2cap client not connected\n");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        std::vector<uint8_t> data(5000, 0);
        if (theL2CAPClient->write(data)) {
            bytesSent += data.size();
        } else {
            printf("failed to send!\n");
        }
        vTaskDelay(1);
    }
}

extern "C"
void app_main(void) {
    printf("Starting L2CAP client example\n");

    xTaskCreate(connectTask, "connectTask", 5000, NULL, 1, NULL);

    BLEDevice::init("L2CAP-Client");

    auto scan = BLEDevice::getScan();
    auto callbacks = new MyAdvertisedDeviceCallbacks();
    scan->setScanCallbacks(callbacks);
    scan->setInterval(1349);
    scan->setWindow(449);
    scan->setActiveScan(true);
    scan->start(5 * 1000, false);

    int numberOfSeconds = 0;

    while (bytesSent == 0) {
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }

    while (true) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        int bytesSentPerSeconds = bytesSent / ++numberOfSeconds;
        printf("Bandwidth: %d bytes per second (%d kbps)\n", bytesSentPerSeconds, bytesSentPerSeconds / 1024);
    }
}
