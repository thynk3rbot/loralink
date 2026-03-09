#include "PerformanceManager.h"
#include "../utils/DebugMacros.h"
#include <rom/rtc.h>

PerformanceManager::PerformanceManager() {}

void PerformanceManager::init() {
  LOG_PRINTLN("PERF: Initializing Performance Manager");

  // Determine Reset Reason
  int r0 = rtc_get_reset_reason(0);
  int r1 = rtc_get_reset_reason(1);

  auto decodeReason = [](int reason) -> String {
    switch (reason) {
    case 1:
      return "POWERON";
    case 3:
      return "SW_RESET";
    case 4:
      return "OWDT";
    case 5:
      return "DEEPSLEEP";
    case 6:
      return "SDIO";
    case 7:
      return "TG0WDT";
    case 8:
      return "TG1WDT";
    case 9:
      return "RTCWDT";
    case 10:
      return "INTRTCWDT";
    case 11:
      return "TGWDT_CPU";
    case 12:
      return "SW_CPU";
    case 13:
      return "RTCWDT_CPU";
    case 14:
      return "EXT_CPU";
    case 15:
      return "RTCWDT_BROWN";
    case 16:
      return "RTCWDT_RTC";
    default:
      return "UNKNOWN";
    }
  };

  _resetReason = decodeReason(r0);
  if (r0 != r1) {
    _resetReason += "/" + decodeReason(r1);
  }

  LOG_PRINTF("PERF: Reset Reason: %s\n", _resetReason.c_str());
}

void PerformanceManager::loopTickStart() { _loopStartMicros = micros(); }

void PerformanceManager::loopTickEnd() {
  unsigned long duration = micros() - _loopStartMicros;
  _accumulatedMicros += duration;
  _loopCount++;

  unsigned long durationMs = duration / 1000;
  if (durationMs > _loopMaxMs) {
    _loopMaxMs = durationMs;
  }

  // Calculate average every 1000 loops to avoid overflow
  if (_loopCount >= 1000) {
    _loopAvgMs = (_accumulatedMicros / 1000) / 1000;
    _accumulatedMicros = 0;
    _loopCount = 0;

    // Reset max loop duration every period to catch real-time spikes rather
    // than absolute boot peaks
    _loopMaxMs = 0;
  }
}

void PerformanceManager::addTimeOnAir(unsigned long ms) {
  _totalTimeOnAirMs += ms;
}
void PerformanceManager::addBytesSaved(uint32_t bytes) {
  _totalBytesSaved += bytes;
}

void PerformanceManager::reportConfiguratorActivity() {
  _lastConfiguratorPing = millis();
}

bool PerformanceManager::isConfiguratorAttached() const {
  if (_lastConfiguratorPing == 0)
    return false;
  return (millis() - _lastConfiguratorPing) < CONFIGURATOR_TIMEOUT_MS;
}
