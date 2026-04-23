#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include "cert.h"
#include "secrets.h"

WiFiClientSecure net;
PubSubClient client(net);

unsigned long lastReconnectAttempt = 0;

// Non-blocking reconnect function
boolean reconnect() {
  Serial.print("Attempting MQTT connection...");
  String clientId = "Nurtura-" + String(random(0xffff), HEX);
  
  if (client.connect(clientId.c_str(), SECRET_MQTT_USER, SECRET_MQTT_PASS)) {
    Serial.println("CONNECTED!");
  } else {
    Serial.print("failed, rc=");
    Serial.println(client.state());
  }
  return client.connected();
}

void setup() {
  // 1. Give the power supply 2 seconds to stabilize 
  // (Crucial for wall adapters!)
  delay(2000); 

  Serial.begin(115200);
  WiFi.begin(SECRET_SSID, SECRET_PASS);
  
  // 2. Wait for WiFi
  int wifi_retries = 0;
  while (WiFi.status() != WL_CONNECTED && wifi_retries < 20) { 
    delay(500); 
    Serial.print("."); 
    wifi_retries++;
  }
  
  // 3. Sync Time with a Timeout
  configTime(0, 0, "pool.ntp.org");
  int time_retries = 0;
  while (time(nullptr) < 1000000000 && time_retries < 10) { 
    delay(1000); 
    time_retries++;
  }

  net.setCACert(root_ca);
  client.setServer(SECRET_MQTT_HOST, 8883);
}

void loop() {
  // Check connection status without stopping the loop
  if (!client.connected()) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > 5000) { // Try to reconnect every 5 seconds
      lastReconnectAttempt = now;
      if (reconnect()) {
        lastReconnectAttempt = 0;
      }
    }
  } else {
    // Client is connected, process incoming messages
    client.loop();
  }

  // --- YOUR SENSOR DATA (ALWAYS RUNS) ---
  static unsigned long lastMsg = 0;
  if (millis() - lastMsg > 10000) {
    lastMsg = millis();
    int moisture = analogRead(35);
    
    if (client.connected()) {
      String payload = "{\"moisture\":" + String(moisture) + "}";
      client.publish("nurtura/noah/rack1", payload.c_str());
      Serial.println("Sent: " + payload);
    } else {
      Serial.println("Moisture is: " + String(moisture) + " (Not sent, MQTT offline)");
    }
  }
  
  // You can put other code here (like blinking an LED) and it will NEVER lag!
}