#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <BH1750.h>

// I2C Objects
Adafruit_BME280 bme;
BH1750 lightMeter;

void setup() {
  Serial.begin(115200);
  
  // Initialize I2C bus once
  Wire.begin(21, 22);
  Serial.println("\n--- Nurtura I2C Dual Sensor Test ---");

  // Initialize BME280 (Environment)
  if (!bme.begin(0x76, &Wire)) {
    Serial.println("❌ Could not find BME280 sensor!");
  } else {
    Serial.println("✅ BME280 Initialized (Address: 0x76)");
  }

  // Initialize GY-302 (Light)
  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("✅ GY-302 Initialized (Address: 0x23)");
  } else {
    Serial.println("❌ Could not find GY-302 sensor!");
  }
  
  Serial.println("------------------------------------");
}

void loop() {
  // Read BME280
  float temp = bme.readTemperature();
  float hum  = bme.readHumidity();
  float pres = bme.readPressure() / 100.0F;

  // Read GY-302
  float lux = lightMeter.readLightLevel();

  // Print everything in one line for comparison
  Serial.print("Temp: "); Serial.print(temp, 1); Serial.print("°C | ");
  Serial.print("Hum: ");  Serial.print(hum, 1);  Serial.print("% | ");
  Serial.print("Light: "); Serial.print(lux, 1); Serial.println(" Lux");

  delay(2000); // Wait 2 seconds between reads
}