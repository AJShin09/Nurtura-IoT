# IoT Test System

Automated tester for an ESP32 IoT device.
Supports both **mocked** (no hardware) and **real device** modes.

## Workflow Tested

```
Bluetooth → WiFi Credentials → WiFi Connect → MQTT Connect → BT Disconnect → Sensor Publish
```

---

## Folder Structure

```
iot-test-system/
├── tester/
│   ├── controller/         ← Main test orchestrator
│   ├── bluetooth_interface/← Mock + Real BLE interfaces
│   ├── mqtt_listener/      ← Subscribes to HiveMQ, collects messages
│   └── validator/          ← Checks each test stage, generates report
├── mock_device/            ← Software simulation of ESP32
├── tests/                  ← pytest automated test suite
│   ├── conftest.py         ← Shared fixtures
│   ├── test_bluetooth.py   ← BLE scan, connect, credentials, status
│   ├── test_mqtt.py        ← MQTT listener message storage and queries
│   ├── test_sensors.py     ← Validator checks for all sensor fields
│   └── test_mock_device.py ← MockIoTDevice state and workflow
├── firmware/
│   └── device.ino          ← Arduino firmware for real ESP32
├── logs/                   ← Auto-generated .log files
├── reports/                ← Auto-generated HTML reports
├── pytest.ini              ← pytest configuration
└── requirements.txt
```

---

## Setup

```bash
cd iot-test-system
pip install -r requirements.txt
```

> `bleak` is only required for real device BLE testing.
> `asyncio-mqtt` is required for both modes.

---

## Run — Automated Tests (pytest)

Install pytest and plugins (one time):

```bash
pip install pytest pytest-asyncio pytest-mock pytest-html
```

Run the full test suite (no hardware needed):

```bash
python -m pytest
```

This will:
- Run 115 automated tests across Bluetooth, MQTT, sensors, and mock device
- Test all validator logic including boundary values for every sensor field
- Print live results to the console
- Save an HTML report to `reports/pytest_report.html`

Run a specific test file:

```bash
python -m pytest tests/test_bluetooth.py
python -m pytest tests/test_sensors.py
```

Run only tests matching a keyword:

```bash
python -m pytest -k "sensor"
python -m pytest -k "mqtt"
```

### Test Coverage

| File | What it tests |
|---|---|
| `test_bluetooth.py` | BLE scan, connect, disconnect, credentials, rack name, status notifications |
| `test_mqtt.py` | Message storage, topic queries, online/offline detection, LWT handling |
| `test_sensors.py` | All validator checks — temperature, humidity, moisture, RSSI, lux, fault flags, pump state, overall pass/fail |
| `test_mock_device.py` | MockIoTDevice state machine, workflow, MQTT publish output |

---

## Run — Mock Test (No Hardware)

```bash
python -m tester.controller.controller mock
```

This will:
- Spin up a virtual IoT device in software
- Simulate Bluetooth → WiFi → MQTT → Sensor workflow
- Connect to HiveMQ public broker
- Validate all stages
- Print console report
- Save `.log` file to `logs/`
- Save `.html` report to `reports/`

---

## Run — Real Device Test (ESP32)

Flash `firmware/device.ino` to your ESP32 first, then:

```bash
python -m tester.controller.controller real "YourWiFiSSID" "YourWiFiPassword"
```

Or run interactively (will prompt for SSID and password):

```bash
python -m tester.controller.controller real
```

---

## Example Console Output

```
======================================================
  NURTURA DEVICE TEST REPORT
======================================================
  [PASS] Bluetooth Connection           PASS
  [PASS] WiFi Credentials Sent          PASS
  [PASS] BLE Status Notification        PASS
  [PASS] Sensor Data Published          PASS
  [PASS] Sensor Data Sanity             PASS
  [PASS] Sensor Fault Flags             PASS
  [PASS] Pump State                     PASS
  [PASS] Device Online                  PASS
======================================================
  OVERALL RESULT: [SUCCESS]
======================================================
```

---

## MQTT Topics Used

| Topic | Purpose |
|---|---|
| `nurtura/{device_id}/sensor` | Temperature, humidity, moisture, lux, flow, pump state |
| `nurtura/{device_id}/diag` | Heap, uptime, RSSI diagnostics |
| `nurtura/noah/status` | LWT online/offline indicator |

Broker: `broker.hivemq.com:1883` (public, no auth)

---

## Sensor Validation Ranges

| Sensor | Valid Range |
|---|---|
| Temperature | -20°C to 60°C |
| Humidity | 0% to 100% |
| Moisture | 0 to 100 |
| Lux | ≥ 0 |
| RSSI | -100 to 0 dBm |
| Pump state | `ON`, `OFF`, or `ERR_DRY` |

---

## Extending

- Add more topics in `mqtt_listener.py` → `topics` property
- Add more checks in `validator.py` and mirror them in `tests/test_sensors.py`
- Replace mock sensor data in `mock_device.py` with any simulation you need
- Swap out the MQTT broker by changing `BROKER_HOST` in both `mock_device.py` and `mqtt_listener.py`