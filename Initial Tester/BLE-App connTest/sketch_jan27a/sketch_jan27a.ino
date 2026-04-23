#include <NimBLEDevice.h>

#define SERVICE_UUID        "ABCD"
#define CHARACTERISTIC_UUID "1234" // The "file" where data lives
#define LED_PIN             2

NimBLECharacteristic* pCharacteristic = nullptr;
bool deviceConnected = false;

class MyServerCallbacks: public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) {
        deviceConnected = true;
        digitalWrite(LED_PIN, HIGH);
        Serial.println("✅ App Connected!");
    }
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {
        deviceConnected = false;
        digitalWrite(LED_PIN, LOW);
        Serial.println("❌ App Disconnected.");
        NimBLEDevice::startAdvertising();
    }
};

void setup() {
    Serial.begin(115200);
    pinMode(LED_PIN, OUTPUT);

    NimBLEDevice::init("NURTURA_V4");
    NimBLEServer* pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    // 1. Create the Service
    NimBLEService* pService = pServer->createService(SERVICE_UUID);

    // 2. Create the Characteristic (READ + NOTIFY)
    pCharacteristic = pService->createCharacteristic(
                        CHARACTERISTIC_UUID,
                        NIMBLE_PROPERTY::READ | 
                        NIMBLE_PROPERTY::NOTIFY
                      );

    // 3. Start everything
    pService->start();
    
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setName("NURTURA_V4");
    pAdvertising->enableScanResponse(true);
    pAdvertising->start();

    Serial.println("📡 Waiting for app to subscribe...");
}

void loop() {
    if (deviceConnected) {
        // Send a string to the app every 3 seconds
        static int counter = 0;
        String message = "Nurtura Data #" + String(counter++);
        
        pCharacteristic->setValue(message.c_str());
        pCharacteristic->notify(); // This "pushes" the data to the app
        
        Serial.println("Sent: " + message);
        delay(3000);
    }
}