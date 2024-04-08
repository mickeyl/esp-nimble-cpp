#include <NimBLEDevice.h>

// See the following for generating UUIDs:
// https://www.uuidgenerator.net/

#define SERVICE_UUID        "dcbc7255-1e9e-49a0-a360-b0430b6c6905"
#define CHARACTERISTIC_UUID "371a55c8-f251-4ad2-90b3-c7c195b049be"
#define L2CAP_CHANNEL        150
#define L2CAP_MTU            5000

class L2CAPServiceCallbacks: public BLEL2CAPServiceCallbacks {

public:
    bool shouldAcceptConnection(NimBLEL2CAPService* pService) { return true; }
    void onConnect(NimBLEL2CAPService* pService) {
        printf("L2CAP connection established\n");
    }

    void onRead(NimBLEL2CAPService* pService, std::vector<uint8_t>& data) {
        printf("L2CAP read %d bytes", data.size());
    }
    void onDisconnect(NimBLEL2CAPService* pService) {
        printf("L2CAP disconnected\n");
    }
};

extern "C"
void app_main(void) {
      printf("Starting L2CAP server example\n");

      BLEDevice::init("L2CAP-Server");

      auto cocServer = BLEDevice::createL2CAPServer();
      auto l2capServiceCallbacks = new L2CAPServiceCallbacks();
      cocServer->createService(L2CAP_CHANNEL, L2CAP_MTU, l2capServiceCallbacks);
      
      auto server = BLEDevice::createServer();
      auto service = server->createService(SERVICE_UUID);
      auto characteristic = service->createCharacteristic(CHARACTERISTIC_UUID, NIMBLE_PROPERTY::READ);
      characteristic->setValue(L2CAP_CHANNEL);
      service->start();
      auto advertising = BLEDevice::getAdvertising();
      advertising->addServiceUUID(SERVICE_UUID);
      advertising->setScanResponse(true);

      BLEDevice::startAdvertising();
      printf("Server ready for connection requests.\n");

      while (true) {
          vTaskDelay(1000 / portTICK_PERIOD_MS);
      }
}
