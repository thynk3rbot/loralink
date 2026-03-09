# LoRaLink + NutriCalc Integration

**LoRaLink** and **NutriCalc** are separate products that work together for automated hydroponic cultivation:

- **LoRaLink** - Device control system (firmware + web dashboard)
- **NutriCalc** - Nutrient formulation solver

## Architecture

```
NutriCalc Server
    ↓ (1) Solve formula
    ↓ (2) Publish via MQTT
    ↓
  MQTT Broker
    ↓
LoRaLink Device
    ↓ (3) Parse pump commands
    ↓ (4) Activate relays
    ↓
Pump Controllers (Peristaltic, Solenoid, etc.)
    ↓
Cultivation System
```

## Data Flow

### 1. Formula Creation (NutriCalc → MQTT)

User creates nutrient formula in NutriCalc web UI:

```
Input: Target PPM (N=200, P=50, K=150) + compounds
↓
Solver: Calculate optimal weights
↓
Output: {
  "formula": "My Lettuce Mix",
  "volume_L": 100,
  "channels": [
    {"pump": 1, "label": "Nitrogen Stock", "grams": 45.2},
    {"pump": 2, "label": "Phosphorus Stock", "grams": 12.8},
    {"pump": 3, "label": "Potassium Stock", "grams": 89.5}
  ],
  "ec_target": 1.2,
  "ph_target": 6.5
}
↓
MQTT Publish to: nutricalc/dose (summary)
                 nutricalc/pump/1, /pump/2, /pump/3 (per-pump)
```

### 2. Pump Dispatch (LoRaLink → Hardware)

LoRaLink device receives MQTT messages:

```
MQTT Subscriber receives on nutricalc/pump/{id}
↓
CommandManager routes to appropriate handler
↓
ScheduleManager creates one-time task:
  - PULSE relay for N seconds
  - Calculate volume from weight + density
  - Trigger at specified time
↓
Relay activates pump
↓
Pump dispenses nutrient solution
↓
Device publishes ACK: nutricalc/ack/{pump_id}
```

### 3. Feedback Loop (Optional)

```
LoRaLink → MQTT nutricalc/ack/{pump_id}
             ↓
         NutriCalc receives ACK
             ↓
         Log completion
             ↓
         Update UI dashboard
```

## MQTT Topics

### Dispatch Topics (NutriCalc → LoRaLink)

**Summary Formula**
```
Topic: nutricalc/dose
QoS: 1
Payload: {
  "formula": "My Mix",
  "volume_L": 100,
  "channels": [...],
  "ec_target": 1.2,
  "ph_target": 6.5,
  "timestamp": "2026-03-09T21:00:00Z"
}
```

**Per-Pump Channel**
```
Topic: nutricalc/pump/{pump_id}
QoS: 1
Payload: {
  "pump": 1,
  "label": "Nitrogen Stock",
  "grams": 45.2,
  "ml": 67.8,
  "formula": "My Mix",
  "timestamp": "2026-03-09T21:00:00Z"
}
```

### Acknowledgement Topics (LoRaLink → NutriCalc)

```
Topic: nutricalc/ack/{pump_id}
QoS: 1
Payload: {
  "pump": 1,
  "status": "complete",
  "dispensed_ml": 67.5,
  "duration_ms": 3400,
  "timestamp": "2026-03-09T21:04:30Z"
}
```

## Configuration

### LoRaLink MQTT Setup

Edit `webapp/configs/local_mqtt_config.json`:

```json
{
  "host": "192.168.1.100",
  "port": 1883,
  "topic_prefix": "loralink",
  "client_id": "loralink-gw1",
  "username": "",
  "password": ""
}
```

### NutriCalc MQTT Setup

Edit `nutricalc/mqtt_config.json`:

```json
{
  "host": "192.168.1.100",
  "port": 1883,
  "topic_prefix": "nutricalc",
  "client_id": "nutricalc-server",
  "pump_map": {
    "Nitrogen Stock": "1",
    "Phosphorus Stock": "2",
    "Potassium Stock": "3"
  }
}
```

### Pump Mapping

LoRaLink relays physical pins to MQTT pump IDs:

```
nutricalc/pump/1 → PIN_RELAY_12V_2 (safe relay pin)
nutricalc/pump/2 → PIN_RELAY_12V_3 (safe relay pin)
nutricalc/pump/3 → GPIO_EXPANSION (MCP23017 pin)
```

Edit `loralink/firmware/src/config.h` to map pins:

```cpp
#define PUMP_ID_TO_PIN {
  {1, PIN_RELAY_12V_2},
  {2, PIN_RELAY_12V_3},
  {3, MCP_PIN_0}
}
```

## Workflow Example

### 1. Setup (One-time)

```bash
# Start MQTT broker
mosquitto -v

# Start LoRaLink web dashboard
cd loralink/webapp && python server.py
# http://localhost:8000

# Start NutriCalc server
cd nutricalc && python server.py
# http://localhost:8100

# Flash LoRaLink firmware to ESP32-S3
cd loralink/firmware && pio run -t upload
```

### 2. Daily Operation

**User Creates Formula**
1. Open NutriCalc: http://localhost:8100
2. Select compounds: Nitrogen Stock, Phosphorus Stock, Potassium Stock
3. Set targets: N=200, P=50, K=150 ppm
4. Set volume: 100L
5. Click "Solve"
6. Review results
7. Click "Publish to Pumps"

**LoRaLink Executes**
1. Device receives MQTT message
2. Creates one-time task for each pump
3. Triggers pump relays in sequence
4. Publishes ACK back to NutriCalc

**NutriCalc Logs**
1. Receives ACK from each pump
2. Updates dashboard: ✓ Nitrogen dispensed
3. Records formula execution in history

### 3. Monitoring

**LoRaLink Dashboard:**
- View pump status in real-time
- See task schedule
- Monitor mesh network health

**NutriCalc Dashboard:**
- View formula history
- Track cost per batch
- Monitor EC/pH targets
- View pump acknowledgements

## Troubleshooting

### MQTT Connection Failed

**LoRaLink:**
```
Error: MQTT disconnected rc=1
```
→ Check broker host/port in `local_mqtt_config.json`

**NutriCalc:**
```
ConnectionRefusedError: [Errno 111]
```
→ Check broker running: `ps aux | grep mosquitto`

### Pump Not Responding

**Check:**
1. Is LoRaLink device online? (LoRaLink dashboard → Network)
2. Is relay pin configured? (firmware/src/config.h)
3. Is pin 14 interfering? (never enable with LORA_DIO1)
4. Is MQTT topic correct? (nutricalc/pump/{pump_id})

### Formula Not Publishing

**NutriCalc Error:**
```
MQTT not connected (status 503)
```
→ Click "Configuration" → verify broker settings

**Check:**
1. Is broker running on correct host/port?
2. Is NutriCalc MQTT client connected? (watch /api/mqtt/status)
3. Are pump IDs valid? (chemistry database must match)

## Future Enhancements

- [ ] Closed-loop dosing with sensor feedback
- [ ] Automatic EC/pH adjustment based on measurements
- [ ] Formula templates & presets
- [ ] Multi-tank support
- [ ] Mobile app for monitoring
- [ ] Scheduled formula automation

## Repositories

- **LoRaLink:** https://github.com/thynk3rbot/loralink
- **NutriCalc:** https://github.com/thynk3rbot/nutricalc

---

**Last Updated:** 2026-03-09
**Status:** Production Ready
