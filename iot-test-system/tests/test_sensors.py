"""
Tests for Validator — covers every check method including edge cases
for temperature, humidity, moisture, RSSI, fault flags, pump states,
and the overall pass/fail logic.
"""

import pytest
from tester.validator.validator import Validator, TestStatus
from tester.mqtt_listener.mqtt_listener import MQTTListener, MQTTMessage


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def make_listener(payload: dict) -> MQTTListener:
    """Wrap a single sensor payload in a listener."""
    listener = MQTTListener(device_id="AABBCCDDEEFF")
    listener.messages.append(
        MQTTMessage(topic="nurtura/AABBCCDDEEFF/sensor", payload=payload)
    )
    return listener


VALID_PAYLOAD = {
    "t": 25.0, "h": 55.0, "m": 50,
    "lux": 300.0, "f": 1.5, "total": 100,
    "p": "OFF", "l": "ON",
    "rssi": -60,
    "err_bme": False, "err_lux": False,
    "uptime": 30, "reconnects": 0,
}


# ---------------------------------------------------------------------------
# Bluetooth connection
# ---------------------------------------------------------------------------

def test_bluetooth_connected_pass(validator):
    r = validator.check_bluetooth_connected(True)
    assert r.status == TestStatus.PASS


def test_bluetooth_connected_fail(validator):
    r = validator.check_bluetooth_connected(False)
    assert r.status == TestStatus.FAIL


# ---------------------------------------------------------------------------
# Device ID
# ---------------------------------------------------------------------------

def test_device_id_valid_12_alnum(validator):
    r = validator.check_device_id_read("AABBCCDDEEFF")
    assert r.status == TestStatus.PASS


def test_device_id_too_short(validator):
    r = validator.check_device_id_read("AABBCC")
    assert r.status == TestStatus.FAIL


def test_device_id_contains_colon(validator):
    r = validator.check_device_id_read("AA:BB:CC:DD:EE:FF")
    assert r.status == TestStatus.FAIL


def test_device_id_empty(validator):
    r = validator.check_device_id_read("")
    assert r.status == TestStatus.FAIL


# ---------------------------------------------------------------------------
# Credentials
# ---------------------------------------------------------------------------

def test_credentials_sent_pass(validator):
    r = validator.check_credentials_sent(True)
    assert r.status == TestStatus.PASS


def test_credentials_sent_fail(validator):
    r = validator.check_credentials_sent(False)
    assert r.status == TestStatus.FAIL


# ---------------------------------------------------------------------------
# BLE status notification
# ---------------------------------------------------------------------------

def test_ble_status_connected(validator):
    r = validator.check_ble_status_notification("connected")
    assert r.status == TestStatus.PASS


def test_ble_status_failed(validator):
    r = validator.check_ble_status_notification("failed")
    assert r.status == TestStatus.FAIL


def test_ble_status_timeout(validator):
    r = validator.check_ble_status_notification("timeout")
    assert r.status == TestStatus.FAIL


def test_ble_status_unexpected(validator):
    r = validator.check_ble_status_notification("unknown_value")
    assert r.status == TestStatus.FAIL


# ---------------------------------------------------------------------------
# Sensor data published
# ---------------------------------------------------------------------------

def test_sensor_data_published_pass(validator, mqtt_with_sensor_data):
    r = validator.check_sensor_data_published(mqtt_with_sensor_data)
    assert r.status == TestStatus.PASS


def test_sensor_data_published_fail(validator, mqtt_listener):
    r = validator.check_sensor_data_published(mqtt_listener)
    assert r.status == TestStatus.FAIL


# ---------------------------------------------------------------------------
# Sensor sanity — valid data
# ---------------------------------------------------------------------------

def test_sensor_sanity_pass_valid(validator, mqtt_with_sensor_data):
    r = validator.check_sensor_sanity(mqtt_with_sensor_data)
    assert r.status == TestStatus.PASS


def test_sensor_sanity_skip_when_empty(validator, mqtt_listener):
    r = validator.check_sensor_sanity(mqtt_listener)
    assert r.status == TestStatus.SKIP


# ---------------------------------------------------------------------------
# Sensor sanity — temperature boundaries
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("temp", [-20.0, 0.0, 30.0, 60.0])
def test_sensor_sanity_temperature_in_range(validator, temp):
    listener = make_listener({**VALID_PAYLOAD, "t": temp})
    r = validator.check_sensor_sanity(listener)
    assert r.status == TestStatus.PASS


@pytest.mark.parametrize("temp", [-21.0, 61.0, 999.0, -100.0])
def test_sensor_sanity_temperature_out_of_range(validator, temp):
    listener = make_listener({**VALID_PAYLOAD, "t": temp})
    r = validator.check_sensor_sanity(listener)
    assert r.status == TestStatus.FAIL


# ---------------------------------------------------------------------------
# Sensor sanity — humidity boundaries
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("hum", [0.0, 50.0, 100.0])
def test_sensor_sanity_humidity_in_range(validator, hum):
    listener = make_listener({**VALID_PAYLOAD, "h": hum})
    r = validator.check_sensor_sanity(listener)
    assert r.status == TestStatus.PASS


@pytest.mark.parametrize("hum", [-1.0, 101.0])
def test_sensor_sanity_humidity_out_of_range(validator, hum):
    listener = make_listener({**VALID_PAYLOAD, "h": hum})
    r = validator.check_sensor_sanity(listener)
    assert r.status == TestStatus.FAIL


# ---------------------------------------------------------------------------
# Sensor sanity — moisture boundaries
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("moist", [0, 50, 100])
def test_sensor_sanity_moisture_in_range(validator, moist):
    listener = make_listener({**VALID_PAYLOAD, "m": moist})
    r = validator.check_sensor_sanity(listener)
    assert r.status == TestStatus.PASS


@pytest.mark.parametrize("moist", [-1, 101])
def test_sensor_sanity_moisture_out_of_range(validator, moist):
    listener = make_listener({**VALID_PAYLOAD, "m": moist})
    r = validator.check_sensor_sanity(listener)
    assert r.status == TestStatus.FAIL


# ---------------------------------------------------------------------------
# Sensor sanity — lux
# ---------------------------------------------------------------------------

def test_sensor_sanity_lux_zero_ok(validator):
    listener = make_listener({**VALID_PAYLOAD, "lux": 0.0})
    r = validator.check_sensor_sanity(listener)
    assert r.status == TestStatus.PASS


def test_sensor_sanity_lux_negative_fail(validator):
    listener = make_listener({**VALID_PAYLOAD, "lux": -1.0})
    r = validator.check_sensor_sanity(listener)
    assert r.status == TestStatus.FAIL


# ---------------------------------------------------------------------------
# Sensor sanity — RSSI boundaries
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("rssi", [-100, -60, -1])
def test_sensor_sanity_rssi_in_range(validator, rssi):
    listener = make_listener({**VALID_PAYLOAD, "rssi": rssi})
    r = validator.check_sensor_sanity(listener)
    assert r.status == TestStatus.PASS


@pytest.mark.parametrize("rssi", [-101, 1, 50])
def test_sensor_sanity_rssi_out_of_range(validator, rssi):
    listener = make_listener({**VALID_PAYLOAD, "rssi": rssi})
    r = validator.check_sensor_sanity(listener)
    assert r.status == TestStatus.FAIL


# ---------------------------------------------------------------------------
# Fault flags
# ---------------------------------------------------------------------------

def test_fault_flags_none_pass(validator, mqtt_with_sensor_data):
    r = validator.check_sensor_faults(mqtt_with_sensor_data)
    assert r.status == TestStatus.PASS


def test_fault_flags_bme_fail(validator, mqtt_with_fault_flags):
    r = validator.check_sensor_faults(mqtt_with_fault_flags)
    assert r.status == TestStatus.FAIL


def test_fault_flags_lux_fail(validator):
    listener = make_listener({**VALID_PAYLOAD, "err_lux": True})
    r = validator.check_sensor_faults(listener)
    assert r.status == TestStatus.FAIL


def test_fault_flags_skip_empty(validator, mqtt_listener):
    r = validator.check_sensor_faults(mqtt_listener)
    assert r.status == TestStatus.SKIP


# ---------------------------------------------------------------------------
# Pump state
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("pump", ["ON", "OFF", "ERR_DRY"])
def test_pump_state_valid(validator, pump):
    listener = make_listener({**VALID_PAYLOAD, "p": pump})
    r = validator.check_pump_state_valid(listener)
    assert r.status == TestStatus.PASS


@pytest.mark.parametrize("pump", ["RUNNING", "BROKEN", "", None])
def test_pump_state_invalid(validator, pump):
    listener = make_listener({**VALID_PAYLOAD, "p": pump})
    r = validator.check_pump_state_valid(listener)
    assert r.status == TestStatus.FAIL


def test_pump_state_skip_empty(validator, mqtt_listener):
    r = validator.check_pump_state_valid(mqtt_listener)
    assert r.status == TestStatus.SKIP


# ---------------------------------------------------------------------------
# Device online
# ---------------------------------------------------------------------------

def test_device_online_pass(validator, mqtt_with_sensor_data):
    r = validator.check_device_online(mqtt_with_sensor_data)
    assert r.status == TestStatus.PASS


def test_device_online_fail_after_lwt(validator, mqtt_offline):
    r = validator.check_device_online(mqtt_offline)
    assert r.status == TestStatus.FAIL


# ---------------------------------------------------------------------------
# overall_passed()
# ---------------------------------------------------------------------------

def test_overall_passed_all_pass(validator, mqtt_with_sensor_data):
    validator.check_bluetooth_connected(True)
    validator.check_credentials_sent(True)
    validator.check_ble_status_notification("connected")
    validator.check_sensor_data_published(mqtt_with_sensor_data)
    validator.check_sensor_sanity(mqtt_with_sensor_data)
    assert validator.overall_passed() is True


def test_overall_passed_one_fail(validator, mqtt_with_sensor_data):
    validator.check_bluetooth_connected(True)
    validator.check_credentials_sent(False)   # <-- fail
    assert validator.overall_passed() is False


def test_overall_passed_skips_are_ignored(validator):
    """SKIPs must not be counted as failures."""
    validator.check_bluetooth_connected(True)
    empty = MQTTListener(device_id="AABBCCDDEEFF")
    validator.check_sensor_sanity(empty)      # SKIP
    assert validator.overall_passed() is True


# ---------------------------------------------------------------------------
# reset()
# ---------------------------------------------------------------------------

def test_reset_clears_results(validator):
    validator.check_bluetooth_connected(True)
    validator.reset()
    assert validator.results == []


# ---------------------------------------------------------------------------
# summary()
# ---------------------------------------------------------------------------

def test_summary_contains_pass(validator):
    validator.check_bluetooth_connected(True)
    assert "PASS" in validator.summary()


def test_summary_contains_fail(validator):
    validator.check_bluetooth_connected(False)
    assert "FAIL" in validator.summary()


def test_summary_contains_overall(validator):
    assert "OVERALL" in validator.summary()
