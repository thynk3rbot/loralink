#ifndef POWER_MANAGER_H
#define POWER_MANAGER_H

#include "../config.h"
#include <Arduino.h>

enum class PowerMode : uint8_t { NORMAL = 0, CONSERVE = 1, CRITICAL = 2 };

class PowerManager {
public:
  static PowerManager &getInstance() {
    static PowerManager instance;
    return instance;
  }

  void Init();
  void Update();

  float getBatteryVoltage();
  PowerMode getCurrentMode();
  uint32_t getTargetInterval();

  bool isOledAllowed();
  bool isWifiAllowed();

  // Power-Miser API
  void setManualMode(PowerMode mode, bool manual);
  String getModeString();

private:
  PowerManager();

  PowerMode _currentMode;
  bool _manualOverride;
  float _lastVoltage;
  unsigned long _lastSampleMs;

  void evaluateMode();
};

#endif // POWER_MANAGER_H
