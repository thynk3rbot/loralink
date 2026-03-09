#include "LoRaManager.h"
#include "../utils/DebugMacros.h"
#include "BinaryManager.h"
#include "MQTTManager.h"
#include "PerformanceManager.h"
#include "ScheduleManager.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <SPI.h>

// ISR Trampoline — RX complete (DIO1 in receive mode)
LoRaManager *_loraInstance = NULL;
void IRAM_ATTR setFlag(void) {
  if (_loraInstance) {
    _loraInstance->receivedFlag = true;
  }
}

// ISR Trampoline — TX complete (DIO1 in transmit mode)
void IRAM_ATTR LoRaManager::_setTxFlag() {
  if (_loraInstance) {
    _loraInstance->_transmittedFlag = true;
  }
}

// Global callback pointer (updated to use CommInterface)
void (*_msgCallback)(const String &, CommInterface) = NULL;

LoRaManager::LoRaManager() {
  loraActive = false;
  beaconActive = false;
  lastBeaconMs = 0;
  receivedFlag = false;
  lastRssi = 0;
  lastSnr = 0;
  hashIndex = 0;
  _loraInstance = this;
  for (int i = 0; i < HASH_BUFFER_SIZE; i++)
    seenMsgHashes[i] = 0;
  for (int i = 0; i < MAX_PENDING_ACKS; i++) {
    ackQueue[i].active = false;
  }
  memcpy(currentKey, DEFAULT_AES_KEY, 16);
}

void LoRaManager::Init() {
  LOG_PRINTLN("LoRa: Init Start");
  Serial.flush();

  SPI.begin(9, 11, 10, PIN_LORA_CS);
  LOG_PRINTLN("LoRa: SPI Started");
  Serial.flush();

  Module *mod =
      new Module(PIN_LORA_CS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY);
  radio = new SX1262(mod);

  DataManager &data = DataManager::getInstance();
  if (data.GetCryptoKey(currentKey)) {
    LOG_PRINTLN("LoRa: Loaded custom AES-GCM key");
  } else {
    LOG_PRINTLN("LoRa: Using Default AES-GCM key");
    memcpy(currentKey, DEFAULT_AES_KEY, 16);
  }
  Serial.flush();

  int state = radio->begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC,
                           LORA_PWR, 8);
  if (state == RADIOLIB_ERR_NONE) {
    loraActive = true;
    _enterRx();
    LOG_PRINTLN("LoRa: Initialized OK");
  } else {
    LOG_PRINT("LoRa: Init Failed, code ");
    LOG_PRINTLN(state);
  }
  Serial.flush();
}

// ── Async TX State Machine ──────────────────────────────────────────────────

bool LoRaManager::_startAsyncTx(const uint8_t *data, size_t len) {
  // Self-healing: if ISR fired while HandleRx() wasn't polling, finalize now
  if (_radioState == RadioState::RADIO_TX && _transmittedFlag) {
    _onTxComplete();
  }
  if (_radioState == RadioState::RADIO_TX) {
    _txDropCount++;
    LOG_PRINTLN("LORA: TX dropped (radio busy)");
    return false;
  }
  _transmittedFlag = false;
  _radioState = RadioState::RADIO_TX;
  _txStartMs = millis();
  radio->setPacketSentAction(_setTxFlag);
  int state = radio->startTransmit(const_cast<uint8_t *>(data), len);
  if (state != RADIOLIB_ERR_NONE) {
    LOG_PRINTF("LORA: startTransmit failed (%d)\n", state);
    _enterRx();
    return false;
  }
  return true;
}

void LoRaManager::_onTxComplete() {
  _txCount++;
  _transmittedFlag = false;
  _enterRx();
}

void LoRaManager::_enterRx() {
  receivedFlag = false;
  radio->setPacketReceivedAction(setFlag);
  radio->startReceive();
  _radioState = RadioState::RADIO_RX;
}

void LoRaManager::SetKey(const uint8_t *newKey) {
  memcpy(currentKey, newKey, 16);
  LOG_PRINTLN("LoRa: Key Updated");
}

void LoRaManager::SetCallback(void (*cb)(const String &, CommInterface)) {
  _msgCallback = cb;
}

uint32_t LoRaManager::getMsgHash(MessagePacket *p) {
  uint32_t hash = 5381;
  for (int i = 0; i < 16; i++)
    hash = ((hash << 5) + hash) + p->sender[i];
  for (int i = 0; i < 45; i++)
    hash = ((hash << 5) + hash) + p->text[i];
  return hash;
}

uint16_t LoRaManager::calculateChecksum(MessagePacket *p) {
  uint16_t sum = 0;
  uint8_t *data = (uint8_t *)p;
  for (size_t i = 0; i < sizeof(MessagePacket) - 2; i++) {
    sum += data[i];
  }
  return sum;
}

bool LoRaManager::hasSeenMessage(uint32_t hash) {
  for (int i = 0; i < HASH_BUFFER_SIZE; i++) {
    if (seenMsgHashes[i] == hash)
      return true;
  }
  return false;
}

void LoRaManager::markMessageSeen(uint32_t hash) {
  seenMsgHashes[hashIndex] = hash;
  hashIndex = (hashIndex + 1) % HASH_BUFFER_SIZE;
}

void LoRaManager::SendLoRa(const String &text) {
  if (!loraActive)
    return;

  DataManager &data = DataManager::getInstance();
  DisplayManager &display = DisplayManager::getInstance();

  if (text.length() > 0 && (uint8_t)text[0] == BINARY_TOKEN) {
    // Send as compact binary frame
    uint8_t binBuf[64];
    size_t binLen = text.length();
    if (binLen > 64)
      binLen = 64;
    memcpy(binBuf, text.c_str(), binLen);

    uint8_t encBin[128]; // IV(12) + Tag(16) + Payload
    encryptData(binBuf, binLen, encBin, currentKey);
    size_t txLen = binLen + 28;
    _startAsyncTx(encBin, txLen);

    unsigned long toa = radio->getTimeOnAir(txLen) / 1000;
    PerformanceManager &perf = PerformanceManager::getInstance();
    perf.addTimeOnAir(toa);
    perf.addBytesSaved(92 - txLen);
    LOG_PRINTF("LORA: Binary TX (%u bytes, %lums ToA)\n", (unsigned int)binLen,
               toa);
  } else {
    // Standard text packet
    memset(&txPacket, 0, sizeof(MessagePacket));
    strncpy(txPacket.sender, data.myId.c_str(), sizeof(txPacket.sender) - 1);
    strncpy(txPacket.text, text.c_str(), sizeof(txPacket.text) - 1);
    txPacket.ttl = MAX_TTL;

    lastMsgSent = text;

    data.LogMessage(data.myId, 0, text);
    display.SetDisplayActive(true);
    display.DrawUi();

    txPacket.checksum = calculateChecksum(&txPacket);

    // Variable Sized Payload: only send up to the end of the null-terminated
    // text
    size_t textLen = strlen(txPacket.text);
    if (textLen > 44)
      textLen = 44;
    size_t payloadSize =
        16 + textLen + 1 + 2; // sender(16) + text(textLen) + ttl(1) + cksum(2)

    uint8_t varEncBuf[128];
    size_t txLen = payloadSize + 28;
    encryptData((const uint8_t *)&txPacket, payloadSize, varEncBuf, currentKey);
    _startAsyncTx(varEncBuf, txLen);

    unsigned long toa = radio->getTimeOnAir(txLen) / 1000;
    PerformanceManager &perf = PerformanceManager::getInstance();
    perf.addTimeOnAir(toa);
    perf.addBytesSaved(ENCRYPTED_PACKET_SIZE - txLen);
    LOG_PRINTF("LORA: Var-TX (%u bytes, %lums ToA)\n",
               (unsigned int)payloadSize, toa);
  }
}

void LoRaManager::SendLegacyLoRa(const String &text) {
  if (!loraActive)
    return;

  DataManager &data = DataManager::getInstance();
  DisplayManager &display = DisplayManager::getInstance();

  memset(&txPacket, 0, sizeof(MessagePacket));
  strncpy(txPacket.sender, data.myId.c_str(), sizeof(txPacket.sender) - 1);
  strncpy(txPacket.text, text.c_str(), sizeof(txPacket.text) - 1);
  txPacket.ttl = MAX_TTL;

  lastMsgSent = text;
  data.LogMessage(data.myId, 0, text);
  display.SetDisplayActive(true);
  display.DrawUi();

  txPacket.checksum = calculateChecksum(&txPacket);

  uint8_t legacyEncBuf[ENCRYPTED_PACKET_SIZE];
  encryptPacket(&txPacket, legacyEncBuf, currentKey);
  _startAsyncTx(legacyEncBuf, ENCRYPTED_PACKET_SIZE);

  unsigned long toa = radio->getTimeOnAir(ENCRYPTED_PACKET_SIZE) / 1000;
  PerformanceManager::getInstance().addTimeOnAir(toa);
  LOG_PRINTF("LORA: Legacy-TX (92 bytes, %lums ToA)\n", toa);
}

void LoRaManager::SendLoRaBinary(uint8_t targetSid, uint8_t cmd, uint8_t val) {
  if (!loraActive)
    return;

  uint8_t binBuf[32];
  size_t frameLen = BinaryManager::getInstance().createBinaryFrame(
      targetSid, (BinaryCmd)cmd, &val, 1, binBuf);

  uint8_t encBin[128];
  size_t txLen = frameLen + 28;
  encryptData(binBuf, frameLen, encBin, currentKey);
  _startAsyncTx(encBin, txLen);

  unsigned long toa = radio->getTimeOnAir(txLen) / 1000;
  PerformanceManager::getInstance().addTimeOnAir(toa);
  LOG_PRINTF("LORA: Binary-TX (0x%02X -> 0x%02X) to 0x%02X, %lums\n", cmd, val,
             targetSid, toa);
}

void LoRaManager::HandleRx() {
  static int rxPollCount = 0;
  rxPollCount++;

  // ── Poll TX completion (ISR sets _transmittedFlag) ──────────────────────
  if (_radioState == RadioState::RADIO_TX) {
    if (_transmittedFlag) {
      _onTxComplete();
      LOG_PRINTLN("LORA: Async TX complete");
    } else if (millis() - _txStartMs > LORA_TX_TIMEOUT_MS) {
      // Safety timeout — force back to RX
      _txDropCount++;
      LOG_PRINTLN("LORA: TX timeout — forcing RX");
      _enterRx();
    }
    return; // Skip RX processing while in TX state
  }

  // Periodic diagnostic: every 120 polls (~60s at 50ms interval)
  if (rxPollCount % 1200 == 0) {
    LOG_PRINTF("LORA-DIAG: poll=%d, flag=%d, active=%d, DIO1=%d, tx=%lu rx=%lu drop=%lu\n",
               rxPollCount, receivedFlag ? 1 : 0, loraActive ? 1 : 0,
               digitalRead(PIN_LORA_DIO1), _txCount, _rxCount, _txDropCount);
    // Force re-arm the receiver
    _enterRx();
  }

  if (!loraActive || !receivedFlag)
    return;
  receivedFlag = false;
  _rxCount++;
  LOG_PRINTLN("LORA: *** RX EVENT ***");

  uint8_t rxEncBuf[128];
  int state = radio->readData(rxEncBuf, 128);
  int receivedLen = radio->getPacketLength();

  if (state != RADIOLIB_ERR_NONE) {
    _enterRx();
    return;
  }

  lastRssi = radio->getRSSI();
  lastSnr = radio->getSNR();

  ProcessPacket(rxEncBuf, receivedLen);
}

void LoRaManager::ProcessPacket(uint8_t *rxEncBuf, int size) {
  int payloadLen = size - 28; // IV(12) + Tag(16)
  if (payloadLen <= 0) {
    _enterRx();
    return;
  }

  uint8_t rxPlainBuf[128];
  if (!decryptData(rxEncBuf, payloadLen, rxPlainBuf, currentKey)) {
    LOG_PRINTLN("CRYPTO: GCM auth failed (Wrong Key or Tampered)");
    _enterRx();
    return;
  }

  // Check if it's a binary packet
  if (rxPlainBuf[0] == BINARY_TOKEN) {
    LOG_PRINTF("BIN: Compact RX (%d bytes)\n", payloadLen);
    if (BinaryManager::getInstance().handleBinary(rxPlainBuf, payloadLen,
                                                  CommInterface::COMM_LORA)) {
      _enterRx();
      return;
    }
  }

  // Otherwise treat as MessagePacket if size is within bounds (min 19 bytes)
  if (payloadLen < 19 || payloadLen > (int)sizeof(MessagePacket)) {
    LOG_PRINTF("LORA: Unknown packet size %d (not BIN or MSG)\n", payloadLen);
    _enterRx();
    return;
  }

  memset(&rxPacket, 0, sizeof(MessagePacket));
  memcpy(&rxPacket, rxPlainBuf, payloadLen);
  String sender = String(rxPacket.sender);
  String text = String(rxPacket.text);

  uint32_t msgHash = getMsgHash(&rxPacket);
  if (hasSeenMessage(msgHash)) {
    _enterRx();
    return;
  }
  markMessageSeen(msgHash);

  DataManager &data = DataManager::getInstance();

  uint8_t hops = 0;
  if (MAX_TTL >= rxPacket.ttl) {
    hops = MAX_TTL - rxPacket.ttl;
  }
  data.SawNode(sender.c_str(), lastRssi, hops);

  if (text.startsWith("{") && text.indexOf("\"t\":\"T\"") > 0) {
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, text);
    if (!error) {
      uint32_t uptime = doc["u"] | 0;
      float bat = doc["b"] | 0.0f;
      uint8_t rst = doc["r"] | 0;
      uint8_t hops = doc["h"] | 0;
      uint8_t sid = doc["sid"] | 0xFF;
      data.UpdateNode(rxPacket.sender, uptime, bat, rst, 0.0f, 0.0f, lastRssi,
                      hops, sid);
      LOG_PRINTF("TEL: %s (hops=%d, sid=0x%02X)\n", sender.c_str(), hops, sid);

      if (data.streamToSerial) {
        Serial.printf("DATA,%s,%.2f,%d,%d\n", sender.c_str(), bat, lastRssi,
                      hops);
      }
      MQTTManager::getInstance().publishTelemetry(sender, bat, lastRssi, hops);

      lastMsgReceived = "TEL [" + sender + "]";
      _enterRx();
      return;
    }
  }

  // Intercept Binary ACK
  // New Packet structure: [TOKEN] [TARGET] [SENDER] [CMD] [VAL] [CHECKSUM]
  if (rxPlainBuf[0] == BINARY_TOKEN &&
      (BinaryCmd)rxPlainBuf[3] == BinaryCmd::BC_ACK) {
    uint8_t senderSid = rxPlainBuf[2];
    uint8_t ackToken = rxPlainBuf[4];
    String ackSender = DataManager::getInstance().getNameByShortId(senderSid);
    LOG_PRINTF("BIN: ACK token 0x%02X from %s (SID:0x%02X)\n", ackToken,
               ackSender.c_str(), senderSid);
    clearPendingAck(ackSender, true, ackToken);
    _enterRx();
    return;
  }

  // Telemetry or Binary Check
  bool hasNonPrintable = false;
  for (int i = 0; i < 45 && rxPacket.text[i] != '\0'; i++) {
    if (rxPacket.text[i] < 0x20 || rxPacket.text[i] > 0x7E) {
      hasNonPrintable = true;
      break;
    }
  }

  if (hasNonPrintable) {
    LOG_PRINTLN("LORA: RX Garbled (Key Mismatch or Noise)");
    text = "<GARBLED>";
  }

  lastMsgReceived = "[" + sender + "] " + text;
  data.LogMessage(sender, radio->getRSSI(), text);

  if (data.streamToSerial) {
    Serial.printf("MSG,%s,%d,%s\n", sender.c_str(), radio->getRSSI(),
                  text.c_str());
  }
  MQTTManager::getInstance().publishMessage(sender, radio->getRSSI(), text);

  bool clean = true;
  for (unsigned int i = 0; i < text.length(); i++) {
    if (text[i] < 0x20 || text[i] > 0x7E) {
      clean = false;
      break;
    }
  }
  if (clean) {
    LOG_PRINTLN("RX LORA: [" + sender + "] " + text);
  } else {
    LOG_PRINTLN("RX LORA: [" + sender + "] <Binary/Garbage>");
  }

  // Callback to CommandManager with CommInterface::COMM_LORA
  if (_msgCallback) {
    _msgCallback(text, CommInterface::COMM_LORA);
  }

  // Clear Pending ACK if this is an ACK response
  if (text.startsWith("ACK: ")) {
    clearPendingAck(sender, false);
  }

  // Repeater Logic — deferred TASK_ONCE (Phase 2: no blocking delay)
  if (data.repeaterEnabled && !text.startsWith("ACK:")) {
    String target = "";
    int space = text.indexOf(' ');
    if (space > 0)
      target = text.substring(0, space);
    else
      target = text;

    bool isForMeTarget = target.equalsIgnoreCase(data.myId);
    bool isForAll = target.equalsIgnoreCase("ALL");

    if (!isForMeTarget || isForAll) {
      if (rxPacket.ttl > 0) {
        int jitter = random(REPEATER_JITTER_MIN_MS, REPEATER_JITTER_MAX_MS);
        rxPacket.ttl--;
        rxPacket.checksum = calculateChecksum(&rxPacket);
        uint8_t fwdEncBuf[ENCRYPTED_PACKET_SIZE];
        encryptPacket(&rxPacket, fwdEncBuf, currentKey);
        // Defer the actual TX by jitter ms — zero blocking in this path
        ScheduleManager::getInstance().deferRepeaterSend(fwdEncBuf,
                                                         ENCRYPTED_PACKET_SIZE,
                                                         jitter);
        data.LogMessage("RPTR", radio->getRSSI(), "Queued propagation: " + text);
        LOG_PRINTF("RPTR: Queued deferred TX (jitter=%d ms)\n", jitter);
      } else {
        LOG_PRINTLN("RPTR: Packet dropped (TTL=0)");
      }
    }
  }

  DisplayManager::getInstance().DrawUi();
  _enterRx();
}

void LoRaManager::SendRawLoRa(const uint8_t *buf, size_t len) {
  if (!loraActive)
    return;
  _startAsyncTx(buf, len);
  unsigned long toa = radio->getTimeOnAir(len) / 1000;
  PerformanceManager::getInstance().addTimeOnAir(toa);
  LOG_PRINTF("LORA: Raw-TX (%u bytes, %lums ToA)\n", (unsigned int)len, toa);
}

void LoRaManager::DumpDiagnostics() {
  if (!loraActive) {
    LOG_PRINTLN("LoRa: Radio not active");
    return;
  }
  LOG_PRINTF("LoRa Diag: RSSI=%d, SNR=%.1f\n", lastRssi, lastSnr);
  LOG_PRINTF("LoRa Config: Freq=%.1f, BW=%.1f, SF=%d, SYNC=0x%02X\n", LORA_FREQ,
             LORA_BW, LORA_SF, LORA_SYNC);
}

void LoRaManager::SendHeartbeat() {
  if (!loraActive)
    return;

  DataManager &data = DataManager::getInstance();

  memset(&txPacket, 0, sizeof(txPacket));
  strncpy(txPacket.sender, data.myId.c_str(), sizeof(txPacket.sender) - 1);

  JsonDocument doc;
  doc["t"] = "T";
  doc["u"] = millis() / 1000;
  doc["h"] = 0; // hop count: originated here

  PerformanceManager &perf = PerformanceManager::getInstance();

  float batVal = (analogRead(PIN_BAT_ADC) / 4095.0f) * 3.3f * BAT_VOLT_MULTI;
  String rstReason = String(perf.getResetReason());

  // Dirty-flag suppression: skip TX if state hasn't changed meaningfully
  bool batChanged = fabs(batVal - _lastHBBat) > 0.05f;
  bool rstChanged = (rstReason != _lastHBRst);
  bool forceTX = (_hbSkipCount >= HB_FORCE_INTERVAL);

  if (!batChanged && !rstChanged && !forceTX) {
    _hbSkipCount++;
    LOG_PRINTF("LORA: HB suppressed (skip %d/%d, bat=%.2f)\n", _hbSkipCount,
               HB_FORCE_INTERVAL, batVal);
    return;
  }

  // State changed or forced — update references and transmit
  _lastHBBat = batVal;
  _lastHBRst = rstReason;
  _hbSkipCount = 0;

  doc["b"] = round(batVal * 100.0) / 100.0;
  doc["r"] = 0;
  doc["l_avg"] = perf.getLoopAvgMs();
  doc["l_max"] = perf.getLoopMaxMs();
  doc["toa"] = perf.getTimeOnAir();
  doc["rst"] = perf.getResetReason();
  if (perf.isConfiguratorAttached()) {
    doc["cfg"] = 1;
  }

  String json;
  serializeJson(doc, json);
  strncpy(txPacket.text, json.c_str(), sizeof(txPacket.text) - 1);

  txPacket.ttl = MAX_TTL;
  txPacket.checksum = calculateChecksum(&txPacket);
  encryptPacket(&txPacket, encBuf, currentKey);

  _startAsyncTx(encBuf, ENCRYPTED_PACKET_SIZE);
  LOG_PRINTLN("LORA: Heartbeat TX queued -> " + json);
}
void LoRaManager::QueueReliableCommand(const String &targetId,
                                       const String &commandText) {
  for (int i = 0; i < MAX_PENDING_ACKS; i++) {
    if (!ackQueue[i].active) {
      ackQueue[i].targetId = targetId;
      ackQueue[i].commandText = commandText;
      ackQueue[i].isBinary = false;
      ackQueue[i].useFailover = false;
      ackQueue[i].retryCount = 0;
      ackQueue[i].lastAttemptMs = millis();
      ackQueue[i].active = true;
      LOG_PRINTLN("LORA: Queued reliable transmission -> " + targetId);
      SendLoRa(targetId + " " + commandText);
      return;
    }
  }
  LOG_PRINTLN("ERR: ACK Queue is full!");
}

void LoRaManager::QueueReliableBinaryCommand(const String &targetId,
                                             uint8_t cmd, uint8_t val) {
  uint8_t targetSid = DataManager::getInstance().getShortIdByName(targetId);

  for (int i = 0; i < MAX_PENDING_ACKS; i++) {
    if (!ackQueue[i].active) {
      ackQueue[i].targetId = targetId;
      ackQueue[i].targetSid = targetSid;
      ackQueue[i].cmd = cmd;
      ackQueue[i].val = val;
      ackQueue[i].isBinary = true;
      ackQueue[i].useFailover = false;
      ackQueue[i].retryCount = 0;
      ackQueue[i].lastAttemptMs = millis();
      ackQueue[i].active = true;
      LOG_PRINTF("LORA: Queued reliable BINARY -> %s (0x%02X) [SID:0x%02X]\n",
                 targetId.c_str(), cmd, targetSid);
      SendLoRaBinary(targetSid, cmd, val);
      return;
    }
  }
}

void LoRaManager::QueueFailoverPing(const String &targetId) {
  uint8_t targetSid = DataManager::getInstance().getShortIdByName(targetId);

  for (int i = 0; i < MAX_PENDING_ACKS; i++) {
    if (!ackQueue[i].active) {
      ackQueue[i].targetId = targetId;
      ackQueue[i].targetSid = targetSid;
      ackQueue[i].cmd = (uint8_t)BinaryCmd::BC_PING;
      ackQueue[i].val = 0;
      ackQueue[i].isBinary = true;
      ackQueue[i].useFailover = true;
      ackQueue[i].retryCount = 0;
      ackQueue[i].lastAttemptMs = millis();
      ackQueue[i].active = true;
      LOG_PRINTF("LORA: Queued FAILOVER PING -> %s [SID:0x%02X]\n",
                 targetId.c_str(), targetSid);
      SendLoRaBinary(targetSid, (uint8_t)BinaryCmd::BC_PING, 0);
      return;
    }
  }
  LOG_PRINTLN("ERR: ACK Queue full (FPING)");
}

void LoRaManager::clearPendingAck(const String &targetId, bool isBinary,
                                  uint8_t cmd) {
  for (int i = 0; i < MAX_PENDING_ACKS; i++) {
    if (ackQueue[i].active && ackQueue[i].targetId.equalsIgnoreCase(targetId)) {
      if (isBinary == ackQueue[i].isBinary) {
        if (!isBinary || (isBinary && cmd == ackQueue[i].cmd)) {
          ackQueue[i].active = false;
          LOG_PRINTF("LORA: %s ACK Verified from %s\n",
                     isBinary ? "BINARY" : "TEXT", targetId.c_str());
          return;
        }
      }
    }
  }
}

void LoRaManager::periodicTick() {
  if (!loraActive)
    return;

  unsigned long now = millis();

  // Beacon Mode: Fire an RTEST pulse every 60 seconds
  if (beaconActive && (now - lastBeaconMs > 60000 || lastBeaconMs == 0)) {
    lastBeaconMs = now;
    LOG_PRINTLN("LORA: Firing Beacon Pulse (Boosted + Legacy)...");
    SendLoRa("BEACON_TEST");
    // Deferred legacy send — avoids delay() between the two transmits (Phase 3)
    ScheduleManager::getInstance().deferLegacyBeacon("BEACON_TEST");
  }

  for (int i = 0; i < MAX_PENDING_ACKS; i++) {
    if (ackQueue[i].active) {
      if (now - ackQueue[i].lastAttemptMs > 3000) {
        ackQueue[i].retryCount++;
        if (ackQueue[i].retryCount > 3) {
          if (ackQueue[i].useFailover && ackQueue[i].isBinary) {
            // Protocol Failover Algorithm: Binary failed -> Try Legacy Text
            ackQueue[i].isBinary = false;
            ackQueue[i].commandText = "PING";
            ackQueue[i].retryCount = 0;
            ackQueue[i].lastAttemptMs = now;
            LOG_PRINTLN("LORA: Protocol Failover -> Trying Legacy PING for " +
                        ackQueue[i].targetId);
            SendLoRa(ackQueue[i].targetId + " PING");
          } else {
            ackQueue[i].active = false;
            LOG_PRINTLN("LORA: Delivery Failed (Max retries) -> " +
                        ackQueue[i].targetId);
            DataManager::getInstance().LogMessage(
                "SYS", 0, "Delivery failed -> " + ackQueue[i].targetId);
          }
        } else {
          ackQueue[i].lastAttemptMs = now;
          LOG_PRINTF("LORA: Resending reliable command (Try %d/3)...\n",
                     ackQueue[i].retryCount);
          if (ackQueue[i].isBinary) {
            SendLoRaBinary(ackQueue[i].targetSid, ackQueue[i].cmd,
                           ackQueue[i].val);
          } else {
            SendLoRa(ackQueue[i].targetId + " " + ackQueue[i].commandText);
          }
        }
      }
    }
  }
}
