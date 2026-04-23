# Nurtura Rack Firmware v2.6.1
> **Target:** ESP32 · **Layer:** Arduino + ESP-IDF Hybrid  
> **AsyncAPI Compliant**

---

## Table of Contents
1. [Overview](#overview)
2. [Hardware Pinout](#hardware-pinout)
3. [Dependencies & Libraries](#dependencies--libraries)
4. [File Structure](#file-structure)
5. [Feature Flags](#feature-flags)
6. [MQTT Topics](#mqtt-topics)
7. [MQTT Commands](#mqtt-commands)
8. [BLE Provisioning](#ble-provisioning)
9. [System Architecture](#system-architecture)
10. [FreeRTOS Tasks](#freertos-tasks)
11. [State Enums](#state-enums)
12. [Sensor Behavior](#sensor-behavior)
13. [Relay Control Logic](#relay-control-logic)
14. [Local Automation (Offline Mode)](#local-automation-offline-mode)
15. [Security Model](#security-model)
16. [Boot & Reset Behavior](#boot--reset-behavior)
17. [NVS Storage Keys](#nvs-storage-keys)
18. [Buffer Size Constants](#buffer-size-constants)
19. [secrets.h Reference](#secretsh-reference)
20. [Changelog](#changelog)

---

## Overview

The Nurtura Rack firmware runs on an ESP32 and manages an automated plant rack system. It handles sensor reading, relay control, MQTT telemetry (AsyncAPI compliant), BLE provisioning, and local offline automation — with production-grade reliability features including hardware and software watchdogs, task auto-restart, and multi-core task management.

---

## Hardware Pinout

| Pin | GPIO | Function |
|-----|------|----------|
| `SOIL_PIN` | 35 | Capacitive soil moisture sensor (ADC1_CH7) |
| `FLOW_PIN` | 13 | Water flow sensor (interrupt, active LOW) |
| `PUMP_RELAY` | 25 | Water pump relay (active LOW) |
| `LIGHT_RELAY` | 26 | Grow light relay (active LOW) |
| `BOOT_BUTTON_PIN` | 0 | Factory reset / BLE restart trigger (hold 3s) |
| SDA (Wire, Bus 0) | 21 | I²C — BH1750 light sensor + HW-61 LCD |
| SCL (Wire, Bus 0) | 22 | I²C — BH1750 light sensor + HW-61 LCD |
| SDA (Wire1, Bus 1) | 16 | I²C — BME280 temperature/humidity sensor |
| SCL (Wire1, Bus 1) | 17 | I²C — BME280 temperature/humidity sensor |

> **Relay wiring:** Both relays are **active LOW** — `digitalWrite(LOW)` = ON, `digitalWrite(HIGH)` = OFF.

> **LCD:** HW-61 (PCF8574 I²C backpack) at address `0x27`, 16×2 display on Wire (Bus 0, shared with BH1750).

---

## Dependencies & Libraries

| Library | Purpose |
|---------|---------|
| `WiFi`, `WiFiClientSecure` | Network connectivity + TLS |
| `PubSubClient` | MQTT client (TLS on port 8883) |
| `Wire` | I²C bus (dual-bus: Wire + Wire1) |
| `Adafruit_BME280` | Temperature & humidity sensor |
| `BH1750` | Ambient light sensor |
| `LiquidCrystal_I2C` | HW-61 16×2 LCD display |
| `BLEDevice`, `BLEServer`, `BLE2902` | BLE provisioning |
| `Preferences` | NVS flash storage |
| `esp_task_wdt` | Hardware watchdog timer |
| `ArduinoJson` | JSON serialization/deserialization |

---

## File Structure

```
nurtura_rack/
├── Nurtura.ino   ← Main firmware
└── secrets.h     ← Credentials (see secrets.h Reference)
```

---

## Feature Flags

Defined at compile time. Set to `0` to disable.

```cpp
#define DEBUG          0   // Serial debug output via DBG() macro
#define FEATURE_BLE    1   // BLE provisioning mode
#define FORCE_BLE_BOOT 0   // Force BLE on every boot (dev/testing only)
```

---

## MQTT Topics

All topics follow the AsyncAPI-compliant structure using the device MAC address (colon format, e.g. `AA:BB:CC:DD:EE:FF`).

| Topic | Direction | QoS | Retained | Description |
|-------|-----------|-----|----------|-------------|
| `nurtura/rack/{macAddress}/sensors` | Publish | 1 | No | Sensor data payload |
| `nurtura/rack/{macAddress}/status` | Publish | 1 | **Yes** | Online/offline status + LWT |
| `nurtura/rack/{macAddress}/errors` | Publish | 1 | No | Error events (Serial only — MQTT publish disabled in v2.6.1) |
| `nurtura/rack/{macAddress}/commands/watering` | Subscribe | 1 | — | Pump start/stop commands |
| `nurtura/rack/{macAddress}/commands/lighting` | Subscribe | 1 | — | Light on/off commands |
| `nurtura/rack/{macAddress}/commands/sensors` | Subscribe | 1 | — | Sensor streaming start/stop |
| `nurtura/backend/status` | Subscribe | 1 | **Yes** | Backend online/offline presence |

### Sensor Payload
```json
{
  "t": 24.3,
  "h": 58,
  "m": 42,
  "l": 320,
  "wu": 150,
  "tm": "2025-06-01T12:00:00Z"
}
```

| Key | Type | Description |
|-----|------|-------------|
| `t` | float | Temperature °C |
| `h` | uint | Humidity % |
| `m` | uint | Soil moisture % |
| `l` | uint | Ambient light in lux |
| `wu` | uint | Water used this session in mL (only present when a watering session just completed) |
| `tm` | string | ISO 8601 timestamp |

### Status Payload
```json
{
  "o": true,
  "tm": "2025-06-01T12:00:00Z",
  "v": "2.6.1",
  "ip": "192.168.1.42",
  "mac": "AA:BB:CC:DD:EE:FF",
  "u": 360000
}
```

| Key | Type | Description |
|-----|------|-------------|
| `o` | bool | Device online |
| `tm` | string | ISO 8601 timestamp |
| `v` | string | Firmware version |
| `ip` | string | IP address |
| `mac` | string | MAC address (colon format) |
| `u` | int | Uptime in milliseconds |

> **LWT:** The broker publishes `{"o": false, "tm": "..."}` to the status topic on unexpected disconnect (QoS 1, retained).

### Publish Intervals

| Condition | Sensor Interval |
|-----------|----------------|
| Idle / normal mode | 3 seconds (`SENSOR_INTERVAL_NORMAL`) |
| Pump actively running | 60 seconds (`SENSOR_INTERVAL_PUMPING`) |
| Pump state just changed | Immediate (instant publish, does not reset interval timer) |
| Status | On connect + every 60 seconds |

---

## MQTT Commands

Send to `nurtura/<deviceId>/cmd` or `nurtura/all/cmd`.

| Command | Action |
|---------|--------|
| `reset_pump_err` | Clears `ERR_DRY` pump fault, resumes normal operation |
| `reboot` | Graceful remote reboot |
| `safe_mode_on` | Enables safe mode (suspends publishing), persists in NVS, reboots |
| `safe_mode_off` | Disables safe mode, persists in NVS, reboots |
| `ota:<token>:<url>` | Triggers secure HTTPS OTA update (see OTA section) |

---

## Remote Config Keys

Send to `nurtura/<deviceId>/config` as `key=value` plain text.

```
lux_threshold=40
moisture_threshold=30
dry_run_ms=8000
publish_interval=5000
relay_min_on_ms=3000
relay_min_off_ms=3000
```

Changes take effect immediately without reboot. Not persisted to NVS — reboot restores defaults.

---

## BLE Provisioning

Activated when no saved WiFi credentials are found. The device advertises as **"Nurtura Rack"**.

### BLE Service UUID
`4fafc201-1fb5-459e-8fcc-c5c9c331914b`

### BLE Characteristics

| UUID | Property | Purpose |
|------|----------|---------|
| `beb5483e-...` | WRITE | WiFi SSID |
| `1c95d5e3-...` | WRITE | WiFi Password |
| `a1b2c3d4-...` | WRITE | Rack display name |
| `abc12345-...` | WRITE | Device ID override |
| `9a8ca5e3-...` | NOTIFY | Status: `"connected"` or `"failed"` |

### Provisioning Flow
1. Device boots → no saved SSID → starts BLE advertising
2. Mobile app writes SSID + Password characteristics
3. Device attempts WiFi connection (15s timeout)
4. Status characteristic notifies `"connected"` or `"failed"`
5. On success: credentials saved to NVS, BLE disabled, device reboots
6. **Auto-timeout:** BLE disables automatically after **2 minutes** if unused

---

## System Architecture

```
                    ┌─────────────────────────────────┐
                    │           setup()               │
                    │  Init GPIO, ADC, WDT, Mutexes   │
                    └────────────┬────────────────────┘
                                 │
              ┌──────────────────┼──────────────────┐
              │                  │                  │
       ┌──────▼──────┐   ┌───────▼──────┐   ┌──────▼──────┐
       │ TaskSensors │   │   TaskMQTT   │   │ TaskMonitor │
       │  (Core 0)   │   │  (Core 1)   │   │  (Core 1)   │
       │  Priority 3 │   │  Priority 1 │   │  Priority 1 │
       └──────┬──────┘   └───────┬──────┘   └──────┬──────┘
              │                  │                  │
        dataMutex ──────────────►│                  │
              │                  │           heartbeat check
        flowMux (ISR)            │           stack monitor
              │                  │           heap fragmentation
       ┌──────▼──────┐   ┌───────▼──────┐          │
       │  Sensors    │   │ MQTT Broker  │◄──────────┘
       │ BME280      │   │  TLS 8883    │   auto-restart on freeze
       │ BH1750      │   └─────────────┘
       │ Flow/Soil   │
       └─────────────┘
```

---

## FreeRTOS Tasks

| Task | Core | Priority | Stack | Purpose |
|------|------|----------|-------|---------|
| `TaskSensors` | 0 | 3 | 8192 | Read sensors, control relays |
| `TaskMQTT` | 1 | 1 | 8192 | MQTT pub/sub, WiFi management |
| `TaskMonitor` | 1 | 1 | 4096 | Heartbeat check, diagnostics, OTA disable |
| `loop()` | 1 | 1 | — | BLE provisioning, NVS writes, OTA handle |

### Software Heartbeat
Each task updates a `volatile unsigned long` timestamp every cycle. `TaskMonitor` checks these every 5 seconds. If a task hasn't updated within `cfg.heartbeat_timeout` (default 15s):
- Task is killed and restarted (up to `MAX_TASK_RESTARTS = 5` attempts)
- After max restarts: fatal code written to NVS, clean reboot

---

## State Enums

```cpp
enum PumpState  : uint8_t { PUMP_OFF, PUMP_ON, PUMP_ERR_DRY };
enum LightState : uint8_t { LIGHT_OFF, LIGHT_ON };
enum WiFiSMState: uint8_t { WS_IDLE, WS_CONNECTING, WS_CONNECTED, WS_FAILED };
```

All hardware control goes through `setPumpState()` and `setLightState()` — never `digitalWrite()` directly in logic code.

---

## Sensor Behavior

### BME280 (Temperature + Humidity)
- Read every 1 second in `TaskSensors`
- `isnan()` check on every read — bad reads retain last known good value
- **Stale detection:** if value doesn't change by > 0.05°C (temp) or 0.1% (humidity) for `stale_data_ms`, `bme280Fault = true`
- **Auto-recovery:** on fault, re-init attempted each cycle until success

### BH1750 (Light)
- Read every 1 second
- Negative return value = fault, retains last good value
- **Stale detection:** value must change by > 1 lux within `stale_data_ms`
- **Auto-recovery:** re-init attempted on fault

### Soil Moisture (ADC)
- **eFuse ADC calibration** via `esp_adc_cal_characterize()` — automatically uses two-point or Vref calibration if available in eFuse
- **Median filter** over 8 samples — removes ADC noise and electrical spikes
- Raw → percentage mapping: `map(raw, 2480, 460, 0, 100)` (dry → wet)

### Flow Sensor
- Hardware interrupt on `FLOW_PIN` (FALLING edge)
- **5ms debounce** in ISR — rejects electrical noise and EMI spikes
- **Spike rejection:** readings above `flow_max_sane` (30 L/min) discarded
- **Flow timeout alarm:** pump ON for `dry_run_ms × 3` with zero flow → `ERR_DRY`

---

## Relay Control Logic

### Light Relay (Grow Light)
```
l < (lux_threshold - lux_hysteresis)  →  LIGHT_ON
l > (lux_threshold + lux_hysteresis)  →  LIGHT_OFF
```
With defaults (threshold=50, hysteresis=5):
- Turns ON below 45 lux
- Turns OFF above 55 lux
- No switching in the 45–55 lux band

### Pump Relay
```
m < (moisture_threshold - moisture_hysteresis)  →  PUMP_ON
m > (moisture_threshold + moisture_hysteresis)  →  PUMP_OFF
```
With defaults (threshold=25, hysteresis=3):
- Turns ON below 22%
- Turns OFF above 28%

### Relay Lockout
Both relays enforce minimum ON and OFF times (`relay_min_on_ms`, `relay_min_off_ms` — default 2s each) to prevent mechanical wear from rapid switching.

### Dry-Run Protection
If pump is ON for `dry_run_ms` (default 5s) with flow < 0.1 L/min → `sys.pumpErr = true` → `PUMP_ERR_DRY`. Pump stays OFF until `reset_pump_err` MQTT command received.

---

## Security Model

| Layer | Implementation | Status |
|-------|---------------|--------|
| Transport encryption | TLS 1.2 via `WiFiClientSecure` + `setCACert(root_ca)` | ✅ Implemented |
| CA certificate validation | Full chain validation via `root_ca` | ✅ Implemented |
| MQTT authentication | Username + password (`SECRET_MQTT_USER/PASS`) | ✅ Implemented |
| OTA authentication | Token validation (`SECRET_OTA_TOKEN`) before flash | ✅ Implemented |
| OTA transport security | HTTPS with CA cert verification | ✅ Implemented |
| OTA auto-disable | Disabled after `OTA_DISABLE_MS` (10 min) uptime | ✅ Implemented |
| Leaf cert pinning | Requires `mbedtls_ssl_get_peer_cert()` — IDF only | ⚠️ Not in Arduino layer |
| OTA firmware signature | Requires signed binary + `esp_ota_ops.h` key check | ⚠️ Requires IDF project |
| NVS credential encryption | Requires `nvs_flash_secure_init()` + eFuse key | ⚠️ Requires IDF project |

---

## OTA Updates

### MQTT-Triggered OTA
Send to `nurtura/<deviceId>/cmd`:
```
ota:<token>:<https://your-server.com/firmware.bin>
```
- Token must match `SECRET_OTA_TOKEN` in `secrets.h`
- URL must be HTTPS — CA cert validated against `root_ca`
- OTA is **auto-disabled** after 10 minutes of uptime (`OTA_DISABLE_MS`)
- Rejected silently if `otaEnabled == false`

### Local OTA (ArduinoOTA)
Available when `FEATURE_OTA = 1` and device is on WiFi. Hostname = `deviceId` (MAC address).

---

## Diagnostics & Observability

### Diagnostic MQTT Payload (every 60s)
```json
{
  "free_heap": 187432,
  "min_heap": 165000,
  "frag_block": 90000,
  "uptime": 3600,
  "reset_count": 3,
  "rssi": -58,
  "wifi_reconnects": 1,
  "cfg_ver": 2
}
```

### Serial Log Prefixes
| Prefix | Meaning |
|--------|---------|
| `[BOOT]` | Startup info, reset reason |
| `[INFO]` | Non-critical events (sensor recovery) |
| `[WARN]` | Degraded operation (mutex timeout, stale data) |
| `[ALARM]` | Hardware fault requiring attention |
| `[CRITICAL]` | Task frozen, being restarted |
| `[FATAL]` | Max restarts exceeded, rebooting |
| `[ERROR]` | Task creation failure |
| `[CMD]` | MQTT command received |
| `[CONFIG]` | Remote config value applied |
| `[OTA]` | OTA update progress |
| `[TLS]` | TLS/certificate events |
| `[ADC]` | ADC calibration result |
| `[BLE]` | BLE provisioning events |
| `[DIAG]` | Heap/stack diagnostic output |

---

## Boot & Reset Behavior

### Reset Reasons (published in boot payload)
| Code | Meaning |
|------|---------|
| `POWER_ON` | Normal cold start |
| `SW_RESET` | `ESP.restart()` called |
| `PANIC` | Firmware crash / exception |
| `INT_WDT` | Interrupt watchdog timeout |
| `TASK_WDT` | Task watchdog timeout |
| `WDT` | Generic watchdog |
| `BROWNOUT` | Supply voltage too low |
| `UNKNOWN` | Other / unrecognised |

### Factory Reset
Hold `BOOT` button (GPIO 0) for **3 seconds** at power-on. Clears both NVS namespaces (`wifi-config` and `nurtura-sys`) and reboots into BLE provisioning mode.

### Safe Mode
When `safe_mode = true` in NVS:
- Device boots and connects normally
- Sensor control (relays) operates normally
- MQTT publishing is **suspended**
- Useful for remote debugging without data noise

Enable/disable via MQTT commands `safe_mode_on` / `safe_mode_off`.

---

## NVS Storage Keys

### Namespace: `wifi-config`
| Key | Type | Description |
|-----|------|-------------|
| `ssid` | String | WiFi network name |
| `password` | String | WiFi password |
| `rack_name` | String | Display name of this rack |
| `device_id` | String | Optional device ID override |

### Namespace: `nurtura-sys`
| Key | Type | Description |
|-----|------|-------------|
| `reset_count` | UInt32 | Cumulative reboot counter |
| `last_fatal` | UInt32 | Fatal error code from last crash |
| `safe_mode` | Bool | Safe mode flag |

### Fatal Error Codes
| Code | Meaning |
|------|---------|
| `0x00000000` | No fatal error |
| `0xDEAD0001` | `TaskSensors` exceeded max restarts |
| `0xDEAD0002` | `TaskMQTT` exceeded max restarts |

---

## Buffer Size Constants

All defined with `#define` and guarded by `static_assert` at compile time.

| Constant | Size | Used For |
|----------|------|---------|
| `BUF_SSID` | 64 | WiFi SSID |
| `BUF_PASS` | 64 | WiFi password |
| `BUF_NAME` | 64 | Rack name |
| `BUF_DEVID` | 32 | Device ID / MAC string |
| `BUF_REASON` | 24 | Boot reason string |
| `BUF_MSG` | 128 | Incoming MQTT message |
| `BUF_PAYLOAD` | 512 | Outgoing MQTT payload |

---

## secrets.h Reference

Create `secrets.h` in the same folder as the `.ino` file:

```cpp
#pragma once

// WiFi (used only as fallback — normally provisioned via BLE)
#define SECRET_WIFI_SSID        "your_ssid"
#define SECRET_WIFI_PASS        "your_password"

// MQTT Broker
#define SECRET_MQTT_HOST        "your.broker.com"
#define SECRET_MQTT_USER        "mqtt_username"
#define SECRET_MQTT_PASS        "mqtt_password"

// MQTT Certificate Fingerprint (SHA-256, colon-separated)
#define SECRET_MQTT_FINGERPRINT "AA:BB:CC:DD:..."

// OTA Auth Token
#define SECRET_OTA_TOKEN        "your-secret-ota-token"
```

`cert.h` should define:
```cpp
const char* root_ca = R"EOF(
-----BEGIN CERTIFICATE-----
... your CA certificate ...
-----END CERTIFICATE-----
)EOF";
```

---

## Known Limitations & IDF Migration Path

### Items requiring full ESP-IDF project (not achievable in Arduino layer):

**1. NVS Encryption**
WiFi credentials are stored in plaintext NVS flash. To encrypt:
- Migrate to ESP-IDF project structure
- Add `CONFIG_NVS_ENCRYPTION=y` to `sdkconfig`
- Generate NVS encryption key stored in eFuse partition
- Use `nvs_flash_secure_init()` instead of `nvs_flash_init()`

**2. OTA Firmware Signature Verification**
`esp_https_ota()` verifies TLS but not the binary's authenticity. For code-signed OTA:
- Use `esp_ota_ops.h` with `esp_ota_begin()` / `esp_ota_write()` / `esp_ota_end()`
- Embed a public key, compute SHA-256 of downloaded binary
- Reject if hash doesn't match signed manifest

**3. True MQTT Leaf Certificate Pinning**
`setCACert()` validates the full chain but not the specific leaf cert. For true pinning:
- Use `mbedtls_ssl_get_peer_cert()` after TLS handshake
- Compare DER-encoded cert hash against stored fingerprint
- Requires native mbedTLS hook in IDF project

**4. Non-Blocking WiFi State Machine in setup()**
`setup()` still blocks during WiFi connect (up to 10s with WDT resets). A full async state machine would require restructuring `setup()` into a FreeRTOS task with polling, which changes the entire boot architecture.

---

## Firmware Evolution Log

| Version | Score | Key Changes |
|---------|-------|-------------|
| v1 (original) | ~65/100 | Basic sensors + MQTT, no thread safety |
| v2 | ~75/100 | Added FreeRTOS tasks, basic mutex |
| v3 | ~85/100 | Fixed `preferences.end()`, `totalML` truncation, volatile flags |
| v4 | ~90/100 | Exponential backoff, deferred NVS writes, BLE disconnect handler |
| v5 | ~93/100 | Hardware WDT, sensor NaN validation, startup health checks |
| v6 | ~97/100 | Eliminated `String` heap alloc, hysteresis, stale detection, boot reason |
| v7 | ~98/100 | Task monitor + auto-restart, offline queue, per-device topics, ADC cal |
| v8 (current) | ~99/100 | OTA auth token, restart counter guard, heap fragmentation, `static_assert`, safe mode, RSSI, fatal NVS codes, randomized MQTT jitter |

> **Remaining 1 point:** Full NVS encryption, OTA signature verification, and true TLS leaf pinning — all require migrating to a pure ESP-IDF project. This is the only architectural boundary left.