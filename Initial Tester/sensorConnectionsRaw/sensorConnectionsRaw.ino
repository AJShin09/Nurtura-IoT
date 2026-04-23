#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

// pins
#define LDR_PIN 34
#define SOIL_PIN 35
#define FLOW_PIN 13
#define PUMP_RELAY 25   // Trigger HIGH for ON
#define LIGHT_RELAY 26  // Trigger LOW for ON

// sensor calibrations 
// --moistureSensor
const int AirValue = 2480;
const int WaterValue = 460;
// --waterFlowSensor
const float calibrationFactor = 7.5;

// sensor variables
// --waterFlowSensor
volatile byte pulseCount = 0;
float flowRate = 0.0;
unsigned long totalMilliLitres = 0; 
unsigned long oldTime = 0;
// --BME280
Adafruit_BME280 bme;

void IRAM_ATTR pulseCounter() { pulseCount++; }

// waterPump check
bool pumpError = false;           
unsigned long pumpActivationTime = 0; 
bool pumpWasOff = true;

// growLight & waterPump status
String pumpStatus = "OFF";
String lightStatus = "OFF";

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);

  pinMode(PUMP_RELAY, OUTPUT);
  pinMode(LIGHT_RELAY, OUTPUT);

  digitalWrite(PUMP_RELAY, LOW);   
  digitalWrite(LIGHT_RELAY, HIGH); 

  if (!bme.begin(0x76, &Wire)) Serial.println("BME280 not found");

  pinMode(FLOW_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), pulseCounter, FALLING);
}

void loop() {
  // sensors
  float temp = bme.readTemperature();
  float hum  = bme.readHumidity();
  int lightRaw = analogRead(LDR_PIN); 
  int soilRaw = analogRead(SOIL_PIN);
  int moisture = constrain(map(soilRaw, AirValue, WaterValue, 0, 100), 0, 100);

  // calculate flow
  if ((millis() - oldTime) > 1000) {
    detachInterrupt(digitalPinToInterrupt(FLOW_PIN));
    flowRate = ((1000.0 / (millis() - oldTime)) * pulseCount) / calibrationFactor;
    unsigned int flowMilliLitres = (flowRate / 60) * 1000;
    totalMilliLitres += flowMilliLitres;
    pulseCount = 0;
    oldTime = millis();
    attachInterrupt(digitalPinToInterrupt(FLOW_PIN), pulseCounter, FALLING);
  }

  // relay stuff (growLight & waterPump)
  if (lightRaw < 1200) { 
    digitalWrite(LIGHT_RELAY, LOW);  
    lightStatus = "ON";
  } else {
    digitalWrite(LIGHT_RELAY, HIGH); 
    lightStatus = "OFF";
  }

  // check pump if bad
  if (pumpError) {
    digitalWrite(PUMP_RELAY, LOW); 
    pumpStatus = "ERROR/LOCKED";
  } 
  // if pump not bad, check moisture
  else if (moisture < 25) { 
    if (pumpWasOff) {
      pumpActivationTime = millis();
      pumpWasOff = false;
    }
    digitalWrite(PUMP_RELAY, HIGH); 
    pumpStatus = "ON";
    // if pump turned on, but waterFlowSensor sensed no flow for 5secs, turn off waterPump
    if (millis() - pumpActivationTime > 5000) {
      if (flowRate <= 0.1) { 
        pumpError = true; 
        digitalWrite(PUMP_RELAY, LOW);
        pumpStatus = "ERROR/LOCKED";
      }
    }
  } 
  else {
    digitalWrite(PUMP_RELAY, LOW); 
    pumpStatus = "OFF";
    pumpWasOff = true; 
  }

  // display values n' stuff
  Serial.print("ENV: "); Serial.print(temp, 1); Serial.print("C "); 
  Serial.print(hum, 1); Serial.print("%H | ");
  Serial.print("SOIL: "); Serial.print(moisture); Serial.print("% | ");
  
  Serial.print("LIGHT: "); Serial.print(lightRaw); Serial.print(" | ");
  
  Serial.print("FLOW: "); Serial.print(flowRate, 1); Serial.print("L/m (Total: ");
  Serial.print(totalMilliLitres); Serial.println("mL)");

  Serial.print("DEVICE STATUS -> Growlight: "); Serial.print(lightStatus);
  Serial.print(" | Water Pump: "); Serial.println(pumpStatus);
  Serial.println("---------------------------------------------------------");

  delay(1000);
}