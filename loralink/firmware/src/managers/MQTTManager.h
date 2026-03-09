#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>

class MQTTManager {
public:
  static MQTTManager &getInstance() {
    static MQTTManager instance;
    return instance;
  }

  void Init();
  void loop();
  void publishTelemetry(const String &nodeId, float battery, int rssi,
                        int hops);
  void publishMessage(const String &nodeId, int rssi, const String &text);
  void publishTrace(const String &nodeId, const String &msg);
  void publish(const String &topic, const String &payload);

private:
  MQTTManager();
  WiFiClient wifiClient;
  PubSubClient client;
  unsigned long lastReconnectAttempt;
  bool reconnect();
};

#endif // MQTT_MANAGER_H
