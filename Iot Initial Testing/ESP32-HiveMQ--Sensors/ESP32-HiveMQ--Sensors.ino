#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <BH1750.h>
#include "mqtt_client.h"
#include <time.h>
#include "secrets.h"
#include "cert.h"

const int PUMP_TIMEOUT_MS = 10000;
const float CALIBRATION_FACTOR = 7.5;

const int GROWLIGHT_PIN = 26;
const int PUMP_PIN = 27;
const int SOIL_PIN = 35;
const int FLOW_PIN = 12;

esp_mqtt_client_handle_t mqtt_client;
TimerHandle_t pumpTimer;
Adafruit_BME280 bme;
BH1750 lightMeter;
bool bmeFound = false;
bool lightFound = false;
bool lastLightState = false;
volatile int pulseCount = 0;
float flowRate = 0.0;
float totalMilliLitres = 0.0;

void IRAM_ATTR pulseCounter()
{
    pulseCount++;
}

void stopPumpCallback(TimerHandle_t xTimer)
{
    digitalWrite(PUMP_PIN, HIGH);

    String report = "{\"event\":\"watering_finished\",\"total_ml\":" + String(totalMilliLitres, 1) + "}";
    esp_mqtt_client_publish(mqtt_client, "nurtura/rack/AA:BB:CC:DD:EE:FF/watered", report.c_str(), 0, 1, 0);

    Serial.println("Stopped pump. Total water: " + String(totalMilliLitres) + "mL");
    totalMilliLitres = 0;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    auto event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        Serial.println("MQTT connected");
        esp_mqtt_client_subscribe(mqtt_client, "nurtura/rack/control", 1);
        break;

    case MQTT_EVENT_DATA:
    {
        if (event->retain)
            break;

        if (event->topic_len == 20 && strncmp(event->topic, "nurtura/rack/control", 20) == 0)
        {
            if (event->data_len == 5 && strncmp(event->data, "water", 5) == 0)
            {
                Serial.println("Command Received: Starting pump.");
                digitalWrite(PUMP_PIN, LOW); // ON
                xTimerReset(pumpTimer, 0);
            }
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        Serial.println("MQTT Event Error occurred.");
        break;
    default:
        break;
    }
}

void sendSensorData()
{
    float t = bmeFound ? bme.readTemperature() : 0.0;
    float h = bmeFound ? bme.readHumidity() : 0.0;
    float l = lightFound ? lightMeter.readLightLevel() : -1.0;
    int m = constrain(map(analogRead(SOIL_PIN), 3200, 1200, 0, 100), 0, 100);
    int lightIsOn = (digitalRead(GROWLIGHT_PIN) == LOW) ? 1 : 0;

    String p = "{\"t\":" + String(t, 1) + ",\"h\":" + String(h, 1) + ",\"m\":" + String(m);
    p += ",\"l\":" + String(l, 0) + ",\"light\":" + String(lightIsOn);
    p += ",\"flow\":" + String(flowRate, 1) + "}";

    esp_mqtt_client_publish(mqtt_client, "nurtura/rack/AA:BB:CC:DD:EE:FF/sensors", p.c_str(), 0, 1, 0);
    Serial.println("Sensor data sent to HiveMQ");
}

void setup()
{

    digitalWrite(PUMP_PIN, HIGH);
    digitalWrite(GROWLIGHT_PIN, HIGH);
    pinMode(PUMP_PIN, OUTPUT);
    pinMode(GROWLIGHT_PIN, OUTPUT);

    pinMode(FLOW_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(FLOW_PIN), pulseCounter, FALLING);

    Serial.begin(115200);
    Wire.begin(21, 22);

    if (bme.begin(0x76, &Wire))
        bmeFound = true;
    if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE))
        lightFound = true;

    pumpTimer = xTimerCreate("PumpTimer", pdMS_TO_TICKS(PUMP_TIMEOUT_MS), pdFALSE, (void *)0, stopPumpCallback);

    WiFi.begin(SECRET_SSID, SECRET_PASS);
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    configTime(0, 0, "pool.ntp.org");

    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = "mqtts://" SECRET_MQTT_HOST;
    mqtt_cfg.broker.address.port = 8883;
    mqtt_cfg.broker.verification.certificate = root_ca;
    mqtt_cfg.credentials.username = SECRET_MQTT_USER;
    mqtt_cfg.credentials.authentication.password = SECRET_MQTT_PASS;

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, MQTT_EVENT_ANY, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

void loop()
{
    static unsigned long lastMsg = 0;
    static unsigned long lastFlowCalc = 0;
    static unsigned long lastLightCheck = 0;
    unsigned long now = millis();

    if (now - lastFlowCalc > 1000)
    {
        flowRate = ((1000.0 / (now - lastFlowCalc)) * pulseCount) / CALIBRATION_FACTOR;
        lastFlowCalc = now;

        float flowMilliLitres = (flowRate / 60.0) * 1000.0;
        if (digitalRead(PUMP_PIN) == LOW)
        {
            totalMilliLitres += flowMilliLitres;
        }
        pulseCount = 0;
    }

    if (now - lastLightCheck > 2000)
    {
        lastLightCheck = now;
        if (lightFound)
        {
            float currentLux = lightMeter.readLightLevel();
            bool shouldBeOn = (currentLux < 5.0);
            if (shouldBeOn != lastLightState)
            {
                digitalWrite(GROWLIGHT_PIN, shouldBeOn ? LOW : HIGH);
                lastLightState = shouldBeOn;
                sendSensorData();
            }
        }
    }

    if (now - lastMsg > 10000)
    {
        lastMsg = now;
        sendSensorData();
    }
}