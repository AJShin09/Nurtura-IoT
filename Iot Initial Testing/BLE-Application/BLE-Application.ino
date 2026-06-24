#include <NimBLEDevice.h>

#define SERVICE_UUID "ABCD"
#define CHARACTERISTIC_UUID "1234"
#define LED_PIN 2

NimBLECharacteristic *pCharacteristic = nullptr;
bool deviceConnected = false;

class MyServerCallbacks : public NimBLEServerCallbacks
{
    void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo)
    {
        deviceConnected = true;
        digitalWrite(LED_PIN, HIGH);
        Serial.println("✅ App Connected!");
    }
    void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason)
    {
        deviceConnected = false;
        digitalWrite(LED_PIN, LOW);
        Serial.println("❌ App Disconnected.");
        NimBLEDevice::startAdvertising();
    }
};

void setup()
{
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);

    NimBLEDevice::init("NURTURA_V4");
    NimBLEServer *pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    NimBLEService *pService = pServer->createService(SERVICE_UUID);

    pCharacteristic = pService->createCharacteristic(
        CHARACTERISTIC_UUID,
        NIMBLE_PROPERTY::READ |
            NIMBLE_PROPERTY::NOTIFY);

    pService->start();

    NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setName("NURTURA_V4");
    pAdvertising->enableScanResponse(true);
    pAdvertising->start();

    Serial.println("📡 Waiting for app to subscribe...");
}

void loop()
{
    if (deviceConnected)
    {
        static int counter = 0;
        String message = "Nurtura Data #" + String(counter++);

        pCharacteristic->setValue(message.c_str());
        pCharacteristic->notify(); 

        Serial.println("Sent: " + message);
        delay(3000);
    }
}