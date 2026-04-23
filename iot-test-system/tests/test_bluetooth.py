"""
Tests for MockBluetoothInterface ↔ MockIoTDevice interaction.
"""

import pytest
import pytest_asyncio
from mock_device.mock_device import MockIoTDevice
from tester.bluetooth_interface.bluetooth_interface import MockBluetoothInterface


pytestmark = pytest.mark.asyncio


# ---------------------------------------------------------------------------
# Scan
# ---------------------------------------------------------------------------

async def test_scan_returns_devices(bluetooth):
    devices = await bluetooth.scan()
    assert len(devices) > 0, "Scan should find at least one mock device"


async def test_scan_returns_list(bluetooth):
    devices = await bluetooth.scan()
    assert isinstance(devices, list)


# ---------------------------------------------------------------------------
# Connect / Disconnect
# ---------------------------------------------------------------------------

async def test_connect_succeeds(bluetooth):
    devices = await bluetooth.scan()
    result = await bluetooth.connect(devices[0])
    assert result is True


async def test_is_connected_after_connect(bluetooth):
    devices = await bluetooth.scan()
    await bluetooth.connect(devices[0])
    assert await bluetooth.is_connected() is True


async def test_is_not_connected_before_connect(bluetooth):
    assert await bluetooth.is_connected() is False


async def test_disconnect_after_connect(bluetooth):
    devices = await bluetooth.scan()
    await bluetooth.connect(devices[0])
    result = await bluetooth.disconnect()
    assert result is True
    assert await bluetooth.is_connected() is False


# ---------------------------------------------------------------------------
# Device ID
# ---------------------------------------------------------------------------

async def test_get_device_id_format(bluetooth):
    devices = await bluetooth.scan()
    await bluetooth.connect(devices[0])
    device_id = await bluetooth.get_device_id()
    assert len(device_id) == 12
    assert device_id.isalnum()


async def test_get_device_id_is_uppercase(bluetooth):
    devices = await bluetooth.scan()
    await bluetooth.connect(devices[0])
    device_id = await bluetooth.get_device_id()
    assert device_id == device_id.upper()


# ---------------------------------------------------------------------------
# Credentials
# ---------------------------------------------------------------------------

async def test_send_credentials_when_connected(bluetooth):
    devices = await bluetooth.scan()
    await bluetooth.connect(devices[0])
    result = await bluetooth.send_credentials("TestSSID", "TestPass123")
    assert result is True


async def test_send_credentials_when_not_connected(bluetooth):
    # Never called connect — should fail gracefully
    result = await bluetooth.send_credentials("TestSSID", "TestPass123")
    assert result is False


async def test_mock_device_receives_ssid(bluetooth, mock_device):
    devices = await bluetooth.scan()
    await bluetooth.connect(devices[0])
    await bluetooth.send_credentials("MyNetwork", "Secret")
    assert mock_device._ssid == "MyNetwork"


async def test_mock_device_receives_password(bluetooth, mock_device):
    devices = await bluetooth.scan()
    await bluetooth.connect(devices[0])
    await bluetooth.send_credentials("MyNetwork", "Secret")
    assert mock_device._password == "Secret"


# ---------------------------------------------------------------------------
# Rack name
# ---------------------------------------------------------------------------

async def test_send_rack_name(bluetooth, mock_device):
    devices = await bluetooth.scan()
    await bluetooth.connect(devices[0])
    await bluetooth.send_credentials("Net", "Pass")
    await bluetooth.send_rack_name("Rack A")
    assert mock_device._rack_name == "Rack A"


# ---------------------------------------------------------------------------
# Status notification
# ---------------------------------------------------------------------------

async def test_wait_for_status_returns_connected(bluetooth):
    devices = await bluetooth.scan()
    await bluetooth.connect(devices[0])
    await bluetooth.send_credentials("TestSSID", "TestPass")
    status = await bluetooth.wait_for_status(timeout_sec=15.0)
    assert status == "connected"


async def test_wait_for_status_timeout(mock_device):
    """If no credentials are ever sent, status never arrives → timeout."""
    bt = MockBluetoothInterface(mock_device)
    status = await bt.wait_for_status(timeout_sec=0.5)
    assert status == "timeout"
