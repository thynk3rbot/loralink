#ifndef BLE_MANAGER_H
#define BLE_MANAGER_H

#include "../config.h"
#include <Arduino.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

class BLEManager {
public:
  static BLEManager &getInstance() {
    static BLEManager instance;
    return instance;
  }

  void init();
  void boostAdvertising(bool active);
  void notify(const String &text);
  bool poll(String &cmdOut);
  void enqueue(const String &cmd);

  bool deviceConnected;

private:
  BLEManager();
  BLEServer *pServer;
  BLECharacteristic *pTxCharacteristic;

  static const int BLE_QUEUE_SIZE = 4;
  String cmdQueue[BLE_QUEUE_SIZE];
  volatile int qHead;
  volatile int qTail;
};

#endif // BLE_MANAGER_H
