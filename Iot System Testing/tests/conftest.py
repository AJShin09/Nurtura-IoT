"""
Shared pytest fixtures for the Nurtura IoT Test Suite.
"""

import asyncio
import pytest
import pytest_asyncio
from unittest.mock import AsyncMock, MagicMock

from mock_device.mock_device import MockIoTDevice
from tester.bluetooth_interface.bluetooth_interface import MockBluetoothInterface
from tester.mqtt_listener.mqtt_listener import MQTTListener, MQTTMessage
from tester.validator.validator import Validator


# ---------------------------------------------------------------------------
# Core component fixtures
# ---------------------------------------------------------------------------

@pytest.fixture
def mock_device():
    """Fresh MockIoTDevice for each test."""
    return MockIoTDevice(device_id="AABBCCDDEEFF")


@pytest.fixture
def bluetooth(mock_device):
    """MockBluetoothInterface wired to the mock device."""
    return MockBluetoothInterface(mock_device)


@pytest.fixture
def validator():
    """Fresh Validator instance."""
    return Validator()


@pytest.fixture
def mqtt_listener():
    """MQTTListener with device id matching MockIoTDevice default."""
    return MQTTListener(device_id="AABBCCDDEEFF")


# ---------------------------------------------------------------------------
# Pre-populated MQTTListener stubs (no real broker needed)
# ---------------------------------------------------------------------------

@pytest.fixture
def mqtt_with_sensor_data():
    """MQTTListener pre-loaded with one valid sensor reading."""
    listener = MQTTListener(device_id="AABBCCDDEEFF")
    payload = {
        "t": 25.0, "h": 55.0, "m": 50,
        "lux": 300.0, "f": 1.5, "total": 100,
        "p": "OFF", "l": "ON",
        "rssi": -60,
        "err_bme": False, "err_lux": False,
        "uptime": 30, "reconnects": 0,
    }
    listener.messages.append(
        MQTTMessage(topic="nurtura/AABBCCDDEEFF/sensor", payload=payload)
    )
    return listener


@pytest.fixture
def mqtt_with_bad_sensor_data():
    """MQTTListener pre-loaded with an out-of-range sensor reading."""
    listener = MQTTListener(device_id="AABBCCDDEEFF")
    payload = {
        "t": 999.0,   # wildly out of range
        "h": -5.0,    # negative humidity
        "m": 50, "lux": 300.0, "f": 0.0, "total": 0,
        "p": "OFF", "l": "OFF",
        "rssi": -60,
        "err_bme": False, "err_lux": False,
        "uptime": 10, "reconnects": 0,
    }
    listener.messages.append(
        MQTTMessage(topic="nurtura/AABBCCDDEEFF/sensor", payload=payload)
    )
    return listener


@pytest.fixture
def mqtt_with_fault_flags():
    """MQTTListener pre-loaded with sensor fault flags set."""
    listener = MQTTListener(device_id="AABBCCDDEEFF")
    payload = {
        "t": 25.0, "h": 55.0, "m": 50,
        "lux": 300.0, "f": 0.0, "total": 0,
        "p": "OFF", "l": "OFF",
        "rssi": -60,
        "err_bme": True,   # BME280 fault
        "err_lux": False,
        "uptime": 10, "reconnects": 0,
    }
    listener.messages.append(
        MQTTMessage(topic="nurtura/AABBCCDDEEFF/sensor", payload=payload)
    )
    return listener


@pytest.fixture
def mqtt_offline():
    """MQTTListener that received an LWT offline message."""
    listener = MQTTListener(device_id="AABBCCDDEEFF")
    # One sensor reading then LWT
    listener.messages.append(
        MQTTMessage(topic="nurtura/AABBCCDDEEFF/sensor",
                    payload={"t": 25.0, "h": 55.0, "m": 50,
                             "lux": 300.0, "f": 0.0, "total": 0,
                             "p": "OFF", "l": "OFF", "rssi": -60,
                             "err_bme": False, "err_lux": False,
                             "uptime": 5, "reconnects": 0})
    )
    listener.messages.append(
        MQTTMessage(topic="nurtura/AABBCCDDEEFF/status", payload={"online": False})
    )
    return listener
