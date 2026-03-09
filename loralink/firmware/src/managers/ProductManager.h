#ifndef PRODUCT_MANAGER_H
#define PRODUCT_MANAGER_H

#include "../config.h"
#include "CommandManager.h"
#include <ArduinoJson.h>

class ProductManager {
public:
  static ProductManager &getInstance() {
    static ProductManager instance;
    return instance;
  }

  // Save product JSON to LittleFS /products/<name>.json
  bool saveProduct(const String &json);

  // Load a product by name from LittleFS, apply pins + schedules + alerts
  bool loadProduct(const String &name, CommInterface source);

  // Re-apply pin modes only — called on boot (schedules/names already
  // persisted)
  void restoreActiveProduct();

  // Returns JSON array of product names in LittleFS
  String listProducts();

  // Returns active product name (from NVS), or ""
  String getActiveProduct() const { return _activeProduct; }

  // Returns raw JSON string of a stored product, or ""
  String getProductJson(const String &name);

private:
  ProductManager() = default;
  ~ProductManager() = default;
  ProductManager(const ProductManager &) = delete;
  ProductManager &operator=(const ProductManager &) = delete;

  String _activeProduct;

  void applyPins(const JsonArray &pins, CommInterface source);
  void applySchedules(const JsonArray &schedules, CommInterface source);
  void applyAlerts(const JsonArray &alerts, CommInterface source);
};

#endif // PRODUCT_MANAGER_H
