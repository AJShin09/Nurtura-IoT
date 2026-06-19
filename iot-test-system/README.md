<div align="center">

# Nurtura IoT

**Automated plant rack firmware for ESP32**  
BLE provisioning · MQTT telemetry · FreeRTOS dual-core · Production-grade reliability

![ESP32](https://img.shields.io/badge/ESP32-Arduino%20%2B%20ESP--IDF-E7352C?style=flat-square&logo=espressif&logoColor=white)
![MQTT](https://img.shields.io/badge/MQTT-HiveMQ%20TLS-660066?style=flat-square&logo=eclipse-mosquitto&logoColor=white)
![Tests](https://img.shields.io/badge/tests-115%20passing-00ff88?style=flat-square)
![Version](https://img.shields.io/badge/firmware-v2.6.1-0d1117?style=flat-square&color=00ff88)

</div>

---

## What it does

Nurtura is a firmware system for an ESP32-controlled plant rack. It reads sensors, drives relays, streams telemetry over MQTT, and configures itself over BLE — no hardcoded credentials, no cloud dependency for local control.

```
BLE Provisioning → WiFi → MQTT → Sensor Publish → Relay Automation
```

---

## Hardware

| Pin | Function |
|-----|----------|
| GPIO 35 | Capacitive soil moisture (ADC) |
| GPIO 13 | Water flow sensor (interrupt) |
| GPIO 25 | Pump relay (active LOW) |
| GPIO 26 | Grow light relay (active LOW) |
| GPIO 0 | Factory reset / BLE trigger (hold 3s) |
| I²C Bus 0 (21/22) | BH1750 light sensor + 16×2 LCD |
| I²C Bus 1 (16/17) | BME280 temperature + humidity |

---

## Features

**Connectivity**
- BLE provisioning — send WiFi credentials wirelessly, no reflashing
- MQTT over TLS (port 8883) to HiveMQ — AsyncAPI compliant topics
- Per-device topics derived from MAC address
- MQTT Last Will for online/offline detection

**Reliability**
- Hardware watchdog — device recovers from hangs automatically
- FreeRTOS dual-core task management with timed mutex guards
- Task auto-restart on failure — self-healing firmware
- WiFi auto-reconnect — survives network drops
- Sensor NaN validation and stale detection
- ADC eFuse calibration + median filter for accurate soil readings

**Control**
- Relay hysteresis — no rapid ON/OFF chatter near thresholds
- Relay lockout timers — protects hardware during extended runs
- Pump dry-run protection
- Local offline automation — works without MQTT
- Remote reboot and remote config via MQTT commands

**Observability**
- Heap monitoring, stack high-water marks
- Boot reason reporting (watchdog / power / manual reset)
- Sensor fault flags in every payload
- NTP timestamps on all published data

---

## MQTT Topics

| Topic | Description |
|-------|-------------|
| `nurtura/{mac}/sensor` | Temperature, humidity, moisture, lux, flow, pump state |
| `nurtura/{mac}/diag` | Heap, uptime, RSSI, firmware version |
| `nurtura/{mac}/status` | LWT — online / offline |
| `nurtura/{mac}/cmd` | Remote commands (reboot, config, pump reset) |

---

## Firmware Versions

| Version | Score | Recommended For |
|---------|-------|-----------------|
| v4 | 87/100 | Initial hardware bring-up and calibration |
| v6 | 97/100 | Multi-day prototype runs with real plants |
| **v8** | **99/100** | **Production deployment** |

> Start on **v4** to calibrate sensors. Move to **v6** for extended testing. Deploy **v8**.

---

## Test System

An automated test suite validates the full device workflow — no hardware required.

```bash
cd iot-test-system
pip install -r requirements.txt
python -m pytest          # 115 tests, no hardware needed
python -m tester.controller.controller mock   # full workflow simulation
python -m tester.controller.controller real   # real ESP32
```

**Test coverage**

| Suite | What it covers |
|-------|---------------|
| `test_bluetooth.py` | BLE scan, connect, credentials, status notifications |
| `test_mqtt.py` | Message storage, topic queries, LWT handling |
| `test_sensors.py` | All sensor fields, boundary values, fault flags |
| `test_mock_device.py` | State machine, full workflow, MQTT output |

---

## Quickstart

```bash
# 1. Copy secrets template
cp Nurtura/secrets.h.example Nurtura/secrets.h

# 2. Flash firmware (Arduino IDE or arduino-cli)
#    Board: ESP32 Dev Module

# 3. Power on → hold GPIO 0 for 3s to enter BLE mode
#    Send WiFi credentials via BLE
#    Device connects and starts publishing
```

**Dependencies:** `PubSubClient`, `Adafruit_BME280`, `BH1750`, `LiquidCrystal_I2C`, `BLEDevice`, `ArduinoJson`, `esp_task_wdt`

---

## Sensor Validation Ranges

| Sensor | Valid Range |
|--------|------------|
| Temperature | −20°C to 60°C |
| Humidity | 0% to 100% |
| Soil moisture | 0 to 100 |
| Lux | ≥ 0 |
| RSSI | −100 to 0 dBm |
| Pump state | `ON` · `OFF` · `ERR_DRY` |

---

<div align="center">

Built with ESP32 · FreeRTOS · MQTT · BLE · Python

</div>
