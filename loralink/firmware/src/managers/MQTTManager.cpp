#include "MQTTManager.h"
#include "../utils/DebugMacros.h"
#include "CommandManager.h"
#include "DataManager.h"
#include <ArduinoJson.h>

MQTTManager::MQTTManager() : client(wifiClient) { lastReconnectAttempt = 0; }

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String topicStr = String(topic);
  String msg;
  unsigned int limit = (length > 256) ? 256 : length;
  for (unsigned int i = 0; i < limit; i++) {
    msg += (char)payload[i];
  }
  if (length > 256) {
    LOG_PRINTF("MQTT_RX: Truncated message (len=%u)\n", length);
  }
  LOG_PRINTLN("MQTT_RX: [" + topicStr + "] " + msg);

  DataManager &data = DataManager::getInstance();
  String cmdTopic = "loralink/cmd/" + data.myId;
  if (topicStr.equalsIgnoreCase(cmdTopic)) {
    // Treat MQTT received string as a command target
    CommandManager::getInstance().handleCommand(msg,
                                                CommInterface::COMM_INTERNAL);
  }
}

void MQTTManager::Init() {
  DataManager &data = DataManager::getInstance();
  if (!data.mqttEnabled || data.mqttServer.length() == 0)
    return;

  client.setServer(data.mqttServer.c_str(), data.mqttPort);
  client.setCallback(mqttCallback);
  LOG_PRINTLN("MQTT: Initialized to server " + data.mqttServer);
}

bool MQTTManager::reconnect() {
  DataManager &data = DataManager::getInstance();
  if (!data.mqttEnabled)
    return false;

  String clientId = "LoRaLink_" + data.myId;
  String statusTopic = "loralink/status/" + data.myId;
  bool result;

  // LWT: Set Last Will to report "OFFLINE" if we disconnect unexpectedly
  if (data.mqttUser.length() > 0) {
    result = client.connect(clientId.c_str(), data.mqttUser.c_str(),
                            data.mqttPass.c_str(), statusTopic.c_str(), 0, true,
                            "OFFLINE");
  } else {
    result = client.connect(clientId.c_str(), statusTopic.c_str(), 0, true,
                            "OFFLINE");
  }

  if (result) {
    LOG_PRINTLN("MQTT: Connected to " + data.mqttServer);

    // Publish "ONLINE" status immediately upon connection
    client.publish(statusTopic.c_str(), "ONLINE", true);

    // Subscribe to individual and global command topics
    String cmdTopic = "loralink/cmd/" + data.myId;
    client.subscribe(cmdTopic.c_str());
    client.subscribe("loralink/cmd/all");

    LOG_PRINTLN("MQTT: Subscribed to control topics");
    return true;
  }
  return false;
}

void MQTTManager::loop() {
  DataManager &data = DataManager::getInstance();
  if (!data.mqttEnabled)
    return;

  if (WiFi.status() != WL_CONNECTED)
    return;

  if (!client.connected()) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = now;
      if (reconnect()) {
        lastReconnectAttempt = 0;
      }
    }
  } else {
    client.loop();
  }
}

void MQTTManager::publishTelemetry(const String &nodeId, float battery,
                                   int rssi, int hops) {
  if (!client.connected())
    return;

  JsonDocument doc;
  doc["node"] = nodeId;
  doc["battery"] = battery;
  doc["rssi"] = rssi;
  doc["hops"] = hops;

  String json;
  serializeJson(doc, json);

  String topic = "loralink/telemetry/" + nodeId;
  client.publish(topic.c_str(), json.c_str(), true); // Retained
}

void MQTTManager::publishMessage(const String &nodeId, int rssi,
                                 const String &text) {
  if (!client.connected())
    return;

  JsonDocument doc;
  doc["node"] = nodeId;
  doc["rssi"] = rssi;
  doc["text"] = text;

  String json;
  serializeJson(doc, json);

  String topic = "loralink/msg/" + nodeId;
  client.publish(topic.c_str(), json.c_str());
}

void MQTTManager::publishTrace(const String &nodeId, const String &msg) {
  if (!client.connected())
    return;

  String topic = "loralink/trace/" + nodeId;
  client.publish(topic.c_str(), msg.c_str());
}

void MQTTManager::publish(const String &topic, const String &payload) {
  if (!client.connected())
    return;
  client.publish(topic.c_str(), payload.c_str());
}
