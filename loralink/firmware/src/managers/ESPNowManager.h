#ifndef ESPNOW_MANAGER_H
#define ESPNOW_MANAGER_H

#include "../config.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>

struct EspNowTxItem {
  uint8_t mac[6];
  String message;
};

struct PendingPeer {
  uint8_t mac[6];
  char name[16];
  volatile bool pending;
};

class ESPNowManager {
public:
  static ESPNowManager &getInstance() {
    static ESPNowManager instance;
    return instance;
  }

  void init();
  void sendToAll(const String &message);
  void sendToPeer(const uint8_t *mac, const String &message);
  bool addPeer(const uint8_t *mac, const char *name);
  bool removePeer(const uint8_t *mac);
  bool poll(String &msgOut);
  void enqueue(const String &msg);

  // Main-loop deferred operations (called from TaskScheduler task)
  void processPendingPeer();
  void processTxQueue();

  bool espNowActive;

  // Last send status
  bool lastSendSuccess;
  String lastSentMessage;

  // Packet counters
  uint32_t rxCount;
  uint32_t txCount;
  uint32_t txDropCount;

private:
  ESPNowManager();

  // Receive queue (populated by callback, drained by scheduler)
  static const int QUEUE_SIZE = ESPNOW_QUEUE_SIZE;
  String rxQueue[ESPNOW_QUEUE_SIZE];
  volatile int qHead;
  volatile int qTail;

  // Deferred peer discovery — single slot, set from callback, consumed in main loop
  PendingPeer _pendingPeer;

  // Async TX queue — filled by sendToAll/sendToPeer, drained one-per-cycle
  EspNowTxItem _txQueue[ESPNOW_TX_QUEUE_SIZE];
  uint8_t _txQueueHead;
  uint8_t _txQueueTail;
  bool _enqueueTx(const uint8_t *mac, const String &message);

  // Static callbacks for esp_now
  static void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status);
  static void onDataRecv(const uint8_t *mac_addr, const uint8_t *data,
                         int data_len);
};

#endif // ESPNOW_MANAGER_H
