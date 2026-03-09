#ifndef LORA_MANAGER_H
#define LORA_MANAGER_H

#include "../config.h"
#include "../crypto.h"
#include "DataManager.h"
#include "DisplayManager.h"
#include <Arduino.h>
#include <RadioLib.h>
#include <cstring>

void IRAM_ATTR setFlag(void);

struct PendingAck {
  String targetId;
  uint8_t targetSid;
  String commandText;
  uint8_t cmd;
  uint8_t val;
  bool isBinary;
  bool useFailover;
  int retryCount;
  unsigned long lastAttemptMs;
  bool active;
};

// ── Radio State Machine ─────────────────────────────────────────────────────
enum class RadioState : uint8_t {
  RADIO_IDLE = 0,
  RADIO_RX = 1,
  RADIO_TX = 2
};

class LoRaManager {
public:
  static void SetCallback(void (*cb)(const String &, CommInterface));
  static LoRaManager &getInstance() {
    static LoRaManager instance;
    return instance;
  }

  void Init();
  void SendLoRa(const String &text);
  void SendLegacyLoRa(const String &text);
  void SendLoRaBinary(uint8_t targetSid, uint8_t cmd, uint8_t val);
  void SendRawLoRa(const uint8_t *buf, size_t len); // Send pre-encrypted buffer
  void SendHeartbeat();
  void HandleRx();
  void ProcessPacket(uint8_t *rxEncBuf, int size);
  void SetKey(const uint8_t *newKey);
  void DumpDiagnostics();
  void clearPendingAck(const String &targetId, bool isBinary = false,
                       uint8_t cmd = 0);
  void QueueReliableCommand(const String &targetId, const String &commandText);
  void QueueReliableBinaryCommand(const String &targetId, uint8_t cmd,
                                  uint8_t val);
  void QueueFailoverPing(const String &targetId);
  void periodicTick();

  bool loraActive;
  bool beaconActive;
  unsigned long lastBeaconMs;
  volatile bool receivedFlag;

  // Radio state machine
  RadioState getRadioState() const { return _radioState; }

  // Packet counters
  uint32_t getTxCount() const { return _txCount; }
  uint32_t getRxCount() const { return _rxCount; }
  uint32_t getTxDropCount() const { return _txDropCount; }

  int lastRssi;
  float lastSnr;
  String lastMsgReceived;
  String lastMsgSent;

  uint8_t currentKey[16];

private:
  LoRaManager();
  SX1262 *radio;

  // ── Async TX state machine ──────────────────────────────────────────────
  RadioState _radioState = RadioState::RADIO_IDLE;
  volatile bool _transmittedFlag = false;
  unsigned long _txStartMs = 0;

  static void IRAM_ATTR _setTxFlag();
  bool _startAsyncTx(const uint8_t *data, size_t len);
  void _onTxComplete();
  void _enterRx();

  // ── Packet counters ─────────────────────────────────────────────────────
  uint32_t _txCount = 0;
  uint32_t _rxCount = 0;
  uint32_t _txDropCount = 0;

  static const int MAX_PENDING_ACKS = 5;
  PendingAck ackQueue[MAX_PENDING_ACKS];

  // Dirty-flag heartbeat suppression — skip TX when state hasn't changed
  float _lastHBBat = -1.0f; // Last transmitted battery voltage
  String _lastHBRst;        // Last transmitted reset reason
  uint8_t _hbSkipCount = 0; // Consecutive skips since last forced TX
  static const uint8_t HB_FORCE_INTERVAL =
      12; // Force TX every N skips (~60 min at 300s)

  uint32_t seenMsgHashes[HASH_BUFFER_SIZE];
  int hashIndex;
  uint8_t encBuf[ENCRYPTED_PACKET_SIZE];
  MessagePacket txPacket;
  MessagePacket rxPacket;

  uint32_t getMsgHash(MessagePacket *p);
  uint16_t calculateChecksum(MessagePacket *p);
  bool hasSeenMessage(uint32_t hash);
  void markMessageSeen(uint32_t hash);
};

#endif // LORA_MANAGER_H
