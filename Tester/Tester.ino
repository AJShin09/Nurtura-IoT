#include <Wire.h>
#include <BH1750.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

// ─── CONFIGURATION ───────────────────────────
#define LED_PIN          2
#define SDA_PIN         21
#define SCL_PIN         22
#define SDA_PIN_2       16   // BME280 dedicated bus
#define SCL_PIN_2       17   // BME280 dedicated bus
#define READ_INTERVAL_MS 1000
#define NUM_SAMPLES      10
#define SAMPLE_DELAY_MS   5
#define THRESHOLD_DRY    30
#define THRESHOLD_WET    70

// ── Soil sensor definitions ──
struct Sensor {
  const char *name;
  int pin;
  int dryVal;
  int wetVal;
};

Sensor sensors[] = {
  {"v1.2", 34, 2650, 1300},
  {"v2.0", 35, 3500, 1100},
};
const int NUM_SENSORS = sizeof(sensors) / sizeof(sensors[0]);

// ── I2C peripherals ──
TwoWire I2C_BME = TwoWire(1);   // second I2C bus instance for BME280

BH1750         lightMeter;
Adafruit_BME280 bme;
bool bh1750_ok  = false;
bool bme280_ok  = false;
// ─────────────────────────────────────────────

// ─── HELPERS ─────────────────────────────────
int readAveraged(int pin) {
  long sum = 0;
  for (int i = 0; i < NUM_SAMPLES; i++) {
    sum += analogRead(pin);
    delay(SAMPLE_DELAY_MS);
  }
  return (int)(sum / NUM_SAMPLES);
}

float toPercent(int raw, int dryVal, int wetVal) {
  raw = constrain(raw, wetVal, dryVal);
  float pct = (float)(dryVal - raw) / (float)(dryVal - wetVal) * 100.0;
  return constrain(pct, 0.0, 100.0);
}

const char *getStatus(float pct) {
  if (pct < THRESHOLD_DRY) return "DRY";
  if (pct > THRESHOLD_WET) return "WET";
  return "OK";
}

// ─── SETUP ───────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  // Bus 0 — BH1750 (GPIO21/22)
  Wire.begin(SDA_PIN, SCL_PIN);

  // Bus 1 — BME280 (GPIO16/17)
  I2C_BME.begin(SDA_PIN_2, SCL_PIN_2);

  // BH1750 on default Wire bus
  bh1750_ok = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);

  // BME280 on dedicated I2C_BME bus
  bme280_ok = bme.begin(0x76, &I2C_BME);
  if (!bme280_ok) bme280_ok = bme.begin(0x77, &I2C_BME); // try alt address

  Serial.println();
  Serial.println("=========================================");
  Serial.println("  ESP32 Soil + Environment Monitor");
  Serial.println("=========================================");
  for (int i = 0; i < NUM_SENSORS; i++) {
    Serial.print("  Soil  "); Serial.print(sensors[i].name);
    Serial.print(" → GPIO"); Serial.print(sensors[i].pin);
    Serial.print("  dry="); Serial.print(sensors[i].dryVal);
    Serial.print("  wet="); Serial.println(sensors[i].wetVal);
  }
  Serial.print("  BH1750  → I2C bus 0 (SDA=21 SCL=22) : ");
  Serial.println(bh1750_ok ? "OK" : "NOT FOUND");
  Serial.print("  BME280  → I2C bus 1 (SDA=16 SCL=17) : ");
  Serial.println(bme280_ok ? "OK" : "NOT FOUND");
  Serial.println("-----------------------------------------");
  Serial.println("  Format: DATA,<name>,<raw>,<pct>,<status>");
  Serial.println("          ENV,<lux>,<tempC>,<hum%>,<hPa>");
  Serial.println("=========================================");
  Serial.println();
}

// ─── LOOP ────────────────────────────────────
unsigned long lastRead = 0;

void loop() {
  unsigned long now = millis();
  if (now - lastRead < READ_INTERVAL_MS) return;
  lastRead = now;

  bool anyDry = false;

  // ── Soil sensors ──
  for (int i = 0; i < NUM_SENSORS; i++) {
    int raw         = readAveraged(sensors[i].pin);
    float pct       = toPercent(raw, sensors[i].dryVal, sensors[i].wetVal);
    const char *status = getStatus(pct);

    if (pct < THRESHOLD_DRY) anyDry = true;

    // Human-readable
    Serial.print("["); Serial.print(sensors[i].name); Serial.print("] ");
    Serial.print("Moisture: "); Serial.print(pct, 1);
    Serial.print("%  Raw: "); Serial.print(raw);
    Serial.print("  Status: "); Serial.println(status);

    // Machine-parseable
    Serial.print("DATA,"); Serial.print(sensors[i].name);
    Serial.print(","); Serial.print(raw);
    Serial.print(","); Serial.print(pct, 1);
    Serial.print(","); Serial.println(status);
  }

  // ── Environment ──
  float lux  = bh1750_ok ? lightMeter.readLightLevel() : -1;
  float temp = bme280_ok ? bme.readTemperature()        : -1;
  float hum  = bme280_ok ? bme.readHumidity()           : -1;
  float pres = bme280_ok ? bme.readPressure() / 100.0F  : -1;

  // Human-readable
  if (bh1750_ok) { Serial.print("[BH1750] Light: "); Serial.print(lux, 1); Serial.println(" lx"); }
  else           { Serial.println("[BH1750] NOT FOUND"); }
  if (bme280_ok) {
    Serial.print("[BME280] Temp: "); Serial.print(temp, 1); Serial.print("°C  ");
    Serial.print("Hum: ");  Serial.print(hum, 1);  Serial.print("%  ");
    Serial.print("Pres: "); Serial.print(pres, 1); Serial.println(" hPa");
  } else { Serial.println("[BME280] NOT FOUND"); }

  // Machine-parseable
  Serial.print("ENV,");
  Serial.print(lux,  1); Serial.print(",");
  Serial.print(temp, 1); Serial.print(",");
  Serial.print(hum,  1); Serial.print(",");
  Serial.println(pres, 1);

  Serial.println();

  // LED blink
  if (anyDry) {
    for (int b = 0; b < 3; b++) {
      digitalWrite(LED_PIN, HIGH); delay(80);
      digitalWrite(LED_PIN, LOW);  delay(80);
    }
  } else {
    digitalWrite(LED_PIN, HIGH); delay(200);
    digitalWrite(LED_PIN, LOW);
  }
}
