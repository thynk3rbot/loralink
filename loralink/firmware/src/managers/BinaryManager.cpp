#include "BinaryManager.h"
#include "../utils/DebugMacros.h"
#include "CommandManager.h"
#include "DataManager.h"
#include "LoRaManager.h"

bool BinaryManager::handleBinary(const uint8_t *data, size_t len,
                                 CommInterface source) {
  if (len < 5 || data[0] != BINARY_TOKEN)
    return false;

  DataManager &dm = DataManager::getInstance();

  // Validate Target (data[1])
  uint8_t target = data[1];
  if (target != 0xFF && target != dm.getMyShortId()) {
    // Not for us
    return false;
  }

  // Validate checksum (last byte)
  uint8_t receivedSum = data[len - 1];
  uint8_t calculatedSum = calculateSum(data, len - 1);
  if (receivedSum != calculatedSum) {
    LOG_PRINTLN("BIN: Checksum Fail");
    return false;
  }

  uint8_t senderShortId = data[2];
  BinaryCmd cmd = (BinaryCmd)data[3];
  uint8_t val = data[4];

  switch (cmd) {
  case BinaryCmd::BC_GPIO_SET: {
    // [TOKEN] [TARGET] [SENDER] [CMD] [PIN] [VAL] [CHECKSUM] (7 bytes)
    if (len >= 7) {
      int pin = data[4];
      bool pinVal = data[5] == 1;
      String cmdStr = "SET " + String(pin) + " " + (pinVal ? "1" : "0");
      CommandManager::getInstance().handleCommand(cmdStr, source);
      LOG_PRINTF("BIN: GPIO %d -> %d\n", pin, pinVal);
    }
  } break;

  case BinaryCmd::BC_PWM_SET: {
    if (len >= 7) {
      int pin = data[4];
      int duty = data[5];
      String cmdStr = "PWM " + String(pin) + " " + String(duty);
      CommandManager::getInstance().handleCommand(cmdStr, source);
      LOG_PRINTF("BIN: PWM %d -> %d\n", pin, duty);
    }
  } break;

  case BinaryCmd::BC_SERVO_SET: {
    if (len >= 7) {
      int pin = data[4];
      int angle = data[5];
      String cmdStr = "SERVO " + String(pin) + " " + String(angle);
      CommandManager::getInstance().handleCommand(cmdStr, source);
      LOG_PRINTF("BIN: SERVO %d -> %d deg\n", pin, angle);
    }
  } break;

  case BinaryCmd::BC_REBOOT:
    CommandManager::getInstance().handleCommand("RESTART", source);
    break;

  case BinaryCmd::BC_PING:
    LOG_PRINTLN("BIN: Ping received");
    break;

  case BinaryCmd::BC_STATUS:
    CommandManager::getInstance().handleCommand("STATUS", source);
    break;

  case BinaryCmd::BC_ACK:
    // ACK handled in LoRaManager::clearPendingAck
    return true;

  default:
    LOG_PRINTF("BIN: Unknown Code 0x%02X\n", (uint8_t)cmd);
    return false;
  }

  // Send Binary ACK back to sender
  if (source == CommInterface::COMM_LORA && cmd != BinaryCmd::BC_ACK) {
    LoRaManager::getInstance().SendLoRaBinary(
        senderShortId, (uint8_t)BinaryCmd::BC_ACK, (uint8_t)cmd);
  }

  return true;
}

size_t BinaryManager::createBinaryFrame(uint8_t target, BinaryCmd cmd,
                                        uint8_t *args, size_t argLen,
                                        uint8_t *outBuf) {
  outBuf[0] = BINARY_TOKEN;
  outBuf[1] = target;
  outBuf[2] = DataManager::getInstance().getMyShortId(); // SENDER
  outBuf[3] = (uint8_t)cmd;
  for (size_t i = 0; i < argLen; i++) {
    outBuf[4 + i] = args[i];
  }
  size_t total = 4 + argLen; // TOKEN, TARGET, SENDER, CMD, ARGS...
  outBuf[total] = calculateSum(outBuf, total);
  return total + 1;
}

uint8_t BinaryManager::calculateSum(const uint8_t *data, size_t len) {
  uint8_t sum = 0;
  for (size_t i = 0; i < len; i++) {
    sum ^= data[i];
  }
  return sum;
}
