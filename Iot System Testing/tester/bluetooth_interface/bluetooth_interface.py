import asyncio
import logging

logger = logging.getLogger("BluetoothInterface")

DEVICE_ID_CHAR_UUID = "abc12345-1234-5678-1234-56789abcdef0"
SSID_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8"
PASSWORD_CHAR_UUID = "1c95d5e3-d8f7-413a-bf3d-7a2e5d7be87e"
STATUS_CHAR_UUID = "9a8ca5e3-d8f7-413a-bf3d-7a2e5d7be123"
RACK_NAME_CHAR_UUID = "a1b2c3d4-e5f6-7890-abcd-ef1234567890"
SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b"


class BluetoothInterface:
    async def scan(self) -> list[str]:
        raise NotImplementedError

    async def connect(self, device_address: str) -> bool:
        raise NotImplementedError

    async def get_device_id(self) -> str:
        raise NotImplementedError

    async def send_credentials(self, ssid: str, password: str) -> bool:
        raise NotImplementedError

    async def send_rack_name(self, rack_name: str) -> bool:
        raise NotImplementedError

    async def wait_for_status(self, timeout_sec: float = 20.0) -> str:
        raise NotImplementedError

    async def disconnect(self) -> bool:
        raise NotImplementedError

    async def is_connected(self) -> bool:
        raise NotImplementedError


class MockBluetoothInterface(BluetoothInterface):
    """
    Simulates BLE provisioning flow matching Nurtura firmware exactly.
    Communicates with MockIoTDevice via direct method calls.
    """

    def __init__(self, mock_device):
        self.mock_device = mock_device
        self._connected = False
        self._device_id = "AABBCCDDEEFF"

    async def scan(self) -> list[str]:
        await asyncio.sleep(0.3)
        logger.info("Mock BT scan found: Nurtura Rack")
        return ["MOCK_NURTURA_001"]

    async def connect(self, device_address: str) -> bool:
        await asyncio.sleep(0.2)
        self._connected = True
        await self.mock_device.on_bluetooth_connected()
        logger.info(f"Mock BT connected to {device_address}")
        return True

    async def get_device_id(self) -> str:
        await asyncio.sleep(0.1)
        logger.info(f"Mock device ID: {self._device_id}")
        return self._device_id

    async def send_credentials(self, ssid: str, password: str) -> bool:
        if not self._connected:
            logger.error("Cannot send credentials: not connected")
            return False
        await asyncio.sleep(0.1)
        await self.mock_device.on_credentials_received(ssid, password)
        logger.info(f"Mock BT sent credentials: SSID={ssid}")
        return True

    async def send_rack_name(self, rack_name: str) -> bool:
        await asyncio.sleep(0.1)
        await self.mock_device.on_rack_name_received(rack_name)
        logger.info(f"Mock BT sent rack name: {rack_name}")
        return True

    async def wait_for_status(self, timeout_sec: float = 20.0) -> str:
        deadline = asyncio.get_event_loop().time() + timeout_sec
        while asyncio.get_event_loop().time() < deadline:
            status = await self.mock_device.get_ble_status()
            if status in ("connected", "failed"):
                logger.info(f"Mock BT status notification: {status}")
                return status
            await asyncio.sleep(0.3)
        logger.error("Mock BT status notification timeout")
        return "timeout"

    async def disconnect(self) -> bool:
        await asyncio.sleep(0.1)
        self._connected = False
        logger.info("Mock BT disconnected")
        return True

    async def is_connected(self) -> bool:
        return self._connected


class RealBluetoothInterface(BluetoothInterface):
    """
    Real BLE interface for Nurtura ESP32 firmware.

    Provisioning flow:
      1. scan()                          — find "Nurtura Rack" by name
      2. connect(address)                — BLE connect
      3. get_device_id()                 — read DEVICE_ID_CHAR (MAC address string)
      4. send_credentials(ssid, pass)    — write SSID_CHAR then PASSWORD_CHAR
      5. wait_for_status()               — wait for STATUS_CHAR notify → "connected"/"failed"
      6. send_rack_name(name)            — write RACK_NAME_CHAR (optional, within 30s window)
      7. device reboots automatically    — BLE drops, WiFi+MQTT starts

    Install: pip install bleak
    """

    TARGET_NAME = "Nurtura Rack"

    def __init__(self):
        self.client = None
        self._status_event = asyncio.Event()
        self._status_value = ""

    async def scan(self) -> list[str]:
        try:
            from bleak import BleakScanner
            logger.info(f"Scanning for '{self.TARGET_NAME}'...")
            devices = await BleakScanner.discover(timeout=8.0)
            found = [d.address for d in devices if (
                d.name or "") == self.TARGET_NAME]
            logger.info(f"Found {len(found)} Nurtura device(s)")
            return found
        except ImportError:
            logger.error("bleak not installed. Run: pip install bleak")
            return []

    async def connect(self, device_address: str) -> bool:
        try:
            from bleak import BleakClient
            self.client = BleakClient(device_address)
            await self.client.connect()
            logger.info(f"BLE connected to {device_address}")
            return self.client.is_connected
        except Exception as e:
            logger.error(f"BLE connect failed: {e}")
            return False

    async def get_device_id(self) -> str:
        if not self.client:
            return ""
        try:
            data = await self.client.read_gatt_char(DEVICE_ID_CHAR_UUID)
            device_id = data.decode().replace(":", "").upper()
            logger.info(f"Device ID (MAC): {device_id}")
            return device_id
        except Exception as e:
            logger.error(f"Failed to read device ID: {e}")
            return ""

    async def send_credentials(self, ssid: str, password: str) -> bool:
        if not self.client or not self.client.is_connected:
            return False
        try:
            def _status_handler(sender, data):
                value = data.decode().strip()
                logger.info(f"STATUS_CHAR notify: {value}")
                self._status_value = value
                self._status_event.set()

            await self.client.start_notify(STATUS_CHAR_UUID, _status_handler)

            await self.client.write_gatt_char(SSID_CHAR_UUID, ssid.encode())
            logger.info(f"SSID written: {ssid}")
            await asyncio.sleep(0.2)

            await self.client.write_gatt_char(PASSWORD_CHAR_UUID, password.encode())
            logger.info("Password written — device now connecting to WiFi...")
            return True
        except Exception as e:
            logger.error(f"BLE send credentials failed: {e}")
            return False

    async def send_rack_name(self, rack_name: str) -> bool:
        if not self.client or not self.client.is_connected:
            return False
        try:
            await self.client.write_gatt_char(RACK_NAME_CHAR_UUID, rack_name.encode())
            logger.info(f"Rack name written: {rack_name}")
            return True
        except Exception as e:
            logger.error(f"Failed to write rack name: {e}")
            return False

    async def wait_for_status(self, timeout_sec: float = 30.0) -> str:
        try:
            await asyncio.wait_for(self._status_event.wait(), timeout=timeout_sec)
            return self._status_value
        except asyncio.TimeoutError:
            logger.error(f"STATUS_CHAR notify timeout after {timeout_sec}s")
            return "timeout"

    async def disconnect(self) -> bool:
        if self.client:
            try:
                await self.client.disconnect()
                logger.info("BLE disconnected")
            except Exception:
                pass
        return True

    async def is_connected(self) -> bool:
        return self.client.is_connected if self.client else False
