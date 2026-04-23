import platform
import asyncio
import logging
import sys
from datetime import datetime
from pathlib import Path

# Windows Python 3.12+ defaults to ProactorEventLoop which does not support
# add_reader/add_writer required by paho-mqtt. Force SelectorEventLoop.
if platform.system() == "Windows":
    asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())

from tester.bluetooth_interface.bluetooth_interface import MockBluetoothInterface, RealBluetoothInterface
from tester.mqtt_listener.mqtt_listener import MQTTListener
from tester.validator.validator import Validator
from mock_device.mock_device import MockIoTDevice

LOGS_DIR = Path(__file__).parent.parent.parent / "logs"
REPORTS_DIR = Path(__file__).parent.parent.parent / "reports"

LOGS_DIR.mkdir(exist_ok=True)
REPORTS_DIR.mkdir(exist_ok=True)


def setup_logging(mode: str) -> logging.Logger:
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    log_file = LOGS_DIR / f"test_{mode}_{timestamp}.log"
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s  %(levelname)-8s  %(name)s  %(message)s",
        handlers=[
            logging.StreamHandler(),
            logging.FileHandler(log_file),
        ]
    )
    return logging.getLogger("TestController")


def save_html_report(validator: Validator, mode: str, device_id: str, duration_sec: float) -> Path:
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    report_file = REPORTS_DIR / f"report_{mode}_{timestamp}.html"

    rows = ""
    for r in validator.results:
        color = {"PASS": "#22c55e", "FAIL": "#ef4444",
                 "SKIP": "#f59e0b"}.get(r.status, "#888")
        icon = {"PASS": "✅", "FAIL": "❌", "SKIP": "⚠️"}.get(r.status, "?")
        rows += f"""
        <tr>
            <td>{r.name}</td>
            <td style="color:{color}; font-weight:bold">{icon} {r.status}</td>
            <td>{r.message}</td>
            <td style="font-size:0.8em;color:#888">{r.timestamp}</td>
        </tr>"""

    overall = validator.overall_passed()
    overall_color = "#22c55e" if overall else "#ef4444"
    overall_text = "✅ SUCCESS" if overall else "❌ FAILED"

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>Nurtura Test Report — {mode.upper()}</title>
    <style>
        body {{ font-family: 'Segoe UI', sans-serif; background: #0f172a; color: #e2e8f0; margin: 0; padding: 2rem; }}
        h1 {{ color: #38bdf8; font-size: 1.8rem; margin-bottom: 0.2rem; }}
        .meta {{ color: #64748b; font-size: 0.9rem; margin-bottom: 2rem; }}
        table {{ width: 100%; border-collapse: collapse; background: #1e293b; border-radius: 8px; overflow: hidden; }}
        th {{ background: #0f172a; padding: 0.8rem 1rem; text-align: left; color: #94a3b8; font-size: 0.85rem; text-transform: uppercase; letter-spacing: 0.05em; }}
        td {{ padding: 0.75rem 1rem; border-bottom: 1px solid #334155; }}
        tr:last-child td {{ border-bottom: none; }}
        .overall {{ margin-top: 1.5rem; padding: 1rem 1.5rem; background: #1e293b; border-radius: 8px; }}
        .overall-label {{ font-size: 0.85rem; color: #64748b; text-transform: uppercase; letter-spacing: 0.05em; }}
        .overall-value {{ font-size: 1.4rem; font-weight: bold; color: {overall_color}; }}
        .badge {{ display: inline-block; padding: 0.2rem 0.6rem; border-radius: 9999px; font-size: 0.75rem; font-weight: bold; background: #334155; color: #94a3b8; }}
    </style>
</head>
<body>
    <h1>🌿 Nurtura Rack Test Report</h1>
    <div class="meta">
        Mode: <span class="badge">{mode.upper()}</span> &nbsp;
        Device: <span class="badge">{device_id}</span> &nbsp;
        Duration: <span class="badge">{duration_sec:.1f}s</span> &nbsp;
        Generated: <span class="badge">{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}</span>
    </div>
    <table>
        <thead>
            <tr><th>Test</th><th>Result</th><th>Details</th><th>Timestamp</th></tr>
        </thead>
        <tbody>{rows}
        </tbody>
    </table>
    <div class="overall">
        <div class="overall-label">Overall Result</div>
        <div class="overall-value">{overall_text}</div>
    </div>
</body>
</html>"""

    report_file.write_text(html, encoding="utf-8")
    return report_file


async def run_mock_test(ssid: str = "TestNetwork", password: str = "TestPass123", rack_name: str = "Test Rack"):
    logger = setup_logging("mock")
    logger.info("Initializing MOCK test environment")

    mock_device = MockIoTDevice(device_id="AABBCCDDEEFF")
    bluetooth = MockBluetoothInterface(mock_device)
    validator = Validator()

    start_time = datetime.now()

    # Stage 1: BLE scan + connect
    devices = await bluetooth.scan()
    bt_connected = bool(devices) and await bluetooth.connect(devices[0])
    validator.check_bluetooth_connected(bt_connected)
    if not bt_connected:
        logger.error("Halting: BLE connection failed")
        return False

    # Stage 2: Read device ID (MAC) from BLE characteristic
    device_id = await bluetooth.get_device_id()
    validator.check_device_id_read(device_id)

    # Stage 3: Start MQTT listener using real device ID
    mqtt_listener = MQTTListener(device_id=device_id)
    await mqtt_listener.start()
    await asyncio.sleep(1)

    # Stage 4: Send WiFi credentials
    creds_sent = await bluetooth.send_credentials(ssid, password)
    validator.check_credentials_sent(creds_sent)

    # Stage 5: Wait for BLE status notification ("connected" / "failed")
    ble_status = await bluetooth.wait_for_status(timeout_sec=20.0)
    validator.check_ble_status_notification(ble_status)

    if ble_status == "connected":
        # Stage 6: Send rack name within 30s window (firmware waits for this)
        await bluetooth.send_rack_name(rack_name)

    # Stage 7: Wait for MQTT sensor data (device reboots and reconnects)
    logger.info("Waiting for device to reboot and publish sensor data...")
    await asyncio.sleep(10)

    # Stage 8: Validate
    validator.check_sensor_data_published(mqtt_listener)
    validator.check_sensor_sanity(mqtt_listener)
    validator.check_sensor_faults(mqtt_listener)
    validator.check_pump_state_valid(mqtt_listener)
    validator.check_device_online(mqtt_listener)

    await mqtt_listener.stop()

    duration = (datetime.now() - start_time).total_seconds()
    print(validator.summary())

    report_path = save_html_report(validator, "mock", device_id, duration)
    logger.info(f"HTML report: {report_path}")

    return validator.overall_passed()


async def run_real_device_test(ssid: str, password: str, rack_name: str = "Nurtura Rack 1"):
    logger = setup_logging("real")
    logger.info("Initializing REAL DEVICE test environment")

    bluetooth = RealBluetoothInterface()
    validator = Validator()

    start_time = datetime.now()

    # Stage 1: BLE scan + connect
    devices = await bluetooth.scan()
    if not devices:
        logger.error("No Nurtura Rack found via BLE scan")
        validator.check_bluetooth_connected(False)
        print(validator.summary())
        return False

    bt_connected = await bluetooth.connect(devices[0])
    validator.check_bluetooth_connected(bt_connected)
    if not bt_connected:
        logger.error("Halting: BLE connection failed")
        print(validator.summary())
        return False

    # Stage 2: Read device ID (MAC) — needed for MQTT topic construction
    device_id = await bluetooth.get_device_id()
    validator.check_device_id_read(device_id)
    if not device_id:
        logger.error("Halting: could not read device ID")
        print(validator.summary())
        return False

    # Stage 3: Start MQTT listener with correct device ID BEFORE credentials are sent
    mqtt_listener = MQTTListener(device_id=device_id)
    await mqtt_listener.start()
    await asyncio.sleep(1)
    logger.info(f"MQTT listening on nurtura/{device_id}/sensor")

    # Stage 4: Send credentials — also subscribes to STATUS_CHAR notifications
    creds_sent = await bluetooth.send_credentials(ssid, password)
    validator.check_credentials_sent(creds_sent)

    # Stage 5: Wait for BLE status notification from firmware
    logger.info("Waiting for device WiFi result via BLE notification...")
    ble_status = await bluetooth.wait_for_status(timeout_sec=30.0)
    validator.check_ble_status_notification(ble_status)

    if ble_status == "connected":
        # Stage 6: Send rack name — firmware waits up to 30s for this before rebooting
        await bluetooth.send_rack_name(rack_name)
        logger.info("Rack name sent — device will reboot and start MQTT")
    else:
        logger.error(
            f"WiFi provisioning failed ({ble_status}) — cannot proceed to MQTT checks")
        await bluetooth.disconnect()
        await mqtt_listener.stop()
        duration = (datetime.now() - start_time).total_seconds()
        print(validator.summary())
        save_html_report(validator, "real", device_id, duration)
        return False

    await bluetooth.disconnect()

    # Stage 7: Wait for device reboot + MQTT connection + sensor publish
    logger.info("Waiting for device to reboot and publish sensor data (~15s)...")
    await asyncio.sleep(15)

    # Stage 8: Validate MQTT data
    validator.check_sensor_data_published(mqtt_listener)
    validator.check_sensor_sanity(mqtt_listener)
    validator.check_sensor_faults(mqtt_listener)
    validator.check_pump_state_valid(mqtt_listener)
    validator.check_device_online(mqtt_listener)

    await mqtt_listener.stop()

    duration = (datetime.now() - start_time).total_seconds()
    print(validator.summary())

    report_path = save_html_report(validator, "real", device_id, duration)
    logger.info(f"HTML report: {report_path}")

    return validator.overall_passed()


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "mock"

    if mode == "mock":
        result = asyncio.run(run_mock_test())
    elif mode == "real":
        ssid = sys.argv[2] if len(sys.argv) > 2 else input("WiFi SSID: ")
        password = sys.argv[3] if len(
            sys.argv) > 3 else input("WiFi Password: ")
        rack_name = sys.argv[4] if len(sys.argv) > 4 else input(
            "Rack name (default: Nurtura Rack 1): ") or "Nurtura Rack 1"
        result = asyncio.run(run_real_device_test(ssid, password, rack_name))
    else:
        print(
            "Usage: python controller.py [mock|real] [ssid] [password] [rack_name]")
        sys.exit(1)

    sys.exit(0 if result else 1)
