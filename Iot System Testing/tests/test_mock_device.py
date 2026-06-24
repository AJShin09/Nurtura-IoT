"""
Tests for MockIoTDevice — verifies state transitions and workflow
without connecting to any real broker.
"""

import asyncio
import pytest
import pytest_asyncio
from unittest.mock import patch, MagicMock
from mock_device.mock_device import MockIoTDevice

pytestmark = pytest.mark.asyncio


# ---------------------------------------------------------------------------
# Initialisation  (sync tests — no asyncio mark)
# ---------------------------------------------------------------------------

def test_device_id_stored(mock_device):
    assert mock_device.device_id == "AABBCCDDEEFF"


def test_initial_state_not_connected(mock_device):
    assert mock_device._bt_connected is False
    assert mock_device._wifi_connected is False


def test_initial_ble_status_is_none(mock_device):
    assert mock_device._ble_status is None


def test_custom_device_id():
    device = MockIoTDevice(device_id="112233445566")
    assert device.device_id == "112233445566"


def test_base_topic_format(mock_device):
    assert mock_device.base_topic == "nurtura/AABBCCDDEEFF"


# ---------------------------------------------------------------------------
# on_bluetooth_connected
# ---------------------------------------------------------------------------

async def test_on_bluetooth_connected_sets_flag(mock_device):
    await mock_device.on_bluetooth_connected()
    assert mock_device._bt_connected is True


# ---------------------------------------------------------------------------
# on_credentials_received
# ---------------------------------------------------------------------------

async def test_credentials_stored_on_ssid(mock_device):
    with patch.object(mock_device, '_publish_sync', return_value=None):
        await mock_device.on_credentials_received("MyNet", "MyPass")
        await asyncio.sleep(0.1)
    assert mock_device._ssid == "MyNet"


async def test_credentials_stored_on_password(mock_device):
    with patch.object(mock_device, '_publish_sync', return_value=None):
        await mock_device.on_credentials_received("MyNet", "MyPass")
        await asyncio.sleep(0.1)
    assert mock_device._password == "MyPass"


async def test_credentials_trigger_workflow(mock_device):
    """Workflow runs and eventually sets _ble_status to 'connected'."""
    with patch.object(mock_device, '_publish_sync', return_value=None):
        await mock_device.on_credentials_received("SSID", "Pass")
        # Give the internal asyncio.create_task time to run
        await asyncio.sleep(5)
    assert mock_device._ble_status == "connected"


# ---------------------------------------------------------------------------
# on_rack_name_received
# ---------------------------------------------------------------------------

async def test_rack_name_stored(mock_device):
    await mock_device.on_rack_name_received("Rack B")
    assert mock_device._rack_name == "Rack B"


# ---------------------------------------------------------------------------
# get_ble_status
# ---------------------------------------------------------------------------

async def test_get_ble_status_initially_none(mock_device):
    status = await mock_device.get_ble_status()
    assert status is None


async def test_get_ble_status_after_workflow(mock_device):
    with patch.object(mock_device, '_publish_sync', return_value=None):
        await mock_device.on_credentials_received("Net", "Pass")
        await asyncio.sleep(5)
    status = await mock_device.get_ble_status()
    assert status == "connected"


# ---------------------------------------------------------------------------
# _simulate_wifi_connect
# ---------------------------------------------------------------------------

async def test_wifi_connect_sets_flag(mock_device):
    await mock_device._simulate_wifi_connect()
    assert mock_device._wifi_connected is True


async def test_wifi_connect_sets_ble_status(mock_device):
    await mock_device._simulate_wifi_connect()
    assert mock_device._ble_status == "connected"


# ---------------------------------------------------------------------------
# _simulate_sensor_publish (mocked broker)
# ---------------------------------------------------------------------------

async def test_sensor_publish_calls_publish_sync(mock_device):
    published = []

    def capture(topic, payload):
        published.append((topic, payload))

    with patch.object(mock_device, '_publish_sync', side_effect=capture):
        await mock_device._simulate_sensor_publish()

    assert len(published) == 3
    for topic, payload in published:
        assert topic == "nurtura/AABBCCDDEEFF/sensor"
        assert "t" in payload
        assert "h" in payload
        assert "p" in payload


async def test_sensor_publish_temperature_in_range(mock_device):
    captured = []

    def capture(topic, payload):
        captured.append(payload)

    with patch.object(mock_device, '_publish_sync', side_effect=capture):
        await mock_device._simulate_sensor_publish()

    for p in captured:
        assert -20.0 <= p["t"] <= 60.0


async def test_sensor_publish_humidity_in_range(mock_device):
    captured = []

    def capture(topic, payload):
        captured.append(payload)

    with patch.object(mock_device, '_publish_sync', side_effect=capture):
        await mock_device._simulate_sensor_publish()

    for p in captured:
        assert 0.0 <= p["h"] <= 100.0


async def test_sensor_publish_no_faults(mock_device):
    captured = []

    def capture(topic, payload):
        captured.append(payload)

    with patch.object(mock_device, '_publish_sync', side_effect=capture):
        await mock_device._simulate_sensor_publish()

    for p in captured:
        assert p["err_bme"] is False
        assert p["err_lux"] is False


async def test_sensor_publish_pump_state_valid(mock_device):
    captured = []
    valid_states = {"ON", "OFF", "ERR_DRY"}

    def capture(topic, payload):
        captured.append(payload)

    with patch.object(mock_device, '_publish_sync', side_effect=capture):
        await mock_device._simulate_sensor_publish()

    for p in captured:
        assert p["p"] in valid_states
