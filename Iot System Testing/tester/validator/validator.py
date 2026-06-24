import logging
from dataclasses import dataclass, field
from datetime import datetime
from enum import Enum

logger = logging.getLogger("Validator")


class TestStatus(str, Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    SKIP = "SKIP"


@dataclass
class TestResult:
    name: str
    status: TestStatus
    message: str = ""
    timestamp: str = field(default_factory=lambda: datetime.now().isoformat())


class Validator:
    """
    Validates all stages of the Nurtura device test flow.

    Nurtura sensor payload keys (short names from firmware):
      t       - temperature (C)
      h       - humidity (%)
      m       - soil moisture (0-100)
      lux     - light level (lux)
      f       - flow rate (L/min)
      total   - total water dispensed (mL)
      p       - pump state ("ON" / "OFF" / "ERR_DRY")
      l       - light relay state ("ON" / "OFF")
      rssi    - WiFi signal strength (dBm, negative)
      err_bme - BME280 fault flag (bool)
      err_lux - BH1750 fault flag (bool)
      uptime  - seconds since boot
      reconnects - WiFi reconnect count
    """

    TEMP_MIN = -20.0
    TEMP_MAX = 60.0
    HUM_MIN = 0.0
    HUM_MAX = 100.0
    MOIST_MIN = 0
    MOIST_MAX = 100
    LUX_MIN = 0.0
    RSSI_MIN = -100
    RSSI_MAX = 0

    def __init__(self):
        self.results: list[TestResult] = []

    def _record(self, name: str, passed: bool, message: str = "") -> TestResult:
        status = TestStatus.PASS if passed else TestStatus.FAIL
        result = TestResult(name=name, status=status, message=message)
        self.results.append(result)
        icon = "[PASS]" if passed else "[FAIL]"
        logger.info(f"{icon} {name}: {message}")
        return result

    def _skip(self, name: str, message: str = "") -> TestResult:
        result = TestResult(name=name, status=TestStatus.SKIP, message=message)
        self.results.append(result)
        logger.warning(f"[SKIP] {name}: {message}")
        return result

    def check_bluetooth_connected(self, connected: bool) -> TestResult:
        return self._record(
            "Bluetooth Connection",
            connected,
            "Nurtura Rack found and connected" if connected else "Could not find or connect to Nurtura Rack"
        )

    def check_device_id_read(self, device_id: str) -> TestResult:
        valid = len(device_id) == 12 and device_id.isalnum()
        return self._record(
            "Device ID Read",
            valid,
            f"MAC-based device ID: {device_id}" if valid else f"Invalid device ID received: '{device_id}'"
        )

    def check_credentials_sent(self, sent: bool) -> TestResult:
        return self._record(
            "WiFi Credentials Sent",
            sent,
            "SSID and password written to BLE characteristics" if sent else "Failed to write credentials"
        )

    def check_ble_status_notification(self, status: str) -> TestResult:
        connected = status == "connected"
        messages = {
            "connected": "Device notified STATUS_CHAR: connected",
            "failed":    "Device notified STATUS_CHAR: failed (wrong password or network down)",
            "timeout":   "No STATUS_CHAR notification received within timeout",
        }
        return self._record(
            "BLE Status Notification",
            connected,
            messages.get(status, f"Unexpected status: '{status}'")
        )

    def check_sensor_data_published(self, mqtt_listener) -> TestResult:
        readings = mqtt_listener.get_sensor_readings()
        has_data = len(readings) > 0
        return self._record(
            "Sensor Data Published",
            has_data,
            f"Received {len(readings)} sensor reading(s) on nurtura/.../sensor" if has_data
            else "No sensor readings received - device may not have connected to MQTT"
        )

    def check_sensor_sanity(self, mqtt_listener) -> TestResult:
        readings = mqtt_listener.get_sensor_readings()
        if not readings:
            return self._skip("Sensor Data Sanity", "No readings to validate")

        issues = []
        for reading in readings:
            t = reading.get("t")
            h = reading.get("h")
            m = reading.get("m")
            lux = reading.get("lux")
            rssi = reading.get("rssi")

            if t is not None and not (self.TEMP_MIN <= float(t) <= self.TEMP_MAX):
                issues.append(
                    f"Temperature {t}C out of range [{self.TEMP_MIN}, {self.TEMP_MAX}]")

            if h is not None and not (self.HUM_MIN <= float(h) <= self.HUM_MAX):
                issues.append(
                    f"Humidity {h}% out of range [{self.HUM_MIN}, {self.HUM_MAX}]")

            if m is not None and not (self.MOIST_MIN <= int(m) <= self.MOIST_MAX):
                issues.append(
                    f"Moisture {m} out of range [{self.MOIST_MIN}, {self.MOIST_MAX}]")

            if lux is not None and float(lux) < self.LUX_MIN:
                issues.append(f"Lux {lux} is negative (sensor fault)")

            if rssi is not None and not (self.RSSI_MIN <= int(rssi) <= self.RSSI_MAX):
                issues.append(f"RSSI {rssi} dBm out of expected range")

        passed = len(issues) == 0
        return self._record(
            "Sensor Data Sanity",
            passed,
            "All readings within valid range" if passed else "; ".join(issues)
        )

    def check_sensor_faults(self, mqtt_listener) -> TestResult:
        readings = mqtt_listener.get_sensor_readings()
        if not readings:
            return self._skip("Sensor Fault Flags", "No readings to check")

        faults = []
        for r in readings:
            if r.get("err_bme") is True:
                faults.append("BME280 fault reported (err_bme=true)")
                break
        for r in readings:
            if r.get("err_lux") is True:
                faults.append("BH1750 fault reported (err_lux=true)")
                break

        passed = len(faults) == 0
        return self._record(
            "Sensor Fault Flags",
            passed,
            "No sensor faults detected" if passed else "; ".join(faults)
        )

    def check_pump_state_valid(self, mqtt_listener) -> TestResult:
        readings = mqtt_listener.get_sensor_readings()
        if not readings:
            return self._skip("Pump State", "No readings to check")
        valid_states = {"ON", "OFF", "ERR_DRY"}
        invalid = [r.get("p")
                   for r in readings if r.get("p") not in valid_states]
        passed = len(invalid) == 0
        return self._record(
            "Pump State",
            passed,
            f"All pump states valid ({valid_states})" if passed else f"Invalid pump states seen: {invalid}"
        )

    def check_device_online(self, mqtt_listener) -> TestResult:
        online = mqtt_listener.is_online()
        return self._record(
            "Device Online",
            online,
            "Device is publishing data (online)" if online
            else "LWT offline message received - device dropped connection"
        )

    def overall_passed(self) -> bool:
        return all(r.status == TestStatus.PASS for r in self.results if r.status != TestStatus.SKIP)

    def summary(self) -> str:
        lines = ["\n" + "=" * 54, "  NURTURA DEVICE TEST REPORT", "=" * 54]
        for r in self.results:
            icon = {"PASS": "[PASS]", "FAIL": "[FAIL]",
                    "SKIP": "[SKIP]"}.get(r.status, "?")
            lines.append(f"  {icon} {r.name:<34} {r.status.value}")
        lines.append("=" * 54)
        overall = "[SUCCESS]" if self.overall_passed() else "[FAILED]"
        lines.append(f"  OVERALL RESULT: {overall}")
        lines.append("=" * 54 + "\n")
        return "\n".join(lines)

    def reset(self):
        self.results.clear()
