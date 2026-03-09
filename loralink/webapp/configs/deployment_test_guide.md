# Deployment & Configuration Testing Guide

This guide walks through testing the LoRaLink configuration management system using the provided test configurations. It validates the complete config lifecycle: export → modify → import → persistence → deployment.

---

## Prerequisites

- **Hardware**: One or more Heltec WiFi LoRa 32 V3 devices running firmware v1.4.1+
- **Tools**: PC control webapp (`python tools/webapp/server.py --ip 172.16.0.26`)
- **Network**: Devices accessible via HTTP (WiFi or BLE gateway)
- **Time**: 30-45 minutes per test cycle

---

## Test Configurations Included

### Device Configs (Network & Hardware Settings)
- **heltec_v3_generic.json** — Lab/bench testing (WiFi off, BLE on, minimal pins)
- **heltec_v3_generic.csv** — Same config in CSV key-value format
- **heltec_v3_farm_automation.json** — Production scenario (WiFi on, MQTT enabled, 7 pins)
- **heltec_v3_farm_automation.csv** — Same config in CSV format

### Task Schedules (GPIO Automation)
- **tasks_generic_toggle.json** — Simple GPIO tests (LED heartbeat + relay pulses)
- **tasks_generic_toggle.csv** — CSV version
- **tasks_farm_scenario.json** — Realistic irrigation/climate control (most tasks disabled for safety)
- **tasks_farm_scenario.csv** — CSV version

---

## Test Procedure 1: Baseline Export & Validation

**Goal**: Confirm device can export its current config and establish a baseline for comparison.

### Steps

1. **Connect to device via webapp**:
   - Open `http://localhost:8000`
   - Navigate to **Admin** tab → **Device Settings**
   - Verify device shows current hostname and network info

2. **Export current config**:
   - Click **Export Config**
   - Save to `baseline_config.json` in your working directory
   - Verify JSON is valid (no parse errors)

3. **Inspect exported config**:
   - Confirm `hardware_id` = `"Heltec ESP32 LoRa V3"`
   - Confirm `schema` = `"1.0"`
   - Note current `dev_name` and `wifi_ssid` values

4. **Document baseline**:
   - Record device hostname, IP, WiFi SSID
   - Note any custom pin names already configured
   - Screenshot or copy the exported JSON

**Expected Result**: A valid device config exported, confirmed against schema.

---

## Test Procedure 2: Import Generic Config

**Goal**: Load the lab/bench configuration and verify device accepts it.

### Steps

1. **Prepare generic config**:
   - Copy `heltec_v3_generic.json` to webapp working directory
   - Review: WiFi disabled, BLE enabled, LED + relays configured

2. **Import config via webapp**:
   - Navigate to **Admin** → **Device Settings**
   - Click **Import Config**
   - Select `heltec_v3_generic.json`
   - Confirm: "Config applied successfully"

3. **Verify device response**:
   - Device should briefly disconnect (applies config, reboots internally)
   - Webapp should reconnect automatically
   - New device name should show: `"LoRaLink-TestUnit-V3"`

4. **Validate on device**:
   - Open Config Files page
   - Should see new pin definitions: `LED`, `RL12_2`, `RL12_3`, `BatSensor`
   - Load Device Config → inspect JSON in editor

**Expected Result**: Device accepts import, persists new config, and reboots cleanly.

---

## Test Procedure 3: Persistence Check (Reboot Test)

**Goal**: Verify imported config survives a device reboot.

### Steps

1. **Reboot device**:
   - Via webapp: Admin → Send Command → `RESET`
   - Or press PRG button (hold 5 seconds)
   - Wait 10 seconds for device to come back online

2. **Re-export config**:
   - Navigate to Admin → **Export Config**
   - Save as `after_reboot.json`

3. **Compare configs**:
   - Use a JSON diff tool or visual comparison
   - Expected: `after_reboot.json` == `heltec_v3_generic.json`
   - Verify all pin names are preserved
   - Verify device name is still `"LoRaLink-TestUnit-V3"`

4. **Check device name**:
   - Dashboard should still show `LoRaLink-TestUnit-V3` as hostname
   - Confirms NVS (non-volatile storage) persisted the config

**Expected Result**: Config survives reboot unchanged.

---

## Test Procedure 4: Task Schedule Loading

**Goal**: Load task definitions and verify they execute.

### Steps

1. **Import task schedule**:
   - Navigate to **Test Bench** → **Saved Sequences** or **Config Files**
   - Import `tasks_generic_toggle.json`
   - Device should parse and save as `/schedule.json`

2. **Enable Heartbeat task**:
   - Via Command Console: `SCHED ADD Heartbeat TOGGLE LED 2 0`
   - Or manually check the Heartbeat task in saved sequences

3. **Observe LED**:
   - Onboard orange LED (pin 35) should blink every 2 seconds
   - Confirm visible blinking (indicates task is executing)
   - Check device logs in Dashboard → should show SCHED commands

4. **Test Relay Pulse (optional)**:
   - **If you have a relay connected to pin 6 (RL12_2)**:
     - Command: `SCHED ADD MainRelay_Test PULSE RL12_2 30 5`
     - Relay should activate for 5s every 30s
     - Measure/observe relay response

**Expected Result**: Tasks execute with correct timing; LED blinks visibly every 2s.

---

## Test Procedure 5: Format Migration (JSON ↔ CSV)

**Goal**: Verify configs can be converted between JSON and CSV without data loss.

### Steps

1. **Export device config as JSON**:
   - Admin → Export Config → save as `config_from_device.json`

2. **Convert JSON to CSV manually**:
   - Use a JSON-to-CSV converter or spreadsheet tool
   - Compare structure to `heltec_v3_generic.csv` provided
   - Verify all key-value pairs match

3. **Load CSV config**:
   - Admin → Import Config → select CSV version
   - Wait for device to apply and reboot

4. **Export back to JSON**:
   - Admin → Export Config → save as `back_to_json.json`

5. **Compare originals**:
   - Original JSON vs. JSON after round-trip through CSV
   - Expected: Identical (with possible whitespace/formatting differences)

**Expected Result**: Config survives round-trip conversion between formats.

---

## Test Procedure 6: Farm Scenario Deployment

**Goal**: Load the realistic farm configuration and verify all pins are recognized.

### Steps

1. **Import farm config**:
   - Admin → Import Config → `heltec_v3_farm_automation.json`
   - Device applies: WiFi enabled, MQTT enabled, 7 pins configured

2. **Verify pin configuration**:
   - Config Files page should show:
     - `SystemLED` (pin 35)
     - `IrrigationPump` (pin 6)
     - `GrowLight` (pin 7)
     - `Heater` (pin 5)
     - `DHT_Sensor` (pin 15)
     - `PowerRail` (pin 36)
     - `SoilMoisture` (pin 1 / ADC)

3. **Check WiFi settings**:
   - Dashboard should show WiFi SSID: `"FarmNetwork"` (configured but unconnected if credentials wrong)
   - Static IP: `192.168.1.50`

4. **Load farm task schedule**:
   - Test Bench → Import `tasks_farm_scenario.json`
   - Verify 6 tasks are loaded
   - **Confirm**: Dangerous tasks are **disabled** (IrrigationCycle, Heater, etc.)
   - Only **SystemHeartbeat** should be **enabled**

**Expected Result**: All farm pins are configured; dangerous tasks remain safely disabled.

---

## Test Procedure 7: Multi-Device Deployment (Optional)

**Goal**: Deploy configuration to multiple devices simultaneously.

### Steps

1. **Configure two or more devices**:
   - Device A: Apply `heltec_v3_generic.json`
   - Device B: Apply `heltec_v3_farm_automation.json`

2. **Use webapp multi-apply**:
   - Nodes page → add both devices
   - Test Bench → Create/select sequence
   - Click **▾ Apply to…** → check both devices
   - Click **Apply to Selected**

3. **Verify per-device results**:
   - Webapp console should show results for each device
   - Example: `"DeviceA: 3/3 tasks applied"`, `"DeviceB: 6/6 tasks applied"`

4. **Confirm on each device**:
   - Switch to Device A → Admin → Export Config
   - Switch to Device B → Admin → Export Config
   - Configs should reflect their respective imports

**Expected Result**: Each device retains its unique configuration; multi-apply reports per-device results.

---

## Test Procedure 8: Rollback Test

**Goal**: Simulate accidental config change and recovery from backup.

### Steps

1. **Create backup**:
   - Export farm config → save as `farm_backup.json`

2. **Intentionally corrupt settings**:
   - Edit farm_backup.json: Change `mqtt_prt` to invalid value (e.g., `999999`)
   - Or change `hardware_id` to something else temporarily

3. **Try to import bad config**:
   - Admin → Import Config → select corrupted file
   - Expected: **Error: "Invalid config or hardware mismatch"**

4. **Recover from backup**:
   - Admin → Import Config → select `farm_backup.json` (uncorrupted)
   - Device applies successfully

5. **Verify recovery**:
   - Export config again
   - Should match original farm_automation.json
   - All pins and settings restored

**Expected Result**: Bad configs are rejected; backup restoration succeeds.

---

## Troubleshooting

### Device won't accept config import
- **Check hardware ID**: Ensure config has `"hardware_id": "Heltec ESP32 LoRa V3"`
- **Check JSON validity**: Paste JSON into online validator (jsonlint.com)
- **Device logs**: Look for error messages in Dashboard → message log

### LED not blinking after task import
- **Check pin name**: Task uses pin name (`LED`), verify pin mapping exists in config
- **Command syntax**: Try manual command: `SCHED ADD Test TOGGLE LED 2 0`
- **Device responsiveness**: Send `STATUS` command, check if device responds

### CSV import fails
- **Encoding**: Ensure CSV is UTF-8 encoded, not Excel's default encoding
- **Delimiters**: CSV parser expects commas; tab-separated may fail
- **Trailing whitespace**: Remove extra spaces in key names

### Multi-device apply shows "failed — no HTTP address"
- **Node type**: Device must be WiFi or LoRa (BLE/serial not supported for HTTP apply)
- **Network**: Device must be reachable via HTTP from PC
- **Firewall**: Check PC and device firewall allows port 80

---

## Reference: File Locations

```
Project Root (antigravity/)
├── tools/webapp/configs/
│   ├── heltec_v3_generic.json           ← Device config for lab testing
│   ├── heltec_v3_generic.csv
│   ├── heltec_v3_farm_automation.json   ← Device config for farm scenario
│   ├── heltec_v3_farm_automation.csv
│   └── deployment_test_guide.md         ← This file
└── sched/
    ├── tasks_generic_toggle.json        ← GPIO test tasks
    ├── tasks_generic_toggle.csv
    ├── tasks_farm_scenario.json         ← Farm automation tasks
    └── tasks_farm_scenario.csv
```

---

## Next Steps

Once all 8 test procedures pass:

1. **Archive results**: Save successful export/import screenshots and configs as evidence
2. **Document custom use cases**: Create additional configs for your specific hardware
3. **Deploy to production**: Use farm config as template for real field deployment
4. **Monitor telemetry**: Enable MQTT in farm config and verify cloud data flow

---

## Schema Reference

### Device Config Schema (ExportConfig)
```json
{
  "schema": "1.0",
  "hardware_id": "Heltec ESP32 LoRa V3",
  "settings": {
    "dev_name": "...",
    "wifi_ssid": "...",
    "wifi_pass": "...",
    "static_ip": "...",
    "gateway": "...",
    "subnet": "...",
    "repeater": true/false,
    "mqtt_en": true/false,
    "mqtt_srv": "...",
    "mqtt_prt": 1883,
    "mqtt_usr": "...",
    "mqtt_pwd": "...",
    "espnow_en": true/false,
    "wifi_en": true/false,
    "ble_en": true/false,
    "crypto_key": "..."
  },
  "pins": {
    "PIN_NUMBER": {
      "name": "CustomName",
      "en": true/false
    }
  }
}
```

### Task Schedule Schema
```json
{
  "schedules": [
    {
      "name": "TaskName",
      "type": "TOGGLE|PULSE",
      "pin": "LED|RELAY1|GPIO_NUMBER",
      "interval": 2000,
      "duration": 5000,
      "enabled": true/false
    }
  ]
}
```

---

**Last Updated**: 2026-03-02
**Firmware Version**: v1.4.1+
**Platform**: Heltec WiFi LoRa 32 V3
