#include "BLEManager.h"
#include "../utils/DebugMacros.h"
#include "CommandManager.h"
#include "DataManager.h"
#include <WiFi.h>
#include <esp_mac.h>
#include <string>

#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHAR_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

class MyServerCallbacks : public BLEServerCallbacks {
public:
  void onConnect(BLEServer *pServer, esp_ble_gatts_cb_param_t *param) override {
    BLEManager::getInstance().deviceConnected = true;
    Serial.println("BLE: Connected");
  };
  void onDisconnect(BLEServer *pServer) override {
    BLEManager::getInstance().deviceConnected = false;
    Serial.println("BLE: Disconnected");
    pServer->getAdvertising()->start();
  }
};

class MyCallbacks : public BLECharacteristicCallbacks {
public:
  void onWrite(BLECharacteristic *pCharacteristic) override {
    std::string value = pCharacteristic->getValue();
    if (value.length() > 0) {
      String msg = String(value.c_str());
      msg.trim();
      BLEManager::getInstance().enqueue(msg);
    }
  }
};

BLEManager::BLEManager() {
  deviceConnected = false;
  pServer = NULL;
  pTxCharacteristic = NULL;
  qHead = 0;
  qTail = 0;
}

void BLEManager::enqueue(const String &cmd) {
  int next = (qTail + 1) % BLE_QUEUE_SIZE;
  if (next == qHead)
    return;
  cmdQueue[qTail] = cmd;
  qTail = next;
}

bool BLEManager::poll(String &cmdOut) {
  if (qHead == qTail)
    return false;
  cmdOut = cmdQueue[qHead];
  qHead = (qHead + 1) % BLE_QUEUE_SIZE;
  return true;
}

void BLEManager::init() {
  DataManager &data = DataManager::getInstance();

  // [FIX] Disabled Base MAC Spoofing entirely
  // Calling esp_base_mac_addr_set() while radio initialization is pending
  // causes an RTCWDT_RTC_RST (watchdog reset) on the ESP32-S3 board.

  String devName = data.myId;
  if (devName.length() == 0)
    devName = "HT-LoRa";

  BLEDevice::init(devName.c_str());

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(
      CHAR_UUID_RX,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pRxCharacteristic->setCallbacks(new MyCallbacks());

  pTxCharacteristic = pService->createCharacteristic(
      CHAR_UUID_TX,
      BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ);

  BLE2902 *p2902 = new BLE2902();
  p2902->setNotifications(true);
  p2902->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE);
  pTxCharacteristic->addDescriptor(p2902);

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();

  // [CONFIG] DUAL PACKET STRATEGY (Discovery Fix)
  // Solution: UUID in primary Advertisement, Name in Scan Response.
  pAdvertising->addServiceUUID(SERVICE_UUID);

  BLEAdvertisementData oAdvertisementData = BLEAdvertisementData();
  oAdvertisementData.setFlags(0x06); // General Discoverable
  oAdvertisementData.setCompleteServices(BLEUUID(SERVICE_UUID));
  pAdvertising->setAdvertisementData(oAdvertisementData);

  BLEAdvertisementData oScanResponseData = BLEAdvertisementData();
  oScanResponseData.setName(devName.c_str());
  pAdvertising->setScanResponseData(oScanResponseData);

  // Enable Scan Response
  pAdvertising->setScanResponse(true);

  // [CONFIG] RELAXED TIMING
  pAdvertising->setMinInterval(0x100); // 160ms
  pAdvertising->setMaxInterval(0x200); // 320ms

  // [CONFIG] PASSIVE CONNECTION PARAMS
  pAdvertising->setMinPreferred(0);
  pAdvertising->setMaxPreferred(0);

  pAdvertising->start();
  delay(100);
  Serial.println("BLE: Started as " + devName);
}

void BLEManager::boostAdvertising(bool active) {
  if (!pServer)
    return;
  BLEAdvertising *pAdvertising = pServer->getAdvertising();
  pAdvertising->stop();

  if (active) {
    LOG_PRINTLN("BLE: Boosting advertisement rate (Proactive Fallback)");
    pAdvertising->setMinInterval(0x20); // 20ms
    pAdvertising->setMaxInterval(0x40); // 40ms
  } else {
    LOG_PRINTLN("BLE: Normalizing advertisement rate");
    pAdvertising->setMinInterval(0x100); // 160ms
    pAdvertising->setMaxInterval(0x200); // 320ms
  }

  pAdvertising->start();
}

void BLEManager::notify(const String &text) {
  if (deviceConnected && pTxCharacteristic) {
    pTxCharacteristic->setValue((uint8_t *)text.c_str(), text.length());
    pTxCharacteristic->notify();
    delay(20);
  }
}
