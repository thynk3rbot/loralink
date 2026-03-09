#include "ESPNowManager.h"
#include "../utils/DebugMacros.h"
#include "DataManager.h"
#include <esp_wifi.h>

// Static instance pointer for callbacks
static ESPNowManager *_espNowInstance = nullptr;

ESPNowManager::ESPNowManager() {
  espNowActive = false;
  lastSendSuccess = true;
  rxCount = 0;
  txCount = 0;
  txDropCount = 0;
  qHead = 0;
  qTail = 0;
  _txQueueHead = 0;
  _txQueueTail = 0;
  _pendingPeer.pending = false;
  _espNowInstance = this;
}

// ── Static Callbacks (WiFi task context — no logging, no NVS, no heap alloc) ─

void ESPNowManager::onDataSent(const uint8_t *mac_addr,
                               esp_now_send_status_t status) {
  if (!_espNowInstance)
    return;
  _espNowInstance->lastSendSuccess = (status == ESP_NOW_SEND_SUCCESS);
  if (status == ESP_NOW_SEND_SUCCESS) {
    _espNowInstance->txCount++;
  } else {
    _espNowInstance->txDropCount++;
  }
  // No LOG_PRINTF — this runs in WiFi task context (core 0)
}

void ESPNowManager::onDataRecv(const uint8_t *mac_addr, const uint8_t *data,
                               int data_len) {
  if (!_espNowInstance || data_len <= 0 || data_len > 250)
    return;

  // Auto-discovery: defer addPeer() to main loop — avoids NVS write in callback
  DataManager &dm = DataManager::getInstance();
  bool found = false;
  for (int i = 0; i < dm.numEspNowPeers; i++) {
    if (dm.espNowPeers[i].active &&
        memcmp(dm.espNowPeers[i].mac, mac_addr, 6) == 0) {
      found = true;
      break;
    }
  }

  if (!found && !_espNowInstance->_pendingPeer.pending) {
    // Single-slot deferred peer: consumed by processPendingPeer() in main loop
    memcpy(_espNowInstance->_pendingPeer.mac, mac_addr, 6);
    snprintf(_espNowInstance->_pendingPeer.name,
             sizeof(_espNowInstance->_pendingPeer.name), "AUTO-%02X%02X%02X",
             mac_addr[3], mac_addr[4], mac_addr[5]);
    _espNowInstance->_pendingPeer.pending = true;
    // No LOG_PRINTF — callback context
  }

  // Enqueue received message for main loop processing
  char buf[251];
  int len = min(data_len, 250);
  memcpy(buf, data, len);
  buf[len] = '\0';

  String msg = String(buf);
  msg.trim();

  if (msg.length() > 0) {
    _espNowInstance->enqueue(msg);
    _espNowInstance->rxCount++;
    // No LOG_PRINTF — callback context
  }
}

// ── Main-Loop Deferred Operations ─────────────────────────────────────────────

void ESPNowManager::processPendingPeer() {
  if (!_pendingPeer.pending)
    return;
  // Safe to call addPeer() here — we're in main loop / TaskScheduler context
  addPeer(_pendingPeer.mac, _pendingPeer.name);
  LOG_PRINTF("ESPNOW: Auto-discovered peer: %s\n", _pendingPeer.name);
  _pendingPeer.pending = false;
}

bool ESPNowManager::_enqueueTx(const uint8_t *mac, const String &message) {
  uint8_t next = (_txQueueTail + 1) % ESPNOW_TX_QUEUE_SIZE;
  if (next == _txQueueHead) {
    txDropCount++;
    return false; // TX queue full — drop
  }
  memcpy(_txQueue[_txQueueTail].mac, mac, 6);
  _txQueue[_txQueueTail].message = message;
  _txQueueTail = next;
  return true;
}

void ESPNowManager::processTxQueue() {
  if (_txQueueHead == _txQueueTail)
    return; // Queue empty

  EspNowTxItem &item = _txQueue[_txQueueHead];
  esp_err_t result = esp_now_send(
      item.mac, (const uint8_t *)item.message.c_str(), item.message.length());
  if (result != ESP_OK) {
    txDropCount++;
    LOG_PRINTF("ESPNOW: TX queue send error: %d\n", (int)result);
  }
  _txQueueHead = (_txQueueHead + 1) % ESPNOW_TX_QUEUE_SIZE;
}

// ── Public API ────────────────────────────────────────────────────────────────

void ESPNowManager::init() {
  DataManager &data = DataManager::getInstance();

  if (!data.espNowEnabled) {
    LOG_PRINTLN("ESPNOW: Disabled in config");
    return;
  }

  // WiFi must be in STA mode (or AP+STA) for ESP-NOW
  if (WiFi.getMode() == WIFI_OFF) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(); // Ensure we are on the primary channel initially
  }

  // Set channel
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(data.espNowChannel, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) {
    LOG_PRINTLN("ESPNOW: Init FAILED");
    return;
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  // Register saved peers
  for (int i = 0; i < data.numEspNowPeers; i++) {
    if (data.espNowPeers[i].active) {
      esp_now_peer_info_t peerInfo = {};
      memcpy(peerInfo.peer_addr, data.espNowPeers[i].mac, 6);
      peerInfo.channel = 0; // Use system channel
      peerInfo.ifidx = WIFI_IF_STA;
      peerInfo.encrypt = false;

      if (esp_now_add_peer(&peerInfo) == ESP_OK) {
        LOG_PRINTF("ESPNOW: Peer %s added (%02X:%02X:%02X:%02X:%02X:%02X)\n",
                   data.espNowPeers[i].name, data.espNowPeers[i].mac[0],
                   data.espNowPeers[i].mac[1], data.espNowPeers[i].mac[2],
                   data.espNowPeers[i].mac[3], data.espNowPeers[i].mac[4],
                   data.espNowPeers[i].mac[5]);
      }
    }
  }

  espNowActive = true;
  LOG_PRINTLN("ESPNOW: Initialized OK");
}

bool ESPNowManager::addPeer(const uint8_t *mac, const char *name) {
  if (!espNowActive)
    return false;

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0; // Use system channel
  peerInfo.ifidx = WIFI_IF_STA;
  peerInfo.encrypt = false;

  esp_err_t result = esp_now_add_peer(&peerInfo);
  if (result == ESP_OK) {
    // Save to DataManager (NVS write — safe here, we're in main loop context)
    DataManager &data = DataManager::getInstance();
    int slot = -1;
    for (int i = 0; i < ESPNOW_MAX_PEERS; i++) {
      if (!data.espNowPeers[i].active) {
        slot = i;
        break;
      }
    }
    if (slot >= 0) {
      data.SaveESPNowPeer(slot, mac, name);
    }
    LOG_PRINTF("ESPNOW: Peer added: %s\n", name);
    return true;
  }
  LOG_PRINTLN("ESPNOW: Failed to add peer");
  return false;
}

bool ESPNowManager::removePeer(const uint8_t *mac) {
  if (!espNowActive)
    return false;
  esp_err_t result = esp_now_del_peer(mac);
  if (result == ESP_OK) {
    DataManager &data = DataManager::getInstance();
    for (int i = 0; i < ESPNOW_MAX_PEERS; i++) {
      if (data.espNowPeers[i].active &&
          memcmp(data.espNowPeers[i].mac, mac, 6) == 0) {
        data.RemoveESPNowPeer(i);
        break;
      }
    }
    return true;
  }
  return false;
}

void ESPNowManager::sendToAll(const String &message) {
  if (!espNowActive)
    return;

  DataManager &data = DataManager::getInstance();
  int queued = 0;
  for (int i = 0; i < data.numEspNowPeers; i++) {
    if (data.espNowPeers[i].active) {
      if (_enqueueTx(data.espNowPeers[i].mac, message))
        queued++;
    }
  }
  // Log once per broadcast, not once per peer
  if (queued > 0) {
    DataManager::getInstance().LogMessage(
        "ESPNOW_TX", 0, "BC(" + String(queued) + "): " + message);
  }
  lastSentMessage = message;
}

void ESPNowManager::sendToPeer(const uint8_t *mac, const String &message) {
  if (!espNowActive)
    return;
  _enqueueTx(mac, message);
}

void ESPNowManager::enqueue(const String &msg) {
  int next = (qTail + 1) % QUEUE_SIZE;
  if (next == qHead)
    return; // queue full
  rxQueue[qTail] = msg;
  qTail = next;
}

bool ESPNowManager::poll(String &msgOut) {
  if (qHead == qTail)
    return false;
  msgOut = rxQueue[qHead];
  qHead = (qHead + 1) % QUEUE_SIZE;
  return true;
}
