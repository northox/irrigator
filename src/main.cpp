#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <Ticker.h>

#define RELAY0_PIN D1  // 05
#define RELAY1_PIN D2  // 04
#define RELAY2_PIN D5  // 14
#define LED_PIN LED_BUILTIN 

const char* ssid = "x";
const char* password = "y";
const char* mqttServer = "192.168.1.2";
const int mqttPort = 1883;
const long  OFF_DELAY  = 30UL * 60UL * 1000UL;  // 30 minutes in millis

WiFiClient espClient;
PubSubClient mqttClient(espClient);
Ticker wifiReconnectTicker;
Ticker watchdog;

// Per-relay “last ON” timestamps
unsigned long lastOnTime[3] = { 0, 0, 0 };

void watchdogReset() {
  ESP.restart();
}

void checkWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
  } else {
    Serial.println("WiFi connected");
  }
}

void connectToMQTT() {
  while (!mqttClient.connected()) {
    if (mqttClient.connect("ESP8266Client")) {
      mqttClient.subscribe("irrigation/control0");
      mqttClient.subscribe("irrigation/control1");
      mqttClient.subscribe("irrigation/control2");
    }
    delay(500);
  }
  Serial.println("Connected to MQTT and subscribed.");
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  int idx = -1;
  if (String(topic) == "irrigation/control0") idx = 0;
  if (String(topic) == "irrigation/control1") idx = 1;
  if (String(topic) == "irrigation/control2") idx = 2;

  if (idx >= 0) {
    uint8_t pin = (idx == 0 ? RELAY0_PIN : idx == 1 ? RELAY1_PIN : RELAY2_PIN);
    if (msg.equalsIgnoreCase("on")) {
      digitalWrite(pin, HIGH);
      lastOnTime[idx] = millis();
      Serial.printf("Relay %d ON @ %lu\n", idx, lastOnTime[idx]);
    }
    else if (msg.equalsIgnoreCase("off")) {
      digitalWrite(pin, LOW);
      lastOnTime[idx] = 0;
      Serial.printf("Relay %d OFF\n", idx);
    }
  } 
}

void setup() {
  Serial.begin(9600);
  Serial.println("Booting");

  pinMode(LED_PIN, OUTPUT);
  pinMode(RELAY0_PIN, OUTPUT);
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  digitalWrite(RELAY0_PIN, LOW);
  digitalWrite(RELAY1_PIN, LOW);
  digitalWrite(RELAY2_PIN, LOW);
  Serial.println("All relays off by default");

  watchdog.attach(5 * 60, watchdogReset);

  WiFi.begin(ssid, password);
  wifiReconnectTicker.attach(30, checkWiFiConnection);
  Serial.println("Wifi completed");

  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setCallback(mqttCallback);
  Serial.println("MQTT completed");

  connectToMQTT();
  Serial.println("Setup completed");
}

void loop() {
  if (!mqttClient.connected()) {
    connectToMQTT();
  }
  mqttClient.loop();

  // check each relay for 30-minute timeout
  unsigned long now = millis();
  for (int i = 0; i < 3; i++) {
    if (lastOnTime[i] != 0 && now - lastOnTime[i] >= OFF_DELAY) {
      // timeout expired -> turn OFF
      uint8_t pin = (i == 0 ? RELAY0_PIN : i == 1 ? RELAY1_PIN : RELAY2_PIN);
      digitalWrite(pin, LOW);
      Serial.printf("Relay %d AUTO-OFF after 30 min\n", i);
      lastOnTime[i] = 0;
    }
  }

  delay(500);
  digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  Serial.println("Loop");
}
