#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <BH1750.h>

Adafruit_BME280 bme;
BH1750 lightMeter;

void setup()
{
  Serial.begin(115200);

  Wire.begin(21, 22);
  Serial.println("\n--- Nurtura I2C Dual Sensor Test ---");

  if (!bme.begin(0x76, &Wire))
  {
    Serial.println("System: could not find BME280 sensor!");
  }
  else
  {
    Serial.println("System: BME280 Initialized (Address: 0x76)");
  }

  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE))
  {
    Serial.println("System: GY-302 Initialized (Address: 0x23)");
  }
  else
  {
    Serial.println("System: could not find GY-302 sensor!");
  }

  Serial.println("------------------------------------");
}

void loop()
{
  float temp = bme.readTemperature();
  float hum = bme.readHumidity();
  float pres = bme.readPressure() / 100.0F;
  float lux = lightMeter.readLightLevel();

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