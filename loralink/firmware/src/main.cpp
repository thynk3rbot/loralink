// ============================================================================
//  LoRaLink-AnyToAny — Unified Wireless Communication Framework
//  Main Firmware for Heltec WiFi LoRa 32 V3
//  (c) 2026 Steven P Williams (spw1.com)
// ============================================================================

#include "config.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>

// Managers
#include "managers/BLEManager.h"
#include "managers/CommandManager.h"
#include "managers/DataManager.h"
#include "managers/DisplayManager.h"
#include "managers/ESPNowManager.h"
#include "managers/LoRaManager.h"
#include "managers/MCPManager.h"
#include "managers/MQTTManager.h"
#include "managers/PerformanceManager.h"
#include "managers/ProductManager.h"
#include "managers/ScheduleManager.h"
#include "managers/WiFiManager.h"

#ifndef UNIT_TEST
// ============================================================================
//   BOOT SEQUENCE
// ============================================================================
void setup() {
  // 1. Serial
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n########################################");
  Serial.println("#  LORALINK-ANY2ANY " FIRMWARE_VERSION "             #");
  Serial.println("#  Unified Wireless Gateway            #");
  Serial.println("########################################");

  // 2. PRG button factory reset window
  pinMode(PIN_BUTTON_PRG, INPUT_PULLUP);
  Serial.println("BOOT: Hold PRG for 3s to factory reset...");
  unsigned long windowStart = millis();
  unsigned long pressStart = 0;
  bool pressing = false;
  bool resetTriggered = false;
  while (millis() - windowStart < 5000) {
    if (digitalRead(PIN_BUTTON_PRG) == LOW) {
      if (!pressing) {
        pressing = true;
        pressStart = millis();
      }
      if (millis() - pressStart >= 3000) {
        resetTriggered = true;
        break;
      }
    } else {
      pressing = false;
    }
    delay(10);
  }
  if (resetTriggered) {
    Serial.println("BOOT: Factory Reset triggered!");
    DataManager::getInstance().FactoryReset();
    delay(1000);
    ESP.restart();
  }

  // 3. Power rail initialization (critical for Heltec V3 peripherals)
  pinMode(PIN_VEXT_CTRL, OUTPUT);
  digitalWrite(PIN_VEXT_CTRL, LOW); // Power ON display/LoRa rail
  pinMode(PIN_BAT_CTRL, OUTPUT);
  digitalWrite(PIN_BAT_CTRL, LOW); // Power ON battery divider
  delay(100);

  // 4. CPU clock optimization - Temporarily DISABLED to rule out clock
  // instability setCpuFrequencyMhz(80);
  Serial.printf("SYS: CPU Clock = %dMHz\n", getCpuFrequencyMhz());

  // 5. Heltec init (display only, LoRa handled by LoRaManager)
  Heltec.begin(true, false, true, false, 0);
  if (Heltec.display) {
    Heltec.display->setContrast(255);
    Heltec.display->setBrightness(255);
  }

  // 6. Data Manager
  Serial.println("BOOT: DataManager...");
  DataManager &data = DataManager::getInstance();
  data.Init();

  // 6.1 Performance Manager
  PerformanceManager::getInstance().init();

  Serial.println("ID: " + data.myId + " [VAL:" + data.getMacSuffix() + "]");
  Serial.flush();

  // 6.5. MCP23017 I2C GPIO Expander
  delay(50);
  MCPManager::getInstance().init();
  Serial.flush();

  // 6.6. Product Manager - restore active product pin modes
  ProductManager::getInstance().restoreActiveProduct();
  Serial.flush();

  // 7. Command Manager - restore hardware state
  Serial.println("BOOT: Restoring hardware state...");
  CommandManager::getInstance().restoreHardwareState();
  Serial.flush();

  // 8. LoRa Manager
  Serial.println("BOOT: LoRaManager...");
  LoRaManager::getInstance().Init();
  LoRaManager::SetCallback([](const String &msg, CommInterface ifc) {
    CommandManager::getInstance().handleCommand(msg, ifc);
  });
  Serial.flush();

  // 9. BLE Manager (deferred to ScheduleManager task for staggered start)
  Serial.println("BOOT: BLEManager... (Deferred)");
  Serial.flush();

  // 10. WiFi Manager
  Serial.println("BOOT: WiFiManager...");
  if (DataManager::getInstance().wifiEnabled) {
    WiFiManager::getInstance().init();
  } else {
    Serial.println("WiFi: Disabled by RADIO profile.");
  }
  setWebCallback([](const String &cmd, CommInterface ifc) {
    CommandManager::getInstance().handleCommand(cmd, ifc);
  });
  Serial.flush();

  // 10.5 Transport negotiation window
  // Spend up to transNegotiateMs probing the preferred transport, then lock
  // currentLink. Configurable via NVS "trans_neg_ms" (default 10 000ms).
  {
    DataManager &d = DataManager::getInstance();
    Serial.printf("BOOT: Link pref=%s  negotiate=%lums\n",
                  DataManager::linkName(d.preferredLink), d.transNegotiateMs);
    if (d.wifiEnabled) {
      WiFiManager::getInstance().negotiate(d.transNegotiateMs);
    } else {
      d.currentLink = LinkPreference::LINK_LORA;
      Serial.println("BOOT: WiFi disabled — locked to LORA");
    }
    Serial.printf("BOOT: Current link → %s\n",
                  DataManager::linkName(d.currentLink));
  }
  Serial.flush();

  // 11. ESP-NOW Manager
  Serial.println("BOOT: ESPNowManager...");
  ESPNowManager::getInstance().init();
  Serial.flush();

  // 12. Display Manager
  Serial.println("BOOT: DisplayManager...");
  DisplayManager::getInstance().Init();
  Serial.flush();

  // 13. MQTT Manager
  Serial.println("BOOT: MQTTManager...");
  MQTTManager::getInstance().Init();
  Serial.flush();

  // 13. Schedule Manager - start all tasks
  Serial.println("BOOT: ScheduleManager...");
  ScheduleManager::getInstance().init();
  Serial.flush();

  Serial.println("BOOT: Setup OK — Entering Event Loop.");
  Serial.printf("BOOT: Free Heap: %u bytes\n", ESP.getFreeHeap());
  Serial.println("########################################\n");
}

// ============================================================================
//   MAIN LOOP
// ============================================================================
void loop() {
  PerformanceManager::getInstance().loopTickStart();

  ScheduleManager::getInstance().execute();
  MQTTManager::getInstance().loop();

  PerformanceManager::getInstance().loopTickEnd();
}
#endif
