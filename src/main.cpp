#include <ESP8266WiFi.h>
#include <PubSubClient.h>

// Wi-Fi + MQTT credentials live in config.h, which is git-ignored.
// Copy config.example.h to config.h and fill in your own values.
#include "config.h"

#define RELAY0_PIN D1  // GPIO5  - zone 0
#define RELAY1_PIN D2  // GPIO4  - zone 1
#define RELAY2_PIN D5  // GPIO14 - zone 2
#define LED_PIN    LED_BUILTIN

const char* ssid         = WIFI_SSID;
const char* password     = WIFI_PASSWORD;
const char* mqttServer   = MQTT_SERVER;
const int   mqttPort     = MQTT_PORT;
const char* mqttUser     = MQTT_USER;
const char* mqttPassword = MQTT_PASSWORD;

const unsigned long OFF_DELAY   = 30UL * 60UL * 1000UL;  // 30 min auto-off
const unsigned long SLEEP_MS    = 10000UL;               // health check cadence
const unsigned long MAX_BACKOFF = 40000UL;               // cap for WiFi backoff

WiFiClient espClient;
PubSubClient mqttClient(espClient);

volatile unsigned long lastOnTime[3] = {0,0,0}; // per-relay ON timestamps
unsigned long lastHealthMs = 0;
unsigned long wifiBackoff  = 3000UL;

unsigned long ledPatternStart = 0;

void armHardwareWDT() {
  ESP.wdtDisable();
  ESP.wdtEnable(60000); // 60 s window
}

void setupWIFI() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.begin(ssid, password);

  const unsigned long deadline = millis() + 20000UL; // 20 s boot window
  while (WiFi.status() != WL_CONNECTED && (long)(millis() - deadline) < 0) {
    yield();
    delay(50);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi up: "); Serial.println(WiFi.localIP());
    wifiBackoff = 2000UL;
  } else {
    Serial.println("WiFi boot connect timeout; will retry in loop.");
  }
}

void ensureWIFI() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.print("WiFi down. Retrying for ");
  Serial.print(wifiBackoff); Serial.println(" ms");
  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED && (millis() - start) < wifiBackoff) {
    WiFi.reconnect();
    for (int i = 0; i < 10; i++) { yield(); delay(50); } // ~500 ms with yields
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi reconnected.");
    wifiBackoff = 2000UL; // reset
  } else {
    Serial.println("WiFi retry failed.");
    wifiBackoff = min(wifiBackoff * 2, MAX_BACKOFF); // exponential backoff
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  int idx = -1;
  if      (strcmp(topic, "irrigation/control0") == 0) idx = 0;
  else if (strcmp(topic, "irrigation/control1") == 0) idx = 1;
  else if (strcmp(topic, "irrigation/control2") == 0) idx = 2;
  if (idx < 0) return;

  bool isOn = false;
  if (length >= 2) {
    char c0 = (char)payload[0];
    char c1 = (char)payload[1];
    if ((c0=='o'||c0=='O') && (c1=='n'||c1=='N')) isOn = true;
  }

  uint8_t pin = (idx == 0 ? RELAY0_PIN : idx == 1 ? RELAY1_PIN : RELAY2_PIN);
  if (isOn) {
    digitalWrite(pin, HIGH);
    lastOnTime[idx] = millis();
    Serial.printf("Relay %d ON @ %lu\n", idx, lastOnTime[idx]);
  } else {
    digitalWrite(pin, LOW);
    lastOnTime[idx] = 0;
    Serial.printf("Relay %d OFF\n", idx);
  }
}

void setupMQTT() {
  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(30);
  mqttClient.setBufferSize(512);
}

void ensureMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) return;

  Serial.print("MQTT down. Reconnecting... ");
  String cid = String("ESP8266Client-irrigator-") + String(ESP.getChipId(), HEX);

  for (int attempt = 0; attempt < 3 && !mqttClient.connected(); attempt++) {
    mqttClient.connect(cid.c_str(), mqttUser, mqttPassword);
    for (int k = 0; k < 10; k++) { mqttClient.loop(); yield(); delay(50); }
  }

  if (!mqttClient.connected()) {
    Serial.println("failed.");
    return;
  }

  Serial.println("connected.");
  mqttClient.subscribe("irrigation/control0");
  mqttClient.subscribe("irrigation/control1");
  mqttClient.subscribe("irrigation/control2");
  mqttClient.publish("irrigation/log", "Reconnect.");
}

inline uint8_t pinOf(int i) {
  return (i == 0 ? RELAY0_PIN : i == 1 ? RELAY1_PIN : RELAY2_PIN);
}

void enforceAutoOff() {
  unsigned long now = millis();
  for (int i = 0; i < 3; i++) {
    if (lastOnTime[i] != 0 && now - lastOnTime[i] >= OFF_DELAY) {
      uint8_t pin = pinOf(i);
      digitalWrite(pin, LOW);
      lastOnTime[i] = 0;
      Serial.printf("Relay %d AUTO-OFF after 30 min\n", i);
    }
  }
}

bool anyRelayOn() {
  return lastOnTime[0] || lastOnTime[1] || lastOnTime[2];
}

// Map system health to LED pattern
// solid when active; fast blink if WiFi down; medium blink if MQTT down; normal otherwise
void getLedPattern(unsigned long& periodMs, unsigned long& onMs, bool& solidOn) {
  bool wifiOK = (WiFi.status() == WL_CONNECTED);
  bool mqttOK = mqttClient.connected();

  if (anyRelayOn()) { solidOn = true; periodMs = 0; onMs = 0; return; }
  if (!wifiOK)      { solidOn = false; periodMs = 250;  onMs = 125;  return; }
  if (!mqttOK)      { solidOn = false; periodMs = 500;  onMs = 250;  return; }
  solidOn = false;  periodMs = 1000; onMs = 500;        // healthy
}

void driveLedNonBlocking() {
  static bool ledInit = false;
  if (!ledInit) { ledInit = true; ledPatternStart = millis(); }

  unsigned long periodMs, onMs; bool solidOn;
  getLedPattern(periodMs, onMs, solidOn);
  unsigned long now = millis();

  if (solidOn) {
    digitalWrite(LED_PIN, HIGH);
    ledPatternStart = now;
    return;
  }
  if (periodMs == 0) {
    digitalWrite(LED_PIN, LOW);
    ledPatternStart = now;
    return;
  }
  unsigned long phase = (now - ledPatternStart) % periodMs;
  digitalWrite(LED_PIN, (phase < onMs) ? HIGH : LOW);
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

  armHardwareWDT();
  setupWIFI();
  setupMQTT();
}

void loop() {
  unsigned long now = millis();

  if (now - lastHealthMs >= SLEEP_MS) {
    ensureWIFI();
    ensureMQTT();
    lastHealthMs = now;
  }

  mqttClient.loop();
  enforceAutoOff();
  driveLedNonBlocking();

  ESP.wdtFeed();
  delay(1);
}
