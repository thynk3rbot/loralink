#include "PowerManager.h"
#include "../utils/DebugMacros.h"
#include "DataManager.h"
#include "DisplayManager.h"
#include "WiFiManager.h"

PowerManager::PowerManager() {
  _currentMode = PowerMode::NORMAL;
  _manualOverride = false;
  _lastVoltage = 4.2f;
  _lastSampleMs = 0;
}

void PowerManager::Init() {
  LOG_PRINTLN("POWER: Miser initializing...");
  pinMode(PIN_BAT_ADC, INPUT);

  // Heltec V3 External Power Control
  pinMode(PIN_VEXT_CTRL, OUTPUT);
  digitalWrite(PIN_VEXT_CTRL, LOW);

  // Heltec V3 Battery Divider Control
  pinMode(PIN_BAT_CTRL, OUTPUT);
  digitalWrite(PIN_BAT_CTRL, LOW);

  Update();
}

float PowerManager::getBatteryVoltage() {
  uint32_t raw = analogRead(PIN_BAT_ADC);
  // Heltec V3: 1M / 390k divider (approx 1/2.766 multiplier)
  // ADC is 12-bit (4095). 3.3V reference.
  float volt = (raw / 4095.0f) * 3.3f * BAT_VOLT_MULTI;
  _lastVoltage = volt;
  return volt;
}

void PowerManager::Update() {
  unsigned long now = millis();
  if (now - _lastSampleMs > 30000 || _lastSampleMs == 0) { // Every 30s
    _lastSampleMs = now;
    float v = getBatteryVoltage();
    evaluateMode();
    LOG_PRINTF("POWER: Miser V=%.2fV Mode=%s\n", v, getModeString().c_str());
  }
}

void PowerManager::evaluateMode() {
  if (_manualOverride)
    return;

  PowerMode prev = _currentMode;

  // USB/Mains detection: If voltage is near 0 or very low (No battery),
  // we force NORMAL mode and skip power-saving logic.
  // 2.0V is a safe threshold since a dead LiPo is ~3.0V.
  if (_lastVoltage < 2.0f || _lastVoltage > 4.1f) {
    _currentMode = PowerMode::NORMAL;
  } else if (_lastVoltage >= POWER_MISER_VOLT_NORMAL) {
    _currentMode = PowerMode::NORMAL;
  } else if (_lastVoltage >= POWER_MISER_VOLT_CONSERVE) {
    _currentMode = PowerMode::CONSERVE;
  } else {
    _currentMode = PowerMode::CRITICAL;
  }

  if (prev != _currentMode) {
    LOG_PRINTF("POWER: Miser switched to %s\n", getModeString().c_str());

    // Apply instant policy changes
    if (_currentMode == PowerMode::CRITICAL) {
      DisplayManager::getInstance().SetDisplayActive(false);
    }

    // Notify WiFi Manager for immediate radio coordination
    WiFiManager::getInstance().onPowerStateChange(_currentMode);
  }
}

PowerMode PowerManager::getCurrentMode() { return _currentMode; }

uint32_t PowerManager::getTargetInterval() {
  switch (_currentMode) {
  case PowerMode::NORMAL:
    return POWER_MISER_HB_NORMAL;
  case PowerMode::CONSERVE:
    return POWER_MISER_HB_CONSERVE;
  case PowerMode::CRITICAL:
    return POWER_MISER_HB_CRITICAL;
  default:
    return POWER_MISER_HB_NORMAL;
  }
}

bool PowerManager::isOledAllowed() {
  return (_currentMode != PowerMode::CRITICAL);
}

bool PowerManager::isWifiAllowed() {
  return (_currentMode != PowerMode::CRITICAL);
}

String PowerManager::getModeString() {
  switch (_currentMode) {
  case PowerMode::NORMAL:
    return "NORMAL";
  case PowerMode::CONSERVE:
    return "CONSERVE";
  case PowerMode::CRITICAL:
    return "CRITICAL";
  default:
    return "UNKNOWN";
  }
}

void PowerManager::setManualMode(PowerMode mode, bool manual) {
  _manualOverride = manual;
  if (manual)
    _currentMode = mode;
}
