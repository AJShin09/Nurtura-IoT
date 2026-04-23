  #include <Wire.h>
  #include <Adafruit_Sensor.h>
  #include <Adafruit_BME280.h>
  #include <BH1750.h>

  // Pins
  const int GROWLIGHT_PIN = 26;

  // Thresholds
  const float LUX_THRESHOLD = 50.0;

  // I2C Objects
  Adafruit_BME280 bme;
  BH1750 lightMeter;

  void setup() {
    Serial.begin(115200);
    
    // Initialize Growlight Pin
    pinMode(GROWLIGHT_PIN, OUTPUT);
    digitalWrite(GROWLIGHT_PIN, LOW); // Start with light OFF (Active High)
    
    // Initialize I2C bus
    Wire.begin(21, 22);
    Serial.println("\n--- Nurtura Smart Light Test ---");

    // Initialize BME280
    if (!bme.begin(0x76, &Wire)) {
      Serial.println("❌ Could not find BME280 sensor!");
    } else {
      Serial.println("✅ BME280 Initialized (0x76)");
    }

    // Initialize GY-302
    if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
      Serial.println("✅ GY-302 Initialized (0x23)");
    } else {
      Serial.println("❌ Could not find GY-302 sensor!");
    }
    
    Serial.println("System Monitoring Started...");
    Serial.println("------------------------------------");
  }

  void loop() {
    // 1. Read Sensors
    float temp = bme.readTemperature();
    float hum  = bme.readHumidity();
    float lux  = lightMeter.readLightLevel();

    // 2. Logic: Control Growlight based on Lux
    // If light is below 50 Lux, turn ON. Otherwise, turn OFF.
    if (lux > LUX_THRESHOLD) {
      digitalWrite(GROWLIGHT_PIN, HIGH);
      Serial.print("[RELAY: ON]  ");
    } else {
      digitalWrite(GROWLIGHT_PIN, LOW);
      Serial.print("[RELAY: OFF] ");
    }

    // 3. Serial Output
    Serial.print("Temp: "); Serial.print(temp, 1); Serial.print("°C | ");
    Serial.print("Hum: ");  Serial.print(hum, 1);  Serial.print("% | ");
    Serial.print("Light: "); Serial.print(lux, 1); Serial.println(" Lux");

    delay(2000); // Check every 2 seconds
  }