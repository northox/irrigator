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

const unsigned long OFF_DELAY      = 30UL * 60UL * 1000UL;  // 30 min auto-off
const unsigned long SLEEP_MS       = 10000UL;               // health check cadence
const unsigned long MAX_BACKOFF    = 40000UL;               // cap for WiFi backoff
const unsigned long HEARTBEAT_MS   = 5UL * 60UL * 1000UL;   // diagnostics cadence

// Connectivity watchdog: if we cannot reach both WiFi and the broker for this
// long, close every valve and reboot. The hardware/software WDT cannot catch
// this - loop() keeps running happily while offline, so it stays fed forever.
const unsigned long MAX_OFFLINE_MS = 15UL * 60UL * 1000UL;  // 15 min

const char* CONTROL_TOPIC[3] = { "irrigation/control0",
                                 "irrigation/control1",
                                 "irrigation/control2" };
const char* STATUS_TOPIC[3]  = { "irrigation/status0",
                                 "irrigation/status1",
                                 "irrigation/status2" };
const char* LOG_TOPIC        = "irrigation/log";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

volatile unsigned long lastOnTime[3] = {0,0,0}; // per-relay ON timestamps
unsigned long lastHealthMs   = 0;
unsigned long lastHeartbeatMs = 0;
unsigned long wifiBackoff    = 3000UL;
unsigned long lastOnlineMs   = 0;   // last moment WiFi + MQTT were both up
unsigned int  wifiRetryCount = 0;

unsigned long ledPatternStart = 0;

inline uint8_t pinOf(int i) {
  return (i == 0 ? RELAY0_PIN : i == 1 ? RELAY1_PIN : RELAY2_PIN);
}

// Declared up front because ensureWIFI() calls it during its blocking wait.
// The Arduino preprocessor would normally generate this, but relying on that
// breaks under some toolchains.
void enforceAutoOff();

void armHardwareWDT() {
  // NOTE: the timeout argument to wdtEnable() is ignored by the ESP8266 core.
  // The software WDT fires at roughly 1.6-3.2 s and the hardware WDT at about
  // 6 s; neither period is configurable from here. This only guards against a
  // genuinely hung loop() - see MAX_OFFLINE_MS for the connectivity watchdog.
  ESP.wdtDisable();
  ESP.wdtEnable(0);
}

// Publish a zone's state so Home Assistant's MQTT switch can resolve.
// Retained, so HA picks up the correct state after a restart.
void publishStatus(int idx) {
  if (!mqttClient.connected()) return;
  mqttClient.publish(STATUS_TOPIC[idx], lastOnTime[idx] ? "on" : "off", true);
}

// Single place where a relay changes state, so status is never out of sync.
void setRelay(int idx, bool isOn) {
  digitalWrite(pinOf(idx), isOn ? HIGH : LOW);
  lastOnTime[idx] = isOn ? millis() : 0;
  publishStatus(idx);
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
  if (WiFi.status() == WL_CONNECTED) { wifiRetryCount = 0; return; }

  wifiRetryCount++;
  // WiFi.reconnect() alone can fail indefinitely if the station config has
  // gone stale (AP changed channel, DHCP lease expired, association wedged).
  // Every third attempt, tear it down and start from scratch instead.
  const bool hardRetry = (wifiRetryCount % 3 == 0);

  Serial.printf("WiFi down (attempt %u, %s). Retrying for %lu ms\n",
                wifiRetryCount, hardRetry ? "full re-begin" : "reconnect", wifiBackoff);

  if (hardRetry) {
    WiFi.disconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
  } else {
    WiFi.reconnect();
  }

  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < wifiBackoff) {
    yield();
    delay(50);
    enforceAutoOff();   // keep the safety timer honest during a long wait
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi reconnected: "); Serial.println(WiFi.localIP());
    wifiBackoff = 2000UL;   // reset
    wifiRetryCount = 0;
  } else {
    Serial.println("WiFi retry failed.");
    wifiBackoff = min(wifiBackoff * 2, MAX_BACKOFF); // exponential backoff
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  int idx = -1;
  for (int i = 0; i < 3; i++) {
    if (strcmp(topic, CONTROL_TOPIC[i]) == 0) { idx = i; break; }
  }
  if (idx < 0) return;

  bool isOn = false;
  if (length >= 2) {
    char c0 = (char)payload[0];
    char c1 = (char)payload[1];
    if ((c0=='o'||c0=='O') && (c1=='n'||c1=='N')) isOn = true;
  }

  setRelay(idx, isOn);
  Serial.printf("Relay %d %s\n", idx, isOn ? "ON" : "OFF");
}

void setupMQTT() {
  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(30);
  mqttClient.setBufferSize(512);
}

void publishDiagnostics(const char* why) {
  if (!mqttClient.connected()) return;
  char buf[160];
  snprintf(buf, sizeof(buf),
           "%s reset=%s ip=%s rssi=%d uptime=%lus",
           why,
           ESP.getResetReason().c_str(),
           WiFi.localIP().toString().c_str(),
           WiFi.RSSI(),
           millis() / 1000UL);
  mqttClient.publish(LOG_TOPIC, buf);
  Serial.println(buf);
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
  for (int i = 0; i < 3; i++) mqttClient.subscribe(CONTROL_TOPIC[i]);

  // Tell HA what the valves are actually doing, so the switches stop
  // sitting at "unknown", then report why we last rebooted.
  for (int i = 0; i < 3; i++) publishStatus(i);
  publishDiagnostics("connected");
}

void enforceAutoOff() {
  unsigned long now = millis();
  for (int i = 0; i < 3; i++) {
    if (lastOnTime[i] != 0 && now - lastOnTime[i] >= OFF_DELAY) {
      setRelay(i, false);
      Serial.printf("Relay %d AUTO-OFF after 30 min\n", i);
    }
  }
}

bool anyRelayOn() {
  return lastOnTime[0] || lastOnTime[1] || lastOnTime[2];
}

// Reboot if we have been unreachable for too long. The WDT cannot do this:
// loop() keeps executing while offline, so it stays fed indefinitely.
void checkConnectivityWatchdog(unsigned long now) {
  if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
    lastOnlineMs = now;
    return;
  }
  if (now - lastOnlineMs < MAX_OFFLINE_MS) return;

  Serial.println("Offline too long - closing valves and rebooting.");
  for (int i = 0; i < 3; i++) digitalWrite(pinOf(i), LOW);
  Serial.flush();
  delay(200);

  ESP.restart();

  // ESP.restart() can wedge if the chip was last started by a serial upload
  // rather than a real reset. If we are still here, stop feeding the software
  // WDT and let the hardware WDT (~6 s) force the reset instead.
  ESP.wdtDisable();
  while (true) { }
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
  Serial.print("Reset reason: "); Serial.println(ESP.getResetReason());

  pinMode(LED_PIN, OUTPUT);
  pinMode(RELAY0_PIN, OUTPUT);
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  digitalWrite(RELAY0_PIN, LOW);
  digitalWrite(RELAY1_PIN, LOW);
  digitalWrite(RELAY2_PIN, LOW);

  lastOnlineMs = millis();   // do not reboot instantly on a slow first connect

  armHardwareWDT();
  setupWIFI();
  setupMQTT();
}

void loop() {
  unsigned long now = millis();

  if (now - lastHealthMs >= SLEEP_MS) {
    ensureWIFI();
    ensureMQTT();
    checkConnectivityWatchdog(now);
    lastHealthMs = now;
  }

  if (now - lastHeartbeatMs >= HEARTBEAT_MS) {
    publishDiagnostics("heartbeat");
    lastHeartbeatMs = now;
  }

  mqttClient.loop();
  enforceAutoOff();
  driveLedNonBlocking();

  ESP.wdtFeed();
  delay(1);
}
