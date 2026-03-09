#ifndef PERFORMANCE_MANAGER_H
#define PERFORMANCE_MANAGER_H

#include <Arduino.h>

class PerformanceManager {
public:
  static PerformanceManager &getInstance() {
    static PerformanceManager instance;
    return instance;
  }

  void init();

  // Loop profiling
  void loopTickStart();
  void loopTickEnd();

  // Radio Time-on-Air
  void addTimeOnAir(unsigned long ms);
  void addBytesSaved(uint32_t bytes);

  // Configurator presence
  void reportConfiguratorActivity();

  // Getters for telemetry
  unsigned long getLoopAvgMs() const { return _loopAvgMs; }
  unsigned long getLoopMaxMs() const { return _loopMaxMs; }
  unsigned long getTimeOnAir() const { return _totalTimeOnAirMs; }
  uint32_t getBytesSaved() const { return _totalBytesSaved; }
  String getResetReason() const { return _resetReason; }
  bool isConfiguratorAttached() const;

private:
  PerformanceManager();

  // Loop timing
  unsigned long _loopStartMicros = 0;
  unsigned long _loopAvgMs = 0;
  unsigned long _loopMaxMs = 0;

  // Averages
  unsigned long _accumulatedMicros = 0;
  unsigned long _loopCount = 0;

  // Radio
  unsigned long _totalTimeOnAirMs = 0;
  uint32_t _totalBytesSaved = 0;

  // Diagnostics
  String _resetReason;

  // Configurator tracking
  unsigned long _lastConfiguratorPing = 0;
  const unsigned long CONFIGURATOR_TIMEOUT_MS = 60000; // 60s timeout
};

#endif // PERFORMANCE_MANAGER_H
