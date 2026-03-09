#include "DisplayManager.h"
#include "../utils/DebugMacros.h"
#include "DataManager.h"
#include "heltec.h"
#include <WiFi.h>

DisplayManager::DisplayManager() {
  currentPage = 0;
  displayActive = true;
  lastDisplayActivity = 0;
  lastButtonPress = 0;
  batteryVolts = 0;
}

void DisplayManager::Init() {
  Serial.println("Display: VEXT Powering ON...");
  Serial.flush();

  pinMode(PIN_VEXT_CTRL, OUTPUT);
  digitalWrite(PIN_VEXT_CTRL, LOW);
  pinMode(PIN_BAT_CTRL, OUTPUT);
  digitalWrite(PIN_BAT_CTRL, LOW);
  delay(100);

  displayActive = true;
  lastDisplayActivity = millis();
  Serial.println("Display: VEXT OK");
  Serial.flush();

  if (Heltec.display) {
    Heltec.display->setContrast(255);
    Heltec.display->setBrightness(255);
  }
  ShowSplash();
  delay(2000);
}

void DisplayManager::ShowSplash() {
  if (Heltec.display == NULL)
    return;
  DataManager &data = DataManager::getInstance();
  Heltec.display->clear();
  Heltec.display->setColor(WHITE);

  for (int i = 0; i < 3; i++) {
    Heltec.display->drawCircle(64, 32, 20 + i * 15);
  }
  Heltec.display->drawLine(0, 32, 128, 32);
  Heltec.display->drawLine(64, 0, 64, 64);

  Heltec.display->setColor(BLACK);
  Heltec.display->fillRect(34, 15, 60, 35);
  Heltec.display->setColor(WHITE);

  Heltec.display->setFont(ArialMT_Plain_24);
  Heltec.display->setTextAlignment(TEXT_ALIGN_CENTER);
  Heltec.display->drawString(64, 15, "SPW");

  Heltec.display->setFont(ArialMT_Plain_10);
  Heltec.display->drawString(64, 40, "Any2Any");

  Heltec.display->setTextAlignment(TEXT_ALIGN_LEFT);
  Heltec.display->drawString(2, 52, "B:" + String(data.bootCount));

  Heltec.display->setTextAlignment(TEXT_ALIGN_RIGHT);
  Heltec.display->drawString(126, 52, "R:" + data.getResetReason());

  Heltec.display->display();
}

void DisplayManager::SetDisplayActive(bool active) {
  displayActive = active;
  if (Heltec.display == NULL)
    return;
  if (active) {
    Heltec.display->displayOn();
    lastDisplayActivity = millis();
  } else {
    Heltec.display->displayOff();
  }
}

bool DisplayManager::IsDisplayActive() { return displayActive; }

void DisplayManager::NextPage() {
  currentPage = (currentPage + 1) % NUM_PAGES;
  LOG_PRINTF("DISP: NextPage() -> Page %d\n", currentPage);
  SetDisplayActive(true);
}

void DisplayManager::DrawUi() {
  if (!displayActive || Heltec.display == NULL)
    return;

  DataManager &data = DataManager::getInstance();

  Heltec.display->clear();
  Heltec.display->setColor(WHITE);

  batteryVolts = analogRead(PIN_BAT_ADC) / 4095.0 * 3.3 * BAT_VOLT_MULTI;

  for (int i = 0; i < NUM_PAGES; i++) {
    int x = 128 - (NUM_PAGES - i) * 8;
    if (i == currentPage) {
      Heltec.display->fillCircle(x, 61, 2);
    } else {
      Heltec.display->drawCircle(x, 61, 2);
    }
  }
  Serial.flush();

  switch (currentPage) {
  case 0:
    drawHome(data);
    break;
  case 1:
    drawNetwork(data);
    break;
  case 2:
    drawStatus(data);
    break;
  case 3:
    drawLog(data);
    break;
  }
  Heltec.display->display();
}

void DisplayManager::drawHome(DataManager &data) {
  Heltec.display->drawLine(0, 12, 128, 12);

  // Status Line (10pt)
  Heltec.display->setFont(ArialMT_Plain_10);
  Heltec.display->setTextAlignment(TEXT_ALIGN_LEFT);
  Heltec.display->drawString(0, 0, "LORA READY");

  Heltec.display->setTextAlignment(TEXT_ALIGN_RIGHT);
  String icons = "";
  if (WiFi.status() == WL_CONNECTED)
    icons += "Wi ";
  if (data.repeaterEnabled)
    icons += "Rp ";
  if (data.espNowEnabled)
    icons += "EN ";
  if (data.encryptionActive)
    icons += "Enc ";
  Heltec.display->drawString(128, 0, icons);

  // Device Name (Large 16pt)
  Heltec.display->setFont(ArialMT_Plain_16);
  Heltec.display->setTextAlignment(TEXT_ALIGN_CENTER);
  Heltec.display->drawString(64, 30, data.myId);
}

void DisplayManager::drawNetwork(DataManager &data) {
  Heltec.display->setFont(ArialMT_Plain_10);
  Heltec.display->setTextAlignment(TEXT_ALIGN_LEFT);
  Heltec.display->drawString(0, 0, "NETWORK");
  Heltec.display->drawLine(0, 12, 128, 12);

  if (WiFi.status() == WL_CONNECTED) {
    Heltec.display->setFont(ArialMT_Plain_16);
    Heltec.display->setTextAlignment(TEXT_ALIGN_CENTER);
    Heltec.display->drawString(64, 16, WiFi.localIP().toString());
    Heltec.display->setFont(ArialMT_Plain_10);
    Heltec.display->drawString(64, 36, "SSID: " + WiFi.SSID());
    Heltec.display->drawString(64, 48, String(WiFi.RSSI()) + " dBm");
    Heltec.display->setTextAlignment(TEXT_ALIGN_RIGHT);
    Heltec.display->drawString(128, 0, "[" + data.getMacSuffix() + "]");
  } else {
    Heltec.display->setFont(ArialMT_Plain_10);
    Heltec.display->setTextAlignment(TEXT_ALIGN_CENTER);
    if (data.wifiSsid.length() > 0) {
      Heltec.display->drawString(64, 20, "Connecting to:");
      Heltec.display->setFont(ArialMT_Plain_16);
      Heltec.display->drawString(64, 35, data.wifiSsid);
    } else {
      Heltec.display->setFont(ArialMT_Plain_16);
      Heltec.display->drawString(64, 22, "No WiFi");
      Heltec.display->setFont(ArialMT_Plain_10);
      Heltec.display->drawString(64, 42, "Use Web Config");
    }
  }
  Heltec.display->setTextAlignment(TEXT_ALIGN_RIGHT);
  Heltec.display->drawString(128, 0, "[" + data.getMacSuffix() + "]");
}

void DisplayManager::drawStatus(DataManager &data) {
  Heltec.display->setFont(ArialMT_Plain_10);
  Heltec.display->setTextAlignment(TEXT_ALIGN_LEFT);
  Heltec.display->drawString(0, 0, "STATUS");
  Heltec.display->drawLine(0, 12, 128, 12);

  unsigned long s = millis() / 1000;
  String uptime = String(s / 3600) + "h " + String((s % 3600) / 60) + "m";

  Heltec.display->setFont(ArialMT_Plain_16);
  Heltec.display->setTextAlignment(TEXT_ALIGN_CENTER);
  Heltec.display->drawString(64, 14, "Bat: " + String(batteryVolts, 2) + "V");

  Heltec.display->setFont(ArialMT_Plain_10);
  Heltec.display->setTextAlignment(TEXT_ALIGN_LEFT);
  Heltec.display->drawString(0, 32, "Up: " + uptime);
  Heltec.display->drawString(
      0, 42, "N:" + String(data.numNodes) + " FW:" + FIRMWARE_VERSION);

  String gps = "L:0.00 N:0.00";
  if (data.numNodes > 0) {
    gps = "L:" + String(data.remoteNodes[0].lat, 2) +
          " N:" + String(data.remoteNodes[0].lon, 2);
  }
  Heltec.display->drawString(0, 52, gps);

  Heltec.display->setTextAlignment(TEXT_ALIGN_RIGHT);
  Heltec.display->drawString(128, 0, "[" + data.getMacSuffix() + "]");
}

void DisplayManager::drawLog(DataManager &data) {
  Heltec.display->setFont(ArialMT_Plain_10);
  Heltec.display->setTextAlignment(TEXT_ALIGN_LEFT);
  Heltec.display->drawString(0, 0, "LOG");
  Heltec.display->drawLine(0, 12, 128, 12);

  for (int i = 0; i < 4; i++) {
    int idx = (data.logIndex - 1 - i + LOG_SIZE) % LOG_SIZE;
    if (strlen(data.msgLog[idx].message) > 0) {
      String msg = String(data.msgLog[idx].message);
      Heltec.display->drawString(0, 14 + i * 12, msg.substring(0, 21));
    }
  }
  Heltec.display->setTextAlignment(TEXT_ALIGN_RIGHT);
  Heltec.display->drawString(128, 0, "[" + data.getMacSuffix() + "]");
}
