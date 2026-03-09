# LoRaLink — ESP32-S3 LoRa Device Control System

**LoRaLink** is a unified ESP32-S3 firmware + web control dashboard for LoRa mesh networking and device orchestration.

**Board:** Heltec WiFi LoRa 32 V3 (ESP32-S3, SX1262 915MHz)

## Quick Start

### Build & Flash Firmware
```bash
cd firmware
pio run -e heltec_wifi_lora_32_V3
pio run -t upload
pio device monitor -b 115200
```

### Run Web Dashboard
```bash
cd webapp
pip install -r requirements.txt
python server.py
# Open http://localhost:8000
```

## Architecture

- **15+ Singleton Managers** - Modular architecture for radio, WiFi, BLE, MQTT, scheduling
- **Any-to-Any Command Routing** - Central CommandManager routes messages between interfaces
- **OTA Updates** - Over-the-air firmware flashing
- **MQTT Integration** - Telemetry and external command support
- **Task Scheduling** - Up to 5 dynamic tasks
- **ESP-NOW Mesh** - Peer-to-peer communication

## Key Managers

| Manager | Responsibility |
|---------|-----------------|
| CommandManager | Any-to-any message routing |
| LoRaManager | SX1262 radio & AES encryption |
| WiFiManager | Web dashboard & OTA |
| BLEManager | Nordic UART service |
| ScheduleManager | Task scheduling (5 max) |
| MCPManager | I2C GPIO expander |
| ESPNowManager | Peer registry & broadcast |
| MQTTManager | Telemetry & external commands |

## Interface with Other Products

**NutriCalc Integration:**
- LoRaLink receives pump control commands from NutriCalc via MQTT
- Topic: `loralink/pump/{id}` - Dispatcher sends to relay controllers
- Topic: `loralink/ack/{pump_id}` - Devices acknowledge completion
- See `INTEGRATION.md` for full details

## Specifications

- **Radio:** SX1262 915MHz, 20dBm, AES-128 encryption
- **Mesh:** LoRa + ESP-NOW + BLE + WiFi + Serial
- **Tasks:** 5 concurrent scheduled tasks
- **Peers:** Up to 10 ESP-NOW peers
- **Nodes:** Up to 20 remote nodes tracked

## Pins

**Critical:** Pin 14 is shared by `PIN_RELAY_12V_1` and `LORA_DIO1`—never enable both.

**Safe Relay Pins:** 6, 7

## Documentation

- `docs/hardware.html` - Hardware setup guide
- `docs/api-reference.html` - API endpoints
- `firmware/src/config.h` - Pin definitions & limits
- `webapp/README.md` - Dashboard features

## Development

```bash
# Feature development
git checkout -b feature/your-feature
git push origin feature/your-feature
# Create PR on GitHub
```

## Support

For integration with NutriCalc, see `INTEGRATION.md` in the NutriCalc repository.

---

**Status:** Production Ready
**License:** Open Source
