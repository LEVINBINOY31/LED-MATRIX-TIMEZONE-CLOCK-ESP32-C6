#include <WiFi.h>
#include <WiFiMulti.h>
#include <time.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>

// --- Hardware Settings ---
#define INTERNAL_LED_PIN 8
#define DATA_PIN 15
#define CLK_PIN 20
#define CS_PIN 7
#define MAX_DEVICES 4
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define DATE_BTN_PIN 4
#define CONFIG_BTN_PIN 14

MD_Parola P = MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);
WiFiMulti wifiMulti;
WebServer server(80);
DNSServer dnsServer;
Preferences preferences;

// --- State Variables ---
bool configMode = false;
unsigned long lastSyncAttempt = 0;
const unsigned long syncInterval = 21600000;  // 6 hours
unsigned long wifiTimeoutStart = 0;
enum SyncState { IDLE, CONNECTING_WIFI, GETTING_TIME };
SyncState currentSyncState = IDLE;

bool showingDate = false;
unsigned long dateDisplayStartTime = 0;
static char dateString[30];

// --- Time & Sync Settings ---
long gmtOffset_sec = 19800;        // default: India (UTC+5:30)
bool tzAuto = true;                // true = detect from IP, false = use manual offset
const int daylightOffset_sec = 0;  // offsets already include DST

// --- Manual timezone list (label, offset in seconds) ---
struct TZEntry {
  const char* label;
  long offset;
};
const TZEntry tzList[] = {
  {"UTC-12:00", -43200},
  {"UTC-11:00", -39600},
  {"UTC-10:00 Hawaii", -36000},
  {"UTC-09:00 Alaska", -32400},
  {"UTC-08:00 US Pacific", -28800},
  {"UTC-07:00 US Mountain", -25200},
  {"UTC-06:00 US Central", -21600},
  {"UTC-05:00 US Eastern", -18000},
  {"UTC-04:00 Atlantic", -14400},
  {"UTC-03:00 Brazil", -10800},
  {"UTC-02:00", -7200},
  {"UTC-01:00", -3600},
  {"UTC+00:00 London / UTC", 0},
  {"UTC+01:00 Central Europe", 3600},
  {"UTC+02:00 Eastern Europe", 7200},
  {"UTC+03:00 Moscow / Saudi", 10800},
  {"UTC+03:30 Iran", 12600},
  {"UTC+04:00 UAE / Oman", 14400},
  {"UTC+04:30 Afghanistan", 16200},
  {"UTC+05:00 Pakistan", 18000},
  {"UTC+05:30 India / Sri Lanka", 19800},
  {"UTC+05:45 Nepal", 20700},
  {"UTC+06:00 Bangladesh", 21600},
  {"UTC+07:00 Thailand / Vietnam", 25200},
  {"UTC+08:00 China / Singapore", 28800},
  {"UTC+09:00 Japan / Korea", 32400},
  {"UTC+09:30 Adelaide", 34200},
  {"UTC+10:00 Sydney", 36000},
  {"UTC+12:00 New Zealand", 43200}
};
const int tzCount = sizeof(tzList) / sizeof(tzList[0]);

// --- Ultra Bold Font Mapping ---
const uint8_t boldFont[] PROGMEM = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  2, 0x00, 0x00, 5, 0x00, 0x00, 0x00, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0,
  3, 0xEF, 0x45, 0xEF, 3, 0xEF, 0x45, 0xE7, 0, 3, 0x18, 0x18, 0x18, 0, 0,
  5, 0xFF, 0xFF, 0xC3, 0xFF, 0xFF, 5, 0xC4, 0xC6, 0xFF, 0xFF, 0xC0,
  5, 0xFB, 0xFB, 0xDB, 0xDF, 0xDF, 5, 0xDB, 0xDB, 0xDB, 0xFF, 0xFF,
  5, 0x1F, 0x1F, 0x18, 0xFF, 0xFF, 5, 0xDF, 0xDF, 0xDB, 0xFB, 0xFB,
  5, 0xFF, 0xFF, 0xDB, 0xFB, 0xFB, 5, 0x03, 0x03, 0x03, 0xFF, 0xFF,
  5, 0xFF, 0xFF, 0xDB, 0xFF, 0xFF, 5, 0xDF, 0xDF, 0xDB, 0xFF, 0xFF,
  2, 0x66, 0x66, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  5, 0xDF, 0xDF, 0xDB, 0xFB, 0xFB, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  5, 0xFF, 0xFF, 0xC3, 0xC3, 0xC3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  5, 0xFF, 0xFF, 0x03, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  5, 0xDF, 0xDF, 0xD8, 0xFF, 0xFF, 0, 0, 0, 0, 0
};

bool isButtonPressed(int pin, int requiredTimeMs) {
  if (digitalRead(pin) == HIGH) return false;
  unsigned long start = millis();
  while (digitalRead(pin) == LOW) {
    if (millis() - start >= requiredTimeMs) return true;
    delay(10);
  }
  return false;
}

// --- Auto timezone from IP (skipped when manual mode is selected) ---
void fetchTimezoneOffset() {
  if (!tzAuto) return;                          // manual mode: keep saved offset
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.setTimeout(5000);
  http.begin("http://ip-api.com/line/?fields=offset");
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    payload.trim();
    long off = payload.toInt();
    // Valid offsets are -12h..+14h; "0" must literally be "0" (not a failed parse)
    if ((off != 0 || payload == "0") && off >= -43200 && off <= 50400) {
      gmtOffset_sec = off;
      preferences.begin("clock-settings", false);
      preferences.putLong("gmtOffset", gmtOffset_sec);
      preferences.end();
    }
  }
  http.end();
}

// Loads saved WiFi networks and timezone settings.
// Returns the number of saved networks (0 = nothing saved yet).
int loadWiFiConfigs() {
  int added = 0;
  preferences.begin("clock-wifi", true);
  int count = preferences.getInt("count", 0);
  for (int i = 0; i < count; i++) {
    String s = preferences.getString(("s" + String(i)).c_str(), "");
    String p = preferences.getString(("p" + String(i)).c_str(), "");
    if (s != "") {
      wifiMulti.addAP(s.c_str(), p.c_str());
      added++;
    }
  }
  preferences.end();

  preferences.begin("clock-settings", true);
  gmtOffset_sec = preferences.getLong("gmtOffset", 19800);
  tzAuto = preferences.getBool("tzAuto", true);
  preferences.end();
  return added;
}

// --- Brightness Web Handler ---
void handleBrightness() {
  if (server.hasArg("level")) {
    int level = server.arg("level").toInt();
    level = constrain(level, 0, 15);
    preferences.begin("clock-settings", false);
    preferences.putInt("brightness", level);
    preferences.end();
    P.setIntensity(level);
    server.send(200, "text/html", "Brightness Set to " + String(level) + ". <a href='/'>Back</a>");
  } else {
    server.send(400, "text/html", "Missing level. <a href='/'>Back</a>");
  }
}

// --- Timezone Web Handler ---
void handleSetTZ() {
  String v = server.arg("tz");
  if (v == "") {
    server.send(400, "text/html", "Missing timezone. <a href='/'>Back</a>");
    return;
  }
  preferences.begin("clock-settings", false);
  if (v == "auto") {
    preferences.putBool("tzAuto", true);
  } else {
    long off = v.toInt();
    if (off < -43200 || off > 50400) {
      preferences.end();
      server.send(400, "text/html", "Invalid offset. <a href='/'>Back</a>");
      return;
    }
    preferences.putBool("tzAuto", false);
    preferences.putLong("gmtOffset", off);
  }
  preferences.end();
  server.send(200, "text/html", "Timezone saved! Restarting...");
  delay(2000);
  ESP.restart();
}

void handleRoot() {
  preferences.begin("clock-settings", true);
  int currentBright = preferences.getInt("brightness", 5);
  preferences.end();

  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{background:#121212; color:#00adb5; font-family:sans-serif; text-align:center; padding:20px;}";
  html += "input,select{width:100%; padding:12px; margin:10px 0; border:1px solid #333; background:#222; color:#fff; border-radius:8px;}";
  html += ".btn{background:#00adb5; color:#fff; border:none; padding:15px; width:100%; border-radius:8px; font-weight:bold;}";
  html += ".slider{margin: 20px 0; width:100%;}";
  html += ".list-item{background:#1e1e1e; padding:15px; margin-bottom:10px; border-radius:8px; display:flex; justify-content:space-between; align-items:center;}";
  html += "a{color:#ff4d4d; text-decoration:none; font-size:1.2em;}</style></head><body>";
  html += "<h1>Clock Settings</h1>";

  // Brightness Control UI
  html += "<h3>Brightness (" + String(currentBright) + ")</h3>";
  html += "<form action='/setBright' method='POST'>";
  html += "<input type='range' name='level' min='0' max='15' value='" + String(currentBright) + "' class='slider' onchange='this.form.submit()'>";
  html += "<input type='submit' value='Set Brightness' class='btn'></form><br><hr>";

  // Timezone Control UI
  html += "<h3>Timezone</h3>";
  html += "<form action='/setTZ' method='POST'><select name='tz'>";
  html += "<option value='auto'";
  if (tzAuto) html += " selected";
  html += ">Auto (detect from location)</option>";
  for (int i = 0; i < tzCount; i++) {
    html += "<option value='" + String(tzList[i].offset) + "'";
    if (!tzAuto && gmtOffset_sec == tzList[i].offset) html += " selected";
    html += ">" + String(tzList[i].label) + "</option>";
  }
  html += "</select>";
  html += "<input type='submit' value='Save Timezone & Restart' class='btn'></form>";
  html += "<p style='font-size:0.8em;color:#888'>Manual mode does not adjust for daylight saving.</p><br><hr>";

  // Saved networks
  html += "<h3>Saved Networks</h3>";
  preferences.begin("clock-wifi", true);
  int count = preferences.getInt("count", 0);
  for (int i = 0; i < count; i++) {
    String s = preferences.getString(("s" + String(i)).c_str(), "");
    if (s != "") html += "<div class='list-item'><span>" + s + "</span><a href='/delete?id=" + String(i) + "'>&times;</a></div>";
  }
  preferences.end();

  html += "<h3>Add Network</h3><form action='/save' method='POST'>";
  html += "<input name='ssid' placeholder='SSID' required>";
  html += "<input name='pass' type='password' placeholder='Password'>";
  html += "<input type='submit' value='Save & Restart' class='btn'></form></body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  String s = server.arg("ssid");
  String p = server.arg("pass");
  if (s != "") {
    preferences.begin("clock-wifi", false);
    int count = preferences.getInt("count", 0);
    preferences.putString(("s" + String(count)).c_str(), s);
    preferences.putString(("p" + String(count)).c_str(), p);
    preferences.putInt("count", count + 1);
    preferences.end();
    server.send(200, "text/html", "Saved! Restarting...");
    delay(2000);
    ESP.restart();
  } else {
    server.send(400, "text/html", "SSID required. <a href='/'>Back</a>");
  }
}

void handleDelete() {
  int id = server.arg("id").toInt();
  preferences.begin("clock-wifi", false);
  preferences.remove(("s" + String(id)).c_str());
  preferences.remove(("p" + String(id)).c_str());
  preferences.end();
  server.send(200, "text/html", "Deleted. <a href='/'>Go Back</a>");
}

void enterConfigMode() {
  if (configMode) return;
  configMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32-Matrix-Clock", "");
  dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/delete", handleDelete);
  server.on("/setBright", HTTP_POST, handleBrightness);
  server.on("/setTZ", HTTP_POST, handleSetTZ);
  server.onNotFound(handleRoot);
  server.begin();

  P.displayClear();
  P.setFont(NULL);
  P.print("SET WIFI");
  neopixelWrite(INTERNAL_LED_PIN, 0, 0, 100);
}

void initialSync() {
  P.setFont(NULL);
  P.print("Search...");
  int retry = 0;
  while (wifiMulti.run() != WL_CONNECTED && retry < 15) {
    for (int i = 0; i < 5; i++) {
      if (isButtonPressed(CONFIG_BTN_PIN, 1000)) { enterConfigMode(); return; }
      delay(200);
    }
    retry++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    P.print("Locating...");
    fetchTimezoneOffset();
    P.print("NTP...");
    configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org", "time.google.com");
    struct tm timeinfo;
    int ntpCounter = 0;
    while (!getLocalTime(&timeinfo) && ntpCounter < 10) {
      if (isButtonPressed(CONFIG_BTN_PIN, 1000)) { enterConfigMode(); return; }
      delay(500);
      ntpCounter++;
    }
    if (ntpCounter < 10) { P.print("Done!"); lastSyncAttempt = millis(); }
  } else {
    P.print("Offline");
  }
  delay(1000);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  P.displayClear();
}

void handleBackgroundSync() {
  if (configMode) return;
  switch (currentSyncState) {
    case IDLE:
      if (millis() - lastSyncAttempt >= syncInterval) {
        WiFi.mode(WIFI_STA); wifiMulti.run();
        wifiTimeoutStart = millis(); currentSyncState = CONNECTING_WIFI;
      }
      break;
    case CONNECTING_WIFI:
      if (wifiMulti.run() == WL_CONNECTED) {
        fetchTimezoneOffset();
        configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org");
        currentSyncState = GETTING_TIME; wifiTimeoutStart = millis();
      } else if (millis() - wifiTimeoutStart > 25000) {
        WiFi.mode(WIFI_OFF); lastSyncAttempt = millis(); currentSyncState = IDLE;
      }
      break;
    case GETTING_TIME:
      struct tm timeinfo;
      if (getLocalTime(&timeinfo) || (millis() - wifiTimeoutStart > 10000)) {
        lastSyncAttempt = millis(); WiFi.mode(WIFI_OFF); currentSyncState = IDLE;
      }
      break;
  }
}

void setup() {
  Serial.begin(115200);
  neopixelWrite(INTERNAL_LED_PIN, 0, 0, 0);
  pinMode(DATE_BTN_PIN, INPUT_PULLUP);
  pinMode(CONFIG_BTN_PIN, INPUT_PULLUP);

  P.begin();

  // Load saved brightness
  preferences.begin("clock-settings", true);
  int savedBright = preferences.getInt("brightness", 5);
  preferences.end();
  P.setIntensity(savedBright);

  P.setTextAlignment(PA_CENTER);
  if (loadWiFiConfigs() == 0) {
    enterConfigMode();  // no networks saved yet: open the setup portal
  } else {
    initialSync();
  }
}

void loop() {
  if (isButtonPressed(CONFIG_BTN_PIN, 1000)) enterConfigMode();
  if (configMode) {
    dnsServer.processNextRequest();
    server.handleClient();
    return;
  }

  handleBackgroundSync();
  if (currentSyncState != IDLE) neopixelWrite(INTERNAL_LED_PIN, 20, 0, 0);
  else neopixelWrite(INTERNAL_LED_PIN, 0, 10, 0);

  if (isButtonPressed(DATE_BTN_PIN, 200) && !showingDate) {
    showingDate = true;
    dateDisplayStartTime = millis();
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) strftime(dateString, sizeof(dateString), "%A %d-%m-%Y", &timeinfo);
    else strcpy(dateString, "No Sync");
    P.displayClear();
    P.setFont(NULL);
    P.displayText(dateString, PA_CENTER, 40, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
  }

  if (showingDate) {
    if (P.displayAnimate()) {
      if (millis() - dateDisplayStartTime < 10000) P.displayReset();
      else { showingDate = false; P.displayClear(); }
    }
  } else {
    struct tm timeinfo;
    char timeString[12];
    if (getLocalTime(&timeinfo)) {
      strftime(timeString, sizeof(timeString), "%I:%M", &timeinfo);
      if (timeinfo.tm_sec % 2 != 0) timeString[2] = ' ';
      if (timeString[0] == '0') timeString[0] = ' ';
      int len = strlen(timeString);
      timeString[len] = (timeinfo.tm_hour < 12) ? '*' : '+';
      timeString[len + 1] = '\0';
      P.setFont(boldFont);
      P.print(timeString);
    } else {
      P.setFont(NULL); P.print("--:--");
    }
  }
}
