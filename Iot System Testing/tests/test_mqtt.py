"""
Tests for MQTTListener message storage and query methods.
These tests do NOT connect to a real broker — they work with pre-populated
fixtures from conftest.py so the suite runs fully offline.
"""

import pytest
from tester.mqtt_listener.mqtt_listener import MQTTListener, MQTTMessage


# ---------------------------------------------------------------------------
# Initialisation
# ---------------------------------------------------------------------------

def test_listener_initialises_empty(mqtt_listener):
    assert mqtt_listener.get_sensor_readings() == []


def test_listener_device_id_stored(mqtt_listener):
    assert mqtt_listener.device_id == "AABBCCDDEEFF"


def test_listener_base_topic(mqtt_listener):
    assert mqtt_listener.base_topic == "nurtura/AABBCCDDEEFF"


def test_topics_include_sensor(mqtt_listener):
    assert "nurtura/AABBCCDDEEFF/sensor" in mqtt_listener.topics


def test_topics_include_diag(mqtt_listener):
    assert "nurtura/AABBCCDDEEFF/diag" in mqtt_listener.topics


# ---------------------------------------------------------------------------
# Sensor readings retrieval
# ---------------------------------------------------------------------------

def test_get_sensor_readings_returns_dicts(mqtt_with_sensor_data):
    readings = mqtt_with_sensor_data.get_sensor_readings()
    assert len(readings) == 1
    assert isinstance(readings[0], dict)


def test_sensor_reading_has_temperature(mqtt_with_sensor_data):
    readings = mqtt_with_sensor_data.get_sensor_readings()
    assert "t" in readings[0]


def test_sensor_reading_has_humidity(mqtt_with_sensor_data):
    readings = mqtt_with_sensor_data.get_sensor_readings()
    assert "h" in readings[0]


def test_sensor_reading_has_pump_state(mqtt_with_sensor_data):
    readings = mqtt_with_sensor_data.get_sensor_readings()
    assert "p" in readings[0]


def test_sensor_reading_has_moisture(mqtt_with_sensor_data):
    readings = mqtt_with_sensor_data.get_sensor_readings()
    assert "m" in readings[0]


# ---------------------------------------------------------------------------
# is_online() logic
# ---------------------------------------------------------------------------

def test_is_online_when_sensor_data_present(mqtt_with_sensor_data):
    assert mqtt_with_sensor_data.is_online() is True


def test_is_online_false_after_lwt(mqtt_offline):
    assert mqtt_offline.is_online() is False


def test_is_online_false_when_no_messages():
    listener = MQTTListener(device_id="AABBCCDDEEFF")
    # No messages at all — no sensor data, no LWT → not online
    assert listener.is_online() is False


# ---------------------------------------------------------------------------
# clear()
# ---------------------------------------------------------------------------

def test_clear_removes_all_messages(mqtt_with_sensor_data):
    mqtt_with_sensor_data.clear()
    assert mqtt_with_sensor_data.get_sensor_readings() == []


def test_clear_then_is_online_false(mqtt_with_sensor_data):
    mqtt_with_sensor_data.clear()
    assert mqtt_with_sensor_data.is_online() is False


# ---------------------------------------------------------------------------
# get_messages_by_topic
# ---------------------------------------------------------------------------

def test_get_messages_by_topic_sensor(mqtt_with_sensor_data):
    msgs = mqtt_with_sensor_data.get_messages_by_topic("sensor")
    assert len(msgs) == 1


def test_get_messages_by_topic_missing_returns_empty(mqtt_with_sensor_data):
    msgs = mqtt_with_sensor_data.get_messages_by_topic("diag")
    assert msgs == []


# ---------------------------------------------------------------------------
# Multiple messages
# ---------------------------------------------------------------------------

def test_multiple_sensor_readings():
    listener = MQTTListener(device_id="AABBCCDDEEFF")
    for i in range(5):
        listener.messages.append(
            MQTTMessage(
                topic="nurtura/AABBCCDDEEFF/sensor",
                payload={"t": 20.0 + i, "h": 50.0, "m": 40,
                         "lux": 200.0, "f": 0.0, "total": 0,
                         "p": "OFF", "l": "OFF", "rssi": -65,
                         "err_bme": False, "err_lux": False,
                         "uptime": i * 5, "reconnects": 0}
            )
        )
    assert len(listener.get_sensor_readings()) == 5
