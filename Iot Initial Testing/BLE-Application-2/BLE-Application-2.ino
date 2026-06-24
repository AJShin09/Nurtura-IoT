#include <WiFi.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <Preferences.h>

#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define DEVICE_ID_CHAR_UUID "abc12345-1234-5678-1234-56789abcdef0"
#define SSID_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define PASSWORD_CHAR_UUID "1c95d5e3-d8f7-413a-bf3d-7a2e5d7be87e"
#define STATUS_CHAR_UUID "9a8ca5e3-d8f7-413a-bf3d-7a2e5d7be123"
#define RACK_NAME_CHAR_UUID "a1b2c3d4-e5f6-7890-abcd-ef1234567890"

#define BOOT_BUTTON_PIN 0

BLECharacteristic *statusCharacteristic = NULL;
String receivedSSID = "";
String receivedPassword = "";
bool credentialsReceived = false;
bool rackNameReceived = false;
Preferences preferences;
String deviceMAC = "";

class MyCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pCharacteristic)
  {
    String value = pCharacteristic->getValue().c_str();
    value.trim();

    String uuid = pCharacteristic->getUUID().toString().c_str();

    if (uuid.equalsIgnoreCase(SSID_CHAR_UUID))
    {
      receivedSSID = value;
      Serial.println("Received SSID: [" + receivedSSID + "]");
    }
    else if (uuid.equalsIgnoreCase(PASSWORD_CHAR_UUID))
    {
      receivedPassword = value;
      credentialsReceived = true;
      Serial.println("Received Password: [hidden]");
    }
    else if (uuid.equalsIgnoreCase(RACK_NAME_CHAR_UUID))
    {
      rackNameReceived = true;
      Serial.println("\n========================================");
      Serial.println("  RACK NAME SETTLED: [" + value + "]");
      Serial.println("========================================\n");
      preferences.begin("wifi-config", false);
      preferences.putString("rack_name", value);
      preferences.end();
    }
  }
};

void setup()
{
  Serial.begin(115200);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  delay(500);

  Serial.println("\n========================================");
  Serial.println("      NURTURA RACK - BOOT SEQUENCE");
  Serial.println("========================================");

  if (digitalRead(BOOT_BUTTON_PIN) == LOW)
  {
    Serial.println("\n>>> BOOT BUTTON DETECTED <<<");
    Serial.println("Hold for 3 seconds to FACTORY RESET...");

    delay(3000);

    if (digitalRead(BOOT_BUTTON_PIN) == LOW)
    {
      Serial.println("\n!!! FACTORY RESET TRIGGERED !!!");
      Serial.println("Erasing WiFi credentials...");

      preferences.begin("wifi-config", false);
      preferences.clear();
      preferences.end();

      Serial.println("Factory reset complete. Restarting...");
      delay(1000);
      ESP.restart();
    }
    else
    {
      Serial.println("Button released. Factory reset cancelled.");
    }
  }

  WiFi.mode(WIFI_STA);
  delay(100);
  deviceMAC = WiFi.macAddress();

  while (deviceMAC == "00:00:00:00:00:00")
  {
    delay(200);
    deviceMAC = WiFi.macAddress();
  }

  Serial.println("\nDevice MAC: " + deviceMAC);
  Serial.println("(Print this on a QR code for Step 2 verification)");

  preferences.begin("wifi-config", false);
  String savedSSID = preferences.getString("ssid", "");
  String savedPass = preferences.getString("password", "");

  if (savedSSID.length() > 0)
  {
    Serial.println("\nFound saved WiFi credentials!");
    Serial.println("SSID: " + savedSSID);
    Serial.println("Attempting to connect...");

    WiFi.begin(savedSSID.c_str(), savedPass.c_str());

    int count = 0;
    while (WiFi.status() != WL_CONNECTED && count < 20)
    {
      delay(500);
      Serial.print(".");
      count++;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.println("\n\n✓ WiFi Connected!");
      Serial.print("IP Address: ");
      Serial.println(WiFi.localIP());
      Serial.println("\n========================================");
      Serial.println("    RACK IS ONLINE - MAIN APP RUNNING");
      Serial.println("========================================\n");

      return;
    }
    else
    {
      Serial.println("\n\n✗ WiFi connection failed.");
      Serial.println("Starting BLE setup mode...");
    }
  }
  else
  {
    Serial.println("\nNo saved WiFi credentials found.");
    Serial.println("Starting BLE setup mode...");
  }

  Serial.println("\n========================================");
  Serial.println("        BLE SETUP MODE ACTIVE");
  Serial.println("========================================");
  Serial.println("Waiting for app to connect...\n");

  BLEDevice::init("Nurtura Rack");
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);

  pService->createCharacteristic(
              DEVICE_ID_CHAR_UUID,
              BLECharacteristic::PROPERTY_READ)
      ->setValue(deviceMAC.c_str());

  MyCallbacks *cb = new MyCallbacks();
  pService->createCharacteristic(
              SSID_CHAR_UUID,
              BLECharacteristic::PROPERTY_WRITE)
      ->setCallbacks(cb);

  pService->createCharacteristic(
              PASSWORD_CHAR_UUID,
              BLECharacteristic::PROPERTY_WRITE)
      ->setCallbacks(cb);

  statusCharacteristic = pService->createCharacteristic(
      STATUS_CHAR_UUID,
      BLECharacteristic::PROPERTY_NOTIFY);
  statusCharacteristic->addDescriptor(new BLE2902());

  pService->createCharacteristic(
              RACK_NAME_CHAR_UUID,
              BLECharacteristic::PROPERTY_WRITE)
      ->setCallbacks(cb);

  pService->start();

  BLEAdvertising *pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->setScanResponse(true);
  pAdv->start();

  Serial.println(">>> BLE advertising started <<<");
  Serial.println(">>> Device is discoverable as 'Nurtura Rack' <<<\n");
}

void loop()
{
  if (credentialsReceived)
  {
    credentialsReceived = false;

    Serial.println("\n========================================");
    Serial.println("  TESTING WiFi CREDENTIALS FROM APP");
    Serial.println("========================================");

    delay(1000);

    Serial.println("Disconnecting from previous network...");
    WiFi.disconnect(true);
    delay(500);

    Serial.println("Connecting to: " + receivedSSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(receivedSSID.c_str(), receivedPassword.c_str());

    int timer = 0;
    while (WiFi.status() != WL_CONNECTED && timer < 30)
    {
      delay(500);
      Serial.print(".");
      timer++;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.println("\n\n✓ WiFi CONNECTION SUCCESS!");
      Serial.print("IP Address: ");
      Serial.println(WiFi.localIP());

      preferences.putString("ssid", receivedSSID);
      preferences.putString("password", receivedPassword);
      Serial.println("Credentials saved to flash memory.");

      statusCharacteristic->setValue("connected");
      statusCharacteristic->notify();

      Serial.println("\nWaiting for app to send rack name...");
      int waited = 0;
      while (!rackNameReceived && waited < 60)
      {
        delay(500);
        waited++;
      }

      if (rackNameReceived)
      {
        Serial.println("Rack name received. Restarting...\n");
      }
      else
      {
        Serial.println("Timeout waiting for rack name. Restarting anyway...\n");
      }

      delay(1000);
      ESP.restart();
    }
    else
    {
      Serial.println("\n\n✗ WiFi CONNECTION FAILED");
      Serial.println("Incorrect password or network unavailable.");

      statusCharacteristic->setValue("failed");
      statusCharacteristic->notify();

      Serial.println("\n>>> BLE still active. User can retry. <<<\n");

      delay(2000);
    }
  }
}
