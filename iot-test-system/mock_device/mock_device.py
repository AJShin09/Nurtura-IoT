import asyncio
import json
import logging
import random
import threading
from datetime import datetime

import paho.mqtt.publish as publish

logger = logging.getLogger("MockDevice")

BROKER_HOST = "broker.hivemq.com"
BROKER_PORT = 1883


class MockIoTDevice:
    """
    Software simulation of Nurtura ESP32 firmware.
    Uses paho-mqtt synchronous publish in a thread to avoid Windows asyncio issues.
    """

    def __init__(self, device_id: str = "AABBCCDDEEFF"):
        self.device_id = device_id
        self.base_topic = f"nurtura/{device_id}"
        self._bt_connected = False
        self._wifi_connected = False
        self._ssid = None
        self._password = None
        self._rack_name = None
        self._ble_status = None

    async def on_bluetooth_connected(self):
        self._bt_connected = True
        logger.info("[MockDevice] Bluetooth connected")

    async def on_credentials_received(self, ssid: str, password: str):
        self._ssid = ssid
        self._password = password
        logger.info(f"[MockDevice] Credentials received: SSID={ssid}")
        asyncio.create_task(self._run_device_workflow())

    async def on_rack_name_received(self, rack_name: str):
        self._rack_name = rack_name
        logger.info(f"[MockDevice] Rack name received: {rack_name}")

    async def get_ble_status(self):
        return self._ble_status

    async def _run_device_workflow(self):
        await self._simulate_wifi_connect()
        if not self._wifi_connected:
            self._ble_status = "failed"
            return
        await self._simulate_mqtt_connect()
        await self._simulate_sensor_publish()

    async def _simulate_wifi_connect(self):
        logger.info("[MockDevice] Attempting WiFi connection...")
        await asyncio.sleep(1.5)
        self._wifi_connected = True
        self._ble_status = "connected"
        logger.info(f"[MockDevice] WiFi connected to {self._ssid}")

    async def _simulate_mqtt_connect(self):
        logger.info("[MockDevice] Connecting to MQTT broker...")
        await asyncio.sleep(1.0)
        logger.info("[MockDevice] MQTT connected")

    async def _simulate_sensor_publish(self):
        logger.info("[MockDevice] Publishing sensor data...")
        for _ in range(3):
            await asyncio.sleep(1.5)
            payload = {
                "t":          round(random.uniform(22.0, 32.0), 1),
                "h":          round(random.uniform(45.0, 75.0), 1),
                "m":          random.randint(30, 70),
                "lux":        round(random.uniform(100.0, 800.0), 1),
                "f":          round(random.uniform(0.0, 5.0), 2),
                "total":      random.randint(0, 500),
                "p":          "OFF",
                "l":          "ON",
                "rssi":       random.randint(-80, -40),
                "err_bme":    False,
                "err_lux":    False,
                "uptime":     random.randint(10, 120),
                "reconnects": 0,
            }
            topic = f"{self.base_topic}/sensor"
            self._publish_sync(topic, payload)
            logger.info(
                f"[MockDevice] Sensor published: t={payload['t']} h={payload['h']} m={payload['m']}")

    def _publish_sync(self, topic: str, payload: dict):
        def _do():
            try:
                publish.single(topic, json.dumps(payload),
                               hostname=BROKER_HOST, port=BROKER_PORT)
            except Exception as e:
                logger.error(f"[MockDevice] Publish failed: {e}")
        threading.Thread(target=_do, daemon=True).start()
