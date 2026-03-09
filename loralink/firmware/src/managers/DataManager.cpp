#include "DataManager.h"
#include "../config.h"
#include "../crypto.h"
#include "../utils/DebugMacros.h"
#include "MQTTManager.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_system.h>

DataManager::DataManager() {
  numNodes = 0;
  logIndex = 0;
  repeaterEnabled = false;
  encryptionActive = true;
  espNowEnabled = false;
  espNowChannel = ESPNOW_CHANNEL;
  numEspNowPeers = 0;
  myPrefix = "GW";
  myId = "GW-INIT";
  streamToSerial = false;
  mqttEnabled = false;
  mqttPort = 1883;
  transportMode = 'J';
  traceLogging = false;
  numPeripherals = 0;
  preferredLink = LinkPreference::LINK_AUTO;
  currentLink = LinkPreference::LINK_AUTO;
  transNegotiateMs = TRANSPORT_NEGOTIATE_MS;
  probeBackoffMs = PROBE_BACKOFF_MIN_MS;
  probeFailCount = 0;
  lastProbeResult = "";
  lastProbeAtMs = 0;
}

String DataManager::getHardwareSuffix() {
  uint64_t chipId = ESP.getEfuseMac();
  char suffix[12];
  sprintf(suffix, "%04X", (uint16_t)(chipId >> 32));
  return String(suffix);
}

String DataManager::getMacSuffix() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[12];
  sprintf(buf, "%06X", (uint32_t)((mac >> 24) & 0xFFFFFF));
  return String(buf);
}

uint8_t DataManager::getMyShortId() {
  uint64_t mac = ESP.getEfuseMac();
  return (uint8_t)(mac & 0xFF);
}

void DataManager::Init() {
  Serial.println("INIT: DataManager Starting...");
  Serial.flush();
  InitFilesystem();

  Preferences p;
  p.begin("loralink", false);

  bootCount = p.getInt("bootCount", 0) + 1;
  p.putInt("bootCount", bootCount);
  Serial.printf("BOOT: #%d\n", bootCount);
  Serial.flush();

  // Initialize WiFi defaults if empty (first boot)
  String savedSsid = p.getString("wifi_ssid", "");
  if (savedSsid.length() == 0) {
    Serial.println("INIT: No WiFi credentials found - using defaults...");
    p.putString("wifi_ssid", "");
    p.putString("wifi_pass", "");
  }

  Serial.println("INIT: Loading Settings...");
  Serial.flush();
  p.end();
  LoadSettings();

  Serial.printf("BOOT: Heap Free: %u bytes\n", ESP.getFreeHeap());
  Serial.println("INIT: DataManager OK");
  Serial.flush();
}

bool DataManager::InitFilesystem() {
  if (!LittleFS.begin(true)) {
    LOG_PRINTLN("FS: LittleFS Mount Failed");
    return false;
  }
  LOG_PRINTLN("FS: LittleFS Mounted OK");
  return true;
}

bool DataManager::SaveSchedule(const String &json) {
  File file = LittleFS.open("/schedule.json", "w");
  if (!file) {
    LOG_PRINTLN("FS: Failed to open schedule for writing");
    return false;
  }
  size_t bytes = file.print(json);
  file.close();
  return (bytes > 0);
}

String DataManager::ReadSchedule() {
  if (!LittleFS.exists("/schedule.json"))
    return "{\"schedules\":[]}";
  File file = LittleFS.open("/schedule.json", "r");
  if (!file)
    return "{\"schedules\":[]}";
  String content = file.readString();
  file.close();
  return content;
}

void DataManager::LoadSettings() {
  Preferences p;
  p.begin("loralink", true);

  LOG_PRINTLN("INIT: Loading ID...");
  myId = p.getString("dev_name", myPrefix + "-" + getHardwareSuffix());
  if (myId.length() > 20)
    myId = myId.substring(0, 20);
  for (unsigned int i = 0; i < myId.length(); i++) {
    if (myId[i] < 0x20 || myId[i] > 0x7E)
      myId[i] = '?';
  }

  LOG_PRINTLN("INIT: Loading Repeater...");
  repeaterEnabled = p.getBool("repeater", false);

  LOG_PRINTLN("INIT: Loading WiFi...");
  wifiSsid = p.getString("wifi_ssid", "");
  if (wifiSsid.length() > 64)
    wifiSsid = wifiSsid.substring(0, 64);
  wifiPass = p.getString("wifi_pass", "");
  if (wifiPass.length() > 64)
    wifiPass = wifiPass.substring(0, 64);

  LOG_PRINTLN("INIT: Loading IP...");
  staticIp = p.getString("static_ip", "");
  if (staticIp.length() > 64)
    staticIp = staticIp.substring(0, 64);
  gateway = p.getString("gateway", "");
  if (gateway.length() > 64)
    gateway = gateway.substring(0, 64);
  subnet = p.getString("subnet", "");
  if (subnet.length() > 64)
    subnet = subnet.substring(0, 64);

  LOG_PRINTLN("INIT: Loading Crypto Key...");
  cryptoKey = p.getString("crypto_key", "");
  if (cryptoKey.length() > 64)
    cryptoKey = cryptoKey.substring(0, 64);

  LOG_PRINTLN("INIT: Loading Sched...");
  schedulerInterval110V = p.getULong("sched_int_110", 5000);

  LOG_PRINTF("INIT: Loading ESP-NOW (Default: %s)...\n", "ENABLED");
  espNowEnabled = p.getBool("espnow_en", true);
  espNowChannel = p.getUChar("espnow_ch", ESPNOW_CHANNEL);

  LOG_PRINTLN("INIT: Loading Radio Profiles...");
  wifiEnabled = p.getBool("wifi_en", true);
  bleEnabled = p.getBool("ble_en", true);

  LOG_PRINTLN("INIT: Loading Integrations...");
  mqttEnabled = p.getBool("mqtt_en", false);
  mqttServer = p.getString("mqtt_srv", "");
  if (mqttServer.length() > 128)
    mqttServer = mqttServer.substring(0, 128);
  mqttPort = p.getInt("mqtt_prt", 1883);
  mqttUser = p.getString("mqtt_usr", "");
  if (mqttUser.length() > 64)
    mqttUser = mqttUser.substring(0, 64);
  mqttPass = p.getString("mqtt_pwd", "");
  if (mqttPass.length() > 64)
    mqttPass = mqttPass.substring(0, 64);
  transportMode = p.getString("tp_mode", "J").charAt(0);

  // Transport link preference
  preferredLink = (LinkPreference)p.getUChar(
      "link_pref", (uint8_t)LinkPreference::LINK_AUTO);
  currentLink = preferredLink; // negotiate() will adjust at boot
  transNegotiateMs = p.getULong("trans_neg_ms", TRANSPORT_NEGOTIATE_MS);
  probeBackoffMs = p.getULong("probe_bkoff", PROBE_BACKOFF_MIN_MS);
  probeFailCount = p.getUChar("probe_fails", 0);

  p.end();

  LoadESPNowPeers();
}

void DataManager::SaveSettings() {
  // Individual setters persist immediately
}

void DataManager::SetWifi(const String &ssid, const String &pass) {
  wifiSsid = ssid;
  wifiPass = pass;
  Preferences p;
  p.begin("loralink", false);
  p.putString("wifi_ssid", ssid);
  p.putString("wifi_pass", pass);
  p.end();
}

void DataManager::SetStaticIp(const String &ip, const String &gw,
                              const String &sn) {
  this->staticIp = ip;
  this->gateway = gw;
  this->subnet = sn;
  Preferences p;
  p.begin("loralink", false);
  p.putString("static_ip", ip);
  p.putString("gateway", gw);
  p.putString("subnet", sn);
  p.end();
}

void DataManager::SetName(const String &name) {
  myId = name;
  Preferences p;
  p.begin("loralink", false);
  p.putString("dev_name", name);
  p.end();
}

void DataManager::SetRepeater(bool enabled) {
  repeaterEnabled = enabled;
  Preferences p;
  p.begin("loralink", false);
  p.putBool("repeater", enabled);
  p.end();
}

void DataManager::SetCryptoKey(const String &hexKey) {
  cryptoKey = hexKey;
  Preferences p;
  p.begin("loralink", false);
  p.putString("crypto_key", hexKey);
  p.end();
}

bool DataManager::GetCryptoKey(uint8_t *keyBuf) {
  if (cryptoKey.length() == 32) {
    return parseHexKey(cryptoKey.c_str(), keyBuf);
  }
  return false;
}

void DataManager::SetMqtt(bool enabled, const String &server, int port,
                          const String &user, const String &pass) {
  this->mqttEnabled = enabled;
  this->mqttServer = server;
  this->mqttPort = port;
  this->mqttUser = user;
  this->mqttPass = pass;

  Preferences p;
  p.begin("loralink", false);
  p.putBool("mqtt_en", enabled);
  p.putString("mqtt_srv", server);
  p.putInt("mqtt_prt", port);
  p.putString("mqtt_usr", user);
  p.putString("mqtt_pwd", pass);
  p.end();
}

void DataManager::SetESPNowEnabled(bool enabled) {
  espNowEnabled = enabled;
  Preferences p;
  p.begin("loralink", false);
  p.putBool("espnow_en", enabled);
  p.end();
}

void DataManager::SetWifiEnabled(bool enabled) {
  wifiEnabled = enabled;
  Preferences p;
  p.begin("loralink", false);
  p.putBool("wifi_en", enabled);
  p.end();
}

void DataManager::SetBleEnabled(bool enabled) {
  bleEnabled = enabled;
  Preferences p;
  p.begin("loralink", false);
  p.putBool("ble_en", enabled);
  p.end();
}

void DataManager::SaveESPNowPeer(int index, const uint8_t *mac,
                                 const char *name) {
  if (index < 0 || index >= ESPNOW_MAX_PEERS)
    return;

  memcpy(espNowPeers[index].mac, mac, 6);
  strncpy(espNowPeers[index].name, name, 15);
  espNowPeers[index].name[15] = '\0';
  espNowPeers[index].active = true;

  Preferences p;
  p.begin("espnow", false);
  String key_mac = "peer_mac_" + String(index);
  String key_name = "peer_name_" + String(index);
  p.putBytes(key_mac.c_str(), mac, 6);
  p.putString(key_name.c_str(), name);
  p.putInt("peer_count", max(numEspNowPeers, index + 1));
  p.end();

  if (index >= numEspNowPeers)
    numEspNowPeers = index + 1;
}

void DataManager::RemoveESPNowPeer(int index) {
  if (index < 0 || index >= ESPNOW_MAX_PEERS)
    return;
  espNowPeers[index].active = false;
  memset(espNowPeers[index].mac, 0, 6);
  espNowPeers[index].name[0] = '\0';

  Preferences p;
  p.begin("espnow", false);
  String key_mac = "peer_mac_" + String(index);
  String key_name = "peer_name_" + String(index);
  uint8_t zeroMac[6] = {0};
  p.putBytes(key_mac.c_str(), zeroMac, 6);
  p.putString(key_name.c_str(), "");
  // Note: We don't decrement peer_count to avoid index-shifting complexities,
  // but active=false handles it.
  p.end();
}

void DataManager::LoadESPNowPeers() {
  Preferences p;
  p.begin("espnow", true);
  numEspNowPeers = p.getInt("peer_count", 0);
  if (numEspNowPeers > ESPNOW_MAX_PEERS)
    numEspNowPeers = ESPNOW_MAX_PEERS;

  for (int i = 0; i < numEspNowPeers; i++) {
    String key_mac = "peer_mac_" + String(i);
    String key_name = "peer_name_" + String(i);
    size_t len = p.getBytes(key_mac.c_str(), espNowPeers[i].mac, 6);
    String name = p.getString(key_name.c_str(), "");
    strncpy(espNowPeers[i].name, name.c_str(), 15);
    espNowPeers[i].name[15] = '\0';
    espNowPeers[i].active = (len == 6);
  }
  p.end();
  LOG_PRINTF("INIT: Loaded %d ESP-NOW peers\n", numEspNowPeers);
}

void DataManager::UpdateNode(const char *id, uint32_t uptime, float battery,
                             uint8_t resetCode, float lat, float lon, int rssi,
                             uint8_t hops, uint8_t shortId) {
  if (strcmp(id, myId.c_str()) == 0)
    return;
  for (int i = 0; i < numNodes; i++) {
    if (strcmp(remoteNodes[i].id, id) == 0) {
      remoteNodes[i].lastSeen = millis();
      remoteNodes[i].battery = battery;
      remoteNodes[i].resetCode = resetCode;
      remoteNodes[i].uptime = uptime;
      remoteNodes[i].rssi = rssi;
      remoteNodes[i].hops = hops;
      remoteNodes[i].lat = lat;
      remoteNodes[i].lon = lon;
      if (shortId != 0xFF)
        remoteNodes[i].shortId = shortId;
      return;
    }
  }
  if (numNodes < MAX_NODES) {
    strncpy(remoteNodes[numNodes].id, id, 15);
    remoteNodes[numNodes].id[15] = 0;
    remoteNodes[numNodes].lastSeen = millis();
    remoteNodes[numNodes].battery = battery;
    remoteNodes[numNodes].resetCode = resetCode;
    remoteNodes[numNodes].uptime = uptime;
    remoteNodes[numNodes].rssi = rssi;
    remoteNodes[numNodes].hops = hops;
    remoteNodes[numNodes].lat = lat;
    remoteNodes[numNodes].lon = lon;
    remoteNodes[numNodes].shortId = shortId;
    numNodes++;
  }
}

void DataManager::SawNode(const char *id, int rssi, uint8_t hops,
                          uint8_t shortId) {
  if (strcmp(id, myId.c_str()) == 0)
    return;
  for (int i = 0; i < numNodes; i++) {
    if (strcmp(remoteNodes[i].id, id) == 0) {
      remoteNodes[i].lastSeen = millis();
      remoteNodes[i].rssi = rssi;
      remoteNodes[i].hops = hops;
      if (shortId != 0xFF)
        remoteNodes[i].shortId = shortId;
      return;
    }
  }
  if (numNodes < MAX_NODES) {
    strncpy(remoteNodes[numNodes].id, id, 15);
    remoteNodes[numNodes].id[15] = 0;
    remoteNodes[numNodes].lastSeen = millis();
    remoteNodes[numNodes].battery = 0.0f;
    remoteNodes[numNodes].resetCode = 0;
    remoteNodes[numNodes].uptime = 0;
    remoteNodes[numNodes].rssi = rssi;
    remoteNodes[numNodes].hops = hops;
    remoteNodes[numNodes].lat = 0.0f;
    remoteNodes[numNodes].lon = 0.0f;
    remoteNodes[numNodes].shortId = shortId;
    numNodes++;
  }
}

uint8_t DataManager::getShortIdByName(const String &name) {
  if (name.equalsIgnoreCase("ALL"))
    return 0xFF;
  if (name.equalsIgnoreCase(myId))
    return getMyShortId();
  for (int i = 0; i < numNodes; i++) {
    if (name.equalsIgnoreCase(remoteNodes[i].id))
      return remoteNodes[i].shortId;
  }
  return 0xFF;
}

String DataManager::getNameByShortId(uint8_t shortId) {
  if (shortId == 0xFF)
    return "ALL";
  if (shortId == getMyShortId())
    return myId;
  for (int i = 0; i < numNodes; i++) {
    if (remoteNodes[i].shortId == shortId)
      return String(remoteNodes[i].id);
  }
  return "UNKNOWN_0x" + String(shortId, HEX);
}

void DataManager::PruneStaleNodes() {
  unsigned long now = millis();
  for (int i = 0; i < numNodes; i++) {
    if (now - remoteNodes[i].lastSeen > 300000) { // 5 minutes
      LOG_PRINTF("MESH: Pruned stale node: %s\n", remoteNodes[i].id);
      // Shift remaining nodes down
      for (int j = i; j < numNodes - 1; j++) {
        remoteNodes[j] = remoteNodes[j + 1];
      }
      numNodes--;
      i--; // Re-check this index
    }
  }
}

void DataManager::LogMessage(const String &source, int rssi,
                             const String &msg) {
  msgLog[logIndex].timestamp = millis() / 1000;
  strncpy(msgLog[logIndex].source, source.c_str(), 15);
  msgLog[logIndex].source[15] = '\0';
  msgLog[logIndex].rssi = (int16_t)rssi;

  strncpy(msgLog[logIndex].message, msg.c_str(), 63);
  msgLog[logIndex].message[63] = '\0';

  logIndex = (logIndex + 1) % LOG_SIZE;

  if (traceLogging) {
    String formattedMsg =
        String("[") + (millis() / 1000) + "] " + source + ": " + msg;

    File f = LittleFS.open("/trace.log", "a");
    if (f) {
      f.printf("%s\n", formattedMsg.c_str());
      f.close();
    }

    // MQTT Trace Bridge
    if (mqttEnabled) {
      MQTTManager::getInstance().publishTrace(myId, formattedMsg);
    }
  }
}

void DataManager::SetSchedulerInterval(unsigned long ms) {
  schedulerInterval110V = ms;
  Preferences p;
  p.begin("loralink", false);
  p.putULong("sched_int_110", ms);
  p.end();
}

void DataManager::SetGpioState(const String &pinName, bool state) {
  Preferences p;
  p.begin("lora_hw", false);
  p.putBool(pinName.c_str(), state);
  p.end();
}

bool DataManager::GetGpioState(const String &pinName) {
  Preferences p;
  p.begin("lora_hw", true);
  bool state = p.getBool(pinName.c_str(), false);
  p.end();
  return state;
}

void DataManager::SetPinName(const String &pin, const String &name) {
  Preferences p;
  p.begin("pin_names", false);
  p.putString(pin.c_str(), name);
  p.end();
}

String DataManager::GetPinName(const String &pin) {
  Preferences p;
  p.begin("pin_names", true);
  String name = p.getString(pin.c_str(), "");
  p.end();
  return name;
}

void DataManager::ClearTrace() {
  if (LittleFS.exists("/trace.log")) {
    LittleFS.remove("/trace.log");
  }
}

void DataManager::FactoryReset() {
  LOG_PRINTLN("SYS: FACTORY RESET (Clearing NVS)...");
  Preferences p;
  p.begin("loralink", false);
  p.clear();
  p.end();
  p.begin("lora_hw", false);
  p.clear();
  p.end();
  p.begin("espnow", false);
  p.clear();
  p.end();
}

String DataManager::getResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  switch (reason) {
  case ESP_RST_POWERON:
    return "Power On";
  case ESP_RST_EXT:
    return "Ext Pin";
  case ESP_RST_SW:
    return "Soft Reset";
  case ESP_RST_PANIC:
    return "Crash";
  case ESP_RST_INT_WDT:
    return "Int WDT";
  case ESP_RST_TASK_WDT:
    return "Task WDT";
  case ESP_RST_WDT:
    return "Other WDT";
  case ESP_RST_DEEPSLEEP:
    return "Deep Sleep";
  case ESP_RST_BROWNOUT:
    return "Brownout";
  default:
    return "Unknown";
  }
}

bool DataManager::ImportConfig(const String &json) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, json);
  if (error)
    return false;

  // 1. Validate Hardware ID
  const char *hw = doc["hardware_id"];
  if (!hw || strcmp(hw, HARDWARE_ID) != 0) {
    LOG_PRINTF("CFG: Hardware mismatch (Got: %s, Expected: %s)\n",
               hw ? hw : "NULL", HARDWARE_ID);
    return false;
  }

  // 2. Apply settings
  JsonObject s = doc["settings"];
  if (s.isNull())
    return false;

  if (s.containsKey("dev_name"))
    SetName(s["dev_name"].as<String>());
  if (s.containsKey("wifi_ssid"))
    SetWifi(s["wifi_ssid"].as<String>(), s["wifi_pass"] | "");
  if (s.containsKey("static_ip"))
    SetStaticIp(s["static_ip"].as<String>(), s["gateway"] | "",
                s["subnet"] | "");
  if (s.containsKey("repeater"))
    SetRepeater(s["repeater"].as<bool>());
  if (s.containsKey("mqtt_en"))
    SetMqtt(s["mqtt_en"].as<bool>(), s["mqtt_srv"] | "", s["mqtt_prt"] | 1883,
            s["mqtt_usr"] | "", s["mqtt_pwd"] | "");
  if (s.containsKey("espnow_en"))
    SetESPNowEnabled(s["espnow_en"].as<bool>());
  if (s.containsKey("wifi_en"))
    SetWifiEnabled(s["wifi_en"].as<bool>());
  if (s.containsKey("ble_en"))
    SetBleEnabled(s["ble_en"].as<bool>());
  if (s.containsKey("crypto_key"))
    SetCryptoKey(s["crypto_key"].as<String>());

  if (doc.containsKey("pins")) {
    JsonObject pins = doc["pins"];
    for (JsonPair p : pins) {
      int pin = atoi(p.key().c_str());
      JsonObject pinData = p.value().as<JsonObject>();
      if (pinData.containsKey("name")) {
        SetPinName(String(pin), pinData["name"].as<String>());
      }
      if (pinData.containsKey("en")) {
        SetPinEnabled(pin, pinData["en"].as<bool>());
      }
    }
  }

  return true;
}

String DataManager::ExportConfig() {
  JsonDocument doc;
  doc["schema"] = CONFIG_SCHEMA;
  doc["hardware_id"] = HARDWARE_ID;
  JsonObject s = doc.createNestedObject("settings");

  s["dev_name"] = myId;
  s["wifi_ssid"] = wifiSsid;
  s["wifi_pass"] = wifiPass;
  s["static_ip"] = staticIp;
  s["gateway"] = gateway;
  s["subnet"] = subnet;
  s["repeater"] = repeaterEnabled;
  s["mqtt_en"] = mqttEnabled;
  s["mqtt_srv"] = mqttServer;
  s["mqtt_prt"] = mqttPort;
  s["mqtt_usr"] = mqttUser;
  s["mqtt_pwd"] = mqttPass;
  s["espnow_en"] = espNowEnabled;
  s["wifi_en"] = wifiEnabled;
  s["ble_en"] = bleEnabled;
  s["crypto_key"] = cryptoKey;

  JsonObject pins = doc.createNestedObject("pins");
  for (int i = 0; i < 48; i++) {
    // Only export if customized (name exists or enabled)
    String name = GetPinName(String(i));
    bool en = GetPinEnabled(i);
    if (name.length() > 0 || en) {
      JsonObject p = pins.createNestedObject(String(i));
      if (name.length() > 0)
        p["name"] = name;
      if (en)
        p["en"] = en;
    }
  }

  String out;
  serializeJson(doc, out);
  return out;
}

void DataManager::SetPinEnabled(int pin, bool enabled) {
  Preferences p;
  p.begin("loralink", false);
  String key = "en_" + String(pin);
  p.putBool(key.c_str(), enabled);
  p.end();
}

bool DataManager::GetPinEnabled(int pin) {
  Preferences p;
  p.begin("loralink", true);
  String key = "en_" + String(pin);
  bool state = p.getBool(key.c_str(), false);
  p.end();
  return state;
}

void DataManager::SetTransportMode(char mode) {
  transportMode = mode;
  Preferences p;
  p.begin("loralink", false);
  p.putString("tp_mode", String(mode));
  p.end();
}

char DataManager::GetTransportMode() { return transportMode; }

void DataManager::AddToRegistry(const String &id, const String &hwType) {
  JsonDocument doc;
  if (LittleFS.exists("/hw_reg.json")) {
    File f = LittleFS.open("/hw_reg.json", "r");
    deserializeJson(doc, f);
    f.close();
  }
  doc[id] = hwType;
  File f = LittleFS.open("/hw_reg.json", "w");
  serializeJson(doc, f);
  f.close();
}

String DataManager::GetRegistryJson() {
  if (!LittleFS.exists("/hw_reg.json"))
    return "{}";
  File f = LittleFS.open("/hw_reg.json", "r");
  String s = f.readString();
  f.close();
  return s;
}

void DataManager::RegisterPeripheral(const String &id, const String &hwType,
                                     const String &fw, const String &caps) {
  // Check if already exists
  for (int i = 0; i < numPeripherals; i++) {
    if (strcmp(peripherals[i].id, id.c_str()) == 0) {
      strncpy(peripherals[i].hwType, hwType.c_str(), 15);
      peripherals[i].hwType[15] = '\0';
      strncpy(peripherals[i].fwVersion, fw.c_str(), 11);
      peripherals[i].fwVersion[11] = '\0';
      strncpy(peripherals[i].caps, caps.c_str(), 31);
      peripherals[i].caps[31] = '\0';
      peripherals[i].lastSeen = millis();
      return;
    }
  }

  // Add new
  if (numPeripherals < MAX_PERIPHERALS) {
    strncpy(peripherals[numPeripherals].id, id.c_str(), 15);
    peripherals[numPeripherals].id[15] = '\0';
    strncpy(peripherals[numPeripherals].hwType, hwType.c_str(), 15);
    peripherals[numPeripherals].hwType[15] = '\0';
    strncpy(peripherals[numPeripherals].fwVersion, fw.c_str(), 11);
    peripherals[numPeripherals].fwVersion[11] = '\0';
    strncpy(peripherals[numPeripherals].caps, caps.c_str(), 31);
    peripherals[numPeripherals].caps[31] = '\0';
    strcpy(peripherals[numPeripherals].lastReadings, "{}");
    peripherals[numPeripherals].lastSeen = millis();
    numPeripherals++;
    LOG_PRINTF("REG: Peripheral registered: %s (%s)\n", id.c_str(),
               hwType.c_str());
  }
}

void DataManager::UpdateSensorTelemetry(const String &id,
                                        const String &jsonReadings) {
  for (int i = 0; i < numPeripherals; i++) {
    if (strcmp(peripherals[i].id, id.c_str()) == 0) {
      strncpy(peripherals[i].lastReadings, jsonReadings.c_str(), 63);
      peripherals[i].lastReadings[63] = '\0';
      peripherals[i].lastSeen = millis();
      return;
    }
  }
}

String DataManager::GetPeripheralsJson() {
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  for (int i = 0; i < numPeripherals; i++) {
    JsonObject obj = array.add<JsonObject>();
    obj["id"] = String(peripherals[i].id);
    obj["hw"] = String(peripherals[i].hwType);
    obj["fw"] = String(peripherals[i].fwVersion);
    obj["caps"] = String(peripherals[i].caps);
    obj["data"] = serialized(String(peripherals[i].lastReadings));
    obj["lastSeen"] = (millis() - peripherals[i].lastSeen) / 1000;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

// ── Transport Link Helpers
// ────────────────────────────────────────────────────

const char *DataManager::linkName(LinkPreference lp) {
  switch (lp) {
  case LinkPreference::LINK_AUTO:
    return "AUTO";
  case LinkPreference::LINK_BLE:
    return "BLE";
  case LinkPreference::LINK_WIFI_MQTT:
    return "WIFI_MQTT";
  case LinkPreference::LINK_WIFI_HTTP:
    return "WIFI_HTTP";
  case LinkPreference::LINK_LORA:
    return "LORA";
  default:
    return "UNKNOWN";
  }
}

void DataManager::SetPreferredLink(LinkPreference pref) {
  preferredLink = pref;
  currentLink = pref;
  Preferences p;
  p.begin("loralink", false);
  p.putUChar("link_pref", (uint8_t)pref);
  p.end();
  LOG_PRINTF("TRANS: Preferred link set → %s\n", linkName(pref));
}

void DataManager::SetProbeState(uint32_t backoffMs, uint8_t failCount) {
  probeBackoffMs = backoffMs;
  probeFailCount = failCount;
  Preferences p;
  p.begin("loralink", false);
  p.putULong("probe_bkoff", backoffMs);
  p.putUChar("probe_fails", failCount);
  p.end();
}

void DataManager::ResetProbeState() {
  SetProbeState(PROBE_BACKOFF_MIN_MS, 0);
  lastProbeResult = "";
  lastProbeAtMs = 0;
}
