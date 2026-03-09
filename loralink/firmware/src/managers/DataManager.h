#ifndef DATA_MANAGER_H
#define DATA_MANAGER_H

#include "../config.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>

struct LogEntry {
  uint32_t timestamp;
  char source[16];
  int16_t rssi;
  char message[64];
};

struct PeripheralInfo {
  char id[16];
  char hwType[16];
  char fwVersion[12];
  char caps[32];
  char lastReadings[64]; // JSON string snippet
  uint32_t lastSeen;
};

class DataManager {
public:
  static DataManager &getInstance() {
    static DataManager instance;
    return instance;
  }

  // Settings
  String myId;
  String myPrefix;
  bool repeaterEnabled;
  bool encryptionActive;
  String wifiSsid;
  String wifiPass;
  String staticIp;
  String gateway;
  String subnet;
  String cryptoKey;

  // Integrations
  bool streamToSerial;
  bool mqttEnabled;
  String mqttServer;
  int mqttPort;
  String mqttUser;
  String mqttPass;

  // ESP-NOW & Protocol Settings
  bool espNowEnabled;
  uint8_t espNowChannel;
  bool wifiEnabled;
  bool bleEnabled;
  bool traceLogging;
  char transportMode; // 'J'=JSON, 'C'=CSV, 'K'=KV, 'B'=BIN (message format)

  // ── Transport Link State
  // ──────────────────────────────────────────────────── preferredLink:
  // factory/user setting — persisted in NVS (key: "link_pref") currentLink:
  // runtime state — set by negotiate(), updated on up/downgrade
  LinkPreference preferredLink;
  LinkPreference currentLink;

  // Negotiation + probe timing (NVS-persisted so downgrade survives reboot)
  uint32_t transNegotiateMs; // NVS: "trans_neg_ms" — boot window duration
  uint32_t probeBackoffMs;   // NVS: "probe_bkoff"  — current backoff interval
  uint8_t probeFailCount;    // NVS: "probe_fails"  — consecutive probe failures
  String lastProbeResult;    // Runtime only — "OK_MQTT","OK_HTTP","NO_AP",etc.
  unsigned long lastProbeAtMs; // Runtime only — millis() of last probe

  void SetPreferredLink(LinkPreference pref);
  void SetProbeState(uint32_t backoffMs, uint8_t failCount);
  void ResetProbeState();
  static const char *linkName(LinkPreference lp);

  // State
  int bootCount;
  RemoteNode remoteNodes[MAX_NODES];
  int numNodes;
  LogEntry msgLog[LOG_SIZE];
  int logIndex;

  // Peripheral Registry
  PeripheralInfo peripherals[MAX_PERIPHERALS];
  int numPeripherals;

  // ESP-NOW Peers
  ESPNowPeer espNowPeers[ESPNOW_MAX_PEERS];
  int numEspNowPeers;

  // Config Methods
  void Init();
  void LoadSettings();
  void SaveSettings();
  void SetWifi(const String &ssid, const String &pass);
  void SetStaticIp(const String &ip, const String &gateway,
                   const String &subnet);
  void SetName(const String &name);
  void SetRepeater(bool enabled);
  void SetCryptoKey(const String &hexKey);
  bool GetCryptoKey(uint8_t *keyBuf);
  void SetMqtt(bool enabled, const String &server, int port, const String &user,
               const String &pass);

  // Persistence
  void SetESPNowEnabled(bool enabled);
  void SetWifiEnabled(bool enabled);
  void SetBleEnabled(bool enabled);
  void SaveESPNowPeer(int index, const uint8_t *mac, const char *name);
  void RemoveESPNowPeer(int index);
  void LoadESPNowPeers();

  // Scheduler
  unsigned long schedulerInterval110V;
  void SetSchedulerInterval(unsigned long ms);

  // Persistence: GPIO States
  void SetGpioState(const String &pinName, bool state);
  bool GetGpioState(const String &pinName);
  void SetPinName(const String &pin, const String &name);
  String GetPinName(const String &pin);
  void SetPinEnabled(int pin, bool enabled);
  bool GetPinEnabled(int pin);
  void SetTransportMode(char mode);
  char GetTransportMode();
  void AddToRegistry(const String &id, const String &hwType);
  String GetRegistryJson();

  // Peripheral Methods
  void RegisterPeripheral(const String &id, const String &hwType,
                          const String &fw, const String &caps);
  void UpdateSensorTelemetry(const String &id, const String &jsonReadings);
  String GetPeripheralsJson();

  void ClearTrace();
  void FactoryReset();

  // Node & Log Methods
  void UpdateNode(const char *id, uint32_t uptime, float battery,
                  uint8_t resetCode, float lat, float lon, int rssi,
                  uint8_t hops = 0, uint8_t shortId = 0xFF);
  void SawNode(const char *id, int rssi, uint8_t hops, uint8_t shortId = 0xFF);
  void PruneStaleNodes();
  uint8_t getShortIdByName(const String &name);
  String getNameByShortId(uint8_t shortId);
  void LogMessage(const String &source, int rssi, const String &msg);
  bool ImportConfig(const String &json);
  String ExportConfig();

  // Filesystem Search/Load
  bool SaveSchedule(const String &json);
  String ReadSchedule();
  bool InitFilesystem();

  // Utils
  String getHardwareSuffix();
  String getMacSuffix();
  String getResetReason();
  uint8_t getMyShortId();

private:
  DataManager();
};

#endif // DATA_MANAGER_H
