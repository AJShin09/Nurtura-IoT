#include <WiFi.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <nvs_flash.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <BH1750.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>
#include "secrets.h"

/* ===================== FIRMWARE IDENTITY ===================== */
#define FW_VERSION    "2.6.1"
#define FW_BUILD_DATE __DATE__ " " __TIME__

/* ===================== FEATURE FLAGS ===================== */
#define DEBUG            0
#define FEATURE_BLE      1
#define FORCE_BLE_BOOT   0

#if DEBUG
  #define DBG(...)  Serial.printf(__VA_ARGS__)
#else
  #define DBG(...)  (void)0
#endif

/* ===================== BUFFER SIZES ===================== */
#define BUF_SSID   48
#define BUF_PASS   48
#define BUF_DEVID  18

/* ===================== BLE UUIDs ===================== */
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define DEVICE_ID_CHAR_UUID "abc12345-1234-5678-1234-56789abcdef0"
#define SSID_CHAR_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define PASSWORD_CHAR_UUID  "1c95d5e3-d8f7-413a-bf3d-7a2e5d7be87e"
#define STATUS_CHAR_UUID    "9a8ca5e3-d8f7-413a-bf3d-7a2e5d7be123"
#define RESET_CHAR_UUID     "ffffffff-ffff-ffff-ffff-ffffffffffff"

/* ===================== PINS ===================== */
#define SOIL_PIN         35
#define FLOW_PIN         13
#define PUMP_RELAY       25
#define LIGHT_RELAY      26
#define BOOT_BUTTON_PIN   0

/* ===================== LCD (HW-61) ===================== */
// Wire  (Bus 0) → BME280             : SDA=16, SCL=17
// Wire1 (Bus 1) → BH1750 + HW-61 LCD: SDA=21, SCL=22  ← plug LCD here
#define LCD_ADDR     0x27  
#define LCD_COLS     16
#define LCD_ROWS      2
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

/* ===================== TIMING ===================== */
#define WDT_TIMEOUT             30
#define BLE_TIMEOUT_MS          600000UL
#define BTN_HOLD_MS             3000
#define MQTT_RECONNECT_INTERVAL 5000
/* ---- Sensor publish intervals ---- */
#define SENSOR_INTERVAL_NORMAL  3000UL  
#define SENSOR_INTERVAL_PUMPING  60000UL 

/* ===================== RELAY GUARD TIMES ===================== */
#define RELAY_MIN_ON_MS  2000
#define RELAY_MIN_OFF_MS 2000

/* ===================== STATE ENUMS ===================== */
enum PumpState   : uint8_t { PUMP_STOPPED, PUMP_RUNNING, PUMP_ERR_DRY };
enum LightState  : uint8_t { LIGHT_OFF, LIGHT_ON };
enum WiFiSMState : uint8_t { WS_IDLE, WS_CONNECTING, WS_CONNECTED, WS_FAILED };

/* ===================== LCD DISPLAY STATES ===================== */
enum LCDState : uint8_t {
    LCD_BOOT,              // Nurtura Rack     / Starting...
    LCD_BLE_ADVERTISING,   // Bluetooth Ready  / Open Nurtura App
    LCD_BLE_CONNECTED,     // BLE Connected    / Verifying...
    LCD_BLE_VERIFYING,     // BLE Connected    / Scanning QR...
    LCD_BLE_DISCONNECTED,  // BLE Disconnected / Reconnecting...
    LCD_WIFI_CONNECTING,   // WiFi             / <SSID name>
    LCD_WIFI_CONNECTED,    // WiFi             / Connected!
    LCD_MQTT_CONNECTING,   // Server           / Connecting...
    LCD_SERVER_LOST,       // Server Lost      / Reconnecting...
    LCD_CONNECTED,         // Nurtura Online   / T:xx H:xx (alt M:xx L:xx)
    LCD_PUMP_RUNNING,      // Nurtura Online   / Pump ON M:xx%
    LCD_DISCONNECTED,      // WiFi Lost        / Reconnecting...
    LCD_BACK_PRESSED,      // App Disconnected / Bluetooth Ready
    LCD_FACTORY_RESET,     // Factory Reset    / Clearing data...
    LCD_BTN_HOLD,          // Hold to Reset    / Releasing in x
    LCD_BLE_TIMEOUT,       // Bluetooth Off    / Hold Btn to Wake
    LCD_SENSOR_ERROR,      // Sensor Error     / Check wiring
};
volatile LCDState lcdState     = LCD_BOOT;
volatile LCDState lcdLastState = (LCDState)0xFF;  // Sentinel: forces redraw on first TaskLCD tick
PumpState    pumpState   = PUMP_STOPPED;
LightState   lightState  = LIGHT_OFF;
WiFiSMState  wifiSMState = WS_IDLE;

/* ===================== SYSTEM DATA ===================== */
struct SystemData
{
    int16_t  temp_x10       = 0;
    uint8_t  hum            = 0;
    uint16_t lux            = 0;
    uint8_t  moisture       = 0;
    uint16_t waterUsed_ml   = 0;  
    bool     wateringOccurred = false;  
} sys;

/* ===================== GLOBALS ===================== */
Adafruit_BME280  bme;
BH1750           lightMeter;

WiFiClientSecure wifiClient;
PubSubClient     mqttClient(wifiClient);

Preferences      preferences;

BLECharacteristic *statusCharacteristic = NULL;
BLECharacteristic *resetCharacteristic  = NULL;

SemaphoreHandle_t dataMutex;
SemaphoreHandle_t credMutex;

TaskHandle_t hTaskSensors = NULL;
TaskHandle_t hTaskMQTT    = NULL;

volatile unsigned long sensorHeartbeat = 0;
volatile unsigned long mqttHeartbeat   = 0;
uint8_t sensorRestarts = 0;
uint8_t mqttRestarts   = 0;

volatile bool credentialsReceived = false;
volatile bool isProvisioning      = false;
volatile bool bleActive           = false;
volatile bool resetRequested      = false;
// Reconnect timestamp — set to millis() on disconnect, loop() fires WiFi.begin()
// only after a cooldown and only when not already associating.
// 0 = no reconnect pending.
volatile unsigned long wifiReconnectAt = 0;
#define WIFI_RECONNECT_COOLDOWN_MS  5000UL   // Wait 5 s after disconnect before retrying
unsigned long bleStartTime        = 0;

char receivedSSID[BUF_SSID]   = "";
char receivedPass[BUF_PASS]   = "";
char wifiSSIDForLCD[BUF_SSID] = "";  // SSID shown on LCD (never the password)
char macAddress[BUF_DEVID]    = "";
char mqttClientId[48]       = "";

unsigned long uptimeStart = 0;

volatile bool bme280Fault = false;
volatile bool bh1750Fault = false;

unsigned long pumpLastOn  = 0, pumpLastOff  = 0;
unsigned long lightLastOn = 0, lightLastOff = 0;
bool pumpOn  = false;
bool lightOn = false;

portMUX_TYPE flowMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t pulseCount         = 0;
volatile unsigned long lastPulseTime = 0;

// Flow sensor: pulses accumulated for the CURRENT watering session only.
// Reset to 0 when pump starts; snapshot taken when pump stops.
volatile uint32_t sessionPulses = 0;

#define ADC_SAMPLES 8
int soilBuffer[ADC_SAMPLES] = {0};
int soilBufIdx = 0;

static bool   sPostWiFiDone           = false;
unsigned long mqttLastAttempt         = 0;
volatile bool mqttConnected           = false;
volatile bool mqttPreviouslyConnected = false;
volatile bool bleFallbackRequested    = false;
volatile bool bleRestartRequested     = false;
volatile bool appDisconnectRequested  = false;  // Set when app sends APP_DISCONNECT (back button)
volatile bool instantPublishRequested = false;  // Set by setPumpState() to force immediate sensor publish
volatile bool sensorStreaming         = false;  // OFF until backend sends sensor_start command
volatile bool bleTimedOut             = false;  // Set when BLE advertising window expires — button restarts BLE
volatile int  btnHoldSecsLeft         = 3;       // Countdown shown on LCD during button hold

/* ===================== BACKEND PRESENCE ===================== */
// Tracks whether the backend is actively online.
// Parsed from nurtura/backend/status {"o": true/false}.
// When false, TaskAutomation takes over watering and lighting locally.
volatile bool backendOnline = true;   // Assume online until told otherwise

/* ===================== FORWARD DECLARATIONS ===================== */
void setPumpState(PumpState newState, unsigned long now);
void setLightState(LightState newState, unsigned long now);
void publishSensorData();
void publishStatus();
void publishError(const char *errorCode, const char *message, const char *severity, const char *sensorType);
String getTopic(const char *topicType);
void startBLEProvisioning();
void mqttMessageCallback(char *topic, byte *payload, unsigned int length);
void TaskSensors(void *);
void TaskMQTT(void *);
void TaskMonitor(void *);
void TaskBootButton(void *);
void TaskAutomation(void *);
void TaskLCD(void *);
void lcdShow(LCDState state);
String getISO8601Timestamp();

/* ===================== LCD HELPERS ===================== */

SemaphoreHandle_t lcdMutex = NULL;

void lcdShow(LCDState state)
{
    lcdState = state;   
}

void TaskLCD(void *pvParameters)
{
    unsigned long lastSensorRefresh = 0;
    bool          sensorRowA        = true;  

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(500));

        bool stateChanged  = (lcdState != lcdLastState);
        bool sensorRefresh = (lcdState == LCD_CONNECTED || lcdState == LCD_PUMP_RUNNING)
                             && (millis() - lastSensorRefresh >= 3000);
        bool btnRefresh    = (lcdState == LCD_BTN_HOLD); 

        if (!stateChanged && !sensorRefresh && !btnRefresh) continue;

        lcdLastState = lcdState;

        char line1[17] = {0};
        char line2[17] = {0};

        switch (lcdState)
        {
        // ── Boot ──────────────────────────────────────────────────────────
        case LCD_BOOT:
            strncpy(line1, "  Nurtura Rack  ", 16);
            strncpy(line2, "   Starting...  ", 16);
            break;

        // ── BLE ───────────────────────────────────────────────────────────
        case LCD_BLE_ADVERTISING:
            strncpy(line1, "Bluetooth Ready ", 16);
            strncpy(line2, "Open Nurtura App", 16);
            break;

        case LCD_BLE_CONNECTED:
            strncpy(line1, " BLE Connected  ", 16);
            strncpy(line2, "  Verifying...  ", 16);
            break;

        case LCD_BLE_VERIFYING:
            strncpy(line1, " BLE Connected  ", 16);
            strncpy(line2, " Scanning QR... ", 16);
            break;

        case LCD_BLE_DISCONNECTED:
            strncpy(line1, "BLE Disconnected", 16);
            strncpy(line2, " Reconnecting...", 16);
            break;

        case LCD_BLE_TIMEOUT:
            strncpy(line1, " Bluetooth Off  ", 16);
            strncpy(line2, "Hold Btn to Wake", 16);
            break;

        case LCD_BACK_PRESSED:
            strncpy(line1, "App Disconnected", 16);
            strncpy(line2, "Bluetooth Ready ", 16);
            break;

        // ── WiFi ──────────────────────────────────────────────────────────
        case LCD_WIFI_CONNECTING:
        {
            strncpy(line1, "     WiFi       ", 16);
            char ssidLine[17] = {0};
            if (wifiSSIDForLCD[0] != '\0')
                snprintf(ssidLine, sizeof(ssidLine), "%-16s", wifiSSIDForLCD);
            else
                strncpy(ssidLine, "  Connecting... ", 16);
            strncpy(line2, ssidLine, 16);
            break;
        }

        case LCD_WIFI_CONNECTED:
            strncpy(line1, "     WiFi       ", 16);
            strncpy(line2, "   Connected!   ", 16);
            break;

        case LCD_DISCONNECTED:
            strncpy(line1, "   WiFi Lost    ", 16);
            strncpy(line2, " Reconnecting...", 16);
            break;

        // ── MQTT / Server ─────────────────────────────────────────────────
        case LCD_MQTT_CONNECTING:
            strncpy(line1, "    Server      ", 16);
            strncpy(line2, "  Connecting... ", 16);
            break;

        case LCD_SERVER_LOST:
            strncpy(line1, "  Server Lost   ", 16);
            strncpy(line2, " Reconnecting...", 16);
            break;

        // ── Connected / Running ───────────────────────────────────────────
        case LCD_CONNECTED:
        case LCD_PUMP_RUNNING:
        {
            strncpy(line1, " Nurtura Online ", 16);

            int16_t  t_x10  = 0;
            uint8_t  hum    = 0;
            uint8_t  moist  = 0;
            uint16_t lux    = 0;
            if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)))
            {
                t_x10 = sys.temp_x10;
                hum   = sys.hum;
                moist = sys.moisture;
                lux   = sys.lux;
                xSemaphoreGive(dataMutex);
            }

            if (pumpState == PUMP_RUNNING)
            {
                snprintf(line2, sizeof(line2), "Pump ON  M:%3u%% ", moist);
            }
            else if (sensorRowA)
            {
                int t_int  = t_x10 / 10;
                int t_frac = abs(t_x10 % 10);
                snprintf(line2, sizeof(line2), "T:%d.%dC  H:%u%%  ",
                         t_int, t_frac, hum);
            }
            else
            {
                snprintf(line2, sizeof(line2), "M:%3u%%  L:%4ulx",
                         moist, lux);
            }

            sensorRowA        = !sensorRowA;
            lastSensorRefresh = millis();
            break;
        }

        // ── Factory Reset ─────────────────────────────────────────────────
        case LCD_FACTORY_RESET:
            strncpy(line1, " Factory Reset  ", 16);
            strncpy(line2, " Clearing data..", 16);
            break;

        // ── Button Hold countdown ─────────────────────────────────────────
        case LCD_BTN_HOLD:
        {
            int s = btnHoldSecsLeft;
            strncpy(line1, " Hold to Reset  ", 16);
            snprintf(line2, sizeof(line2), "  Releasing in %d ", s);
            break;
        }

        // ── Sensor Error ──────────────────────────────────────────────────
        case LCD_SENSOR_ERROR:
            strncpy(line1, " Sensor Error   ", 16);
            strncpy(line2, " Check wiring   ", 16);
            break;

        default:
            break;
        }

        // Write to LCD
        if (xSemaphoreTake(lcdMutex, pdMS_TO_TICKS(100)))
        {
            lcd.setCursor(0, 0); lcd.print(line1);
            lcd.setCursor(0, 1); lcd.print(line2);
            xSemaphoreGive(lcdMutex);
        }
    }
}

/* ===================== ISO 8601 TIMESTAMP HELPER ===================== */

String getISO8601Timestamp() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        // Fallback if NTP sync hasn't completed yet
        return "1970-01-01T00:00:00Z"; 
    }
    char buffer[25];
    // Formats to: YYYY-MM-DDTHH:MM:SSZ
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    return String(buffer);
}

/* ===================== TOPIC HELPER ===================== */

String getTopic(const char *topicType)
{
    static char buffer[96] = {0};
    snprintf(buffer, sizeof(buffer), "nurtura/rack/%s/%s", macAddress, topicType);
    return String(buffer);
}

/* ===================== PUBLISH HELPERS ===================== */

void publishSensorData()
{
    if (!mqttClient.connected()) return;

    // Backend offline — print sensor readings to Serial and skip MQTT publish
    if (!backendOnline)
    {
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(200)))
        {
            Serial.printf_P(PSTR(F("[OFFLINE] T:%.1f C  H:%u%%  M:%u%%  L:%u lx  tm:%s\n")),
                            sys.temp_x10 / 10.0f,
                            sys.hum,
                            sys.moisture,
                            sys.lux,
                            getISO8601Timestamp().c_str());
            xSemaphoreGive(dataMutex);
        }
        return;
    }

    if (!sensorStreaming) return;    // Gated by sensor_start/sensor_stop command

    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(200)))
    {
        StaticJsonDocument<256> doc;
        doc["t"]  = sys.temp_x10 / 10.0f;
        doc["h"]  = sys.hum;
        doc["m"]  = sys.moisture;
        doc["l"]  = sys.lux;
        
        // Include water used only if a watering session completed since last publish
        if (sys.wateringOccurred && sys.waterUsed_ml > 0)
        {
            doc["wu"] = sys.waterUsed_ml;
            sys.wateringOccurred = false;  // Consumed — clear until next session ends
            sys.waterUsed_ml     = 0;      // Reset so it doesn't repeat on subsequent publishes
        }
        
        doc["tm"] = getISO8601Timestamp();

        xSemaphoreGive(dataMutex);

        char payload[256];
        serializeJson(doc, payload, sizeof(payload));

        DBG(F("[MQTT] sensors (not published): %s\n"), payload);
    }
}

void publishStatus()
{
    if (!mqttClient.connected()) return;

    char ipStr[16];
    snprintf(ipStr, sizeof(ipStr), "%s", WiFi.localIP().toString().c_str());

    // MAC in colon format for status
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    StaticJsonDocument<192> doc;
    doc["o"]  = true;                                    // online
    doc["tm"] = getISO8601Timestamp();                   // ISO 8601 timestamp
    doc["v"]  = FW_VERSION;                              // version
    doc["ip"] = ipStr;                                   // IP address
    doc["mac"] = macStr;                                 // MAC address
    doc["u"]  = (int)(millis() - uptimeStart);           // uptime ms

    char payload[192];
    serializeJson(doc, payload, sizeof(payload));

    String topic = getTopic("status");
    mqttClient.publish(topic.c_str(), (uint8_t*)payload, strlen(payload), true);  // retain=true
    Serial.printf_P(PSTR(F("[MQTT] TX status: v=%s ip=%s mac=%s\n")),
                    FW_VERSION, ipStr, macStr);
    DBG(F("[MQTT] TX status (full): %s\n"), payload);
}

void publishError(const char *errorCode, const char *message, const char *severity, const char *sensorType)
{
    if (!mqttClient.connected()) return;

    StaticJsonDocument<256> doc;
    doc["c"]  = errorCode;      // Error code
    doc["m"]  = message;        // Message
    doc["s"]  = severity;       // Severity
    doc["tm"] = getISO8601Timestamp();       // Timestamp (ms since epoch)
    doc["ht"] = sensorType;     // Sensor type

    // Optional details object (empty for now, can be extended)
    JsonObject details = doc.createNestedObject("d");
    details["ed"] = message;    // Error description in details

    char payload[256];
    serializeJson(doc, payload, sizeof(payload));

    DBG(F("[MQTT] error (not published): %s\n"), payload);
}

/* ===================== MQTT MESSAGE CALLBACK ===================== */

void mqttMessageCallback(char *topic, byte *payload, unsigned int length)
{
    char msg[128] = {0};
    size_t copyLen = (length < sizeof(msg) - 1) ? length : sizeof(msg) - 1;
    memcpy(msg, payload, copyLen);

    Serial.printf_P(PSTR(F("[MQTT] RX topic: %s\n")), topic);
    DBG(F("[MQTT] RX payload: %s\n"), msg);

    // ---- Handle backend presence topic ----
    if (strcmp(topic, "nurtura/backend/status") == 0)
    {
        StaticJsonDocument<96> bdoc;
        if (deserializeJson(bdoc, msg) == DeserializationError::Ok)
        {
            bool wasOnline  = backendOnline;
            backendOnline   = bdoc["o"] | false;  
            if (wasOnline != backendOnline)
            {
                // State changed — log transition
                Serial.printf_P(PSTR(F("[BACKEND] Status changed → %s\n")),
                                backendOnline ? "ONLINE  — handing control to backend"
                                              : "OFFLINE — local automation active");
            }
            else
            {
                // State unchanged (e.g. retained message on reconnect) — confirm receipt
                Serial.printf_P(PSTR(F("[BACKEND] Status received — already %s (no change)\n")),
                                backendOnline ? "ONLINE" : "OFFLINE");
            }
        }
        else
        {
            Serial.println(F("[BACKEND] Status parse failed — defaulting to OFFLINE (automation safe)"));
            backendOnline = false;
        }
        return;
    }

    // Build base topic prefix for comparison
    char base[96];
    snprintf(base, sizeof(base), "nurtura/rack/%s/commands/", macAddress);
    size_t baseLen = strlen(base);
    
    if (strncmp(topic, base, baseLen) != 0) return;

    const char *cmd = topic + baseLen;
    unsigned long now = millis();

    // Parse JSON payload
    StaticJsonDocument<128> doc;
    DeserializationError err = deserializeJson(doc, msg);

    if (err)
    {
        Serial.printf_P(PSTR(F("[JSON] Parse error on commands/%s: %s\n")), cmd, err.c_str());
        publishError("invalid_json",
                     "Malformed JSON on command topic",
                     "LOW",
                     "WATER_PUMP");
        return;
    }

    // Handle watering commands
    if (strcmp(cmd, "watering") == 0)
    {
        const char *action = doc["action"] | "";

        if (strcmp(action, "watering_start") == 0)
        {
            Serial.println(F("[PUMP] START command (AsyncAPI)"));
            setPumpState(PUMP_RUNNING, now);
        }
        else if (strcmp(action, "watering_stop") == 0)
        {
            Serial.println(F("[PUMP] STOP command (AsyncAPI)"));
            setPumpState(PUMP_STOPPED, now);
        }
        else
        {
            Serial.printf_P(PSTR(F("[PUMP] Unknown action: %s\n")), action);
            publishError("invalid_action",
                         "Unknown watering action",
                         "LOW",
                         "WATER_PUMP");
        }
    }
    // Handle lighting commands
    else if (strcmp(cmd, "lighting") == 0)
    {
        const char *action = doc["action"] | "";

        if (strcmp(action, "light_on") == 0)
        {
            Serial.println(F("[LIGHT] ON command (AsyncAPI)"));
            setLightState(LIGHT_ON, now);
        }
        else if (strcmp(action, "light_off") == 0)
        {
            Serial.println(F("[LIGHT] OFF command (AsyncAPI)"));
            setLightState(LIGHT_OFF, now);
        }
        else
        {
            Serial.printf_P(PSTR(F("[LIGHT] Unknown action: %s\n")), action);
            publishError("invalid_action",
                         "Unknown lighting action",
                         "LOW",
                         "GROW_LIGHT");
        }
    }
    // Handle sensor streaming commands (AsyncAPI: commands/sensors)
    else if (strcmp(cmd, "sensors") == 0)
    {
        const char *action = doc["action"] | "";

        if (strcmp(action, "sensor_start") == 0)
        {
            sensorStreaming = true;
            Serial.println(F("[SENSOR] Publishing RESUMED by backend command"));
        }
        else if (strcmp(action, "sensor_stop") == 0)
        {
            sensorStreaming = false;
            Serial.println(F("[SENSOR] Publishing PAUSED by backend command — sensor reads continue"));
        }
        else
        {
            Serial.printf_P(PSTR(F("[SENSOR] Unknown sensor action: %s\n")), action);
            publishError("UNKNOWN_ERROR",
                         "Unknown sensor streaming action",
                         "LOW",
                         "TEMPERATURE");
        }
    }
    else
    {
        Serial.printf_P(PSTR(F("[MQTT] Unknown command topic: %s\n")), cmd);
    }
}

/* ===================== FLOW INTERRUPT ===================== */
void IRAM_ATTR pulseCounter()
{
    unsigned long now = millis();
    if (now - lastPulseTime > 5)
    {
        portENTER_CRITICAL_ISR(&flowMux);
        pulseCount++;
        sessionPulses++;          // Per-session accumulator (reset on pump start)
        lastPulseTime = now;
        portEXIT_CRITICAL_ISR(&flowMux);
    }
}

/* ===================== ADC (median smoothing) ===================== */
int readSoilSmoothed()
{
    soilBuffer[soilBufIdx] = analogRead(SOIL_PIN);
    soilBufIdx = (soilBufIdx + 1) % ADC_SAMPLES;

    int sorted[ADC_SAMPLES];
    memcpy(sorted, soilBuffer, sizeof(soilBuffer));
    for (int i = 1; i < ADC_SAMPLES; i++)
    {
        int key = sorted[i], j = i - 1;
        while (j >= 0 && sorted[j] > key) { sorted[j + 1] = sorted[j]; j--; }
        sorted[j + 1] = key;
    }
    return (sorted[ADC_SAMPLES / 2 - 1] + sorted[ADC_SAMPLES / 2]) / 2;
}

/* ===================== RELAY CONTROL ===================== */

#define FLOW_PULSES_PER_LITRE  450UL   // Pulses per litre — tune to your sensor

void setPumpState(PumpState newState, unsigned long now)
{
    if (newState == pumpState) return;

    if (newState == PUMP_RUNNING)
    {
        if ((now - pumpLastOff) < RELAY_MIN_OFF_MS) return;

        // ---- Start of a new watering session: zero the session counter ----
        portENTER_CRITICAL(&flowMux);
        sessionPulses = 0;
        portEXIT_CRITICAL(&flowMux);

        digitalWrite(PUMP_RELAY, LOW);
        pumpLastOn = now;
        pumpOn     = true;
        instantPublishRequested = true;   // Publish immediately on pump start
        lcdShow(LCD_PUMP_RUNNING);

        Serial.printf_P(PSTR(F("[PUMP] Relay ON  — source: %s\n")),
                        backendOnline ? "backend command" : "offline automation");
        Serial.println(F("[FLOW] Session started — pulse counter reset"));
    }
    else
    {
        if (pumpOn && (now - pumpLastOn) < RELAY_MIN_ON_MS) return;
        digitalWrite(PUMP_RELAY, HIGH);
        pumpLastOff = now;
        pumpOn      = false;

        // ---- End of session: snapshot pulses and convert to mL ----
        uint32_t snapshotPulses;
        portENTER_CRITICAL(&flowMux);
        snapshotPulses = sessionPulses;
        sessionPulses  = 0;          // Ready for next session
        portEXIT_CRITICAL(&flowMux);

        uint16_t usedMl = (uint16_t)((snapshotPulses * 1000UL) / FLOW_PULSES_PER_LITRE);

        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)))
        {
            sys.waterUsed_ml     = usedMl;
            sys.wateringOccurred = true;   // publishSensorData() will include "wu" on next publish
            xSemaphoreGive(dataMutex);
        }
        instantPublishRequested = true;   // Publish immediately on pump stop (includes wu)
        if (mqttConnected) lcdShow(LCD_CONNECTED);  // Return to sensor view after pump stops

        Serial.printf_P(PSTR(F("[PUMP] Relay OFF — source: %s\n")),
                        backendOnline ? "backend command" : "offline automation");
        Serial.printf_P(PSTR(F("[FLOW] Session ended — %u pulses → %u mL\n")),
                        snapshotPulses, usedMl);
    }

    pumpState = newState;
}

void setLightState(LightState newState, unsigned long now)
{
    if (newState == lightState) return;
    if (newState == LIGHT_ON)
    {
        if ((now - lightLastOff) < RELAY_MIN_OFF_MS) return;
        digitalWrite(LIGHT_RELAY, LOW);
        lightLastOn = now;
        lightOn = true;
        Serial.printf_P(PSTR(F("[LIGHT] Relay ON  — source: %s\n")),
                        backendOnline ? "backend command" : "offline automation");
    }
    else
    {
        if (lightOn && (now - lightLastOn) < RELAY_MIN_ON_MS) return;
        digitalWrite(LIGHT_RELAY, HIGH);
        lightLastOff = now;
        lightOn = false;
        Serial.printf_P(PSTR(F("[LIGHT] Relay OFF — source: %s\n")),
                        backendOnline ? "backend command" : "offline automation");
    }
    lightState = newState;
}

/* ===================== FACTORY RESET ===================== */
void performFactoryReset()
{
    Serial.println(F("\n[RESET] FACTORY RESET IN PROGRESS"));
    lcdShow(LCD_FACTORY_RESET);
    delay(1000);

    if (hTaskSensors) { vTaskDelete(hTaskSensors); hTaskSensors = NULL; }
    if (hTaskMQTT)    { vTaskDelete(hTaskMQTT);    hTaskMQTT    = NULL; }
    delay(500);

    if (mqttClient.connected()) mqttClient.disconnect();

    if (bleActive)
    {
        BLEDevice::getAdvertising()->stop();
        delay(100);
        BLEDevice::deinit(true);
        bleActive = false;
    }
    delay(200);

    preferences.begin("wifi-config", false);
    preferences.clear();
    preferences.end();
    delay(200);

    WiFi.disconnect(true, true);
    delay(500);
    WiFi.mode(WIFI_OFF);
    delay(300);

    nvs_flash_erase();
    delay(100);
    nvs_flash_init();
    delay(500);

    Serial.println(F("[RESET] COMPLETE - rebooting"));
    // Write "Restarting..." directly — TaskLCD may not tick again before restart
    if (xSemaphoreTake(lcdMutex, pdMS_TO_TICKS(300)))
    {
        lcd.setCursor(0, 0); lcd.print(" Factory Reset  ");
        lcd.setCursor(0, 1); lcd.print("  Restarting... ");
        xSemaphoreGive(lcdMutex);
    }
    delay(1000);
    ESP.restart();
}

/* ===================== BLE CALLBACKS ===================== */
class ServerCallbacks : public BLEServerCallbacks
{
    void onConnect(BLEServer *pServer)
    {
        lcdShow(LCD_BLE_CONNECTED);
        Serial.println(F("[BLE] App connected"));
    }

    void onDisconnect(BLEServer *pServer)
    {
        if (bleActive)
        {
            lcdShow(LCD_BLE_DISCONNECTED);
            Serial.println(F("[BLE] App disconnected — restarting advertising"));
            BLEDevice::getAdvertising()->start();
            // Brief pause then go back to advertising state
            vTaskDelay(pdMS_TO_TICKS(1500));
            lcdShow(LCD_BLE_ADVERTISING);
        }
    }
};

class MyCallbacks : public BLECharacteristicCallbacks
{
    void onRead(BLECharacteristic *pCharacteristic)
    {
        String uuid = pCharacteristic->getUUID().toString();
        // step-2 reads DEVICE_ID_CHAR to verify QR code — show verifying state
        if (uuid == DEVICE_ID_CHAR_UUID)
        {
            lcdShow(LCD_BLE_VERIFYING);
        }
    }

    void onWrite(BLECharacteristic *pCharacteristic)
    {
        String val  = pCharacteristic->getValue();
        String uuid = pCharacteristic->getUUID().toString();

        if (uuid == RESET_CHAR_UUID)
        {
            if (val == F("FACTORY_RESET"))
            {
                Serial.println(F("[BLE] Factory reset command"));
                resetRequested = true;
            }
            else if (val == F("BLE_RESTART"))
            {
                Serial.println(F("[BLE] Soft restart requested"));
                bleRestartRequested = true;
            }
            else if (val == F("APP_DISCONNECT"))
            {
                Serial.println(F("[BLE] App back pressed — graceful disconnect"));
                appDisconnectRequested = true;
            }
            return;
        }

        if (xSemaphoreTake(credMutex, pdMS_TO_TICKS(100)))
        {
            if (uuid == SSID_CHAR_UUID)
            {
                strncpy(receivedSSID,   val.c_str(), BUF_SSID - 1);
                strncpy(wifiSSIDForLCD, val.c_str(), BUF_SSID - 1);  // Safe copy — never shows password
                Serial.printf_P(PSTR(F("[BLE] SSID: %s\n")), receivedSSID);
            }
            else if (uuid == PASSWORD_CHAR_UUID)
            {
                strncpy(receivedPass, val.c_str(), BUF_PASS - 1);
                credentialsReceived = true;
                isProvisioning      = true;
                sPostWiFiDone       = false;
                lcdShow(LCD_WIFI_CONNECTING);   // step-3 sent credentials
                Serial.println(F("[BLE] Password received"));
            }
            xSemaphoreGive(credMutex);
        }
    }
};

static ServerCallbacks serverCbInstance;
static MyCallbacks     charCbInstance;

/* ===================== POST-WIFI INIT ===================== */
void onWiFiConnected()
{
    if (sPostWiFiDone) return;

    if (hTaskSensors) { vTaskDelete(hTaskSensors); hTaskSensors = NULL; }
    if (hTaskMQTT)    { vTaskDelete(hTaskMQTT);    hTaskMQTT    = NULL; }

    sPostWiFiDone = true;

    // Wire  (Bus 0) → BH1750 + LCD on pins 21 & 22 (default Wire, used by LiquidCrystal_I2C)
    Wire.begin(21, 22);
    // Wire1 (Bus 1) → BME280 on pins 16 & 17
    Wire1.begin(16, 17);

    delay(100);

    // Start BME280 on Wire1 (Bus 1)
    if (!bme.begin(0x76, &Wire1) && !bme.begin(0x77, &Wire1))
    {
        bme280Fault = true;
        lcdShow(LCD_SENSOR_ERROR);
        Serial.println(F("[SENSOR] BME280 NOT FOUND — will retry in TaskSensors"));
        // publishError() not called here: MQTT isn't connected yet at this point.
        // TaskSensors will report it once MQTT is up and threshold is crossed.
    }
    else
    {
        Serial.println(F("[SENSOR] BME280 OK"));
    }

    delay(50);

    // Start BH1750 on Wire (Bus 0, shared with LCD)
    if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire))
    {
        if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x5C, &Wire))
        {
            bh1750Fault = true;
            lcdShow(LCD_SENSOR_ERROR);
            Serial.println(F("[SENSOR] BH1750 NOT FOUND — will retry in TaskSensors"));
        }
        else
        {
            Serial.println(F("[SENSOR] BH1750 OK (0x5C)"));
        }
    }
    else
    {
        Serial.println(F("[SENSOR] BH1750 OK (0x23)"));
    }

    snprintf(mqttClientId, sizeof(mqttClientId),
             "Nurtura-%s-%04X", macAddress, (unsigned)(millis() & 0xFFFF));

    wifiClient.setInsecure();
    mqttClient.setServer(SECRET_MQTT_HOST, 8883);
    mqttClient.setCallback(mqttMessageCallback);
    mqttClient.setBufferSize(512);

    Serial.printf_P(PSTR(F("[MQTT] Staging broker (AsyncAPI v3.0.0)\n")));

    mqttLastAttempt = millis();
    BaseType_t rs = xTaskCreatePinnedToCore(TaskSensors,   "Sensors",  4096, NULL, 3, &hTaskSensors, 0);
    BaseType_t rm = xTaskCreatePinnedToCore(TaskMQTT,      "MQTT",     6144, NULL, 1, &hTaskMQTT,    1);
    xTaskCreatePinnedToCore(TaskMonitor,   "Monitor",  2048, NULL, 1, NULL,         1);
    xTaskCreatePinnedToCore(TaskAutomation,"Automation",2048, NULL, 1, NULL,         1);

    if (rs != pdPASS || rm != pdPASS)
        Serial.println(F("[ERROR] Task creation failed"));

    sensorHeartbeat = mqttHeartbeat = millis();
}

/* ===================== WiFi EVENT HANDLER ===================== */
// wifiReconnectAt is declared in globals. Never call WiFi API or
// delay() directly from an event callback — signal loop() instead.
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info)
{
    switch (event)
    {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        Serial.println(F("[WiFi] Associated"));
        lcdShow(LCD_WIFI_CONNECTING);
        break;

    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    {
        // Guard: ignore ghost 0.0.0.0 DHCP events — wait for a real IP
        IPAddress ip = WiFi.localIP();
        if (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0)
        {
            Serial.println(F("[WiFi] GOT_IP spurious 0.0.0.0 — ignoring"));
            break;
        }

        wifiSMState = WS_CONNECTED;
        Serial.printf_P(PSTR(F("[WiFi] Connected: %s\n")), ip.toString().c_str());
        lcdShow(LCD_WIFI_CONNECTED);

        if (statusCharacteristic != NULL && bleActive)
        {
            statusCharacteristic->setValue("connected");
            statusCharacteristic->notify();
        }

        if (bleActive)
        {
            BLEDevice::getAdvertising()->stop();
            BLEDevice::deinit(true);
            bleActive            = false;
            statusCharacteristic = NULL;
            resetCharacteristic  = NULL;
            Serial.println(F("[BLE] Deinitialized"));
        }

        onWiFiConnected();
        break;
    }

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        wifiSMState   = WS_FAILED;
        mqttConnected = false;
        sPostWiFiDone = false;   // Allow onWiFiConnected() to re-run on next valid GOT_IP
        Serial.println(F("[WiFi] Disconnected — reconnect in 5 s"));
        lcdShow(LCD_DISCONNECTED);

        if (statusCharacteristic != NULL && bleActive)
        {
            statusCharacteristic->setValue("failed");
            statusCharacteristic->notify();
        }

        // Schedule a reconnect attempt after cooldown. Do NOT call any
        // WiFi API or delay() here — this runs inside the WiFi task context.
        wifiReconnectAt = millis() + WIFI_RECONNECT_COOLDOWN_MS;
        break;

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

        if (resetRequested)
        {
            Serial.println(F("[MONITOR] Factory reset"));
            resetRequested = false;
            performFactoryReset();
            return;
        }

        if (hTaskSensors && (now - sensorHeartbeat) > 45000)
        {
            if (sensorRestarts < 5)
            {
                Serial.printf_P(PSTR(F("[WARN] TaskSensors restart #%u\n")), sensorRestarts + 1);
                vTaskDelete(hTaskSensors); hTaskSensors = NULL;
                xTaskCreatePinnedToCore(TaskSensors, "Sensors", 4096, NULL, 3, &hTaskSensors, 0);
                sensorRestarts++;
                sensorHeartbeat = millis();
            }
        }

        if (hTaskMQTT && (now - mqttHeartbeat) > 45000)
        {
            if (mqttRestarts < 5)
            {
                Serial.printf_P(PSTR(F("[WARN] TaskMQTT restart #%u\n")), mqttRestarts + 1);
                vTaskDelete(hTaskMQTT); hTaskMQTT = NULL;
                xTaskCreatePinnedToCore(TaskMQTT, "MQTT", 6144, NULL, 1, &hTaskMQTT, 1);
                mqttRestarts++;
                mqttHeartbeat = millis();
            }
        }
    }
}

/* ===================== TASK SENSORS ===================== */
void TaskSensors(void *pvParameters)
{
    esp_task_wdt_add(NULL);
    vTaskDelay(pdMS_TO_TICKS(1000));

    // ---- Per-sensor error tracking ----
    // Consecutive fail counts — error published after SENSOR_FAIL_THRESHOLD bad reads.
    // Reset to 0 on the first successful read (triggers recovery message if was faulted).
    static const uint8_t SENSOR_FAIL_THRESHOLD = 3;  // consecutive failures before reporting

    uint8_t bme_t_fails = 0;   // BME280 temperature consecutive failures
    uint8_t bme_h_fails = 0;   // BME280 humidity  consecutive failures
    uint8_t bh1750_fails = 0;  // BH1750 light     consecutive failures
    // Soil moisture uses ADC — always returns a value; range error reported instead

    // Track last-successful-read millis for diagnostic payload
    unsigned long bme_t_lastOK  = millis();
    unsigned long bme_h_lastOK  = millis();
    unsigned long bh1750_lastOK = millis();

    for (;;)
    {
        esp_task_wdt_reset();
        sensorHeartbeat = millis();

        // ==================== BME280 — Temperature ====================
        float t = bme.readTemperature();
        if (isnan(t) || t < -40.0f || t > 85.0f)
        {
            bme_t_fails++;
            if (bme_t_fails == SENSOR_FAIL_THRESHOLD)
            {
                // First threshold crossing — report SENSOR_FAILURE
                bme280Fault = true;
                lcdShow(LCD_SENSOR_ERROR);
                Serial.printf_P(PSTR(F("[SENSOR] BME280 temp failed %u times\n")), bme_t_fails);
                char ed[64];
                snprintf(ed, sizeof(ed), "Last OK: %lums ago", millis() - bme_t_lastOK);
                publishError("SENSOR_FAILURE",
                             "BME280 temperature read failed",
                             "HIGH",
                             "TEMPERATURE");
            }
            // Don't update sys.temp_x10 — keep last known good value
        }
        else
        {
            if (bme_t_fails >= SENSOR_FAIL_THRESHOLD)
            {
                // Was faulted — now recovered
                Serial.println(F("[SENSOR] BME280 temperature RECOVERED"));
                publishError("SENSOR_RECOVERED",
                             "BME280 temperature sensor recovered",
                             "LOW",
                             "TEMPERATURE");
                if (!bh1750_fails && !bme_h_fails) lcdShow(LCD_CONNECTED);
            }
            bme_t_fails  = 0;
            bme_t_lastOK = millis();
            if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)))
            {
                sys.temp_x10 = (int16_t)(t * 10.0f);
                bme280Fault  = false;
                xSemaphoreGive(dataMutex);
            }
        }

        // ==================== BME280 — Humidity ====================
        float h = bme.readHumidity();
        if (isnan(h) || h < 0.0f || h > 100.0f)
        {
            bme_h_fails++;
            if (bme_h_fails == SENSOR_FAIL_THRESHOLD)
            {
                bme280Fault = true;
                lcdShow(LCD_SENSOR_ERROR);
                Serial.printf_P(PSTR(F("[SENSOR] BME280 humidity failed %u times\n")), bme_h_fails);
                publishError("SENSOR_FAILURE",
                             "BME280 humidity read failed",
                             "MEDIUM",
                             "HUMIDITY");
            }
        }
        else
        {
            if (bme_h_fails >= SENSOR_FAIL_THRESHOLD)
            {
                Serial.println(F("[SENSOR] BME280 humidity RECOVERED"));
                publishError("SENSOR_RECOVERED",
                             "BME280 humidity sensor recovered",
                             "LOW",
                             "HUMIDITY");
                if (!bh1750_fails && !bme_t_fails) lcdShow(LCD_CONNECTED);
            }
            bme_h_fails  = 0;
            bme_h_lastOK = millis();
            if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)))
            {
                sys.hum = (uint8_t)constrain((int)h, 0, 100);
                xSemaphoreGive(dataMutex);
            }
        }

        // ==================== BH1750 — Light ====================
        float l = -1.0f;
        if (!bh1750Fault)
            l = lightMeter.readLightLevel();

        if (l < 0.0f)
        {
            bh1750_fails++;
            if (bh1750_fails == SENSOR_FAIL_THRESHOLD)
            {
                bh1750Fault = true;
                lcdShow(LCD_SENSOR_ERROR);
                Serial.printf_P(PSTR(F("[SENSOR] BH1750 failed %u times\n")), bh1750_fails);
                publishError("SENSOR_FAILURE",
                             "BH1750 light sensor read failed",
                             "MEDIUM",
                             "LIGHT");
            }
        }
        else
        {
            if (bh1750_fails >= SENSOR_FAIL_THRESHOLD)
            {
                Serial.println(F("[SENSOR] BH1750 RECOVERED"));
                publishError("SENSOR_RECOVERED",
                             "BH1750 light sensor recovered",
                             "LOW",
                             "LIGHT");
                if (!bme_t_fails && !bme_h_fails) lcdShow(LCD_CONNECTED);
            }
            bh1750_fails  = 0;
            bh1750_lastOK = millis();
            if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)))
            {
                sys.lux     = (uint16_t)constrain((int)l, 0, 65535);
                bh1750Fault = false;
                xSemaphoreGive(dataMutex);
            }
        }

        // ==================== Soil Moisture (ADC) ====================
        // ADC always returns a value — check for out-of-range only
        int m = constrain(map(readSoilSmoothed(), 2480, 460, 0, 100), 0, 100);
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)))
        {
            sys.moisture = (uint8_t)m;
            xSemaphoreGive(dataMutex);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ===================== TASK MQTT ===================== */

void TaskMQTT(void *pvParameters)
{
    esp_task_wdt_add(NULL);

    unsigned long lastPublish = 0;

    // Build topic strings (ASYNCAPI COMPLIANT)
    char cmdTopic[96];
    snprintf(cmdTopic, sizeof(cmdTopic), "nurtura/rack/%s/commands/+", macAddress);

    // LWT payload — broker publishes this on unexpected disconnect.
    // Timestamp is captured at connect time (MQTT limitation: LWT is registered
    // once at connect; the broker cannot update it when it fires).
    char lwtTopic[96];
    snprintf(lwtTopic, sizeof(lwtTopic), "nurtura/rack/%s/status", macAddress);

    StaticJsonDocument<128> lwtDoc;
    lwtDoc[F("o")]  = false;
    lwtDoc[F("tm")] = getISO8601Timestamp();
    char lwtPayload[128];
    serializeJson(lwtDoc, lwtPayload, sizeof(lwtPayload));

    Serial.println(F("[MQTT] Task started (AsyncAPI v3.0.0)\n"));
    vTaskDelay(pdMS_TO_TICKS(2000));

    for (;;)
    {
        esp_task_wdt_reset();
        mqttHeartbeat = millis();

        if (WiFi.status() != WL_CONNECTED)
        {
            if (mqttConnected)
            {
                mqttConnected = false;
                Serial.println(F("[MQTT] WiFi lost"));
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // ---- RECONNECT LOGIC ----
        if (!mqttClient.connected())
        {
            if (mqttConnected)
            {
                mqttConnected = false;
                Serial.println(F("[MQTT] DISCONNECTED"));
                lcdShow(LCD_SERVER_LOST);   // Was connected before — show "Server Lost"
            }

            if ((millis() - mqttLastAttempt) >= MQTT_RECONNECT_INTERVAL)
            {
                mqttLastAttempt = millis();
                Serial.printf_P(PSTR(F("[MQTT] Connecting to staging broker...\n")));

                // First-time connect: show "Server / Connecting..."
                // After a drop lcdState is already LCD_SERVER_LOST — keep it there
                if (lcdState != LCD_SERVER_LOST)
                    lcdShow(LCD_MQTT_CONNECTING);

                bool ok = mqttClient.connect(
                    mqttClientId,
                    SECRET_MQTT_USER,
                    SECRET_MQTT_PASS,
                    lwtTopic, 1, true, lwtPayload  // QoS 1 (was 0) — ensures broker stores LWT reliably
                );

                if (ok)
                {
                    mqttConnected = true;
                    Serial.println(F("[MQTT] CONNECTED (AsyncAPI)\n"));
                    lcdShow(LCD_CONNECTED);

                    // --- Clear stale retained LWT from broker cache ---
                    // Per MQTT 3.1.1 §3.3.1-6: publishing a zero-byte retained message
                    // instructs the broker to DELETE its stored retained message for that topic.
                    // We must do this BEFORE publishing the online status, and we must
                    // call loop() multiple times to actually flush the TCP packets out.
                    //
                    // We clear BOTH topic variants in case old firmware left a cached LWT:
                    //   1. Current:  nurtura/rack/AA:BB:CC:DD:EE:FF/status  (colon MAC)
                    //   2. Legacy:   nurtura/rack/AABBCCDDEEFF/status        (no-colon MAC)

                    // Build legacy no-colon MAC ("AA:BB:CC:DD:EE:FF" → "AABBCCDDEEFF")
                    char legacyMac[13] = {0};
                    {
                        const char *src = macAddress;
                        char *dst = legacyMac;
                        while (*src && (dst - legacyMac) < 12)
                        {
                            if (*src != ':') *dst++ = *src;
                            src++;
                        }
                    }
                    char legacyTopic[96];
                    snprintf(legacyTopic, sizeof(legacyTopic),
                             "nurtura/rack/%s/status", legacyMac);

                    String statusTopic = getTopic("status");

                    // Helper lambda: publish zero-byte retained and flush until sent
                    auto clearRetained = [&](const char* topic) {
                        // NULL payload + length 0 + retain = broker deletes retained msg
                        bool sent = mqttClient.publish(topic, NULL, 0, true);
                        Serial.printf_P(PSTR(F("[MQTT] Clearing retained: %s (%s)\n")),
                                        topic, sent ? "queued" : "FAILED");
                        // Pump loop() until the packet is flushed (up to 500 ms)
                        unsigned long t = millis();
                        while (millis() - t < 500) {
                            mqttClient.loop();
                            vTaskDelay(pdMS_TO_TICKS(10));
                        }
                    };

                    clearRetained(statusTopic.c_str());   // current (colon) MAC topic
                    clearRetained(legacyTopic);            // legacy (no-colon) MAC topic

                    // Subscribe to command topics (AsyncAPI spec)
                    mqttClient.subscribe(cmdTopic, 1);
                    Serial.printf_P(PSTR(F("[MQTT] Subscribed: %s\n")), cmdTopic);

                    // Subscribe to sensor streaming control (AsyncAPI: commands/sensors)
                    char sensorCmdTopic[96];
                    snprintf(sensorCmdTopic, sizeof(sensorCmdTopic),
                             "nurtura/rack/%s/commands/sensors", macAddress);
                    mqttClient.subscribe(sensorCmdTopic, 1);
                    Serial.printf_P(PSTR(F("[MQTT] Subscribed: %s\n")), sensorCmdTopic);

                    // Subscribe to backend presence — QoS 1, retained so we get
                    // the last known state immediately on (re)connect
                    mqttClient.subscribe("nurtura/backend/status", 1);
                    Serial.println(F("[MQTT] Subscribed: nurtura/backend/status"));

                    // Now publish fresh online status (retained) — fills the cleared slot
                    publishStatus();

                    // Flush outbound packets AND allow inbound retained messages
                    // (nurtura/backend/status, commands/sensors) to arrive and be
                    // processed by the callback BEFORE we decide whether to publish
                    // sensor data. 500 ms is enough for a retained message round-trip.
                    Serial.println(F("[MQTT] Waiting for retained messages..."));
                    unsigned long ft = millis();
                    while (millis() - ft < 500) {
                        mqttClient.loop();
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                    Serial.printf_P(PSTR(F("[MQTT] Ready — backendOnline=%s sensorStreaming=%s\n")),
                                    backendOnline    ? "YES" : "NO",
                                    sensorStreaming  ? "YES" : "NO");

                    // Only arm the immediate publish if backend is actually online
                    // and has activated streaming — avoids publishing into the void.
                    if (backendOnline && sensorStreaming)
                        lastPublish = 0;   // Force immediate sensor publish
                }
                else
                {
                    Serial.printf_P(PSTR(F("[MQTT] Connect failed (rc=%d)\n")),
                                    mqttClient.state());
                }
            }

            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // ---- CONNECTED: pump the client loop ----
        mqttClient.loop();

        unsigned long now = millis();

        // Dynamic publish interval:
        //   - Instant  → pump just started or stopped (instantPublishRequested flag)
        //   - 1 minute → pump is actively running
        //   - 5 minutes → idle / normal mode
        unsigned long currentInterval = (pumpState == PUMP_RUNNING)
                                        ? SENSOR_INTERVAL_PUMPING
                                        : SENSOR_INTERVAL_NORMAL;

        bool doPublish = false;

        if (instantPublishRequested)
        {
            // Pump state just changed — publish right now.
            // Do NOT reset lastPublish so the regular interval timer
            // continues from its previous tick (avoids skipping a
            // scheduled publish right after an instant one).
            instantPublishRequested = false;
            doPublish = true;
            Serial.println(F("[MQTT] Instant sensor publish (pump state change)"));
        }
        else if ((now - lastPublish) >= currentInterval)
        {
            doPublish   = true;
            lastPublish = now;
        }

        if (doPublish) publishSensorData();
    

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ===================== TASK BOOT BUTTON ===================== */

void TaskBootButton(void *pvParameters)
{
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    Serial.println(F("[BTN] Ready — hold IO0 for 3 s (reset) or BLE restart if timed out\n"));

    // Initialize so debounce is always true on first press
    unsigned long lastPress = (unsigned long)(0UL - 6000UL);

    for (;;)
    {
        if (digitalRead(BOOT_BUTTON_PIN) == LOW)
        {
            unsigned long now = millis();
            if (now - lastPress > 5000)
            {
                // ---- Decide action based on current state ----
                bool restartBLE = bleTimedOut;

                if (restartBLE)
                {
                    Serial.println(F("[BTN] Held — BLE timed out, will restart BLE"));
                }
                else
                {
                    Serial.println(F("[BTN] IO0 pressed — hold for factory reset"));
                }

                unsigned long holdStart    = millis();
                int           lastSecShown = -1;
                bool          triggered    = false;

                while (digitalRead(BOOT_BUTTON_PIN) == LOW)
                {
                    unsigned long elapsed = millis() - holdStart;

                    if (elapsed >= BTN_HOLD_MS)
                    {
                        triggered = true;

                        if (restartBLE)
                        {
                            // ---- Restart BLE, keep credentials ----
                            Serial.println(F("[BTN] BLE restart triggered!"));
                            bleTimedOut = false;
                            lcdShow(LCD_BLE_ADVERTISING);
                            startBLEProvisioning();
                        }
                        else
                        {
                            // ---- Full factory reset ----
                            Serial.println(F("[BTN] Factory reset triggered!"));
                            lcdShow(LCD_FACTORY_RESET);
                            vTaskDelay(pdMS_TO_TICKS(300));
                            performFactoryReset();
                        }
                        break;
                    }

                    int secsLeft = (int)((BTN_HOLD_MS - elapsed + 999) / 1000);
                    if (secsLeft != lastSecShown)
                    {
                        btnHoldSecsLeft = secsLeft;
                        lcdShow(LCD_BTN_HOLD);
                        if (restartBLE)
                        {
                            Serial.printf_P(PSTR(F("[BTN] Release to cancel — BLE restart in %d...\n")), secsLeft);
                        }
                        else
                        {
                            Serial.printf_P(PSTR(F("[BTN] Release to cancel — resetting in %d...\n")), secsLeft);
                        }
                        lastSecShown = secsLeft;
                    }

                    vTaskDelay(pdMS_TO_TICKS(50));
                }

                if (!triggered && digitalRead(BOOT_BUTTON_PIN) == HIGH)
                {
                    Serial.println(F("[BTN] Released — cancelled\n"));
                }

                lastPress = millis();
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ===================== TASK AUTOMATION ===================== */

#define AUTO_WATER_MOISTURE_THRESH  40   // % — pump ON below, OFF at-or-above
#define AUTO_LIGHT_LOW_LUX          200  // lux — turn light ON below this
#define AUTO_LIGHT_HIGH_LUX         800  // lux — turn light OFF above this
#define AUTO_LIGHT_HOUR_ON          8    // 08:00 — start of photoperiod
#define AUTO_LIGHT_HOUR_OFF         23   // 23:00 — end   of photoperiod

void TaskAutomation(void *pvParameters)
{
    bool autoWatering = false;  // true while this task is running the pump
    bool autoLight    = false;  // true while this task has the light on

    Serial.println(F("[AUTO] Task started"));

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(5000));   // re-evaluate every 5 s

        unsigned long now = millis();

        // ---- If backend came back online, release all actuators and idle ----
        if (backendOnline)
        {
            if (autoWatering)
            {
                setPumpState(PUMP_STOPPED, now);
                autoWatering = false;
                Serial.println(F("[AUTO] Backend online — stopping auto-pump"));
            }
            if (autoLight)
            {
                setLightState(LIGHT_OFF, now);
                autoLight = false;
                Serial.println(F("[AUTO] Backend online — stopping auto-light"));
            }
            continue;   // nothing more to do while backend is controlling
        }

        // ==================== WATERING LOGIC ====================
        // Sensor-driven only: pump mirrors moisture threshold directly.
        // No timers, no cooldowns — the soil reading is the sole authority.

        // ---- Single atomic read of all sensor values needed this cycle ----
        uint8_t  moisture = 0;
        uint16_t lux      = 0;
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)))
        {
            moisture = sys.moisture;
            lux      = sys.lux;
            xSemaphoreGive(dataMutex);
        }

        // ==================== WATERING LOGIC ====================
        if (!autoWatering && pumpState == PUMP_STOPPED
            && moisture < AUTO_WATER_MOISTURE_THRESH)
        {
            Serial.printf_P(PSTR(F("[AUTO] Watering ON  — soil %u%% (dry, threshold %u%%)\n")),
                            moisture, AUTO_WATER_MOISTURE_THRESH);
            setPumpState(PUMP_RUNNING, now);
            autoWatering = true;
        }
        else if (autoWatering && moisture >= AUTO_WATER_MOISTURE_THRESH)
        {
            Serial.printf_P(PSTR(F("[AUTO] Watering OFF — soil %u%% (wet, threshold %u%%)\n")),
                            moisture, AUTO_WATER_MOISTURE_THRESH);
            setPumpState(PUMP_STOPPED, now);
            autoWatering = false;
        }

        // ==================== LIGHTING LOGIC ====================

        // Determine if we are within the photoperiod window (NTP wall-clock)
        bool inPhotoperiod = false;
        struct tm timeinfo;
        if (getLocalTime(&timeinfo))
        {
            int hour = timeinfo.tm_hour;
            inPhotoperiod = (hour >= AUTO_LIGHT_HOUR_ON && hour < AUTO_LIGHT_HOUR_OFF);
        }
        else
        {
            inPhotoperiod = true;  // NTP not synced — default ON (safe for plants)
        }

        bool dimEnough    = (lux < AUTO_LIGHT_LOW_LUX);
        bool brightEnough = (lux > AUTO_LIGHT_HIGH_LUX);

        if (!autoLight && inPhotoperiod && dimEnough)
        {
            Serial.printf_P(PSTR(F("[AUTO] Light ON  — lux %u (dim, threshold %u), in photoperiod\n")),
                            lux, AUTO_LIGHT_LOW_LUX);
            setLightState(LIGHT_ON, now);
            autoLight = true;
        }
        else if (autoLight && (!inPhotoperiod || brightEnough))
        {
            const char *reason = !inPhotoperiod ? "outside photoperiod" : "too bright";
            Serial.printf_P(PSTR(F("[AUTO] Light OFF — lux %u, reason: %s\n")), lux, reason);
            setLightState(LIGHT_OFF, now);
            autoLight = false;
        }
    }
}

/* ===================== BLE PROVISIONING ===================== */
void startBLEProvisioning()
{
    isProvisioning = bleActive = true;
    bleStartTime   = millis();

    BLEDevice::init("Nurtura Rack");
    BLEServer  *ps  = BLEDevice::createServer();
    ps->setCallbacks(&serverCbInstance);
    BLEService *psv = ps->createService(SERVICE_UUID);

    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    BLECharacteristic *devIdChar = psv->createCharacteristic(
        DEVICE_ID_CHAR_UUID, BLECharacteristic::PROPERTY_READ);
    devIdChar->setValue(macStr);
    devIdChar->setCallbacks(&charCbInstance);   // triggers LCD_BLE_VERIFYING on read

    psv->createCharacteristic(SSID_CHAR_UUID,     BLECharacteristic::PROPERTY_WRITE)
       ->setCallbacks(&charCbInstance);
    psv->createCharacteristic(PASSWORD_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE)
       ->setCallbacks(&charCbInstance);

    statusCharacteristic = psv->createCharacteristic(
        STATUS_CHAR_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    statusCharacteristic->addDescriptor(new BLE2902());

    resetCharacteristic = psv->createCharacteristic(
        RESET_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
    resetCharacteristic->setCallbacks(&charCbInstance);

    psv->start();
    BLEAdvertising *pAdv = BLEDevice::getAdvertising();
    pAdv->addServiceUUID(SERVICE_UUID);
    pAdv->setScanResponse(true);
    pAdv->setMinPreferred(0x06);
    pAdv->start();

    Serial.println(F("[BLE] Broadcasting...\n"));
    lcdShow(LCD_BLE_ADVERTISING);
}

/* ===================== SETUP ===================== */
void setup()
{
    Serial.begin(115200);

    // IMMEDIATELY set relays to OFF (HIGH) to prevent flickering at boot
    pinMode(PUMP_RELAY, OUTPUT);
    digitalWrite(PUMP_RELAY, HIGH);

    pinMode(LIGHT_RELAY, OUTPUT);
    digitalWrite(LIGHT_RELAY, HIGH);

    // Initialize ALL mutexes before spawning any task that may use them
    lcdMutex  = xSemaphoreCreateMutex();
    dataMutex = xSemaphoreCreateMutex();
    credMutex = xSemaphoreCreateMutex();
    uptimeStart = millis();

    // ---- LCD init (shared Wire bus on GPIO 21/22 with BH1750) ----
    lcd.init();
    lcd.clear();
    lcd.backlight();
    lcdShow(LCD_BOOT);

    // Start LCD task — all mutexes are valid, LCD is ready
    xTaskCreatePinnedToCore(TaskLCD, "LCD", 4096, NULL, 1, NULL, 1);

    // Set initial software states
    pumpState  = PUMP_STOPPED;
    lightState = LIGHT_OFF;

    // Flow sensor setup
    pinMode(FLOW_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(FLOW_PIN), pulseCounter, FALLING);

    // Other pin modes
    pinMode(SOIL_PIN, INPUT);

    // Start background tasks
    xTaskCreatePinnedToCore(TaskBootButton, "Button",  2048, NULL, 1, NULL, 1);

    // Identity
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(macAddress, sizeof(macAddress), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    Serial.printf_P(PSTR("\n--- Nurtura v%s ---\n"), FW_VERSION);
    Serial.printf_P(PSTR("Device ID: %s\n"), macAddress);

    // WiFi initialization
    preferences.begin("wifi-config", true);
    String ssid = preferences.getString("ssid", "");
    String pass = preferences.getString("password", "");
    preferences.end();

    if (ssid != "" || FORCE_BLE_BOOT) {
        WiFi.mode(WIFI_STA);
        WiFi.onEvent(onWiFiEvent);
        WiFi.begin(ssid.c_str(), pass.c_str());
        Serial.println(F("[WiFi] Connecting to saved credentials..."));
    } else {
        startBLEProvisioning();
    }
    
    delay(500);

    Serial.println(F("\n[BOOT] Nurtura Rack v" FW_VERSION " (AsyncAPI)"));
    Serial.println(F("[BOOT] Build: " FW_BUILD_DATE "\n"));

    uptimeStart = millis();

    Serial.printf_P(PSTR(F("[BOOT] Device: %s\n\n")), macAddress);

    esp_task_wdt_config_t twdt_config = {
        .timeout_ms     = WDT_TIMEOUT * 1000,
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic  = true
    };
    esp_task_wdt_reconfigure(&twdt_config);

    configTime(0, 0, "pool.ntp.org", "time.google.com");

    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    pinMode(PUMP_RELAY,  OUTPUT); digitalWrite(PUMP_RELAY,  HIGH);
    pinMode(LIGHT_RELAY, OUTPUT); digitalWrite(LIGHT_RELAY, HIGH);
    pinMode(FLOW_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(FLOW_PIN), pulseCounter, FALLING);
    analogSetAttenuation(ADC_11db);

    Serial.println(F("[BTN] Checking IO0 for factory reset..."));
    delay(200);
    if (digitalRead(BOOT_BUTTON_PIN) == LOW)
    {
        Serial.println(F("[BTN] IO0 held — keep holding for factory reset"));
        unsigned long holdStart    = millis();
        unsigned long lastPrinted  = 0;
        int           lastSecShown = -1;

        while (digitalRead(BOOT_BUTTON_PIN) == LOW)
        {
            unsigned long elapsed = millis() - holdStart;

            if (elapsed >= BTN_HOLD_MS)
            {
                Serial.println(F("[BTN] Factory reset triggered!"));
                delay(300);
                performFactoryReset();
                break;   // performFactoryReset() calls ESP.restart(), but just in case
            }

            // Print countdown once per second (3…2…1)
            int secsLeft = (int)((BTN_HOLD_MS - elapsed + 999) / 1000);
            if (secsLeft != lastSecShown)
            {
                Serial.printf_P(PSTR(F("[BTN] Release to cancel — resetting in %d...\n")), secsLeft);
                lastSecShown = secsLeft;
            }

            delay(50);
        }

        if (digitalRead(BOOT_BUTTON_PIN) == HIGH)
            Serial.println(F("[BTN] Released — factory reset cancelled\n"));
    }
    else
        Serial.println(F("[BTN] IO0 not held — normal boot\n"));

#if FORCE_BLE_BOOT
    performFactoryReset();
#endif

    dataMutex = xSemaphoreCreateMutex();
    credMutex = xSemaphoreCreateMutex();
    if (!dataMutex || !credMutex)
    {
        Serial.println(F("[FATAL] Mutex creation failed"));
        while (1) delay(1000);
    }

    xTaskCreatePinnedToCore(TaskBootButton, "BootBtn", 1024, NULL, 2, NULL, 1);

    preferences.begin("wifi-config", true);
    String savedSSID = preferences.getString("ssid",     "");
    String savedPass = preferences.getString("password", "");
    preferences.end();

    if (savedSSID.length() > 0)
    {
        strncpy(wifiSSIDForLCD, savedSSID.c_str(), BUF_SSID - 1);  // Safe SSID for LCD
        lcdShow(LCD_WIFI_CONNECTING);
        Serial.printf_P(PSTR(F("[BOOT] WiFi: %s\n")), savedSSID.c_str());
        Serial.println(F("[BOOT] Connecting...\n"));
        WiFi.onEvent(onWiFiEvent);
        wifiSMState = WS_CONNECTING;
        WiFi.begin(savedSSID.c_str(), savedPass.c_str());
    }
    else
    {
#if FEATURE_BLE
        Serial.println(F("[BOOT] Starting BLE\n"));
        startBLEProvisioning();
#else
        Serial.println(F("[ERROR] No WiFi"));
        while (1) delay(1000);
#endif
    }
}

/* ===================== LOOP ===================== */
void loop()
{
    // Timed WiFi reconnect — only fires after cooldown, never interrupts an
    // association already in progress (WS_CONNECTING means we already called
    // WiFi.begin and are waiting — don't call it again).
    if (!bleActive && wifiReconnectAt != 0 && millis() >= wifiReconnectAt)
    {
        wifiReconnectAt = 0;

        wl_status_t status = WiFi.status();

        // Only retry if not already connected
        if (status != WL_CONNECTED)
        {
            // Read saved credentials fresh from NVS
            preferences.begin("wifi-config", true);
            String ssid = preferences.getString("ssid",     "");
            String pass = preferences.getString("password", "");
            preferences.end();

            if (ssid.length() > 0)
            {
                strncpy(wifiSSIDForLCD, ssid.c_str(), BUF_SSID - 1);  // Safe SSID for LCD
                Serial.printf_P(PSTR(F("[WiFi] Reconnecting to %s\n")), ssid.c_str());
                WiFi.disconnect(false, false);
                delay(200);
                WiFi.begin(ssid.c_str(), pass.c_str());
            }
            else
            {
                Serial.println(F("[WiFi] No saved credentials — cannot reconnect"));
            }
        }
        else
        {
            Serial.println(F("[WiFi] Reconnect skipped — already connected"));
        }
    }

#if FEATURE_BLE
    if (bleRestartRequested)
    {
        bleRestartRequested = false;
        Serial.println(F("[BLE] Soft restart"));

        if (hTaskSensors) { vTaskDelete(hTaskSensors); hTaskSensors = NULL; }
        if (hTaskMQTT)    { vTaskDelete(hTaskMQTT);    hTaskMQTT    = NULL; }

        if (mqttClient.connected()) mqttClient.disconnect();

        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        delay(500);

        preferences.begin("wifi-config", false);
        preferences.clear();
        preferences.end();

        sPostWiFiDone           = false;
        mqttConnected           = false;
        mqttPreviouslyConnected = false;
        bleTimedOut             = false;  // Fresh BLE window starting

        startBLEProvisioning();
    }

    if (appDisconnectRequested)
    {
        appDisconnectRequested = false;
        Serial.println(F("[BLE] Back pressed — disconnecting from app"));
        Serial.println(F("[BLE] Returning to BLE provisioning..."));
        lcdShow(LCD_BACK_PRESSED);

        // Stop tasks and MQTT cleanly
        if (hTaskSensors) { vTaskDelete(hTaskSensors); hTaskSensors = NULL; }
        if (hTaskMQTT)    { vTaskDelete(hTaskMQTT);    hTaskMQTT    = NULL; }
        if (mqttClient.connected()) mqttClient.disconnect();

        // Tear down WiFi
        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        delay(500);

        // NOTE: WiFi credentials are intentionally kept — this is NOT a factory
        // reset. The user only pressed back; they may reconnect via BLE again.
        sPostWiFiDone           = false;
        mqttConnected           = false;
        mqttPreviouslyConnected = false;
        wifiReconnectAt         = 0;
        bleTimedOut             = false;  // Fresh BLE window starting

        Serial.println(F("[BLE] Restarting BLE advertising..."));
        startBLEProvisioning();
    }

    if (bleFallbackRequested)
    {
        bleFallbackRequested = false;
        Serial.println(F("[WiFi] Fallback to BLE"));

        if (mqttClient.connected()) mqttClient.disconnect();

        WiFi.disconnect(true, true);
        WiFi.mode(WIFI_OFF);
        delay(500);

        preferences.begin("wifi-config", false);
        preferences.clear();
        preferences.end();

        sPostWiFiDone = false;

        startBLEProvisioning();
    }

    if (bleActive && (millis() - bleStartTime) > BLE_TIMEOUT_MS)
    {
        Serial.println(F("[BLE] Timeout — hold button 3 s to restart BLE"));
        lcdShow(LCD_BLE_TIMEOUT);
        BLEDevice::getAdvertising()->stop();
        BLEDevice::deinit(true);
        bleActive   = false;
        bleTimedOut = true;   // Button will restart BLE instead of factory reset
    }

    if (isProvisioning && credentialsReceived)
    {
        credentialsReceived = false;

        char ssid[BUF_SSID], pass[BUF_PASS];
        if (xSemaphoreTake(credMutex, pdMS_TO_TICKS(200)))
        {
            strncpy(ssid, receivedSSID, BUF_SSID - 1);
            strncpy(pass, receivedPass, BUF_PASS - 1);
            xSemaphoreGive(credMutex);
        }

        preferences.begin("wifi-config", false);
        preferences.putString("ssid",     String(ssid));
        preferences.putString("password", String(pass));
        preferences.end();
        Serial.println(F("[BLE] Saved"));

        if (bleActive)
        {
            BLEDevice::getAdvertising()->stop();
            Serial.println(F("[BLE] Advertising stopped"));
        }
        delay(300);

        strncpy(wifiSSIDForLCD, ssid, BUF_SSID - 1);  // Safe SSID for LCD
        lcdShow(LCD_WIFI_CONNECTING);
        WiFi.onEvent(onWiFiEvent);
        WiFi.mode(WIFI_STA);
        delay(100);
        WiFi.begin(ssid, pass);
        Serial.println(F("[WiFi] Connecting...\n"));
        isProvisioning = false;
    }
#endif

    vTaskDelay(pdMS_TO_TICKS(100));
}