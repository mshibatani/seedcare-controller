/*********
 Power Controller by Temperature with Moisture Sensors.

 Configuration:
 ESP32 Dev Module (Akiduki)

  Author: Masayuki Shibatani
  Date:   2022/4/4
  Modified (Claude Code Opus 4.6): 2026/3/2 - Non-blocking redesign + checkpoint logging + watchdog timer
*********/

#include <Arduino.h>
#include <time.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <esp_task_wdt.h>
#include "config.h"

// ===== Common =====
#define MAX_MESSAGELENGTH 256
#define TIMEZONE_JST (3600 * 9)

// ===== Checkpoint Logging =====
// RTC memory survives soft reboot, SPIFFS survives power loss
#define CP_LOOP_START  0x01
#define CP_TEMP_BEGIN  0x10
#define CP_TEMP_END    0x11
#define CP_RELAY_BEGIN 0x20
#define CP_RELAY_END   0x21
#define CP_WIFI_BEGIN  0x30
#define CP_WIFI_END    0x31
#define CP_MQTT_BEGIN  0x40
#define CP_MQTT_END    0x41
#define CP_LCD_BEGIN   0x50
#define CP_LCD_END     0x51

RTC_DATA_ATTR uint8_t lastCheckpoint = 0;
RTC_DATA_ATTR uint32_t bootCount = 0;

#define CRASH_LOG_PATH "/crash_log.txt"
#define MAX_CRASH_LOG_ENTRIES 100

static const char* checkpointName(uint8_t cp) {
  switch (cp) {
    case CP_LOOP_START:  return "LOOP_START";
    case CP_TEMP_BEGIN:  return "TEMP_BEGIN";
    case CP_TEMP_END:    return "TEMP_END";
    case CP_RELAY_BEGIN: return "RELAY_BEGIN";
    case CP_RELAY_END:   return "RELAY_END";
    case CP_WIFI_BEGIN:  return "WIFI_BEGIN";
    case CP_WIFI_END:    return "WIFI_END";
    case CP_MQTT_BEGIN:  return "MQTT_BEGIN";
    case CP_MQTT_END:    return "MQTT_END";
    case CP_LCD_BEGIN:   return "LCD_BEGIN";
    case CP_LCD_END:     return "LCD_END";
    default:             return "UNKNOWN";
  }
}

// verbose=true only for the 10-second cycle to avoid serial spam
void checkpoint(uint8_t cp, bool verbose = false) {
  lastCheckpoint = cp;
  if (verbose) {
    Serial.printf("[CP] 0x%02X %s\n", cp, checkpointName(cp));
  }
}

void setupSPIFFS() {
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed");
    return;
  }
  // On boot, record previous crash checkpoint
  if (bootCount > 0 && lastCheckpoint != 0) {
    File f = SPIFFS.open(CRASH_LOG_PATH, FILE_APPEND);
    if (f) {
      char entry[80];
      time_t now = time(NULL);
      if (now > 1700000000) {  // NTP synced (after 2023)
        struct tm* tmNow = localtime(&now);
        snprintf(entry, sizeof(entry), "Boot#%lu CP=0x%02X(%s) %04d/%02d/%02d %02d:%02d:%02d\n",
                 (unsigned long)bootCount, lastCheckpoint, checkpointName(lastCheckpoint),
                 tmNow->tm_year + 1900, tmNow->tm_mon + 1, tmNow->tm_mday,
                 tmNow->tm_hour, tmNow->tm_min, tmNow->tm_sec);
      } else {  // NTP not yet synced
        snprintf(entry, sizeof(entry), "Boot#%lu CP=0x%02X(%s) (no NTP)\n",
                 (unsigned long)bootCount, lastCheckpoint, checkpointName(lastCheckpoint));
      }
      f.print(entry);
      f.close();
      Serial.printf("Crash log recorded: last CP=0x%02X\n", lastCheckpoint);
    }
    // Trim log if too large
    File check = SPIFFS.open(CRASH_LOG_PATH, FILE_READ);
    if (check) {
      int lines = 0;
      while (check.available()) {
        if (check.read() == '\n') lines++;
      }
      check.close();
      if (lines > MAX_CRASH_LOG_ENTRIES) {
        // Read all, keep last MAX_CRASH_LOG_ENTRIES entries
        File src = SPIFFS.open(CRASH_LOG_PATH, FILE_READ);
        String kept = "";
        int skip = lines - MAX_CRASH_LOG_ENTRIES;
        int count = 0;
        while (src.available()) {
          String line = src.readStringUntil('\n');
          if (count >= skip) {
            kept += line + "\n";
          }
          count++;
        }
        src.close();
        File dst = SPIFFS.open(CRASH_LOG_PATH, FILE_WRITE);
        if (dst) {
          dst.print(kept);
          dst.close();
        }
      }
    }
  }
  bootCount++;
  lastCheckpoint = 0;
}

// ===== WiFi (Non-blocking) =====
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

enum WifiState { WIFI_IDLE, WIFI_CONNECTING };
WifiState wifiState = WIFI_IDLE;
unsigned long wifiConnectStartMs = 0;
#define WIFI_CONNECT_TIMEOUT_MS 15000  // 15 seconds max per attempt
#define WIFI_RETRY_INTERVAL_MS  30000  // wait 30s before retrying

unsigned long wifiLastAttemptMs = 0;

bool isWifiConnected() {
  return WiFi.status() == WL_CONNECTED;
}

// Non-blocking WiFi reconnection - call from loop()
void handleWifiReconnect() {
  if (isWifiConnected()) {
    wifiState = WIFI_IDLE;
    return;
  }

  unsigned long now = millis();

  switch (wifiState) {
    case WIFI_IDLE:
      if (now - wifiLastAttemptMs < WIFI_RETRY_INTERVAL_MS && wifiLastAttemptMs != 0) {
        return; // too soon to retry
      }
      Serial.println("WiFi: starting reconnect...");
      WiFi.disconnect();
      WiFi.begin(ssid, password);
      wifiConnectStartMs = now;
      wifiLastAttemptMs = now;
      wifiState = WIFI_CONNECTING;
      break;

    case WIFI_CONNECTING:
      if (isWifiConnected()) {
        Serial.print("WiFi: connected, IP=");
        Serial.println(WiFi.localIP());
        configTime(TIMEZONE_JST, 0, "ntp.nict.jp", "ntp.jst.mfeed.ad.jp");
        wifiState = WIFI_IDLE;
      } else if (now - wifiConnectStartMs > WIFI_CONNECT_TIMEOUT_MS) {
        Serial.println("WiFi: connect timeout");
        WiFi.disconnect();
        wifiState = WIFI_IDLE;
      }
      break;
  }
}

void setupWifi() {
  Serial.println("Setting up WiFi...");
  WiFi.begin(ssid, password);

  // Block briefly in setup only (5 seconds max)
  unsigned long start = millis();
  while (!isWifiConnected() && millis() - start < 5000) {
    delay(100);
  }
  if (isWifiConnected()) {
    Serial.print("WiFi connected, IP=");
    Serial.println(WiFi.localIP());
    configTime(TIMEZONE_JST, 0, "ntp.nict.jp", "ntp.jst.mfeed.ad.jp");
    delay(1000); // wait for NTP
  } else {
    Serial.println("WiFi: initial connect failed, will retry in loop");
  }
}

// ===== Temperature Sensors (Non-blocking) =====
#include <OneWire.h>
#include <DallasTemperature.h>

#define ONE_WIRE_BUS 32
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensors(&oneWire);
#define MAX_TEMPDEVICES 4
int numberOfTempDevices = 0;
float gCurrentTemp[MAX_TEMPDEVICES];

// Non-blocking temperature state
enum TempState { TEMP_IDLE, TEMP_CONVERTING };
TempState tempState = TEMP_IDLE;
unsigned long tempConvStartMs = 0;
#define TEMP_CONVERSION_TIMEOUT_MS 2000  // 2 seconds max (normal is 750ms)

void setupTempSensor() {
  tempSensors.begin();
  tempSensors.setWaitForConversion(false); // Non-blocking mode

  for (int i = 0; i < 10; i++) {
    numberOfTempDevices = tempSensors.getDeviceCount();
    Serial.printf("Temp sensor check: found %d devices\n", numberOfTempDevices);
    if (numberOfTempDevices > 0) break;
    delay(1000);
  }
  if (numberOfTempDevices > MAX_TEMPDEVICES) {
    numberOfTempDevices = MAX_TEMPDEVICES;
  }
}

// Start async temperature conversion
void requestTempAsync() {
  if (tempState == TEMP_IDLE) {
    tempSensors.requestTemperatures();
    tempConvStartMs = millis();
    tempState = TEMP_CONVERTING;
  }
}

// Check if conversion is done, read results. Returns true when data is ready.
bool handleTempConversion() {
  if (tempState != TEMP_CONVERTING) return false;

  if (tempSensors.isConversionComplete() ||
      millis() - tempConvStartMs > TEMP_CONVERSION_TIMEOUT_MS) {
    for (int i = 0; i < numberOfTempDevices; i++) {
      float temp = tempSensors.getTempCByIndex(i);
      if (temp != DEVICE_DISCONNECTED_C) {
        gCurrentTemp[i] = temp;
      } else {
        Serial.printf("Temp sensor %d: disconnected\n", i);
      }
    }
    tempState = TEMP_IDLE;
    return true;
  }
  return false;
}

// ===== Moisture Sensor =====
#define MAX_MOISTUREDEVICES 2
const int AirMoistureValue = 3433;
const int WaterMoistureValue = 1449;
int gCurrentMoisture[MAX_MOISTUREDEVICES];
int portsForMoisture[] = {A6, A7};

void gettingMoisture() {
  for (int i = 0; i < MAX_MOISTUREDEVICES; i++) {
    int raw = analogRead(portsForMoisture[i]);
    gCurrentMoisture[i] = constrain(
      map(raw, AirMoistureValue, WaterMoistureValue, 0, 100), 0, 100);
  }
}

// ===== Power Relay =====
#define MAX_RELAYDEVICES 2
#define RELAY_A 0
#define RELAY_B 1

#define UPPER_TEMP_0 28.0
#define LOWER_TEMP_0 27.5
#define UPPER_TEMP_1 23.0
#define LOWER_TEMP_1 22.5

int portsForRelay[] = {18, 19};
float upperTemp[] = {UPPER_TEMP_0, UPPER_TEMP_1};
float lowerTemp[] = {LOWER_TEMP_0, LOWER_TEMP_1};

#define LOWER_MOISTURE_0 20
#define LOWER_MOISTURE_1 20
#define UPPER_MOISTURE_0 21
#define UPPER_MOISTURE_1 21
int upperMoisture[] = {UPPER_MOISTURE_0, UPPER_MOISTURE_1};
int lowerMoisture[] = {LOWER_MOISTURE_0, LOWER_MOISTURE_1};

char gCurrentRelayState[] = {0, 0};

void setupRelay() {
  for (int i = 0; i < MAX_RELAYDEVICES; i++) {
    pinMode(portsForRelay[i], OUTPUT);
    digitalWrite(portsForRelay[i], LOW);  // Ensure known state at boot
    gCurrentRelayState[i] = 0;
  }
}

void relayOnByForce(int relayNo) {
  digitalWrite(portsForRelay[relayNo], HIGH);
  gCurrentRelayState[relayNo] = 1;
}

void relayOffByForce(int relayNo) {
  digitalWrite(portsForRelay[relayNo], LOW);
  gCurrentRelayState[relayNo] = 0;
}

boolean detectTurnOff(int relayNo) {
  return gCurrentTemp[relayNo] > upperTemp[relayNo];
}

boolean detectTurnOn(int relayNo) {
  return gCurrentTemp[relayNo] < lowerTemp[relayNo];
}

void treatPowerRelay() {
  for (int i = 0; i < MAX_RELAYDEVICES; i++) {
    if (detectTurnOff(i)) {
      relayOffByForce(i);
    } else if (detectTurnOn(i)) {
      relayOnByForce(i);
    }
  }
}

// ===== MQTT (forward declarations for Web UI) =====
#include <PubSubClient.h>

WiFiClient espClient;
PubSubClient mqttClient(espClient);

#define MQTT_MAX_RETRIES 3
#define MQTT_RETRY_INTERVAL_MS 10000
unsigned long mqttLastAttemptMs = 0;
int mqttRetryCount = 0;

// ===== Web Server =====
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

AsyncWebServer server(80);

const char* PARAM_RADIOGROUP_0 = "radiogroup0";
const char* PARAM_RADIOGROUP_1 = "radiogroup1";

const char index_html_template[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head>
  <title>Plant Temperature and Moisture Controller</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  </head><body>
  Temperature 0: %TEMP_0%Celsius<p>
  Temperature 1: %TEMP_1%Celsius<p>
  Moisture 0: %MOISTURE_0%<p>
  Moisture 1: %MOISTURE_1%<p>
  <form action="/get">
    Heater 0 Switch:
    <input type="radio" name="radiogroup0" value="0" %HEATER_RADIO_ZERO0%>Off(Default)
    <input type="radio" name="radiogroup0" value="1" %HEATER_RADIO_ZERO1%>On
    <p>
    Heater 1 Switch:
    <input type="radio" name="radiogroup1" value="0" %HEATER_RADIO_ONE0%>Off(Default)
    <input type="radio" name="radiogroup1" value="1" %HEATER_RADIO_ONE1%>On
    <p>
    <input type="submit" value="Submit">
  </form>
  <hr>
  <b>Status:</b> WiFi=%WIFI_STATUS% | MQTT=%MQTT_STATUS% | Boot#%BOOT_COUNT% | Uptime=%UPTIME%s<p>
  <a href="/update">Firmware update</a><p>
  <a href="/logs">Crash logs</a><p>
  <a href="/">Redraw the page</a>
</body></html>)rawliteral";

void notFound(AsyncWebServerRequest *request) {
  request->send(404, "text/plain", "Not found");
}

String processor(const String& var) {
  char msgStr[MAX_MESSAGELENGTH];

  if (var.substring(0, String("HEATER_RADIO_ZERO").length()) == "HEATER_RADIO_ZERO") {
    int radioNumber = var.substring(String("HEATER_RADIO_ZERO").length()).toInt();
    if (radioNumber == gCurrentRelayState[0]) return F("checked ");
  }
  if (var.substring(0, String("HEATER_RADIO_ONE").length()) == "HEATER_RADIO_ONE") {
    int radioNumber = var.substring(String("HEATER_RADIO_ONE").length()).toInt();
    if (radioNumber == gCurrentRelayState[1]) return F("checked ");
  }
  if (var.substring(0, String("TEMP_").length()) == "TEMP_") {
    int itemNo = var.substring(String("TEMP_").length()).toInt();
    sprintf(msgStr, "%2.2f", gCurrentTemp[itemNo]);
    return String(msgStr);
  }
  if (var.substring(0, String("MOISTURE_").length()) == "MOISTURE_") {
    int itemNo = var.substring(String("MOISTURE_").length()).toInt();
    return String(gCurrentMoisture[itemNo]) + "%%";
  }
  if (var == "WIFI_STATUS") {
    return isWifiConnected() ? "Connected" : "Disconnected";
  }
  if (var == "MQTT_STATUS") {
    char buf[64];
    if (mqttClient.connected()) {
      return "Connected";
    } else {
      snprintf(buf, sizeof(buf), "Disconnected (rc=%d, retries=%d/%d)",
               mqttClient.state(), mqttRetryCount, MQTT_MAX_RETRIES);
      return String(buf);
    }
  }
  if (var == "BOOT_COUNT") {
    return String((unsigned long)bootCount);
  }
  if (var == "UPTIME") {
    return String(millis() / 1000);
  }
  return String();
}

#define RELAY_STATE_ON 1
void relayControl(int whichRelay, int onOff) {
  if (onOff == RELAY_STATE_ON) {
    relayOnByForce(whichRelay);
  } else {
    relayOffByForce(whichRelay);
  }
}

void setupWebServer() {
  for (int i = 0; i < MAX_RELAYDEVICES; i++) {
    relayControl(i, gCurrentRelayState[i]);
  }

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html_template, processor);
  });

  server.on("/get", HTTP_GET, [](AsyncWebServerRequest *request) {
    boolean processed = false;
    if (request->hasParam(PARAM_RADIOGROUP_0)) {
      relayControl(RELAY_A, request->getParam(PARAM_RADIOGROUP_0)->value().toInt());
      processed = true;
    }
    if (request->hasParam(PARAM_RADIOGROUP_1)) {
      relayControl(RELAY_B, request->getParam(PARAM_RADIOGROUP_1)->value().toInt());
      processed = true;
    }
    request->send(200, "text/html",
      "HTTP GET request sent to your ESP<br><a href=\"/\">Return to Home Page</a>");
  });

  // Crash log endpoint
  server.on("/logs", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (SPIFFS.exists(CRASH_LOG_PATH)) {
      request->send(SPIFFS, CRASH_LOG_PATH, "text/plain");
    } else {
      request->send(200, "text/plain", "No crash logs recorded.");
    }
  });

  server.onNotFound(notFound);
  server.begin();
}

// ===== MQTT (Non-blocking) =====
const char* mqtt_server = MQTT_SERVER;
const char mqttClientName[] = MQTT_CLIENT_NAME;
const char mqtt_topic[] = MQTT_TOPIC;

void setupMQTT() {
  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setSocketTimeout(5);  // 5 seconds max for TCP connect (default 15)
}

void publishMQTTmessage(const char *theTopic, char *theMessage) {
  char topicStr[MAX_MESSAGELENGTH];
  snprintf(topicStr, sizeof(topicStr), "%s/%s", mqtt_topic, theTopic);
  mqttClient.publish(topicStr, theMessage);
}

String ipToString(uint32_t ip) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
           ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
  return String(buf);
}

// Non-blocking MQTT reconnection. Returns true if connected.
bool handleMqttReconnect() {
  if (mqttClient.connected()) {
    mqttRetryCount = 0;
    return true;
  }
  if (!isWifiConnected()) return false;

  unsigned long now = millis();
  if (now - mqttLastAttemptMs < MQTT_RETRY_INTERVAL_MS && mqttLastAttemptMs != 0) {
    return false;
  }

  if (mqttRetryCount >= MQTT_MAX_RETRIES) {
    // Reset after cooldown to try again next cycle
    if (now - mqttLastAttemptMs > MQTT_RETRY_INTERVAL_MS * 3) {
      mqttRetryCount = 0;
    }
    return false;
  }

  Serial.printf("MQTT: connect attempt %d/%d...\n", mqttRetryCount + 1, MQTT_MAX_RETRIES);
  mqttLastAttemptMs = now;
  mqttRetryCount++;

  if (mqttClient.connect(mqttClientName)) {
    Serial.println("MQTT: connected");
    char msgStr[MAX_MESSAGELENGTH];
    publishMQTTmessage("Connected", (char *)"");
    snprintf(msgStr, sizeof(msgStr), "%s", ipToString(WiFi.localIP()).c_str());
    publishMQTTmessage("Information", msgStr);
    mqttRetryCount = 0;
    return true;
  }

  Serial.printf("MQTT: failed, rc=%d\n", mqttClient.state());
  return false;
}

void loggingAtMQTT() {
  if (!mqttClient.connected()) return;

  mqttClient.loop();

  char topicStr[MAX_MESSAGELENGTH];
  char msgStr[MAX_MESSAGELENGTH];

  // DateTime
  time_t unixTime = time(NULL);
  struct tm* tmNow = localtime(&unixTime);
  snprintf(msgStr, sizeof(msgStr), "%04d/%02d/%02d %02d:%02d:%02d",
           tmNow->tm_year + 1900, tmNow->tm_mon + 1, tmNow->tm_mday,
           tmNow->tm_hour, tmNow->tm_min, tmNow->tm_sec);
  publishMQTTmessage("DateTime", msgStr);

  // Moisture
  for (int i = 0; i < MAX_MOISTUREDEVICES; i++) {
    snprintf(topicStr, sizeof(topicStr), "Moisture/%d", i);
    snprintf(msgStr, sizeof(msgStr), "%d", gCurrentMoisture[i]);
    publishMQTTmessage(topicStr, msgStr);
  }

  // Temperature
  for (int i = 0; i < numberOfTempDevices; i++) {
    snprintf(topicStr, sizeof(topicStr), "Temperature/%d", i);
    snprintf(msgStr, sizeof(msgStr), "%2.2f", gCurrentTemp[i]);
    publishMQTTmessage(topicStr, msgStr);
  }

  // Relay state
  for (int i = 0; i < MAX_RELAYDEVICES; i++) {
    snprintf(topicStr, sizeof(topicStr), "Relay/%d", i);
    snprintf(msgStr, sizeof(msgStr), "%s", (gCurrentRelayState[i] ? "ON" : "OFF"));
    publishMQTTmessage(topicStr, msgStr);
  }
}

// ===== LCD =====
#include <Wire.h>
#include <ST7032.h>

#define MAX_MESSAGELINES 2
ST7032 lcd;

void setupLCD(const char *whoAreYou) {
  lcd.begin(16, 2);
  lcd.setContrast(30);
  lcd.setCursor(0, 0);
  char helloMsg[MAX_MESSAGELENGTH];
  snprintf(helloMsg, sizeof(helloMsg), "Hello %s!", whoAreYou);
  lcd.print(helloMsg);
}

void displayLCD(char stateChar) {
  char msgStr[MAX_MESSAGELENGTH];
  for (int i = 0; i < MAX_MESSAGELINES; i++) {
    snprintf(msgStr, sizeof(msgStr), "%2.2fC%c %2d%%%c%-3s ",
             gCurrentTemp[i], 0xdf, gCurrentMoisture[i], stateChar,
             (gCurrentRelayState[i] ? "ON" : "OFF"));
    lcd.setCursor(0, i);
    lcd.print(msgStr);
  }
}

// ===== OTA Update =====
#include <AsyncElegantOTA.h>

void setupOTAupdate() {
  AsyncElegantOTA.begin(&server);
}

// ===== Watchdog Timer =====
#define WDT_TIMEOUT_SEC 30  // Reset if stuck for 30 seconds

void setupWatchdog() {
  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);  // true = trigger reset on timeout
  esp_task_wdt_add(NULL);  // Add current task (loopTask)
  Serial.printf("WDT: configured for %d seconds\n", WDT_TIMEOUT_SEC);
}

// ===== Main Loop Timing =====
#define LOOP_INTERVAL_MS 10000  // 10 seconds main cycle

unsigned long lastLoopMs = 0;

// ===== Setup =====
void setup() {
  Serial.begin(115200);
  Serial.printf("\n=== Boot #%lu, last CP=0x%02X ===\n", (unsigned long)bootCount, lastCheckpoint);

  setupSPIFFS();
  setupLCD(mqtt_topic);

  lcd.setCursor(0, 0); lcd.print("WiFi setup      "); setupWifi();
  lcd.setCursor(0, 0); lcd.print("OTA setup       "); setupOTAupdate();
  lcd.setCursor(0, 0); lcd.print("Temp setup      "); setupTempSensor();
  lcd.setCursor(0, 0); lcd.print("Relay setup     "); setupRelay();
  lcd.setCursor(0, 0); lcd.print("WebServer setup "); setupWebServer();
  lcd.setCursor(0, 0); lcd.print("MQTT setup      "); setupMQTT();

  setupWatchdog();

  // Kick off first temperature conversion
  requestTempAsync();
  lastLoopMs = millis();

  Serial.println("Setup complete.");
}

// ===== Loop (millis-based, non-blocking) =====
void loop() {
  esp_task_wdt_reset();  // Feed watchdog every loop iteration

  unsigned long now = millis();

  // Always handle WiFi reconnection (non-blocking, silent checkpoint)
  checkpoint(CP_WIFI_BEGIN);
  handleWifiReconnect();
  checkpoint(CP_WIFI_END);

  // Always handle MQTT reconnection (non-blocking, silent checkpoint)
  checkpoint(CP_MQTT_BEGIN);
  handleMqttReconnect();
  mqttClient.loop();
  checkpoint(CP_MQTT_END);

  // Check if temperature conversion is complete (silent checkpoint)
  checkpoint(CP_TEMP_BEGIN);
  handleTempConversion();
  checkpoint(CP_TEMP_END);

  // Main cycle every LOOP_INTERVAL_MS
  if (now - lastLoopMs >= LOOP_INTERVAL_MS) {
    lastLoopMs = now;
    checkpoint(CP_LOOP_START, true);

    // Read moisture (fast, no delay needed)
    gettingMoisture();

    // Control relays based on latest temperature
    checkpoint(CP_RELAY_BEGIN, true);
    treatPowerRelay();
    checkpoint(CP_RELAY_END, true);

    // Log to MQTT
    checkpoint(CP_MQTT_BEGIN, true);
    if (isWifiConnected() && mqttClient.connected()) {
      loggingAtMQTT();
    }
    checkpoint(CP_MQTT_END, true);

    // Update LCD
    checkpoint(CP_LCD_BEGIN, true);
    displayLCD(isWifiConnected() ? ' ' : '*');
    checkpoint(CP_LCD_END, true);

    // Start next temperature conversion for next cycle
    requestTempAsync();
  }
}
