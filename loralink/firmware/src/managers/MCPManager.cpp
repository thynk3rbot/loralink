#include "MCPManager.h"

// ── ISR (IRAM-safe, no heap allocation) ──────────────────────────────────────
void IRAM_ATTR MCPManager::_isr() {
  MCPManager::getInstance()._intFlag = true;
}

// ── init() ────────────────────────────────────────────────────────────────────
bool MCPManager::init() {
  _chipCount = 0;
  _ready     = false;

  for (int i = 0; i < MCP_MAX_CHIPS; i++) {
    uint8_t addr = MCP_CHIP_ADDR_BASE + i;
    if (_chips[i].begin_I2C(addr)) {
      _present[i] = true;
      _chipCount++;
      Serial.printf("[MCP] Chip %d found at 0x%02X\n", i, addr);
    } else {
      _present[i] = false;
    }
  }

  if (_chipCount == 0) {
    Serial.println("[MCP] No chips found — expander disabled");
    return false;
  }

  // Attach interrupt on INTA line (active-LOW, open-drain).
  // Wire INTA of all chips together to PIN_MCP_INT.
  pinMode(PIN_MCP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_MCP_INT), _isr, FALLING);

  _ready = true;
  Serial.printf("[MCP] Ready — %d chip(s) online, INT on GPIO %d\n",
                _chipCount, PIN_MCP_INT);
  return true;
}

// ── Instance pin operations ───────────────────────────────────────────────────
void MCPManager::mcpPinMode(int extPin, uint8_t mode) {
  int c = chipIndex(extPin);
  int p = pinIndex(extPin);
  if (!_present[c]) return;
  _chips[c].pinMode(p, mode);
}

void MCPManager::mcpDigitalWrite(int extPin, bool val) {
  int c = chipIndex(extPin);
  int p = pinIndex(extPin);
  if (!_present[c]) return;
  _chips[c].digitalWrite(p, val ? HIGH : LOW);
}

bool MCPManager::mcpDigitalRead(int extPin) {
  int c = chipIndex(extPin);
  int p = pinIndex(extPin);
  if (!_present[c]) return false;
  return _chips[c].digitalRead(p);
}

// ── Static routing helpers ────────────────────────────────────────────────────
bool MCPManager::writePin(int pin, bool val) {
  if (!isMcpPin(pin)) {
    digitalWrite(pin, val ? HIGH : LOW);
    return true;
  }
  MCPManager &mgr = getInstance();
  int c = chipIndex(pin);
  if (!mgr._present[c]) {
    Serial.printf("[MCP] ERR: chip %d not present (pin %d)\n", c, pin);
    return false;
  }
  mgr.mcpDigitalWrite(pin, val);
  return true;
}

bool MCPManager::readPin(int pin) {
  if (!isMcpPin(pin)) {
    return digitalRead(pin);
  }
  MCPManager &mgr = getInstance();
  int c = chipIndex(pin);
  if (!mgr._present[c]) {
    Serial.printf("[MCP] ERR: chip %d not present (pin %d)\n", c, pin);
    return false;
  }
  return mgr.mcpDigitalRead(pin);
}

void MCPManager::setupPin(int pin, uint8_t mode) {
  if (!isMcpPin(pin)) {
    pinMode(pin, mode);
    return;
  }
  MCPManager &mgr = getInstance();
  int c = chipIndex(pin);
  if (!mgr._present[c]) {
    Serial.printf("[MCP] ERR: chip %d not present (pin %d)\n", c, pin);
    return;
  }
  mgr.mcpPinMode(pin, mode);
}
