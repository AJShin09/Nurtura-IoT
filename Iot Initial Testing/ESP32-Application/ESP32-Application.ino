#include <NimBLEDevice.h>
#include <WiFi.h>
#include <Preferences.h>

#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define VERIFY_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define SSID_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26c1"
#define PASSWORD_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26c2"
#define STATUS_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26c3"

NimBLECharacteristic *bleCharStatus;
String deviceMacAddress;
bool isVerified = false;
String receivedSSID = "";
String receivedPassword = "";
bool startWifiConnect = false;

Preferences preferences;

class ServerCallbacks : public NimBLEServerCallbacks
{
  void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override
  {
    Serial.println("[BLE] App Connected!");
  }
  void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) override
  {
    Serial.println("[BLE] Disconnected. Advertising...");
    NimBLEDevice::startAdvertising();
  }
};

class VerificationCallbacks : public NimBLECharacteristicCallbacks
{
  void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override
  {
    std::string rxValue = pCharacteristic->getValue();
    if (rxValue.length() > 0)
    {
      String receivedMac = String(rxValue.c_str());
      receivedMac.trim();
      receivedMac.toUpperCase();

      String localMac = deviceMacAddress;
      localMac.toUpperCase();

      if (receivedMac == localMac)
      {
        Serial.println("[VERIFY] Success: " + receivedMac);
        isVerified = true;
        pCharacteristic->setValue("VERIFIED");
      }
      else
      {
        Serial.println("[VERIFY] Failed. Expected: " + localMac + " Got: " + receivedMac);
        pCharacteristic->setValue("FAILED");
      }
      pCharacteristic->notify(); 
    }
  }
};

class WifiCallbacks : public NimBLECharacteristicCallbacks
{
  void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override
  {
    std::string val = pCharacteristic->getValue();
    String uuid = pCharacteristic->getUUID().toString().c_str();

    if (uuid == SSID_CHAR_UUID)
    {
      receivedSSID = String(val.c_str());
      Serial.println("[WIFI] SSID received");
    }
    else if (uuid == PASSWORD_CHAR_UUID)
    {
      receivedPassword = String(val.c_str());
      Serial.println("[WIFI] Password received");
      if (receivedSSID.length() > 0)
        startWifiConnect = true;
    }
  }
};

void connectToWiFi(String ssid, String password)
{
  Serial.println("[WIFI] Connecting to: " + ssid);
  WiFi.begin(ssid.c_str(), password.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20)
  {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println("");
}

void setup()
{
  Serial.begin(115200);
  preferences.begin("nurtura-store", false);

  String savedSSID = preferences.getString("ssid", "");
  String savedPass = preferences.getString("pass", "");

  if (savedSSID != "")
  {
    connectToWiFi(savedSSID, savedPass);
    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.println("[WIFI] Auto-connected! IP: " + WiFi.localIP().toString());
      return;
    }
  }

  NimBLEDevice::init("NURTURA_V4");
  deviceMacAddress = NimBLEDevice::getAddress().toString().c_str();

  NimBLEServer *pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  NimBLEService *pService = pServer->createService(SERVICE_UUID);

  pService->createCharacteristic(CHARACTERISTIC_UUID, NIMBLE_PROPERTY::READ)
      ->setValue(deviceMacAddress.c_str());

  NimBLECharacteristic *pVerify = pService->createCharacteristic(
      VERIFY_CHARACTERISTIC_UUID,
      NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
  pVerify->setCallbacks(new VerificationCallbacks());

  auto *wifiCb = new WifiCallbacks();
  pService->createCharacteristic(SSID_CHAR_UUID, NIMBLE_PROPERTY::WRITE)->setCallbacks(wifiCb);
  pService->createCharacteristic(PASSWORD_CHAR_UUID, NIMBLE_PROPERTY::WRITE)->setCallbacks(wifiCb);

  bleCharStatus = pService->createCharacteristic(STATUS_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);

  pService->start();

  NimBLEAdvertising *pAdv = NimBLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->start();

  Serial.println("[SYSTEM] BLE Ready. MAC: " + deviceMacAddress);
}

void loop()
{
  if (startWifiConnect && isVerified)
  {
    startWifiConnect = false;
    connectToWiFi(receivedSSID, receivedPassword);

    if (WiFi.status() == WL_CONNECTED)
    {
      Serial.println("[WIFI] Success!");
      preferences.putString("ssid", receivedSSID);
      preferences.putString("pass", receivedPassword);

      bleCharStatus->setValue("CONNECTED");
      bleCharStatus->notify();

      delay(3000);
      NimBLEDevice::deinit(true); 
    }
    else
    {
      Serial.println("[WIFI] Failed!");
      bleCharStatus->setValue("FAILED");
      bleCharStatus->notify();
    }
  }
  delay(20);
}