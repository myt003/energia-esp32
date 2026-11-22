/*
 * Energia ESP32 — read-only department display
 * ---------------------------------------------------------------------------
 * Arduino IDE sketch for the supplied hardware. There is no microphone,
 * recording, upload, or voice code. The device only fetches server-approved
 * summaries and shows them on the OLED.
 *
 *   OLED SSD1306 I2C : VCC=3V3  GND=GND  SDA=GPIO21  SCL=GPIO22  addr=0x3C
 *   POWER button     : GPIO32 -> GND (INPUT_PULLUP)
 *                       short press = display off / on
 *                       long press  = deep sleep; press again to wake
 *
 * Pages advance automatically through each department, OVERALL SCORE, and
 * BEST EMPLOYEE, then repeat from the first department.
 *
 * Required Arduino IDE libraries:
 *   - WiFi / WiFiClientSecure / HTTPClient / Wire  (ESP32 core built-ins)
 *   - Adafruit GFX Library
 *   - Adafruit SSD1306
 *   - ArduinoJson (7.x)
 *
 * Fill the LOCAL CONFIG values on your machine. Keep the device token out of
 * source control; it is a bearer credential for this device.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include <string.h>

// ============================ LOCAL CONFIG ============================
#define WIFI_SSID "vivo Y19s"
#define WIFI_PASS "00000000"

// UUID of the ESPDevice record in Django admin ("Lobby display").
#define DEVICE_ID "78671e4c-7aaa-461c-a87e-08ab7f38546e"

// Device bearer token issued by Django admin (demo device token).
#define DEVICE_TOKEN "energia-demo-device-token"

// API origin only, no trailing slash.
static const char API_BASE[] = "https://street-swim-investor-cave.trycloudflare.com";

// 0 = setInsecure(), 1 = validate ROOT_CA below.
#define VERIFY_TLS 0
static const char ROOT_CA[] PROGMEM = "";

#define POLL_SECONDS 60
#define PAGE_SECONDS 4
#define MAX_DEPARTMENTS 12
// ======================================================================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDRESS 0x3C
#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define POWER_PIN 32

#define OLED_COLS 21

struct DepartmentView {
  String name;
  String score;
  String status;
};

static DepartmentView departments[MAX_DEPARTMENTS];
static int departmentCount = 0;
static String overallScore = "--";
static String bestName = "none this period";
static String bestScore = "--";
static String lastMessage = "Starting";

static int page = 0;
static bool displayOn = true;
static unsigned long lastFetch = 0;
static unsigned long lastPageChange = 0;

// ----------------------------- Display helpers -----------------------------

static void drawChrome(const String &title) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(title.substring(0, OLED_COLS));
  display.println(F("---------------------"));
}

static int totalPages() {
  return departmentCount > 0 ? departmentCount + 2 : 1;
}

static void drawFooter() {
  display.setCursor(0, 56);
  display.print(page + 1);
  display.print(F("/"));
  display.print(totalPages());
  display.print(F(" AUTO PWR"));
  display.display();
}

static void printWrapped(const String &text, int maxLines = 3) {
  int lines = 0;
  for (int i = 0; i < (int)text.length() && lines < maxLines; i += OLED_COLS) {
    display.println(text.substring(i, min(i + OLED_COLS, (int)text.length())));
    lines++;
  }
}

static void drawStatus(const String &message) {
  drawChrome(F("Energia ESP32"));
  printWrapped(message, 3);
  drawFooter();
}

static void drawPage() {
  if (!displayOn) return;

  if (departmentCount == 0) {
    drawChrome(F("NO DATA"));
    display.println(lastMessage.substring(0, OLED_COLS));
    display.println(F("Waiting for API..."));
    drawFooter();
    return;
  }

  if (page < departmentCount) {
    const DepartmentView &department = departments[page];
    drawChrome(String("DEPT ") + String(page + 1) + ": " + department.name);
    display.print(F("SCORE "));
    display.print(department.score);
    display.println(F("/100"));
    display.print(F("STATUS "));
    display.println(department.status.substring(0, OLED_COLS));
    display.print(F("Next in "));
    display.print(PAGE_SECONDS);
    display.println(F("s"));
    drawFooter();
    return;
  }

  if (page == departmentCount) {
    drawChrome(F("OVERALL SCORE"));
    display.setTextSize(2);
    display.setCursor(10, 24);
    display.print(overallScore);
    display.setTextSize(1);
    display.setCursor(0, 46);
    display.print(F("Average of "));
    display.print(departmentCount);
    display.println(F(" departments"));
    drawFooter();
    return;
  }

  drawChrome(F("BEST EMPLOYEE"));
  printWrapped(bestName, 2);
  display.print(F("SCORE "));
  display.print(bestScore);
  display.println(F("/100"));
  drawFooter();
}

// ----------------------------- Power / buttons -----------------------------

static void goToSleep() {
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)POWER_PIN, 0);
  esp_deep_sleep_start();
}

static void toggleDisplay() {
  displayOn = !displayOn;
  if (displayOn) {
    display.ssd1306_command(SSD1306_DISPLAYON);
    lastPageChange = millis();
    drawPage();
  } else {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
  }
}

static bool powerHeldLongEnough(unsigned long heldMs) {
  return heldMs >= 2000;
}

// ----------------------------- API -----------------------------------------

static String scoreText(JsonVariantConst value) {
  if (value.is<int>()) return String(value.as<int>());
  if (value.is<float>()) return String(value.as<float>(), 1);
  return String("--");
}

static bool fetchSummary() {
  if (WiFi.status() != WL_CONNECTED) {
    lastMessage = "WiFi not connected";
    return false;
  }
  if (API_BASE[0] == '\0' || DEVICE_ID[0] == '\0' || DEVICE_TOKEN[0] == '\0') {
    lastMessage = "Set API/device config";
    return false;
  }

  String url = String(API_BASE) + "/api/v1/devices/" + String(DEVICE_ID) +
               "/department-summaries/";
  String auth = String("Device ") + String(DEVICE_TOKEN);

  HTTPClient http;
  WiFiClientSecure secure;
  if (url.startsWith("https://")) {
#if VERIFY_TLS
    secure.setCACert(ROOT_CA);
#else
    secure.setInsecure();
#endif
    http.begin(secure, url);
  } else {
    http.begin(url);
  }
  http.addHeader("Authorization", auth.c_str());
  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent", "energia-esp32/1.0");
  http.setTimeout(15000);

  const int code = http.GET();
  if (code != 200) {
    lastMessage = String("HTTP ") + String(code);
    http.end();
    return false;
  }

  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, http.getStream());
  http.end();
  if (error) {
    lastMessage = "Bad JSON response";
    return false;
  }

  departmentCount = 0;
  JsonArray source = doc["departments"].as<JsonArray>();
  for (JsonObject item : source) {
    if (departmentCount >= MAX_DEPARTMENTS) break;
    DepartmentView &view = departments[departmentCount];
    view.name = item["name"] | "-";
    view.score = scoreText(item["score"]);
    view.status = item["status"] | "-";
    departmentCount++;
  }

  overallScore = scoreText(doc["overall_score"]);

  JsonObject top = doc["top_employee"].as<JsonObject>();
  if (top.isNull()) {
    bestName = "none this period";
    bestScore = "--";
  } else {
    bestName = top["display_name"] | "-";
    bestScore = scoreText(top["score"]);
  }

  if (page >= totalPages()) page = 0;
  lastMessage = "Updated";
  return true;
}

static void refreshNow() {
  drawStatus("Updating...");
  fetchSummary();
  lastFetch = millis();
  lastPageChange = millis();
  drawPage();
}

// ----------------------------- Main flow -----------------------------------

static void handlePowerButton() {
  static bool powerWasDown = false;
  static bool longPressHandled = false;
  static unsigned long powerDownAt = 0;
  const unsigned long now = millis();

  const bool powerDown = digitalRead(POWER_PIN) == LOW;

  if (powerDown && !powerWasDown) {
    powerDownAt = now;
    longPressHandled = false;
  }
  if (powerDown && !longPressHandled &&
      powerHeldLongEnough(now - powerDownAt)) {
    longPressHandled = true;
    while (digitalRead(POWER_PIN) == LOW) delay(10);
    goToSleep();
  }
  if (!powerDown && powerWasDown && !longPressHandled &&
      now - powerDownAt >= 40) {
    toggleDisplay();
  }
  powerWasDown = powerDown;
}

void setup() {
  Serial.begin(115200);
  pinMode(POWER_PIN, INPUT_PULLUP);

  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
  display.setTextWrap(false);
  drawStatus(F("Energia ESP32"));

  if (WIFI_SSID[0] != '\0') {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
      delay(500);
      attempts++;
      drawStatus(String(F("WiFi... ")) + String(attempts) + F("/40"));
    }
    if (WiFi.status() != WL_CONNECTED) {
      drawStatus(F("WiFi FAIL"));
    }
  } else {
    drawStatus(F("Set WIFI_SSID"));
  }

  refreshNow();
}

void loop() {
  handlePowerButton();

  if (millis() - lastFetch >= (unsigned long)POLL_SECONDS * 1000UL) {
    refreshNow();
  }

  if (displayOn && departmentCount > 0 &&
      millis() - lastPageChange >= (unsigned long)PAGE_SECONDS * 1000UL) {
    page = (page + 1) % totalPages();
    lastPageChange = millis();
    drawPage();
  }

  delay(20);
}
