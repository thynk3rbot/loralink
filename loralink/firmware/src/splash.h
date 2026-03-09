// splash.h — SPW LoRaLink Splash Screen
// Drawn with OLED primitives for maximum clarity on 128x64 SSD1306
// Copyright 2026 Steven P Williams spw1.com
#pragma once
#include "config.h"
#include <math.h>


// Draws the SPW LoRaLink splash screen on the Heltec OLED.
// Call Heltec.display->display() after this to push to screen.
inline void drawSplash(OLEDDisplay *display) {
  display->clear();
  display->setColor(WHITE);

  // === "SPW" in large bold font (center-top) ===
  display->setFont(ArialMT_Plain_24);
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->drawString(52, 2, "SPW");

  // === Radio wave arcs (emanating from the W) ===
  int cx = 90, cy = 14;

  for (int a = -45; a <= 45; a += 3) {
    float rad = a * 3.14159 / 180.0;
    display->setPixel(cx + (int)(10.0 * cos(rad)), cy + (int)(10.0 * sin(rad)));
  }
  for (int a = -45; a <= 45; a += 2) {
    float rad = a * 3.14159 / 180.0;
    display->setPixel(cx + (int)(16.0 * cos(rad)), cy + (int)(16.0 * sin(rad)));
  }
  for (int a = -45; a <= 45; a += 2) {
    float rad = a * 3.14159 / 180.0;
    display->setPixel(cx + (int)(22.0 * cos(rad)), cy + (int)(22.0 * sin(rad)));
  }

  // === "LoRaLink" below in medium font ===
  display->setFont(ArialMT_Plain_16);
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->drawString(64, 30, "LoRaLink " FIRMWARE_VERSION);

  // === Copyright footer ===
  display->setFont(ArialMT_Plain_10);
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->drawString(64, 52, "\xA9 2026 spw1.com");

  // Decorative lines
  display->drawLine(0, 0, 127, 0);
  display->drawLine(0, 63, 127, 63);
}
