#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include "../config.h"
#include "DataManager.h"
#include "heltec.h"

#define NUM_PAGES 4

class DisplayManager {
public:
  static DisplayManager &getInstance() {
    static DisplayManager instance;
    return instance;
  }

  void Init();
  void ShowSplash();
  void DrawUi();
  void NextPage();
  void SetDisplayActive(bool active);
  bool IsDisplayActive();

  int currentPage;
  bool displayActive;
  unsigned long lastDisplayActivity;
  unsigned long lastButtonPress;

private:
  DisplayManager();
  void drawHome(DataManager &data);
  void drawNetwork(DataManager &data);
  void drawStatus(DataManager &data);
  void drawLog(DataManager &data);

  float batteryVolts;
};

#endif // DISPLAY_MANAGER_H
