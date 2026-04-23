# Nurtura Rack — Version Comparison Table
> Last column = prototype testing suitability (✅ Recommended / ⚠️ Usable / ❌ Not recommended)

---

## Quick Version Summary

| Version | Base | Score | Prototype Testing |
|---------|------|-------|------------------|
| v0 | Original merge (your file) | ~60/100 | ⚠️ Usable with caution |
| v1 | v0 + BLE added | ~65/100 | ⚠️ Usable with caution |
| v2 | v1 + thread safety | ~75/100 | ⚠️ Usable |
| v3 | v2 + NVS fix + totalML fix | ~85/100 | ✅ Good for early prototype |
| v4 | v0 base + 11 fixes (our generated v4) | ~87/100 | ✅ Good for prototype |
| v5 | v4 + hardware WDT + sensor validation | ~93/100 | ✅ Recommended |
| v6 | v5 + no String + hysteresis + stale detect | ~97/100 | ✅ Strongly recommended |
| v7 | v6 + task monitor + offline queue + ADC cal | ~98/100 | ✅ Strongly recommended |
| v8 | v7 + OTA + safe mode + fleet + static_assert | ~99/100 | ✅ Best — if you need all features |

---

## Full Feature Difference Table

| Feature | v0 | v1 | v2 | v3 | v4 | v5 | v6 | v7 | v8 | Prototype Rank |
|---------|----|----|----|----|----|----|----|----|----|----|
| **BLE Provisioning** | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Must-have |
| **BLE Disconnect Re-advertise** | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Important |
| **BLE Auto-timeout** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Nice-to-have |
| **BLE Static Allocation** | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ | Nice-to-have |
| **WiFi NVS Credentials** | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Must-have |
| **preferences.end() fix** | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Must-have |
| **Deferred NVS Write** | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Important |
| **Factory Reset Button** | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Important |
| **Dual-Core FreeRTOS** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Must-have |
| **dataMutex (timed)** | ⚠️ | ⚠️ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Must-have |
| **credMutex for BLE strings** | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Must-have |
| **Mutex Contention Logging** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | Nice-to-have |
| **Task Creation Check** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | Nice-to-have |
| **Hardware Watchdog (WDT)** | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ | Important |
| **Software Heartbeat/Monitor** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Nice-to-have |
| **Task Auto-Restart** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Nice-to-have |
| **Restart Counter Guard** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | Nice-to-have |
| **MQTT Exponential Backoff** | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Important |
| **MQTT Reconnect Jitter** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | Nice-to-have |
| **MQTT Last Will (LWT)** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Nice-to-have |
| **MQTT QoS 1** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Nice-to-have |
| **MQTT Offline Queue** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Nice-to-have |
| **Per-Device MQTT Topics** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Nice-to-have |
| **Publish-on-Change** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Nice-to-have |
| **WiFi Auto-Reconnect** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Important |
| **String → char[] (no heap)** | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ | Important |
| **totalML Float Accumulator** | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Must-have |
| **Relay Hysteresis** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | Important |
| **Relay Lockout Timers** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | Important |
| **Light Relay Logic** | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Must-have |
| **Pump Dry-Run Protection** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | Must-have |
| **Pump Remote Reset** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | Important |
| **Sensor NaN Validation** | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ | Important |
| **Sensor Stale Detection** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | Important |
| **Sensor Auto Re-init** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Nice-to-have |
| **ADC eFuse Calibration** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Nice-to-have |
| **ADC Median Filter** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Important |
| **Flow Sensor Debounce** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Important |
| **Flow Spike Rejection** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Important |
| **NTP Timestamp in Payload** | ✅ | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ | ✅ | Nice-to-have |
| **Boot Reason Reporting** | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ | Important |
| **Reset Counter (NVS)** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Nice-to-have |
| **Fatal Error Codes (NVS)** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | Nice-to-have |
| **Heap Monitoring** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Nice-to-have |
| **Heap Fragmentation Check** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | Nice-to-have |
| **Stack High-Water Mark** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Nice-to-have |
| **RSSI in Payload** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Nice-to-have |
| **Uptime in Payload** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | Nice-to-have |
| **Firmware Version Publish** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Nice-to-have |
| **Sensor Fault Flags in Payload** | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ | Important |
| **OTA Firmware Update** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Nice-to-have |
| **OTA Auth Token** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | Nice-to-have |
| **OTA Auto-Disable** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | Nice-to-have |
| **Remote Config via MQTT** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Nice-to-have |
| **Remote Reboot Command** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Nice-to-have |
| **Safe Mode** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | Nice-to-have |
| **Centralized State Enums** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | Nice-to-have |
| **DeviceConfig Struct** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Nice-to-have |
| **Named Calibration Constants** | ✅ | ⚠️ | ⚠️ | ⚠️ | ✅ | ✅ | ✅ | ✅ | ✅ | Must-have |
| **Compile-Time Feature Flags** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | Nice-to-have |
| **static_assert Buffer Guards** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | Nice-to-have |
| **Startup Sensor Health Check** | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ | Important |
| **Device ID from MAC** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Nice-to-have |
| **Diagnostics MQTT Topic** | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ | ✅ | Nice-to-have |

---

## Prototype Testing Recommendation

### Legend
| Symbol | Meaning |
|--------|---------|
| 🔴 Must-have | Critical — prototype will fail or give wrong data without this |
| 🟡 Important | Strongly recommended — prototype works but is fragile without this |
| 🟢 Nice-to-have | Useful for production but safe to skip during early testing |

---

### Per-Version Checklist

#### v0 — Original Merge
- 🔴 ❌ No BLE provisioning — credentials hardcoded, not testable in the field
- 🔴 ✅ Dry-run protection present
- 🔴 ❌ Light relay defined but never used
- 🔴 ❌ totalML truncation bug — water volume data wrong
- 🟡 ❌ No factory reset
- 🟡 ❌ No NVS credential storage
- **Verdict:** ⚠️ Only usable on a bench with hardcoded WiFi. Not suitable for field prototype testing.

---

#### v1 — BLE Added
- 🔴 ✅ BLE provisioning added
- 🔴 ✅ Light relay logic working
- 🔴 ❌ preferences.end() missing — NVS corruption risk
- 🔴 ❌ totalML truncation bug still present
- 🔴 ❌ Race condition on BLE credential strings
- 🟡 ❌ No BLE disconnect handler
- **Verdict:** ⚠️ BLE works for initial setup but NVS bug can cause silent data corruption. Risky for prototype.

---

#### v2 — Thread Safety
- 🔴 ✅ Timed mutex (no more portMAX_DELAY deadlock)
- 🔴 ❌ preferences.end() still missing
- 🔴 ❌ totalML truncation bug still present
- 🟡 ✅ MQTT exponential backoff added
- 🟡 ❌ No credMutex on BLE strings
- **Verdict:** ⚠️ More stable than v1 but NVS and data bugs still present. Usable for very short bench tests.

---

#### v3 — NVS + Data Fixes
- 🔴 ✅ preferences.end() fixed
- 🔴 ✅ totalML float accumulator fixed
- 🔴 ✅ credMutex protecting BLE strings
- 🔴 ✅ Factory reset button working
- 🔴 ✅ BLE disconnect re-advertise
- 🟡 ❌ No hardware watchdog
- 🟡 ❌ No sensor validation (NaN values published raw)
- 🟢 ❌ No hysteresis (relay may chatter near threshold)
- **Verdict:** ✅ First version genuinely safe for prototype testing. All critical bugs fixed. Good starting point.

---

#### v4 — Our Generated Version (v0 base + 11 fixes)
- 🔴 ✅ All v3 critical fixes included
- 🔴 ✅ Named calibration constants (AirValue, WaterValue, etc.)
- 🔴 ✅ NTP timestamp in payload
- 🔴 ✅ char[] instead of String (no heap risk)
- 🟡 ❌ No hardware watchdog
- 🟡 ❌ No sensor NaN validation
- 🟢 ❌ No hysteresis
- **Verdict:** ✅ Slightly better than v3 for prototype. Best choice if you want clean calibration constants and timestamps. Recommended starting point for your hardware.

---

#### v5 — Watchdog + Sensor Validation
- 🔴 ✅ Hardware watchdog — device recovers from hangs
- 🔴 ✅ Sensor NaN validation — no garbage data published
- 🔴 ✅ Startup health check — immediate feedback if sensor wiring is wrong
- 🔴 ✅ Boot reason reporting — know why the device rebooted
- 🟡 ✅ Sensor fault flags in payload
- 🟢 ❌ No hysteresis
- 🟢 ❌ No stale detection
- **Verdict:** ✅ Recommended for prototype. Watchdog alone makes it significantly more reliable for unattended testing.

---

#### v6 — Hysteresis + Stale Detection
- 🔴 ✅ Relay hysteresis — no more rapid ON/OFF near threshold
- 🔴 ✅ Relay lockout timers — protects relay hardware during testing
- 🔴 ✅ Sensor stale detection — catches frozen sensors
- 🟡 ✅ Pump remote reset via MQTT
- 🟡 ✅ Centralized state enums
- 🟢 ❌ No task auto-restart
- 🟢 ❌ No ADC calibration
- **Verdict:** ✅ Strongly recommended for prototype. Relay protection alone saves hardware during extended tests.

---

#### v7 — Task Monitor + Fleet Features
- 🔴 ✅ Task auto-restart — self-healing firmware
- 🔴 ✅ WiFi auto-reconnect — survives network drops during testing
- 🟡 ✅ ADC median filter — more accurate soil readings
- 🟡 ✅ Flow sensor debounce — eliminates false readings
- 🟡 ✅ Heap monitoring
- 🟢 ✅ OTA updates
- 🟢 ✅ Per-device MQTT topics
- 🟢 ✅ MQTT offline queue
- **Verdict:** ✅ Strongly recommended if testing for more than a few hours unattended. The self-healing task restart and WiFi reconnect are game-changers for long prototype runs.

---

#### v8 — Full Production
- 🔴 ✅ Everything from v7
- 🟢 ✅ OTA auth token
- 🟢 ✅ Safe mode
- 🟢 ✅ Fleet management
- 🟢 ✅ static_assert guards
- 🟢 ✅ Fatal error codes in NVS
- 🟢 ✅ Compile-time feature flags
- **Verdict:** ✅ Best firmware but overkill for early prototype. Use this when moving from prototype to pre-production deployment.

---

## Final Prototype Recommendation Summary

| Version | Prototype Stage | Best For |
|---------|----------------|----------|
| v0 | ❌ Avoid | Nothing — too many critical bugs |
| v1 | ❌ Avoid | Nothing — NVS corruption risk |
| v2 | ⚠️ Bench only | Quick 1-hour bench tests only |
| v3 | ✅ Early prototype | First hardware bring-up |
| **v4** | ✅ **Best for initial prototype** | **Your hardware with clean calibration constants** |
| v5 | ✅ Prototype | Unattended testing up to a few hours |
| **v6** | ✅ **Best for extended prototype** | **Multi-day testing with real plants** |
| v7 | ✅ Pre-production | Week-long unattended runs |
| v8 | ✅ Production | Final deployment |

> **Short answer:** Use **v4** to bring up and calibrate your hardware. Switch to **v6** once sensors are calibrated and you want to run it for more than a day. Move to **v8** when you're ready to deploy for real.