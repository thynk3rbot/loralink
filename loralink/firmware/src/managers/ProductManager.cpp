#include "ProductManager.h"
#include "CommandManager.h"
#include "DataManager.h"
#include "MCPManager.h"
#include "ScheduleManager.h"
#include "utils/DebugMacros.h"
#include <LittleFS.h>
#include <Preferences.h>

// ── Persistence helpers ──────────────────────────────────────────────────────

static String _productPath(const String &name) {
  return "/products/" + name + ".json";
}

static void _ensureDir() {
  if (!LittleFS.exists("/products")) {
    LittleFS.mkdir("/products");
  }
}

// ── Public API ───────────────────────────────────────────────────────────────

bool ProductManager::saveProduct(const String &json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    LOG_PRINTLN("PRODUCT: Invalid JSON in saveProduct");
    return false;
  }
  String name = doc["name"] | "";
  if (name.isEmpty()) {
    LOG_PRINTLN("PRODUCT: Missing 'name' field");
    return false;
  }
  _ensureDir();
  String path = _productPath(name);
  File f = LittleFS.open(path, "w");
  if (!f) {
    LOG_PRINTF("PRODUCT: Cannot open %s for write\n", path.c_str());
    return false;
  }
  f.print(json);
  f.close();
  LOG_PRINTF("PRODUCT: Saved '%s' to %s\n", name.c_str(), path.c_str());
  return true;
}

bool ProductManager::loadProduct(const String &name, CommInterface source) {
  String path = _productPath(name);
  if (!LittleFS.exists(path)) {
    String err = "ERR: Product '" + name + "' not found";
    CommandManager::getInstance().sendResponse(err, source);
    return false;
  }

  File f = LittleFS.open(path, "r");
  String json = f.readString();
  f.close();

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    CommandManager::getInstance().sendResponse("ERR: Product JSON corrupt",
                                               source);
    return false;
  }

  int pinCount = 0, schedCount = 0, alertCount = 0;

  if (doc["pins"].is<JsonArray>()) {
    applyPins(doc["pins"], source);
    pinCount = doc["pins"].size();
  }
  if (doc["schedules"].is<JsonArray>()) {
    applySchedules(doc["schedules"], source);
    schedCount = doc["schedules"].size();
  }
  if (doc["alerts"].is<JsonArray>()) {
    applyAlerts(doc["alerts"], source);
    alertCount = doc["alerts"].size();
  }

  // Persist all new tasks to /schedule.json in one pass
  ScheduleManager::getInstance().saveDynamicTasks();

  // Store active product in NVS
  _activeProduct = name;
  Preferences p;
  p.begin("products", false);
  p.putString("active", name);
  p.end();

  String label = doc["label"] | name;
  String reply = "PRODUCT: loaded '" + label + "' (" + String(pinCount) +
                 " pins, " + String(schedCount) + " schedules, " +
                 String(alertCount) + " alerts)";
  CommandManager::getInstance().sendResponse(reply, source);
  LOG_PRINTLN(reply);
  return true;
}

void ProductManager::restoreActiveProduct() {
  Preferences p;
  p.begin("products", true);
  _activeProduct = p.getString("active", "");
  p.end();

  if (_activeProduct.isEmpty())
    return;

  String path = _productPath(_activeProduct);
  if (!LittleFS.exists(path)) {
    LOG_PRINTF("PRODUCT: Active product '%s' not found on restore\n",
               _activeProduct.c_str());
    _activeProduct = "";
    return;
  }

  File f = LittleFS.open(path, "r");
  String json = f.readString();
  f.close();

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok)
    return;

  // Only re-apply pin modes — names in NVS, tasks in /schedule.json
  if (doc["pins"].is<JsonArray>()) {
    applyPins(doc["pins"].as<JsonArray>(), CommInterface::COMM_INTERNAL);
  }
  LOG_PRINTF("PRODUCT: Restored active product '%s'\n", _activeProduct.c_str());
}

String ProductManager::listProducts() {
  _ensureDir();
  String out = "[";
  bool first = true;
  File root = LittleFS.open("/products");
  if (!root || !root.isDirectory())
    return "[]";
  File f = root.openNextFile();
  while (f) {
    String fname = String(f.name());
    // Strip path prefix if present
    int slash = fname.lastIndexOf('/');
    if (slash >= 0)
      fname = fname.substring(slash + 1);
    if (fname.endsWith(".json")) {
      if (!first)
        out += ",";
      out += "\"" + fname.substring(0, fname.length() - 5) + "\"";
      first = false;
    }
    f = root.openNextFile();
  }
  out += "]";
  return out;
}

String ProductManager::getProductJson(const String &name) {
  String path = _productPath(name);
  if (!LittleFS.exists(path))
    return "";
  File f = LittleFS.open(path, "r");
  String s = f.readString();
  f.close();
  return s;
}

// ── Private helpers ──────────────────────────────────────────────────────────

void ProductManager::applyPins(const JsonArray &pins, CommInterface source) {
  DataManager &data = DataManager::getInstance();
  for (JsonObjectConst p : pins) {
    String id = p["id"] | "";
    String label = p["label"] | id;
    String mode = p["mode"] | "output";
    mode.toLowerCase();

    if (id.isEmpty())
      continue;

    int pinNum = CommandManager::getInstance().getPinFromName(id);
    if (pinNum < 0) {
      LOG_PRINTF("PRODUCT: Unknown pin id '%s'\n", id.c_str());
      continue;
    }

    uint8_t pinMode_val = (mode == "input") ? INPUT : OUTPUT;
    MCPManager::setupPin(pinNum, pinMode_val);

    // Store friendly name keyed by the resolved pin number string
    data.SetPinName(String(pinNum), label);

    // Apply default output state if specified
    if (mode == "output" && p["default"].is<int>()) {
      MCPManager::writePin(pinNum, p["default"].as<int>() != 0);
    }
  }
}

void ProductManager::applySchedules(const JsonArray &schedules,
                                    CommInterface source) {
  ScheduleManager &sched = ScheduleManager::getInstance();
  for (JsonObjectConst s : schedules) {
    String id = s["id"] | "";
    String type = s["type"] | "TOGGLE";
    String pin = s["pin"] | "";
    unsigned long interval = s["interval"] | 60UL;
    unsigned long duration = s["duration"] | 0UL;
    int value = s["value"] | 0;

    if (id.isEmpty() || pin.isEmpty())
      continue;

    sched.addDynamicTask(id, type, pin, interval, duration, "PRODUCT", true,
                         value);
  }
}

void ProductManager::applyAlerts(const JsonArray &alerts,
                                 CommInterface source) {
  ScheduleManager &sched = ScheduleManager::getInstance();
  for (JsonObjectConst a : alerts) {
    String pin = a["pin"] | "";
    String condition = a["condition"] | "lt";
    String notify = a["notify"] | "LORA_TX";
    String message = a["message"] | "";
    int threshold = a["threshold"] | 0;
    unsigned long interval = a["interval"] | 60UL;

    if (pin.isEmpty() || message.isEmpty())
      continue;

    // Build task id from pin name (sanitised)
    String taskId = "alert_" + pin;
    taskId.replace(":", "_");

    // Notify command string dispatched when alert fires
    String notifyCmd = notify + " " + message;

    bool thresholdGreater = (condition == "gt");

    sched.addDynamicTask(taskId, "ALERT", pin, interval, 0, "PRODUCT", true, 0,
                         -1, 0, threshold, thresholdGreater, notifyCmd);
  }
}
