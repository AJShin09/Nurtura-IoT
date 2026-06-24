#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <BH1750.h>

const int GROWLIGHT_PIN = 26;
const float LUX_THRESHOLD = 50.0;

Adafruit_BME280 bme;
BH1750 lightMeter;

void setup()
{
  Serial.begin(115200);

  pinMode(GROWLIGHT_PIN, OUTPUT);
  digitalWrite(GROWLIGHT_PIN, LOW);

  Wire.begin(21, 22);
  Serial.println("\n--- Nurtura Smart Light Test ---");

  if (!bme.begin(0x76, &Wire))
  {
    Serial.println("System: Could not find BME280 sensor!");
  }
  else
  {
    Serial.println("System: BME280 Initialized (0x76)");
  }

  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE))
  {
    Serial.println("System: GY-302 Initialized (0x23)");
  }
  else
  {
    Serial.println("System: Could not find GY-302 sensor!");
  }

  Serial.println("System Monitoring Started...");
  Serial.println("------------------------------------");
}

void loop()
{
  float temp = bme.readTemperature();
  float hum = bme.readHumidity();
  float lux = lightMeter.readLightLevel();

  if (lux > LUX_THRESHOLD)
  {
    digitalWrite(GROWLIGHT_PIN, HIGH);
    Serial.print("[RELAY: ON]  ");
  }
  else
  {
    digitalWrite(GROWLIGHT_PIN, LOW);
    Serial.print("[RELAY: OFF] ");
  }

  Serial.print("Temp: ");
  Serial.print(temp, 1);
  Serial.print("°C | ");
  Serial.print("Hum: ");
  Serial.print(hum, 1);
  Serial.print("% | ");
  Serial.print("Light: ");
  Serial.print(lux, 1);
  Serial.println(" Lux");

  delay(2000);
}