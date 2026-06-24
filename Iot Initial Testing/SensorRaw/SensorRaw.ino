#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

#define LDR_PIN 34
#define SOIL_PIN 35
#define FLOW_PIN 13
#define PUMP_RELAY 25  
#define LIGHT_RELAY 26 

const int AirValue = 2480;
const int WaterValue = 460;

const float calibrationFactor = 7.5;

volatile byte pulseCount = 0;
float flowRate = 0.0;
unsigned long totalMilliLitres = 0;
unsigned long oldTime = 0;
Adafruit_BME280 bme;

void IRAM_ATTR pulseCounter() { pulseCount++; }

bool pumpError = false;
unsigned long pumpActivationTime = 0;
bool pumpWasOff = true;

String pumpStatus = "OFF";
String lightStatus = "OFF";

void setup()
{
  Serial.begin(115200);
  Wire.begin(21, 22);

  pinMode(PUMP_RELAY, OUTPUT);
  pinMode(LIGHT_RELAY, OUTPUT);

  digitalWrite(PUMP_RELAY, LOW);
  digitalWrite(LIGHT_RELAY, HIGH);

  if (!bme.begin(0x76, &Wire))
    Serial.println("BME280 not found");

  pinMode(FLOW_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), pulseCounter, FALLING);
}

void loop()
{
  float temp = bme.readTemperature();
  float hum = bme.readHumidity();
  int lightRaw = analogRead(LDR_PIN);
  int soilRaw = analogRead(SOIL_PIN);
  int moisture = constrain(map(soilRaw, AirValue, WaterValue, 0, 100), 0, 100);

  if ((millis() - oldTime) > 1000)
  {
    detachInterrupt(digitalPinToInterrupt(FLOW_PIN));
    flowRate = ((1000.0 / (millis() - oldTime)) * pulseCount) / calibrationFactor;
    unsigned int flowMilliLitres = (flowRate / 60) * 1000;
    totalMilliLitres += flowMilliLitres;
    pulseCount = 0;
    oldTime = millis();
    attachInterrupt(digitalPinToInterrupt(FLOW_PIN), pulseCounter, FALLING);
  }

  if (lightRaw < 1200)
  {
    digitalWrite(LIGHT_RELAY, LOW);
    lightStatus = "ON";
  }
  else
  {
    digitalWrite(LIGHT_RELAY, HIGH);
    lightStatus = "OFF";
  }

  if (pumpError)
  {
    digitalWrite(PUMP_RELAY, LOW);
    pumpStatus = "ERROR/LOCKED";
  }
  else if (moisture < 25)
  {
    if (pumpWasOff)
    {
      pumpActivationTime = millis();
      pumpWasOff = false;
    }
    digitalWrite(PUMP_RELAY, HIGH);
    pumpStatus = "ON";
    if (millis() - pumpActivationTime > 5000)
    {
      if (flowRate <= 0.1)
      {
        pumpError = true;
        digitalWrite(PUMP_RELAY, LOW);
        pumpStatus = "ERROR/LOCKED";
      }
    }
  }
  else
  {
    digitalWrite(PUMP_RELAY, LOW);
    pumpStatus = "OFF";
    pumpWasOff = true;
  }

  Serial.print("ENV: ");
  Serial.print(temp, 1);
  Serial.print("C ");
  Serial.print(hum, 1);
  Serial.print("%H | ");
  Serial.print("SOIL: ");
  Serial.print(moisture);
  Serial.print("% | ");

  Serial.print("LIGHT: ");
  Serial.print(lightRaw);
  Serial.print(" | ");

  Serial.print("FLOW: ");
  Serial.print(flowRate, 1);
  Serial.print("L/m (Total: ");
  Serial.print(totalMilliLitres);
  Serial.println("mL)");

  Serial.print("DEVICE STATUS -> Growlight: ");
  Serial.print(lightStatus);
  Serial.print(" | Water Pump: ");
  Serial.println(pumpStatus);
  Serial.println("---------------------------------------------------------");

  delay(1000);
}