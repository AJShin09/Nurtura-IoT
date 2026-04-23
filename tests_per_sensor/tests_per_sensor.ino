#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include "cert.h"
#include "secrets.h"

// --- PINS ---
#define LDR_PIN 34
#define SOIL_PIN 35
#define FLOW_PIN 13
#define PUMP_RELAY 25 
#define LIGHT_RELAY 26

// --- SENSOR CALIBRATION & VARS ---
const int AirValue = 2480;
const int WaterValue = 460;
const float calibrationFactor = 7.5;

volatile byte pulseCount = 0;
float flowRate = 0.0;
unsigned long totalMilliLitres = 0; 
unsigned long oldTime = 0;

Adafruit_BME280 bme;
WiFiClientSecure net;
PubSubClient client(net);

// --- STATE VARIABLES ---
unsigned long lastReconnectAttempt = 0;
unsigned long lastMsg = 0;
bool pumpError = false;
unsigned long pumpActivationTime = 0; 
bool pumpWasOff = true;
String pumpStatus = "OFF";
String lightStatus = "OFF";

void IRAM_ATTR pulseCounter() { pulseCount++; }

boolean reconnect() {
  Serial.print("Attempting MQTT connection...");
  String clientId = "Nurtura-" + String(random(0xffff), HEX);
  if (client.connect(clientId.c_str(), SECRET_MQTT_USER, SECRET_MQTT_PASS)) {
    Serial.println("CONNECTED!");
  } else {
    Serial.print("failed, rc=");
    Serial.println(client.state());
  }
  return client.connected();
}

void setup() {
  delay(2000); // Stabilize power
  Serial.begin(115200);
  
  // Pins & Sensors
  Wire.begin(21, 22);
  pinMode(PUMP_RELAY, OUTPUT);
  pinMode(LIGHT_RELAY, OUTPUT);
  digitalWrite(PUMP_RELAY, LOW);   
  digitalWrite(LIGHT_RELAY, HIGH); 
  
  if (!bme.begin(0x76, &Wire)) Serial.println("BME280 not found");
  pinMode(FLOW_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), pulseCounter, FALLING);

  // Networking
  WiFi.begin(SECRET_SSID, SECRET_PASS);
  configTime(0, 0, "pool.ntp.org");
  net.setCACert(root_ca);
  client.setServer(SECRET_MQTT_HOST, 8883);
}

void loop() {
  // 1. MAINTAIN MQTT CONNECTION
  if (!client.connected()) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > 5000) {
      lastReconnectAttempt = now;
      if (reconnect()) lastReconnectAttempt = 0;
    }
  } else {
    client.loop();
  }

  // 2. SENSOR CALCULATIONS (Every 1 second)
  if ((millis() - oldTime) > 1000) {
    detachInterrupt(digitalPinToInterrupt(FLOW_PIN));
    flowRate = ((1000.0 / (millis() - oldTime)) * pulseCount) / calibrationFactor;
    unsigned int flowMilliLitres = (flowRate / 60) * 1000;
    totalMilliLitres += flowMilliLitres;
    pulseCount = 0;
    oldTime = millis();
    attachInterrupt(digitalPinToInterrupt(FLOW_PIN), pulseCounter, FALLING);

    // Read other sensors
    float temp = bme.readTemperature();
    float hum  = bme.readHumidity();
    int lightRaw = analogRead(LDR_PIN); 
    int soilRaw = analogRead(SOIL_PIN);
    int moisture = constrain(map(soilRaw, AirValue, WaterValue, 0, 100), 0, 100);

    // 3. LOGIC (Light & Pump)
    if (lightRaw < 1200) { 
      digitalWrite(LIGHT_RELAY, LOW);  
      lightStatus = "ON";
    } else {
      digitalWrite(LIGHT_RELAY, HIGH); 
      lightStatus = "OFF";
    }

    if (pumpError) {
      digitalWrite(PUMP_RELAY, LOW); 
      pumpStatus = "ERROR/LOCKED";
    } else if (moisture < 25) { 
      if (pumpWasOff) { pumpActivationTime = millis(); pumpWasOff = false; }
      digitalWrite(PUMP_RELAY, HIGH); 
      pumpStatus = "ON";
      if (millis() - pumpActivationTime > 5000 && flowRate <= 0.1) {
          pumpError = true; 
          digitalWrite(PUMP_RELAY, LOW);
          pumpStatus = "ERROR/LOCKED";
      }
    } else {
      digitalWrite(PUMP_RELAY, LOW); 
      pumpStatus = "OFF";
      pumpWasOff = true; 
    }

    // 4. TELEMETRY (Every 10 seconds)
    if (millis() - lastMsg > 10000) {
      lastMsg = millis();
      if (client.connected()) {
        String payload = "{";
        payload += "\"temp\":" + String(temp, 1) + ",";
        payload += "\"hum\":" + String(hum, 1) + ",";
        payload += "\"moist\":" + String(moisture) + ",";
        payload += "\"flow\":" + String(flowRate, 1) + ",";
        payload += "\"pump\":\"" + pumpStatus + "\"";
        payload += "}";
        client.publish("nurtura/noah/rack1", payload.c_str());
        Serial.println("MQTT Sent: " + payload);
      }
    }
  }
}