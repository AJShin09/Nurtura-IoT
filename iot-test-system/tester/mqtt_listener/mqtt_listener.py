import asyncio
import json
import logging
import threading
from dataclasses import dataclass, field
from datetime import datetime

import paho.mqtt.client as mqtt
import paho.mqtt.publish as publish

logger = logging.getLogger("MQTTListener")


@dataclass
class MQTTMessage:
    topic: str
    payload: dict | str
    received_at: str = field(
        default_factory=lambda: datetime.now().isoformat())


class MQTTListener:
    """
    Subscribes to Nurtura device topics on HiveMQ using paho-mqtt directly.
    Works on all platforms including Windows with Python 3.12+.

    Topics:
      nurtura/{device_id}/sensor  -- t, h, m, lux, f, total, p, l, rssi, err_bme, err_lux
      nurtura/{device_id}/diag    -- free_heap, min_heap, uptime, rssi
      nurtura/noah/status         -- LWT: {"online": false}
    """

    BROKER_HOST = "broker.hivemq.com"
    BROKER_PORT = 1883

    def __init__(self, device_id: str):
        self.device_id = device_id
        self.base_topic = f"nurtura/{device_id}"
        self.messages: list[MQTTMessage] = []
        self._client = None
        self._thread = None
        self._connected = False
        self._lock = threading.Lock()

    @property
    def topics(self) -> list[str]:
        return [
            f"{self.base_topic}/sensor",
            f"{self.base_topic}/diag",
            "nurtura/noah/status",
        ]

    async def start(self):
        loop = asyncio.get_event_loop()
        ready = asyncio.Event()

        def on_connect(client, userdata, flags, rc, properties=None):
            if rc == 0:
                for topic in self.topics:
                    client.subscribe(topic)
                    logger.info(f"Subscribed to {topic}")
                self._connected = True
                loop.call_soon_threadsafe(ready.set)
            else:
                logger.error(f"MQTT connect failed: rc={rc}")

        def on_message(client, userdata, msg):
            payload_raw = msg.payload.decode()
            try:
                payload = json.loads(payload_raw)
            except json.JSONDecodeError:
                payload = payload_raw
            message = MQTTMessage(topic=msg.topic, payload=payload)
            with self._lock:
                self.messages.append(message)
            logger.info(f"MQTT [{msg.topic}]: {payload}")

        self._client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
        self._client.on_connect = on_connect
        self._client.on_message = on_message
        self._client.connect_async(self.BROKER_HOST, self.BROKER_PORT)
        self._client.loop_start()

        await asyncio.wait_for(ready.wait(), timeout=10.0)
        logger.info(
            f"MQTT listener started -- broker: {self.BROKER_HOST}, device: {self.device_id}")

    def publish_command(self, command: str):
        """Publish a command synchronously. Valid: pump_on, pump_off, reboot, etc."""
        cmd_topic = f"{self.base_topic}/cmd"
        try:
            publish.single(command, hostname=self.BROKER_HOST,
                           port=self.BROKER_PORT, topic=cmd_topic)
            logger.info(f"Command published -> {cmd_topic}: {command}")
        except Exception as e:
            logger.error(f"Failed to publish command: {e}")

    async def stop(self):
        if self._client:
            self._client.loop_stop()
            self._client.disconnect()
        self._connected = False
        logger.info("MQTT listener stopped")

    def get_messages_by_topic(self, topic_suffix: str) -> list[MQTTMessage]:
        full_topic = f"{self.base_topic}/{topic_suffix}"
        with self._lock:
            return [m for m in self.messages if m.topic == full_topic]

    def get_sensor_readings(self) -> list[dict]:
        return [
            m.payload for m in self.get_messages_by_topic("sensor")
            if isinstance(m.payload, dict)
        ]

    def get_diag_readings(self) -> list[dict]:
        return [
            m.payload for m in self.get_messages_by_topic("diag")
            if isinstance(m.payload, dict)
        ]

    def is_online(self) -> bool:
        with self._lock:
            lwt_msgs = [m for m in self.messages if m.topic ==
                        "nurtura/noah/status"]
        if not lwt_msgs:
            return len(self.get_sensor_readings()) > 0
        last = lwt_msgs[-1].payload
        if isinstance(last, dict):
            return last.get("online", True)
        return True

    def clear(self):
        with self._lock:
            self.messages.clear()
