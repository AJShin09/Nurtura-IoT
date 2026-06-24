/**
 * ============================================================
 *  Nurtura Rack Firmware v2.2.3
 *  Target: ESP32 (Arduino + ESP-IDF hybrid)
 * ============================================================
 *  CRITICAL FIX FROM v2.2.2:
 *  - MUST use FORCE_BLE_BOOT = 1 during testing to ignore secrets.h
 *  - Set FEATURE_BLE = 1 (already default)
 *  - doFactoryReset() calls nvs_flash_erase() BEFORE checking credentials
 *  - After factory reset, device MUST boot into BLE mode, not WiFi
 *  - Removed Preferences::begin("wifi-config") calls BEFORE factory reset
 *
 *  FACTORY RESET / FORCE BLE:
 *  → At boot:    Hold IO0 for 3s (with clear countdown in Serial)
 *  → At runtime: Hold IO0 for 3s (TaskBootButton catches it)
 *  → Via MQTT:   Publish command payloads to nurtura/rack/<mac>/commands/*
 *  → Via flag:   #define FORCE_BLE_BOOT 1 (during development)
 *
 *  All paths: nvs_flash_erase() → reboot → ZERO credentials → BLE provisioning
 *
 *  BOOT FLOW:
 *  ┌─ NVS has credentials? ──────────────────────────────────┐
 *  │  YES → WiFi.begin() non-blocking                        │
 *  │         └─ onWiFiEvent(GOT_IP)                          │
 *  │               └─ Wire/BME/BH1750 → MQTT → Tasks         │
 *  │  NO  → BLE advertising (first boot / factory reset)     │
 *  │         └─ App sends SSID+Password                      │
 *  │               └─ WiFi.begin() non-blocking              │
 *  │                     └─ onWiFiEvent(GOT_IP)              │
 *  │                           └─ Save creds → reboot        │
 *  │                                 └─ Takes YES path above │
 *  └─────────────────────────────────────────────────────────┘
 *
 * ============================================================
 *  PARTITION SCHEME — REQUIRED:
 *  Arduino IDE → Tools → Partition Scheme → "No FS (2MB APP x2)"
 *  Use "Minimal SPIFFS (1.9MB APP with OTA)" if FEATURE_OTA = 1.
 * ============================================================
 *  BUTTON PINOUT:
 *  [EN]  button = hardware reset only, does NOT trigger factory reset
 *  [IO0] button = BOOT_BUTTON_PIN — hold 3s for factory reset
 * ============================================================
 */

#include <WiFi.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <nvs_flash.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <BH1750.h>

/* ===================== COMPILE-TIME FEATURE FLAGS ===================== */
#define DEBUG 0
#if DEBUG
#define DBG_PRINT(x) Serial.print(x)
#define DBG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define DBG_PRINT(x) (void)0
#define DBG_PRINTF(...) (void)0
#endif

#define FEATURE_BLE 1
#define FEATURE_OTA 1
#define FEATURE_DIAGNOSTICS 1
#define FEATURE_LWT 1
#define FEATURE_MQTT_QUEUE 1
#define FEATURE_RSSI 1
#define MQTT_USE_TLS 0

// CRITICAL: Set to 1 to force BLE mode every boot (IGNORE secrets.h)
// Use during development/testing to bypass WiFi credentials
#define FORCE_BLE_BOOT 0

#if FEATURE_OTA
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoOTA.h>
#endif

#if FEATURE_DIAGNOSTICS
#include <esp_heap_caps.h>
#endif

#if FEATURE_BLE
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#endif

#include "ESP32MQTTClient.h"
#include "cert.h"
#include "secrets.h"

/* ===================== FIRMWARE IDENTITY ===================== */
#define FW_VERSION "2.2.3"
#define FW_BUILD_DATE __DATE__ " " __TIME__
#define CONFIG_SCHEMA_VER 2

/* ===================== BUFFER SIZE CONSTANTS ===================== */
#define BUF_SSID 64
#define BUF_PASS 64
#define BUF_NAME 64
#define BUF_DEVID 32
#define BUF_REASON 24
#define BUF_MSG 128
#define BUF_PAYLOAD 512

/* ===================== COMPILE-TIME SAFETY ASSERTIONS ===================== */
static_assert(BUF_SSID >= 64, "SSID buffer too small");
static_assert(BUF_PASS >= 64, "Password buffer too small");
static_assert(BUF_REASON >= 24, "bootReason buffer too small");
static_assert(BUF_PAYLOAD >= 400, "Payload buffer must fit sensor JSON");
static_assert(BUF_MSG >= 64, "Message buffer too small");

/* ===================== CONFIG & UUIDS ===================== */
#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define DEVICE_ID_CHAR_UUID "abc12345-1234-5678-1234-56789abcdef0"
#define SSID_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define PASSWORD_CHAR_UUID "1c95d5e3-d8f7-413a-bf3d-7a2e5d7be87e"
#define STATUS_CHAR_UUID "9a8ca5e3-d8f7-413a-bf3d-7a2e5d7be123"
#define RACK_NAME_CHAR_UUID "a1b2c3d4-e5f6-7890-abcd-ef1234567890"

/* ===================== PIN DEFINITIONS ===================== */
#define SOIL_PIN 35
#define FLOW_PIN 13
#define PUMP_RELAY 25
#define LIGHT_RELAY 26
#define BOOT_BUTTON_PIN 0
#define STATUS_LED_PIN 2
#define LED_BLE_PIN 18
#define LED_WIFI_PIN 19

/* ===================== TIMING CONSTANTS ===================== */
#define WDT_TIMEOUT 30
#define BLE_TIMEOUT_MS 600000UL
#define OTA_AUTH_TOKEN SECRET_OTA_TOKEN
#define OTA_DISABLE_MS 600000UL
#define MAX_TASK_RESTARTS 5
#define WIFI_RECONNECT_MAX 10
#define BTN_HOLD_MS 3000

/* ===================== STRUCTURED CONFIG ===================== */
struct DeviceConfig
{
    uint8_t schema_version = CONFIG_SCHEMA_VER;
    uint8_t moisture_min = 30;
    uint8_t moisture_max = 70;
    uint32_t pump_on_ms = 5000;
    uint32_t pump_rest_ms = 30000;
    float light_on_lux = 50.0f;
    float light_off_lux = 100.0f;
    float lux_threshold = 50.0f;
    float lux_hysteresis = 5.0f;
    int moisture_threshold = 25;
    int moisture_hysteresis = 3;
    float calib_factor = 7.5f;
    uint32_t dry_run_ms = 5000;
    uint32_t stale_data_ms = 120000;
    uint32_t publish_interval = 10000;
    float flow_max_sane = 30.0f;
    uint32_t relay_min_on_ms = 2000;
    uint32_t relay_min_off_ms = 2000;
    uint32_t heartbeat_timeout = 45000;
    uint32_t diag_interval = 60000;
} cfg;

/* ===================== STATE ENUMS ===================== */
enum PumpState : uint8_t
{
    PUMP_OFF,
    PUMP_ON,
    PUMP_ERR_DRY
};
enum LightState : uint8_t
{
    LIGHT_OFF,
    LIGHT_ON
};
enum WiFiSMState : uint8_t
{
    WS_IDLE,
    WS_CONNECTING,
    WS_CONNECTED,
    WS_FAILED
};

PumpState pumpState = PUMP_OFF;
LightState lightState = LIGHT_OFF;
WiFiSMState wifiSMState = WS_IDLE;

/* ===================== SYSTEM DATA ===================== */
struct SystemData
{
    float temp = 0.0f, hum = 0.0f;
    float lux = 0.0f, flowRate = 0.0f;
    float totalMLf = 0.0f;
    uint32_t moisture = 0;
    bool pumpErr = false;
    float lastPubTemp = -999.0f;
    float lastPubHum = -999.0f;
    float lastPubLux = -999.0f;
    uint32_t lastPubMoisture = 999;
    int8_t rssi = 0;
    uint32_t wifiReconnects = 0;
} sys;

/* ===================== OFFLINE MESSAGE QUEUE ===================== */
#if FEATURE_MQTT_QUEUE
#define QUEUE_SIZE 16
#define QUEUE_MSG_LEN BUF_PAYLOAD
struct QueuedMsg
{
    char topic[64];
    char payload[QUEUE_MSG_LEN];
};
static QueuedMsg msgQueue[QUEUE_SIZE];
static uint8_t qHead = 0, qTail = 0, qCount = 0;
SemaphoreHandle_t queueMutex;

bool queuePush(const char *topic, const char *payload)
{
    if (qCount >= QUEUE_SIZE)
        return false;
    strncpy(msgQueue[qTail].topic, topic, 63);
    strncpy(msgQueue[qTail].payload, payload, QUEUE_MSG_LEN - 1);
    qTail = (qTail + 1) % QUEUE_SIZE;
    qCount++;
    return true;
}

bool queuePop(char *topic, char *payload)
{
    if (qCount == 0)
        return false;
    strncpy(topic, msgQueue[qHead].topic, 63);
    strncpy(payload, msgQueue[qHead].payload, QUEUE_MSG_LEN - 1);
    qHead = (qHead + 1) % QUEUE_SIZE;
    qCount--;
    return true;
}
#endif

/* ===================== GLOBALS ===================== */
Adafruit_BME280 bme;
BH1750 lightMeter;
ESP32MQTTClient mqttClient;
Preferences preferences;

#if FEATURE_BLE
BLECharacteristic *statusCharacteristic = NULL;
#endif

SemaphoreHandle_t dataMutex;
SemaphoreHandle_t credMutex;

TaskHandle_t hTaskSensors = NULL;
TaskHandle_t hTaskMQTT = NULL;

volatile unsigned long sensorHeartbeat = 0;
volatile unsigned long mqttHeartbeat = 0;
uint8_t sensorRestarts = 0;
uint8_t mqttRestarts = 0;

volatile bool credentialsReceived = false;
volatile bool isProvisioning = false;
volatile bool bleActive = false;
unsigned long bleStartTime = 0;

char receivedSSID[BUF_SSID] = "";
char receivedPassword[BUF_PASS] = "";
volatile bool pendingRackNameSave = false;
volatile bool pendingDeviceIdSave = false;
char pendingRackName[BUF_NAME] = "";
char pendingDeviceId[BUF_DEVID] = "";

char bootReason[BUF_REASON] = "UNKNOWN";
char deviceId[BUF_DEVID] = "";
uint32_t resetCount = 0;
uint32_t lastFatalCode = 0;
unsigned long uptimeStart = 0;
bool otaEnabled = true;

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
esp_err_t handleMQTT(esp_mqtt_event_handle_t event)
{
    mqttClient.onEventCallback(event);
    return ESP_OK;
}
#else
void handleMQTT(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;
    (void)event_id;
    ESP32MQTTClient *client = static_cast<ESP32MQTTClient *>(handler_args);
    if (client)
    {
        client->onEventCallback((esp_mqtt_event_handle_t)event_data);
    }
}
#endif

volatile bool bme280Fault = false;
volatile bool bh1750Fault = false;

unsigned long lastTempChange = 0, lastHumChange = 0, lastLuxChange = 0;
float lastRawTemp = -999.0f, lastRawHum = -999.0f, lastRawLux = -999.0f;

unsigned long pumpLastOn = 0, pumpLastOff = 0;
unsigned long lightLastOn = 0, lightLastOff = 0;
bool pumpOn = false;
bool lightOn = false;

#define ADC_SAMPLES 8
int soilBuffer[ADC_SAMPLES] = {0};
int soilBufIdx = 0;
bool adcCalibrated = true;

static bool sPostWiFiDone = false;
static unsigned long rackNameWaitStart = 0;
static bool waitingForRackName = false;

/* ===================== LED STATE MACHINE ===================== */
enum LedMode : uint8_t
{
    LED_OFF,
    LED_BLE_PENDING,
    LED_BLE_CONNECTED,
    LED_WIFI_PENDING,
    LED_WIFI_CONNECTED,
    LED_WIFI_FAILED,
    LED_MQTT_PENDING,
    LED_MQTT_CONNECTED,
    LED_RUNNING
};

volatile LedMode ledMode = LED_OFF;

void TaskLED(void *pvParameters)
{
    pinMode(LED_BLE_PIN, OUTPUT);
    digitalWrite(LED_BLE_PIN, LOW);
    pinMode(LED_WIFI_PIN, OUTPUT);
    digitalWrite(LED_WIFI_PIN, LOW);

    for (;;)
    {
        switch (ledMode)
        {
        case LED_BLE_PENDING:
            digitalWrite(LED_BLE_PIN, HIGH);
            vTaskDelay(pdMS_TO_TICKS(500));
            digitalWrite(LED_BLE_PIN, LOW);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;

        case LED_BLE_CONNECTED:
        {
            unsigned long start = millis();
            while (millis() - start < 2000)
            {
                digitalWrite(LED_BLE_PIN, HIGH);
                vTaskDelay(pdMS_TO_TICKS(100));
                digitalWrite(LED_BLE_PIN, LOW);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            digitalWrite(LED_BLE_PIN, HIGH);
            vTaskDelay(pdMS_TO_TICKS(1000));
            digitalWrite(LED_BLE_PIN, LOW);
            ledMode = LED_RUNNING;
            break;
        }

        case LED_WIFI_PENDING:
            digitalWrite(LED_WIFI_PIN, HIGH);
            vTaskDelay(pdMS_TO_TICKS(500));
            digitalWrite(LED_WIFI_PIN, LOW);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;

        case LED_WIFI_FAILED:
            for (int i = 0; i < 3; i++)
            {
                digitalWrite(LED_BLE_PIN, HIGH);
                digitalWrite(LED_WIFI_PIN, HIGH);
                vTaskDelay(pdMS_TO_TICKS(100));
                digitalWrite(LED_BLE_PIN, LOW);
                digitalWrite(LED_WIFI_PIN, LOW);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            ledMode = LED_BLE_PENDING;
            break;

        case LED_WIFI_CONNECTED:
        {
            unsigned long start = millis();
            while (millis() - start < 2000)
            {
                digitalWrite(LED_WIFI_PIN, HIGH);
                vTaskDelay(pdMS_TO_TICKS(100));
                digitalWrite(LED_WIFI_PIN, LOW);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            digitalWrite(LED_WIFI_PIN, HIGH);
            vTaskDelay(pdMS_TO_TICKS(1000));
            digitalWrite(LED_WIFI_PIN, LOW);
            ledMode = LED_MQTT_PENDING;
            break;
        }

        case LED_MQTT_PENDING:
            digitalWrite(LED_WIFI_PIN, LOW);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;

        case LED_MQTT_CONNECTED:
            for (int i = 0; i < 2; i++)
            {
                digitalWrite(LED_BLE_PIN, HIGH);
                digitalWrite(LED_WIFI_PIN, HIGH);
                vTaskDelay(pdMS_TO_TICKS(800));
                digitalWrite(LED_BLE_PIN, LOW);
                digitalWrite(LED_WIFI_PIN, LOW);
                vTaskDelay(pdMS_TO_TICKS(400));
            }
            ledMode = LED_RUNNING;
            break;

        case LED_RUNNING:
        case LED_OFF:
        default:
            digitalWrite(LED_BLE_PIN, LOW);
            digitalWrite(LED_WIFI_PIN, LOW);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }
    }
}

/* ===================== FLOW INTERRUPT ===================== */
portMUX_TYPE flowMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t pulseCount = 0;
volatile unsigned long lastPulseTime = 0;

void IRAM_ATTR pulseCounter()
{
    unsigned long now = millis();
    if (now - lastPulseTime > 5)
    {
        portENTER_CRITICAL_ISR(&flowMux);
        pulseCount++;
        lastPulseTime = now;
        portEXIT_CRITICAL_ISR(&flowMux);
    }
}

/* ===================== ADC ===================== */
void initADC()
{
    analogSetAttenuation(ADC_11db);
    Serial.println("[ADC] Initialized — Core 3.x adc_oneshot, 11dB attenuation");
}

int readSoilRaw() { return analogRead(SOIL_PIN); }

int readSoilSmoothed()
{
    soilBuffer[soilBufIdx] = readSoilRaw();
    soilBufIdx = (soilBufIdx + 1) % ADC_SAMPLES;
    int sorted[ADC_SAMPLES];
    memcpy(sorted, soilBuffer, sizeof(soilBuffer));
    for (int i = 1; i < ADC_SAMPLES; i++)
    {
        int key = sorted[i], j = i - 1;
        while (j >= 0 && sorted[j] > key)
        {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }
    return (sorted[ADC_SAMPLES / 2 - 1] + sorted[ADC_SAMPLES / 2]) / 2;
}

/* ===================== RELAY CONTROL WITH LOCKOUT ===================== */
void setPumpState(PumpState newState, unsigned long now)
{
    if (newState == pumpState)
        return;
    if (newState == PUMP_ON)
    {
        if ((now - pumpLastOff) < cfg.relay_min_off_ms)
            return;
        digitalWrite(PUMP_RELAY, LOW);
        pumpLastOn = now;
        pumpOn = true;
    }
    else
    {
        if (pumpOn && (now - pumpLastOn) < cfg.relay_min_on_ms)
            return;
        digitalWrite(PUMP_RELAY, HIGH);
        pumpLastOff = now;
        pumpOn = false;
    }
    pumpState = newState;
}

void setLightState(LightState newState, unsigned long now)
{
    if (newState == lightState)
        return;
    if (newState == LIGHT_ON)
    {
        if ((now - lightLastOff) < cfg.relay_min_off_ms)
            return;
        digitalWrite(LIGHT_RELAY, LOW);
        lightLastOn = now;
        lightOn = true;
    }
    else
    {
        if (lightOn && (now - lightLastOn) < cfg.relay_min_on_ms)
            return;
        digitalWrite(LIGHT_RELAY, HIGH);
        lightLastOff = now;
        lightOn = false;
    }
    lightState = newState;
}

const char *pumpStateStr()
{
    switch (pumpState)
    {
    case PUMP_ON:
        return "ON";
    case PUMP_ERR_DRY:
        return "ERR_DRY";
    default:
        return "OFF";
    }
}
const char *lightStateStr() { return (lightState == LIGHT_ON) ? "ON" : "OFF"; }

/* ===================== FACTORY RESET HELPER ===================== */
void doFactoryReset(const char *source)
{
    Serial.printf("\n\n");
    Serial.println("╔═══════════════════════════════════════════════════════╗");
    Serial.println("║          FACTORY RESET INITIATED                      ║");
    Serial.printf("║          Source: %-42s║\n", source);
    Serial.println("╚═══════════════════════════════════════════════════════╝");
    Serial.println();

    Serial.println("[RESET] Step 1: Stopping all tasks...");
    if (hTaskSensors)
        vTaskDelete(hTaskSensors);
    if (hTaskMQTT)
        vTaskDelete(hTaskMQTT);
    vTaskDelay(pdMS_TO_TICKS(500));

    Serial.println("[RESET] Step 2: Erasing entire NVS flash partition...");
    esp_err_t ret = nvs_flash_erase();
    if (ret == ESP_OK)
    {
        Serial.println("[RESET]   - nvs_flash_erase() succeeded ✓");
    }
    else
    {
        Serial.printf("[RESET]   - nvs_flash_erase() error: 0x%X (continuing)\n", ret);
    }

    Serial.println("[RESET] Step 3: Re-initializing NVS...");
    ret = nvs_flash_init();
    if (ret == ESP_OK)
    {
        Serial.println("[RESET]   - nvs_flash_init() succeeded ✓");
    }
    else if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        Serial.println("[RESET]   - Retrying NVS init...");
        nvs_flash_erase();
        nvs_flash_init();
        Serial.println("[RESET]   - nvs_flash_init() after erase ✓");
    }
    else
    {
        Serial.printf("[RESET]   - nvs_flash_init() error: 0x%X\n", ret);
    }

    Serial.println("[RESET] Step 4: Clearing Preferences namespaces (redundant safety)...");
    const char *namespaces[] = {"wifi-config", "wifi-config-pending", "nurtura-sys"};
    for (int i = 0; i < 3; i++)
    {
        preferences.begin(namespaces[i], false);
        preferences.clear();
        preferences.end();
        Serial.printf("[RESET]   - Cleared '%s' ✓\n", namespaces[i]);
    }

    Serial.println();
    Serial.println("╔═══════════════════════════════════════════════════════╗");
    Serial.println("║  ALL CREDENTIALS ERASED — Device will boot BLE        ║");
    Serial.println("║  Rebooting in 2 seconds...                            ║");
    Serial.println("╚═══════════════════════════════════════════════════════╝");
    Serial.println();

    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP.restart();
}

/* ===================== BOOT REASON ===================== */
void setBootReason()
{
    switch (esp_reset_reason())
    {
    case ESP_RST_POWERON:
        strncpy(bootReason, "POWER_ON", BUF_REASON);
        break;
    case ESP_RST_SW:
        strncpy(bootReason, "SW_RESET", BUF_REASON);
        break;
    case ESP_RST_PANIC:
        strncpy(bootReason, "PANIC", BUF_REASON);
        break;
    case ESP_RST_INT_WDT:
        strncpy(bootReason, "INT_WDT", BUF_REASON);
        break;
    case ESP_RST_TASK_WDT:
        strncpy(bootReason, "TASK_WDT", BUF_REASON);
        break;
    case ESP_RST_WDT:
        strncpy(bootReason, "WDT", BUF_REASON);
        break;
    case ESP_RST_BROWNOUT:
        strncpy(bootReason, "BROWNOUT", BUF_REASON);
        break;
    default:
        strncpy(bootReason, "UNKNOWN", BUF_REASON);
        break;
    }
    preferences.begin("nurtura-sys", true);
    lastFatalCode = preferences.getUInt("last_fatal", 0);
    preferences.end();
    Serial.printf("[BOOT] Reason:%s | Resets:%u | LastFatal:%u\n",
                  bootReason, resetCount, lastFatalCode);
}

/* ===================== BLE CALLBACKS ===================== */
#if FEATURE_BLE
class ServerCallbacks : public BLEServerCallbacks
{
    void onDisconnect(BLEServer *pServer)
    {
        if (bleActive)
            BLEDevice::getAdvertising()->start();
    }
};

class MyCallbacks : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *pCharacteristic)
    {
        String val = pCharacteristic->getValue();
        String uuid = pCharacteristic->getUUID().toString();
        if (xSemaphoreTake(credMutex, pdMS_TO_TICKS(100)))
        {
            if (uuid == SSID_CHAR_UUID)
            {
                strncpy(receivedSSID, val.c_str(), BUF_SSID - 1);
                Serial.printf("[BLE] SSID received: %s\n", receivedSSID);
            }
            else if (uuid == PASSWORD_CHAR_UUID)
            {
                strncpy(receivedPassword, val.c_str(), BUF_PASS - 1);
                credentialsReceived = true;
                ledMode = LED_WIFI_PENDING;
                Serial.println("[BLE] Password received — connecting WiFi...");
            }
            else if (uuid == RACK_NAME_CHAR_UUID)
            {
                strncpy(pendingRackName, val.c_str(), BUF_NAME - 1);
                pendingRackNameSave = true;
            }
            else if (uuid == DEVICE_ID_CHAR_UUID)
            {
                strncpy(pendingDeviceId, val.c_str(), BUF_DEVID - 1);
                pendingDeviceIdSave = true;
            }
            xSemaphoreGive(credMutex);
        }
        else
            Serial.println("[WARN] credMutex timeout in BLE onWrite");
    }
};

static ServerCallbacks serverCbInstance;
static MyCallbacks charCbInstance;
#endif

/* ===================== FORWARD DECLARATIONS ===================== */
void TaskSensors(void *);
void TaskMQTT(void *);
void TaskMonitor(void *);
void TaskBootButton(void *);

/* ===================== POST-WIFI INIT ===================== */
void onWiFiConnected()
{
    if (sPostWiFiDone)
        return;
    sPostWiFiDone = true;

    Wire.begin(21, 22);
    delay(100);

    if (!bme.begin(0x76, &Wire) && !bme.begin(0x77, &Wire))
    {
        Serial.println("[CRITICAL] BME280 init failed");
        bme280Fault = true;
    }
    else
        Serial.println("[INFO] BME280 initialized");

    delay(50);
    if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire))
    {
        Serial.println("[CRITICAL] BH1750 init failed on 0x23 — trying 0x5C");
        if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x5C, &Wire))
        {
            Serial.println("[CRITICAL] BH1750 failed on both addresses");
            bh1750Fault = true;
        }
    }
    else
        Serial.println("[INFO] BH1750 initialized on 0x23");

    char cid[48];
    snprintf(cid, sizeof(cid), "Nurtura-%s", deviceId);
    mqttClient.setURL(SECRET_MQTT_HOST, 8883, SECRET_MQTT_USER, SECRET_MQTT_PASS);
    mqttClient.setMqttClientName(cid);
    mqttClient.setCaCert(root_ca);
    mqttClient.setKeepAlive(60);
    mqttClient.setMaxPacketSize(BUF_PAYLOAD);
#if FEATURE_LWT
    char statusTopic[96];
    snprintf(statusTopic, sizeof(statusTopic), "nurtura/rack/%s/status", deviceId);
    mqttClient.enableLastWillMessage(statusTopic, "{\"o\":false}", true);
#endif
    mqttClient.loopStart();
    Serial.printf("[MQTT] Connecting to %s...\n", SECRET_MQTT_HOST);

    BaseType_t rs = xTaskCreatePinnedToCore(TaskSensors, "Sensors", 8192, NULL, 3, &hTaskSensors, 0);
    BaseType_t rm = xTaskCreatePinnedToCore(TaskMQTT, "MQTT", 8192, NULL, 1, &hTaskMQTT, 1);
    BaseType_t rn = xTaskCreatePinnedToCore(TaskMonitor, "Monitor", 4096, NULL, 1, NULL, 1);

    if (rs != pdPASS)
        Serial.println("[ERROR] TaskSensors failed to start");
    if (rm != pdPASS)
        Serial.println("[ERROR] TaskMQTT failed to start");
    if (rn != pdPASS)
        Serial.println("[ERROR] TaskMonitor failed to start");

    sensorHeartbeat = mqttHeartbeat = millis();

#if FEATURE_OTA
    ArduinoOTA.setHostname(deviceId);
    ArduinoOTA.begin();
#endif
}

/* ===================== WiFi EVENT HANDLER ===================== */
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info)
{
    switch (event)
    {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        Serial.println("[WiFi] Associated — waiting for IP...");
        break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        wifiSMState = WS_CONNECTED;
        Serial.printf("[WiFi] CONNECTED  IP=%s\n",
                      WiFi.localIP().toString().c_str());
        {
            preferences.begin("wifi-config-pending", true);
            String ps = preferences.getString("ssid", "");
            String pp = preferences.getString("password", "");
            preferences.end();

            if (ps.length() > 0)
            {
                preferences.begin("wifi-config", false);
                preferences.putString("ssid", ps);
                preferences.putString("password", pp);
                preferences.end();
                preferences.begin("wifi-config-pending", false);
                preferences.clear();
                preferences.end();
                Serial.println("[BLE] WiFi credentials saved to NVS");

#if FEATURE_BLE
                if (statusCharacteristic)
                {
                    statusCharacteristic->setValue("connected");
                    statusCharacteristic->notify();
                }
                ledMode = LED_BLE_CONNECTED;
                delay(2000);

                BLEDevice::getAdvertising()->stop();
                BLEDevice::deinit(true);
                bleActive = false;
                isProvisioning = false;
#endif
                rackNameWaitStart = millis();
                waitingForRackName = true;
                return;
            }
        }

        ledMode = LED_WIFI_CONNECTED;
        onWiFiConnected();
        break;

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    {
        wifiSMState = WS_FAILED;
        sys.wifiReconnects++;
        uint8_t reason = info.wifi_sta_disconnected.reason;
        Serial.printf("[WiFi] Disconnected  reason=%u  reconnects=%u\n",
                      reason, sys.wifiReconnects);
        ledMode = LED_WIFI_PENDING;

        if (sys.wifiReconnects < WIFI_RECONNECT_MAX)
        {
            Serial.println("[WiFi] Reconnecting...");
            WiFi.reconnect();
        }
        else
        {
            Serial.println("[WiFi] Max reconnects reached — rebooting");
            preferences.begin("nurtura-sys", false);
            preferences.putUInt("last_fatal", 0xDEAD0003);
            preferences.end();
            delay(500);
            ESP.restart();
        }
        break;
    }

    default:
        break;
    }
}

/* ===================== TASK MONITOR ===================== */
void TaskMonitor(void *pvParameters)
{
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(5000));
        unsigned long now = millis();

        if (hTaskSensors && (now - sensorHeartbeat) > cfg.heartbeat_timeout)
        {
            if (sensorRestarts < MAX_TASK_RESTARTS)
            {
                Serial.printf("[CRITICAL] TaskSensors frozen — restart #%d\n", sensorRestarts + 1);
                vTaskDelete(hTaskSensors);
                hTaskSensors = NULL;
                BaseType_t r = xTaskCreatePinnedToCore(TaskSensors, "Sensors", 8192, NULL, 3, &hTaskSensors, 0);
                if (r != pdPASS)
                    Serial.println("[ERROR] TaskSensors restart failed");
                sensorRestarts++;
                sensorHeartbeat = millis();
            }
            else
            {
                preferences.begin("nurtura-sys", false);
                preferences.putUInt("last_fatal", 0xDEAD0001);
                preferences.end();
                Serial.println("[FATAL] TaskSensors exceeded max restarts — rebooting");
                delay(500);
                ESP.restart();
            }
        }

        if (hTaskMQTT && (now - mqttHeartbeat) > cfg.heartbeat_timeout)
        {
            if (mqttRestarts < MAX_TASK_RESTARTS)
            {
                Serial.printf("[CRITICAL] TaskMQTT frozen — restart #%d\n", mqttRestarts + 1);
                vTaskDelete(hTaskMQTT);
                hTaskMQTT = NULL;
                BaseType_t r = xTaskCreatePinnedToCore(TaskMQTT, "MQTT", 8192, NULL, 1, &hTaskMQTT, 1);
                if (r != pdPASS)
                    Serial.println("[ERROR] TaskMQTT restart failed");
                mqttRestarts++;
                mqttHeartbeat = millis();
            }
            else
            {
                preferences.begin("nurtura-sys", false);
                preferences.putUInt("last_fatal", 0xDEAD0002);
                preferences.end();
                Serial.println("[FATAL] TaskMQTT exceeded max restarts — rebooting");
                delay(500);
                ESP.restart();
            }
        }

#if FEATURE_OTA
        if (otaEnabled && (millis() - uptimeStart) > OTA_DISABLE_MS)
        {
            otaEnabled = false;
            Serial.println("[OTA] Auto-disabled after uptime threshold");
        }
#endif

#if FEATURE_DIAGNOSTICS
        if (hTaskSensors)
        {
            UBaseType_t sw = uxTaskGetStackHighWaterMark(hTaskSensors);
            if (sw < 512)
                Serial.printf("[WARN] Sensors stack low: %u words\n", sw);
        }
        if (hTaskMQTT)
        {
            UBaseType_t mw = uxTaskGetStackHighWaterMark(hTaskMQTT);
            if (mw < 512)
                Serial.printf("[WARN] MQTT stack low: %u words\n", mw);
        }
        size_t freeHeap = esp_get_free_heap_size();
        size_t largestBlk = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        size_t minFreeHeap = esp_get_minimum_free_heap_size();
        if (freeHeap > 0 && (largestBlk < freeHeap / 2))
            Serial.printf("[WARN] Heap fragmented — free:%u largest_block:%u\n", freeHeap, largestBlk);
        Serial.printf("[DIAG] heap:%u min:%u frag_block:%u\n", freeHeap, minFreeHeap, largestBlk);
#endif
    }
}

/* ===================== TASK 0: SENSORS & LOGIC ===================== */
void TaskSensors(void *pvParameters)
{
    esp_task_wdt_add(NULL);
    vTaskDelay(pdMS_TO_TICKS(1000));

    unsigned long oldTime = millis();
    unsigned long pumpStartTime = 0;
    bool pumpWasOff = true;
    unsigned long pumpOnSince = 0;

    lastTempChange = lastHumChange = lastLuxChange = millis();

    for (;;)
    {
        esp_task_wdt_reset();
        sensorHeartbeat = millis();

        unsigned long now = millis();
        float duration = (now - oldTime) / 1000.0f;
        if (duration <= 0.0f)
            duration = 0.1f;

        portENTER_CRITICAL(&flowMux);
        uint32_t localPulses = pulseCount;
        pulseCount = 0;
        portEXIT_CRITICAL(&flowMux);

        float rawFlow = (localPulses / cfg.calib_factor) / duration;
        float cFlow = (rawFlow > cfg.flow_max_sane) ? 0.0f : rawFlow;
        if (rawFlow > cfg.flow_max_sane)
            Serial.printf("[WARN] Flow spike rejected: %.1f L/min\n", rawFlow);

        float t = bme.readTemperature();
        float h = bme.readHumidity();
        float l = lightMeter.readLightLevel();
        int m = constrain(map(readSoilSmoothed(), 2480, 460, 0, 100), 0, 100);

        if (bme280Fault && (bme.begin(0x76, &Wire) || bme.begin(0x77, &Wire)))
        {
            bme280Fault = false;
            Serial.println("[INFO] BME280 recovered");
        }
        if (bh1750Fault && lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE))
        {
            bh1750Fault = false;
            Serial.println("[INFO] BH1750 recovered");
        }

        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)))
        {
            if (!lightOn && l >= 0 && l < (cfg.lux_threshold - cfg.lux_hysteresis))
                setLightState(LIGHT_ON, now);
            else if (lightOn && l > (cfg.lux_threshold + cfg.lux_hysteresis))
                setLightState(LIGHT_OFF, now);

            if (sys.pumpErr)
            {
                setPumpState(PUMP_ERR_DRY, now);
            }
            else if (!pumpOn && m < (cfg.moisture_threshold - cfg.moisture_hysteresis))
            {
                if (pumpWasOff)
                {
                    pumpStartTime = now;
                    pumpOnSince = now;
                    pumpWasOff = false;
                }
                setPumpState(PUMP_ON, now);
                if ((now - pumpStartTime > cfg.dry_run_ms) && (cFlow < 0.1f))
                    sys.pumpErr = true;
                if ((now - pumpOnSince > cfg.dry_run_ms * 3) && (cFlow < 0.1f))
                {
                    Serial.println("[ALARM] Pump ON long-term with no flow — ERR_DRY");
                    sys.pumpErr = true;
                }
            }
            else if (pumpOn && m > (cfg.moisture_threshold + cfg.moisture_hysteresis))
            {
                setPumpState(PUMP_OFF, now);
                pumpWasOff = true;
            }

            if (!isnan(t))
            {
                lastTempChange = now;
                lastRawTemp = t;
                bme280Fault = false;
                sys.temp = t;
            }
            else if ((now - lastTempChange) > cfg.stale_data_ms)
            {
                bme280Fault = true;
                Serial.println("[WARN] BME280 data stale");
            }

            if (!isnan(h))
            {
                lastHumChange = now;
                lastRawHum = h;
                if (!bme280Fault)
                    sys.hum = h;
            }
            else
                bme280Fault = true;

            if (l >= 0)
            {
                lastLuxChange = now;
                lastRawLux = l;
                bh1750Fault = false;
                sys.lux = l;
            }
            else if ((now - lastLuxChange) > cfg.stale_data_ms)
            {
                bh1750Fault = true;
                Serial.println("[WARN] BH1750 data stale");
            }

            sys.moisture = (uint32_t)m;
            sys.flowRate = cFlow;
            if (cFlow > 0.1f)
                sys.totalMLf += (cFlow / 60.0f) * 1000.0f * duration;

            xSemaphoreGive(dataMutex);
        }
        else
            Serial.println("[WARN] dataMutex timeout in TaskSensors");

        oldTime = now;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ===================== TASK 1: MQTT COMMS ===================== */
void TaskMQTT(void *pvParameters)
{
    esp_task_wdt_add(NULL);

    unsigned long lastPublish = 0;
    unsigned long lastDiag = 0;
    bool safeModeFlag = false;

    preferences.begin("nurtura-sys", true);
    safeModeFlag = preferences.getBool("safe_mode", false);
    preferences.end();

    Serial.println("[MQTT] Waiting for connection...");
    vTaskDelay(pdMS_TO_TICKS(10000));

    bool subscribed = false;
    bool lastBmeFaultPublished = false;
    bool lastBhFaultPublished = false;

    for (;;)
    {
        esp_task_wdt_reset();
        mqttHeartbeat = millis();

        if (!mqttClient.isConnected())
        {
            subscribed = false;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (!subscribed)
        {
            char wateringCmdTopic[96], lightingCmdTopic[96];
            snprintf(wateringCmdTopic, sizeof(wateringCmdTopic), "nurtura/rack/%s/commands/watering", deviceId);
            snprintf(lightingCmdTopic, sizeof(lightingCmdTopic), "nurtura/rack/%s/commands/lighting", deviceId);

            mqttClient.subscribe(std::string(wateringCmdTopic), [](const std::string &payload)
                                 {
                if (payload.find("watering_start") != std::string::npos) {
                    setPumpState(PUMP_ON, millis());
                    Serial.println("[CMD] watering_start");
                } else if (payload.find("watering_stop") != std::string::npos) {
                    setPumpState(PUMP_OFF, millis());
                    Serial.println("[CMD] watering_stop");
                } }, 1);

            mqttClient.subscribe(std::string(lightingCmdTopic), [](const std::string &payload)
                                 {
                if (payload.find("light_on") != std::string::npos) {
                    setLightState(LIGHT_ON, millis());
                    Serial.println("[CMD] light_on");
                } else if (payload.find("light_off") != std::string::npos) {
                    setLightState(LIGHT_OFF, millis());
                    Serial.println("[CMD] light_off");
                } }, 1);

            subscribed = true;
            Serial.println("[MQTT] Subscribed");
            ledMode = LED_MQTT_CONNECTED;
        }

        unsigned long now = millis();
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(200)))
        {
#if FEATURE_RSSI
            sys.rssi = WiFi.RSSI();
#endif
            bool changed = fabsf(sys.temp - sys.lastPubTemp) > 0.2f || fabsf(sys.hum - sys.lastPubHum) > 0.5f ||
                           fabsf(sys.lux - sys.lastPubLux) > 5.0f || sys.moisture != sys.lastPubMoisture;

            if (!safeModeFlag && (changed || (now - lastPublish >= cfg.publish_interval)))
            {
                char payload[BUF_PAYLOAD];
                char sensorTopic[80];
                snprintf(sensorTopic, sizeof(sensorTopic), "nurtura/rack/%s/sensors", deviceId);
                snprintf(payload, BUF_PAYLOAD,
                         "{\"t\":%.1f,\"h\":%.1f,\"m\":%u,\"l\":%.0f}",
                         sys.temp, sys.hum, sys.moisture, sys.lux);

                sys.lastPubTemp = sys.temp;
                sys.lastPubHum = sys.hum;
                sys.lastPubLux = sys.lux;
                sys.lastPubMoisture = sys.moisture;
                xSemaphoreGive(dataMutex);

                mqttClient.publish(std::string(sensorTopic), std::string(payload), 1, false);
                lastPublish = now;
            }
            else
                xSemaphoreGive(dataMutex);
        }
        else
            Serial.println("[WARN] dataMutex timeout in TaskMQTT");

        if (now - lastDiag >= cfg.diag_interval)
        {
            char statusPayload[256], statusTopic[96];
            snprintf(statusTopic, sizeof(statusTopic), "nurtura/rack/%s/status", deviceId);
            snprintf(statusPayload, sizeof(statusPayload),
                     "{\"o\":true,\"tm\":%lu,\"v\":\"%s\",\"ip\":\"%s\",\"mac\":\"%s\",\"u\":%lu}",
                     now, FW_VERSION, WiFi.localIP().toString().c_str(), deviceId, (millis() - uptimeStart));
            mqttClient.publish(std::string(statusTopic), std::string(statusPayload), 1, true);
            lastDiag = now;
        }

        if (bme280Fault && !lastBmeFaultPublished)
        {
            char errPayload[256], errTopic[96];
            snprintf(errTopic, sizeof(errTopic), "nurtura/rack/%s/errors", deviceId);
            snprintf(errPayload, sizeof(errPayload),
                     "{\"c\":\"SENSOR_FAILURE\",\"m\":\"BME280 sensor not responding\",\"s\":\"HIGH\",\"tm\":%lu,\"st\":\"TEMPERATURE\"}",
                     now);
            mqttClient.publish(std::string(errTopic), std::string(errPayload), 1, false);
            lastBmeFaultPublished = true;
        }
        if (!bme280Fault)
            lastBmeFaultPublished = false;

        if (bh1750Fault && !lastBhFaultPublished)
        {
            char errPayload[256], errTopic[96];
            snprintf(errTopic, sizeof(errTopic), "nurtura/rack/%s/errors", deviceId);
            snprintf(errPayload, sizeof(errPayload),
                     "{\"c\":\"SENSOR_FAILURE\",\"m\":\"BH1750 sensor not responding\",\"s\":\"HIGH\",\"tm\":%lu,\"st\":\"LIGHT\"}",
                     now);
            mqttClient.publish(std::string(errTopic), std::string(errPayload), 1, false);
            lastBhFaultPublished = true;
        }
        if (!bh1750Fault)
            lastBhFaultPublished = false;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ===================== TASK: BOOT BUTTON ===================== */
void TaskBootButton(void *pvParameters)
{
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    Serial.println("[BTN] TaskBootButton started — monitoring IO0 for runtime reset");

    for (;;)
    {
        int btnVal = digitalRead(BOOT_BUTTON_PIN);

        if (btnVal == LOW)
        {
            Serial.println("\n[BTN] ╔════════════════════════════════════════╗");
            Serial.println("[BTN] ║ IO0 PRESSED — HOLD 3s FOR FACTORY RESET ║");
            Serial.println("[BTN] ╚════════════════════════════════════════╝\n");

            unsigned long holdStart = millis();
            bool held = true;

            while (digitalRead(BOOT_BUTTON_PIN) == LOW)
            {
                unsigned long elapsed = millis() - holdStart;
                if (elapsed % 1000 < 100)
                    Serial.printf("[BTN] Holding... %lus / 3s\n", elapsed / 1000);

                if (elapsed >= BTN_HOLD_MS)
                {
                    doFactoryReset("IO0 button (runtime)");
                    held = false;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }

            if (held)
                Serial.println("[BTN] Released early — factory reset cancelled.\n");
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ===================== SETUP ===================== */
void setup()
{
    Serial.begin(115200);
    delay(500);

    Serial.println("\n\n");
    Serial.println("╔═══════════════════════════════════════════════════════╗");
    Serial.printf("║  Nurtura Rack Firmware v%s                       ║\n", FW_VERSION);
    Serial.printf("║  Built: %s  ║\n", FW_BUILD_DATE);
    Serial.println("╚═══════════════════════════════════════════════════════╝\n");

    uptimeStart = millis();

    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(deviceId, sizeof(deviceId), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.printf("[BOOT] Device ID: %s\n\n", deviceId);

    preferences.begin("nurtura-sys", false);
    resetCount = preferences.getUInt("reset_count", 0) + 1;
    preferences.putUInt("reset_count", resetCount);
    preferences.end();

    setBootReason();

    // In Core 3.x, you must use the new configuration structure
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = WDT_TIMEOUT * 1000,
        .idle_core_mask = (1 << 0) | (1 << 1), // Monitor both cores
        .trigger_panic = true};
    esp_task_wdt_reconfigure(&twdt_config);

    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    pinMode(PUMP_RELAY, OUTPUT);
    digitalWrite(PUMP_RELAY, HIGH);
    pinMode(LIGHT_RELAY, OUTPUT);
    digitalWrite(LIGHT_RELAY, HIGH);
    pinMode(FLOW_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(FLOW_PIN), pulseCounter, FALLING);

    initADC();

    Serial.println("[BTN] Checking IO0 at boot...");
    delay(200);

    if (digitalRead(BOOT_BUTTON_PIN) == LOW)
    {
        Serial.println("\n[BTN] ╔════════════════════════════════════════╗");
        Serial.println("[BTN] ║ IO0 HELD AT BOOT — HOLD 3s FOR RESET    ║");
        Serial.println("[BTN] ╚════════════════════════════════════════╝\n");

        unsigned long holdStart = millis();
        bool triggered = false;

        while (digitalRead(BOOT_BUTTON_PIN) == LOW)
        {
            unsigned long elapsed = millis() - holdStart;
            if (elapsed % 1000 < 100)
                Serial.printf("[BTN] Holding... %lus / 3s\n", elapsed / 1000);

            esp_task_wdt_reset();

            if (elapsed >= BTN_HOLD_MS)
            {
                triggered = true;
                break;
            }
            delay(50);
        }

        if (triggered)
            doFactoryReset("IO0 at boot");
        else
            Serial.println("[BTN] Released early — continuing normal boot.\n");
    }
    else
    {
        Serial.println("[BTN] IO0 not held — continuing normal boot.\n");
    }

#if FORCE_BLE_BOOT
    Serial.println("[BOOT] FORCE_BLE_BOOT is set — wiping credentials and booting BLE");
    doFactoryReset("FORCE_BLE_BOOT compile flag");
#endif

    dataMutex = xSemaphoreCreateMutex();
    credMutex = xSemaphoreCreateMutex();
#if FEATURE_MQTT_QUEUE
    queueMutex = xSemaphoreCreateMutex();
#endif
    if (!dataMutex || !credMutex)
    {
        Serial.println("[FATAL] Mutex creation failed — halting");
        while (1)
            delay(1000);
    }

    xTaskCreatePinnedToCore(TaskLED, "LED", 2048, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(TaskBootButton, "BootBtn", 2048, NULL, 2, NULL, 1);

    Serial.println("[BOOT] Checking for saved WiFi credentials...");
    preferences.begin("wifi-config", true);
    String s = preferences.getString("ssid", "");
    String p = preferences.getString("password", "");
    preferences.end();

    Serial.printf("[BOOT] SSID length: %d bytes\n", s.length());
    Serial.printf("[BOOT] PASS length: %d bytes\n", p.length());
    if (s.length() > 0)
        Serial.printf("[BOOT] Found SSID in NVS: '%s'\n\n", s.c_str());
    else
        Serial.println("[BOOT] NVS is empty — will start BLE provisioning\n");

#if !FEATURE_BLE
    if (s.length() == 0)
    {
        Serial.println("[BOOT] BLE disabled and no credentials — using secrets.h");
        preferences.begin("wifi-config", false);
        preferences.putString("ssid", SECRET_WIFI_SSID);
        preferences.putString("password", SECRET_WIFI_PASS);
        preferences.end();
        s = SECRET_WIFI_SSID;
        p = SECRET_WIFI_PASS;
    }
#endif

    if (s.length() > 0)
    {
        Serial.println("[BOOT] ════════════════════════════════════════");
        Serial.println("[BOOT] BOOTING NORMAL MODE → WiFi → MQTT");
        Serial.println("[BOOT] ════════════════════════════════════════\n");

        WiFi.onEvent(onWiFiEvent);
        wifiSMState = WS_CONNECTING;
        ledMode = LED_WIFI_PENDING;
        WiFi.begin(s.c_str(), p.c_str());
        Serial.printf("[WiFi] Connecting to %s (non-blocking)...\n\n", s.c_str());
    }
    else
    {
#if FEATURE_BLE
        Serial.println("[BOOT] ════════════════════════════════════════");
        Serial.println("[BOOT] BOOTING BLE PROVISIONING MODE");
        Serial.println("[BOOT] ════════════════════════════════════════\n");

        isProvisioning = bleActive = true;
        bleStartTime = millis();
        ledMode = LED_BLE_PENDING;

        BLEDevice::init("Nurtura Rack");
        BLEServer *ps = BLEDevice::createServer();
        ps->setCallbacks(&serverCbInstance);
        BLEService *psv = ps->createService(SERVICE_UUID);

        char macFormatted[18];
        snprintf(macFormatted, sizeof(macFormatted), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        BLECharacteristic *devIdChar = psv->createCharacteristic(
            DEVICE_ID_CHAR_UUID, BLECharacteristic::PROPERTY_READ);
        devIdChar->setValue(macFormatted);
        devIdChar->setCallbacks(&charCbInstance);

        psv->createCharacteristic(SSID_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE)->setCallbacks(&charCbInstance);
        psv->createCharacteristic(PASSWORD_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE)->setCallbacks(&charCbInstance);
        psv->createCharacteristic(RACK_NAME_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE)->setCallbacks(&charCbInstance);

        statusCharacteristic = psv->createCharacteristic(
            STATUS_CHAR_UUID, BLECharacteristic::PROPERTY_NOTIFY);
        statusCharacteristic->addDescriptor(new BLE2902());

        psv->start();
        BLEAdvertising *pAdv = BLEDevice::getAdvertising();
        pAdv->addServiceUUID(SERVICE_UUID);
        pAdv->setScanResponse(true);
        pAdv->setMinPreferred(0x06);
        pAdv->start();

        Serial.printf("[BLE] Started advertising — timeout in %lu seconds\n\n", BLE_TIMEOUT_MS / 1000);
#else
        Serial.println("[ERROR] No credentials and BLE disabled — cannot proceed");
        while (1)
            delay(1000);
#endif
    }
}

/* ===================== LOOP ===================== */
void loop()
{
    esp_task_wdt_reset();

#if FEATURE_OTA
    if (wifiSMState == WS_CONNECTED && otaEnabled)
        ArduinoOTA.handle();
#endif

#if FEATURE_BLE
    if (bleActive && (millis() - bleStartTime) > BLE_TIMEOUT_MS)
    {
        Serial.println("[BLE] Timeout — disabling");
        BLEDevice::getAdvertising()->stop();
        BLEDevice::deinit(true);
        bleActive = false;
        ledMode = LED_OFF;
    }

    if (isProvisioning && credentialsReceived)
    {
        credentialsReceived = false;

        char ssid[BUF_SSID], pass[BUF_PASS];
        if (xSemaphoreTake(credMutex, pdMS_TO_TICKS(200)))
        {
            strncpy(ssid, receivedSSID, BUF_SSID - 1);
            strncpy(pass, receivedPassword, BUF_PASS - 1);
            xSemaphoreGive(credMutex);
        }

        preferences.begin("wifi-config-pending", false);
        preferences.putString("ssid", String(ssid));
        preferences.putString("password", String(pass));
        preferences.end();

        WiFi.onEvent(onWiFiEvent);
        WiFi.begin(ssid, pass);
        Serial.println("[BLE] Credentials received — connecting WiFi (non-blocking)...\n");
    }
#endif

    if (waitingForRackName)
    {
        if (pendingRackNameSave || (millis() - rackNameWaitStart) >= 30000)
        {
            waitingForRackName = false;
            Serial.println(pendingRackNameSave
                               ? "[BLE] Rack name received — rebooting..."
                               : "[BLE] Rack name timeout — rebooting anyway...");
            delay(300);
            ESP.restart();
        }
    }

    if (pendingRackNameSave && xSemaphoreTake(credMutex, pdMS_TO_TICKS(200)))
    {
        char name[BUF_NAME];
        strncpy(name, pendingRackName, BUF_NAME - 1);
        pendingRackNameSave = false;
        xSemaphoreGive(credMutex);
        preferences.begin("wifi-config", false);
        preferences.putString("rack_name", name);
        preferences.end();
    }

    if (pendingDeviceIdSave && xSemaphoreTake(credMutex, pdMS_TO_TICKS(200)))
    {
        char devId[BUF_DEVID];
        strncpy(devId, pendingDeviceId, BUF_DEVID - 1);
        pendingDeviceIdSave = false;
        xSemaphoreGive(credMutex);
        preferences.begin("wifi-config", false);
        preferences.putString("device_id", devId);
        preferences.end();
    }

    if (!isProvisioning)
    {
        esp_task_wdt_delete(NULL);
        vTaskDelay(portMAX_DELAY);
    }
    else
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
