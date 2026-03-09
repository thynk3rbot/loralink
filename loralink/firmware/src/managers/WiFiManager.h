#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "../config.h"
#include "DataManager.h"
#include "DisplayManager.h"
#include "PowerManager.h"
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <WiFi.h>

void setWebCallback(void (*cb)(const String &, CommInterface));

class WiFiManager {
public:
  static WiFiManager &getInstance() {
    static WiFiManager instance;
    return instance;
  }

  void init();
  void handle();
  bool isConnected;
  unsigned long lastApiHit;
  bool modemSleepEnabled;
  void onPowerStateChange(PowerMode mode);

  // Returns true if a PC webapp has hit the API within the given window
  // (default 5 min)
  bool isPCAttached(unsigned long windowMs = 300000UL) const {
    return isConnected && lastApiHit > 0 && (millis() - lastApiHit) < windowMs;
  }

  // Returns true if running on USB/mains (battery reads near 0V or > 4.1V
  // charging)
  static bool isPowered() {
    float bat = (analogRead(PIN_BAT_ADC) / 4095.0f) * 3.3f * BAT_VOLT_MULTI;
    return (bat < 0.1f || bat > 4.2f);
  }

  // ── Transport negotiation & probing ────────────────────────────────────────
  // negotiate(): called once at boot inside main.cpp setup().
  // Spends up to windowMs trying to reach the preferred transport,
  // then locks DataManager::currentLink. Returns the locked preference.
  LinkPreference negotiate(uint32_t windowMs);

  // probeWifi(): lightweight single-shot reachability test (<5s on battery).
  // Safe to call from handle() backoff timer. Returns ProbeResult.
  // Hook: update PROBE_OK_MQTT path when MQTTManager exposes probe().
  ProbeResult probeWifi(uint32_t timeoutMs = PROBE_TIMEOUT_MS);

private:
  WiFiManager();
  bool serverStarted;
  unsigned long lastWifiTry;
  unsigned long _wifiLostAt; // millis() when WiFi last dropped (0 = connected)
  void tryConnect();
  void startServer();
  void checkProbeBackoff(); // Probe timer tick
  void onLinkDowngrade(LinkPreference from, LinkPreference to);
  void onLinkUpgrade(LinkPreference from, LinkPreference to);

  // Page handlers
  void serveHome();
  void serveConfig();
  void serveConfigSave();
  void serveIntegration();
  void serveIntegrationSave();
  void serveHelp();
  void serveScheduling();
  void serveHardware();
  void serveApiStatus();
  void serveApiConfig();
  void serveApiConfigApply();
  void serveApiFileList();
  void serveApiFileRead();
  void serveApiCmd();
  void serveApiPeers();
  void serveApiAddPeer();
  void serveApiRemovePeer();
  void serveApiSchedule();
  void serveApiScheduleAdd();
  void serveApiScheduleRemove();
  void serveApiScheduleClear();
  void serveApiScheduleSave();
  void serveApiPinName();
  void serveApiPinEnable();
  void serveApiTransportMode();
  void serveApiRegistry();
  void serveApiProductSave();
};

#endif // WIFI_MANAGER_H
