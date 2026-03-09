#ifndef MCP_MANAGER_H
#define MCP_MANAGER_H

#include "../config.h"
#include <Adafruit_MCP23X17.h>
#include <Arduino.h>

// ============================================================================
//  MCPManager — MCP23017 I2C GPIO Expander Driver
//
//  Supports up to 8 MCP23017 chips on the shared OLED I2C bus (SDA=17, SCL=18).
//  Extended pin numbering: native 0–99, MCP 100–227.
//    chip 0 (0x20) → pins 100–115
//    chip 1 (0x21) → pins 116–131  … chip 7 (0x27) → pins 212–227
//
//  Usage:
//    MCPManager::getInstance().init();           // call after Heltec.begin()
//    MCPManager::writePin(104, true);            // MCP:0:4 HIGH
//    MCPManager::readPin(108);                   // MCP:0:8 state
//    MCPManager::setupPin(100, OUTPUT);          // configure direction
//
//  Interrupt: wire MCP INTA → PIN_MCP_INT (GPIO 38).
//  Poll MCPManager::getInstance().hasInterrupt() in main loop; call
//  clearInterrupt() after handling.
// ============================================================================

class MCPManager {
public:
  static MCPManager &getInstance() {
    static MCPManager instance;
    return instance;
  }

  // Scan I2C bus, initialise found chips, attach ISR if any found.
  // Must be called after Heltec.begin() (I2C already initialised).
  bool init();

  bool isReady()    const { return _ready; }
  int  chipCount()  const { return _chipCount; }

  // ── Instance-level pin operations (extPin = extended pin number) ──────────
  void mcpPinMode(int extPin, uint8_t mode);
  void mcpDigitalWrite(int extPin, bool val);
  bool mcpDigitalRead(int extPin);

  // ── Static routing helpers ────────────────────────────────────────────────
  // Use these throughout the codebase — they transparently route to MCP or
  // native Arduino functions based on the pin number.
  //
  // writePin / setupPin return false and log an error if the target MCP chip
  // is not present; readPin returns false in that case.
  static bool writePin(int pin, bool val);
  static bool readPin(int pin);
  static void setupPin(int pin, uint8_t mode);

  // ── Classification helpers ────────────────────────────────────────────────
  static bool isMcpPin(int pin)   { return pin >= MCP_PIN_BASE; }
  static int  chipIndex(int pin)  { return (pin - MCP_PIN_BASE) / MCP_CHIP_PINS; }
  static int  pinIndex(int pin)   { return (pin - MCP_PIN_BASE) % MCP_CHIP_PINS; }

  // ── Interrupt ─────────────────────────────────────────────────────────────
  bool hasInterrupt()  const { return _intFlag; }
  void clearInterrupt()      { _intFlag = false; }

  // Non-copyable
  MCPManager(const MCPManager &) = delete;
  MCPManager &operator=(const MCPManager &) = delete;

private:
  MCPManager() = default;

  Adafruit_MCP23X17 _chips[MCP_MAX_CHIPS];
  bool              _present[MCP_MAX_CHIPS] = {};
  int               _chipCount = 0;
  bool              _ready = false;
  volatile bool     _intFlag = false; // set in IRAM_ATTR ISR

  static void IRAM_ATTR _isr();
};

#endif // MCP_MANAGER_H
