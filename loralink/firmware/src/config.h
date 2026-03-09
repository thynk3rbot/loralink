#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <stdint.h>

// ============================================================================
//   FIRMWARE & FEATURE FLAGS
// ============================================================================
#define FIRMWARE_VERSION "v1.6.0"
#define FIRMWARE_NAME "LoRaLink Any2Any"
#define HARDWARE_ID "Heltec ESP32 LoRa V3"
#define CONFIG_SCHEMA "1.0"
#define ALLOW_GPIO_CONTROL true

// ============================================================================
//   LoRa Radio Settings
// ============================================================================
#define LORA_FREQ 915.0
#define LORA_BW 250.0
#define LORA_SF 10
#define LORA_CR 5
#define LORA_SYNC 0x12 // Private Network
#define LORA_PWR 10    // dBm

// ============================================================================
//   ESP-NOW Settings
// ============================================================================
#define ESPNOW_CHANNEL 1
#define ESPNOW_MAX_PEERS 10
#define ESPNOW_QUEUE_SIZE 8

// ============================================================================
//   GPIO PIN MAPPING (Heltec WiFi LoRa 32 V3)
// ============================================================================
#define PIN_LED_BUILTIN 35 // Orange LED
#define PIN_BUTTON_PRG 0   // PRG Button
#define PIN_BAT_ADC 1      // Battery ADC
#define PIN_VEXT_CTRL 36   // External Power (LOW = ON for Heltec V3)
#define PIN_LORA_CS 8      // LoRa Chip Select
#define PIN_LORA_DIO1 14   // LoRa IRQ
#define PIN_LORA_RST 12    // LoRa Reset
#define PIN_LORA_BUSY 13   // LoRa Busy
#define PIN_OLED_SDA 17
#define PIN_OLED_SCL 18
#define PIN_OLED_RST 21
#define PIN_BAT_CTRL 37       // Battery Divider Control (LOW = ON)
#define BAT_VOLT_MULTI 3.564f // Heltec V3: (1M + 390k) / 390k = 3.5641

// GPS Placeholder Pins
#define PIN_GPS_RX 47
#define PIN_GPS_TX 48

// Relay & Sensor Pins
#define PIN_RELAY_110V 5
#define PIN_RELAY_12V_1 46
#define PIN_RELAY_12V_2 6
#define PIN_RELAY_12V_3 7
#define PIN_SENSOR_DHT 15

// ── MCP23017 I2C GPIO Expander ─────────────────────────────────────────────
// Up to 8 MCP23017 chips share the OLED I2C bus (SDA=17, SCL=18).
// Extended pin numbering: pin = MCP_PIN_BASE + chip*MCP_CHIP_PINS + local_pin
//   "MCP:0:4"  → chip 0 (0x20), pin 4  → extended pin 104
//   "MCP:1:12" → chip 1 (0x21), pin 12 → extended pin 128
#define MCP_PIN_BASE 100        // Native pins 0–99; MCP pins 100–227
#define MCP_CHIP_PINS 16        // 16 GPIO per chip (GPA0–GPB7)
#define MCP_MAX_CHIPS 8         // 8 addresses: 0x20–0x27
#define MCP_CHIP_ADDR_BASE 0x20 // I2C base address (A0=A1=A2=GND)
#define PIN_MCP_INT 38 // INTA → GPIO 38 (safe: not shared on Heltec V3)

// ============================================================================
//   COMMUNICATION INTERFACE ENUM
//   Note: Values prefixed with COMM_ to avoid Arduino.h macro conflicts
//   (Arduino #defines SERIAL, WIFI, INPUT etc.)
// ============================================================================
enum class CommInterface : uint8_t {
  COMM_SERIAL = 0,
  COMM_LORA = 1,
  COMM_BLE = 2,
  COMM_WIFI = 3,
  COMM_ESPNOW = 4,
  COMM_INTERNAL = 5
};

// ============================================================================
//   TRANSPORT LINK PREFERENCE
//   Which protocol the device targets after boot negotiation.
//   Persisted in NVS (key: "link_pref"). LINK_AUTO = negotiate each boot.
//   Hook: extend with LINK_LORA_BLE_BRIDGE for gateway/relay role.
// ============================================================================
enum class LinkPreference : uint8_t {
  LINK_AUTO = 0,      // Negotiate on boot (factory default)
  LINK_BLE = 1,       // BLE terminal only — WiFi off after lock-in
  LINK_WIFI_MQTT = 2, // WiFi + MQTT bidirectional
  LINK_WIFI_HTTP = 3, // WiFi + HTTP (no broker required)
  LINK_LORA = 4,      // LoRa mesh only — lowest power hold
};

// Result of a WiFi probe attempt
enum class ProbeResult : uint8_t {
  PROBE_OK_MQTT = 0,   // WiFi associated + MQTT broker reached
  PROBE_OK_HTTP = 1,   // WiFi associated, no MQTT
  PROBE_NO_AP = 2,     // SSID not found / association failed
  PROBE_NO_BROKER = 3, // WiFi up but MQTT broker unreachable
  PROBE_TIMEOUT = 4,   // Association timed out
};

// Boot negotiation window — try all configured transports before locking in.
// Override via NVS key "trans_neg_ms". Factory default: 10 000ms
// (configurable).
#define TRANSPORT_NEGOTIATE_MS 10000UL

// WiFi probe backoff — applied when downgraded to LINK_LORA.
// Sequence doubles each failure: 30s → 60s → 2m → ... → 30m cap.
#define PROBE_BACKOFF_MIN_MS 30000UL   //  30 seconds
#define PROBE_BACKOFF_MAX_MS 1800000UL //  30 minutes
#define PROBE_TIMEOUT_MS 5000UL        //   5 seconds max per probe

// ============================================================================
//   DATA STRUCTURES
// ============================================================================

// Binary Telemetry Struct removed in favor of JSON string payloads
// Data Packet Structure (Optimized: 64 bytes total)
struct __attribute__((packed)) MessagePacket {
  char sender[16];   // Readable Sender Name (null-terminated if short)
  char text[45];     // Message Text or Binary Payload
  uint8_t ttl;       // Time-To-Live
  uint16_t checksum; // Integrity check to filter noise
};

// ── Binary Command Protocol (40% Range Boost Goal) ──────────────────────────
// Binary packets start with byte 0xAA in text[0].
// Structure: [0xAA] [TargetID_Short] [CmdCode] [Args...]
#define BINARY_TOKEN 0xAA

enum class BinaryCmd : uint8_t {
  BC_NOP = 0x00,
  BC_GPIO_SET = 0x01,  // [Pin] [0|1]
  BC_PWM_SET = 0x02,   // [Pin] [Duty 0-255]
  BC_SERVO_SET = 0x03, // [Pin] [Angle 0-180]
  BC_READ_PIN = 0x04,  // [Pin]
  BC_REBOOT = 0x05,
  BC_PING = 0x06,
  BC_STATUS = 0x07,
  BC_ACK = 0x08,       // ACK for binary command [AckToken]
  BC_CONFIG_SEG = 0x09 // [SegIndex] [TotalSegs] [Data...]
};

// ============================================================================
//   NON-BLOCKING TIMING CONSTANTS
// ============================================================================
#define LORA_TX_TIMEOUT_MS 5000          // TX watchdog — force RX if stuck
#define REPEATER_JITTER_MIN_MS 150       // Repeater propagation jitter floor
#define REPEATER_JITTER_MAX_MS 500       // Repeater propagation jitter ceiling
#define BEACON_LEGACY_DELAY_MS 500       // Gap between encrypted & legacy beacon
#define SLEEP_PC_GUARD_MS 3000           // Sleep PC-attached guard window
#define SLEEP_COUNTDOWN_STEP_MS 1200     // Sleep countdown step interval
#define ESPNOW_TX_QUEUE_SIZE 12          // ESP-NOW async send queue depth

#define MAX_TTL 3

// Encrypted packet buffer size (12 IV + 16 Tag + 64 ciphertext)
#define ENCRYPTED_PACKET_SIZE 92

// Remote Node Tracking
struct RemoteNode {
  char id[16];
  uint32_t lastSeen;
  float battery;
  uint8_t resetCode;
  uint32_t uptime;
  int16_t rssi;
  uint8_t hops; // 0 = direct neighbor, 1+ = relayed
  float lat;
  float lon;
  uint8_t shortId; // Last byte of MAC for binary routing
};

// ESP-NOW Peer Info
struct ESPNowPeer {
  uint8_t mac[6];
  char name[16];
  bool active;
};

#define MAX_NODES 16
#define MAX_PERIPHERALS 8
#define LOG_SIZE 20
#define HASH_BUFFER_SIZE 20

// ============================================================================
//   POWER-MISER (SMART AG) CONFIGURATION
// ============================================================================
#define POWER_MISER_VOLT_NORMAL 3.80f
#define POWER_MISER_VOLT_CONSERVE 3.65f
#define POWER_MISER_VOLT_CRITICAL 3.45f

#define POWER_MISER_HB_NORMAL 300UL    // 5 min
#define POWER_MISER_HB_CONSERVE 900UL  // 15 min
#define POWER_MISER_HB_CRITICAL 3600UL // 60 min

#endif // CONFIG_H
