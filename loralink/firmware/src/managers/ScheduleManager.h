#ifndef SCHEDULE_MANAGER_H
#define SCHEDULE_MANAGER_H

#include "../config.h"
#include <ArduinoJson.h>
#include <DHT.h>
#include <TaskScheduler.h>
#include <vector>

class ScheduleManager {
public:
  static ScheduleManager &getInstance() {
    static ScheduleManager instance;
    return instance;
  }

  void init();
  void execute();

  // Task manipulation methods
  void set110VInterval(unsigned long interval_ms);
  void forceRelay110V(bool state);
  void trigger12VPulse();
  void triggerBlink();
  void triggerRestart(unsigned long delayMs);

  // Non-blocking deferred operations
  void deferRepeaterSend(const uint8_t *encBuf, size_t encLen,
                         unsigned long jitterMs);
  void deferLegacyBeacon(const String &payload);
  void startSleepSequence(float hours, const String &trigger,
                          unsigned long initialDelayMs = 0);

  // JSON/CSV Dynamic Scheduling
  void loadDynamicSchedules();
  void loadSchedulesFromCsv(const String &csv);
  String getTaskReport();
  void getTaskJson(JsonDocument &doc);
  bool addDynamicTask(const String &name, const String &type, const String &pin,
                      unsigned long interval, unsigned long duration,
                      const String &source = "INTERNAL", bool enabled = true,
                      int value = 0, int triggerPin = -1, int triggerMode = 0,
                      int threshold = 0, bool thresholdGreater = true,
                      const String &notifyCmd = "");
  bool removeDynamicTask(const String &name);
  void clearDynamicTasks();
  void saveDynamicTasks();
  void setStreamMode(bool mode) { isStreaming = mode; }
  bool isInStreamMode() const { return isStreaming; }
  void processStreamLine(const String &line, CommInterface source);
  static void dynamicTaskCallback();
  void setupDynamicPin(int pin, const String &type, int value);

  struct DynamicTaskConfig {
    String name;
    String type; // TOGGLE, PULSE, PWM, SERVO, READ, LORA_TX, ALERT
    int pin;
    unsigned long interval;
    unsigned long duration;
    bool enabled;
    String updatedBy;
    String lastUpdated;
    int value = 0;

    // Trigger Logic
    int triggerPin = -1;
    int triggerMode = 0; // 0: None, 1: RISING, 2: FALLING, 3: CHANGE
    int threshold = 0;
    bool thresholdGreater = true;

    // ALERT: command dispatched when threshold condition is met
    String notifyCmd;

    bool pulseActive = false;
    unsigned long pulseStart = 0;
  };

  static void IRAM_ATTR globalInterruptHandler(void *arg);
  static void IRAM_ATTR buttonISR();

  // Returns how many dynamic tasks are currently registered
  int getDynamicTaskCount() const { return (int)dynamicConfigs.size(); }

  // Sleep sequence state (checked by batteryMonitorCallback)
  bool sleepSequenceActive = false;

private:
  bool isStreaming = false;
  volatile bool _btnPressed = false; // Set by buttonISR, cleared in displayTask
  volatile bool _xiaoPresent = false; // Set when HELLO handshake received
  ScheduleManager();
  ~ScheduleManager(); // clean up heap-allocated Task objects

  Scheduler runner;
  DHT dht;

  int dhtFailCount;
  bool safetyTripped;

  // Task Callbacks
  static void environmentalCallback();
  static void toggle110VCallback();
  static void pulse12VCallback();
  static void endPulse12VCallback();

  // System Callbacks
  static void loraTask();
  static void wifiTask();
  static void serialTask();
  static void heartbeatTask();
  static void displayTask();
  static void blinkTask();
  static void restartTask();
  static void bleTask();
  static void espNowTask();
  static void peripheralSerialTask();
  static void batteryMonitorCallback();

  // System Callbacks (deferred operations)
  static void repeaterDeferCallback();
  static void beaconLegacyCallback();
  static void sleepSequenceCallback();

  // Tasks
  Task tEnvironmental;
  Task t110V;
  Task t12V;
  Task t12VEnd;

  Task tLoRa;
  Task tWiFi;
  Task tSerial;
  Task tHeartbeat;
  // tButton removed — replaced by buttonISR() hardware interrupt
  Task tDisplay;
  Task tBlink;
  Task tRestart;
  Task tBLE;
  Task tESPNow;
  Task tPeripheralSerial;
  Task tBatteryMonitor;
  int blinkCount;

  // Deferred operation tasks (TASK_ONCE)
  Task tRepeaterDefer;
  Task tBeaconLegacy;
  Task tSleepSequence;

  // Deferred operation state
  uint8_t _repeaterEncBuf[92]; // ENCRYPTED_PACKET_SIZE — avoid circular include
  size_t _repeaterEncLen = 0;
  String _legacyBeaconPayload;
  float _sleepHours = 0.0f;
  String _sleepTrigger;
  uint8_t _sleepStep = 0;

  // Dynamic Task Pool — heap-allocated; unbounded (limited only by free heap).
  // Task objects are new'd on addDynamicTask, deleted on remove/clear.
  std::vector<Task *> tDynamicPool;
  std::vector<DynamicTaskConfig> dynamicConfigs;

  void checkSafetyThreshold(float temp);
};

#endif // SCHEDULE_MANAGER_H
