/*
 * Energia ESP32 department display firmware.
 *
 * Story map:
 *   EN-ESP-001 - WiFiManager provisioning, HTTPS polling, revocable token,
 *                Preferences cache of the last valid allow-listed JSON.
 *   EN-ESP-002 - Two-button finite-state UI, debounced GPIO input, scrolling.
 *   EN-ESP-003 - Score/status/last-update rendering; scores are never recomputed.
 *   EN-ESP-004 - Only approved alias + score are displayed; privacy fallbacks.
 *   EN-ESP-005 - Power short/long press, deep sleep wake, offline/stale marker,
 *                bounded retries and watchdog protection.
 *
 * Hardware: ESP32 + 128x64 SSD1306 (I2C 0x3C) + exactly two active-low buttons.
 * No secrets are compiled in. All identity/token/CA values come from the
 * captive portal and are persisted with Preferences (NVS).
 */

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>

#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <time.h>

#include <cctype>
#include <cstdio>
#include <cstring>

#include "energia_ui_model.h"

using namespace energia;

namespace {

// ---------------------------------------------------------------------------
// Pin map and display
// ---------------------------------------------------------------------------
constexpr int kScreenWidth = 128;
constexpr int kScreenHeight = 64;
constexpr int kOledAddress = 0x3C;
constexpr int kSdaPin = 21;
constexpr int kSclPin = 22;

// Both buttons connect the GPIO to GND; INPUT_PULLUP makes them active LOW.
// GPIO33 is an RTC GPIO, so it can wake the device from deep sleep (EN-ESP-005).
constexpr gpio_num_t kNavButtonPin = GPIO_NUM_32;
constexpr gpio_num_t kPowerButtonPin = GPIO_NUM_33;

constexpr uint32_t kDebounceMs = 40;
constexpr uint32_t kLongPressMs = 1200;
constexpr uint32_t kWatchdogTimeoutS = 20;
constexpr uint8_t kWifiConnectTimeoutS = 30;

// Workspace palette is monochrome-mapped: deep navy #102A43 -> dark background,
// teal #0F766E -> status icons/outlines, lime #A3E635 -> inverted highlight
// block, light background -> WHITE pixels. Icons and labels carry the meaning.

Adafruit_SSD1306 display(kScreenWidth, kScreenHeight, &Wire, -1);
Preferences preferences;

UiModel ui;
DebouncedButton navButton(kDebounceMs, kLongPressMs);
DebouncedButton powerButton(kDebounceMs, kLongPressMs);

bool displayReady = false;
bool displayPowerOn = true;
bool hasDrawn = false;
uint32_t lastDrawnSignature = 0;

bool watchdogActive = false;
bool sleepPending = false;

char toastLine1[22];
char toastLine2[22];
uint32_t toastUntilMs = 0;

// ---------------------------------------------------------------------------
// Configuration (all values are provisioned, never hardcoded secrets)
// ---------------------------------------------------------------------------
struct Config {
  char deviceId[65];
  char apiBase[193];
  char token[513];
  char caPem[2048]; // one root CA PEM; literal "\n" sequences are accepted
  uint32_t pollSeconds;
  int32_t tzOffsetSeconds;
};

struct PollRuntime {
  uint32_t nextAttemptMs;
  uint8_t attempts;
  bool authBlocked;
};

Config config;
PollRuntime pollState{0, 0, false};

bool everFetched = false;
bool networkStale = true;
bool tokenRevoked = false;
uint32_t lastSuccessEpoch = 0;
uint32_t lastSuccessMs = 0;

constexpr uint8_t kMaxFetchAttempts = 3;
constexpr uint32_t kRetryDelaysMs[2] = {1000, 4000};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

void feedWatchdog() {
  if (watchdogActive) (void)esp_task_wdt_reset();
}

uint32_t nowEpoch() {
  const time_t t = time(nullptr);
  return t > 0 ? static_cast<uint32_t>(t) : 0U;
}

void trimInPlace(char* value) {
  if (value == nullptr) return;
  char* start = value;
  while (*start != '\0' && std::isspace(static_cast<unsigned char>(*start))) ++start;
  char* end = start + std::strlen(start);
  while (end > start && std::isspace(static_cast<unsigned char>(*(end - 1)))) --end;
  *end = '\0';
  if (start != value) std::memmove(value, start, static_cast<size_t>(end - start) + 1);
}

bool startsWithHttps(const char* value) {
  static const char prefix[] = "https://";
  for (size_t i = 0; i < sizeof(prefix) - 1; ++i) {
    if (value[i] == '\0') return false;
    if (std::tolower(static_cast<unsigned char>(value[i])) != prefix[i]) return false;
  }
  return true;
}

bool normalizeApiBase(char* url) {
  trimInPlace(url);
  if (!startsWithHttps(url)) return false;
  size_t length = std::strlen(url);
  while (length > 8 && url[length - 1] == '/') url[--length] = '\0';
  if (length <= 8) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(url[i]);
    if (std::isspace(c) || c < 0x20 || c > 0x7e || c == '@') return false;
  }
  return true;
}

bool validDeviceId(const char* id) {
  const size_t length = std::strlen(id);
  if (length == 0 || length > 64) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(id[i]);
    const bool allowed = std::isalnum(c) || c == '-' || c == '_';
    if (!allowed) return false;
  }
  return true;
}

// Accept a pasted PEM with real newlines OR literal "\n" escapes (portal text
// inputs cannot reliably carry actual newlines).
bool normalizePem(char* dst, size_t capacity, const char* src) {
  if (dst == nullptr || src == nullptr || capacity == 0) return false;
  size_t out = 0;
  for (size_t i = 0; src[i] != '\0'; ++i) {
    char c = src[i];
    if (c == '\\' && (src[i + 1] == 'n' || src[i + 1] == 'N')) {
      c = '\n';
      ++i;
    } else if (c == '\\' && src[i + 1] == 'r') {
      ++i; // ignore CR in CRLF escapes
      continue;
    }
    if (out + 1 >= capacity) return false;
    dst[out++] = c;
  }
  dst[out] = '\0';
  return std::strstr(dst, "BEGIN CERTIFICATE") != nullptr &&
         std::strstr(dst, "END CERTIFICATE") != nullptr;
}

uint32_t parsePollSeconds(const char* text) {
  char* end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 10);
  if (end == text || value == 0) return 300;
  if (value < 60) return 60;
  if (value > 86400) return 86400;
  return static_cast<uint32_t>(value);
}

int32_t parseTzOffset(const char* text) {
  char* end = nullptr;
  const long minutes = std::strtol(text, &end, 10);
  if (end == text) return 0;
  long clamped = minutes;
  if (clamped < -720) clamped = -720;
  if (clamped > 840) clamped = 840;
  return static_cast<int32_t>(clamped * 60L);
}

// Portal text inputs cannot carry real newlines reliably; show stored PEM as
// literal "\n" escapes when reopening the portal.
bool escapePemForPortal(const char* src, char* dst, size_t capacity) {
  if (src == nullptr || dst == nullptr || capacity == 0) return false;
  size_t out = 0;
  for (size_t i = 0; src[i] != '\0'; ++i) {
    const char c = src[i];
    const size_t needed = (c == '\n') ? 2 : 1;
    if (out + needed + 1 > capacity) return false;
    if (c == '\n') {
      dst[out++] = '\\';
      dst[out++] = 'n';
    } else if (c != '\r') {
      dst[out++] = c;
    }
  }
  dst[out] = '\0';
  return true;
}

bool validateConfig(Config& cfg) {
  trimInPlace(cfg.deviceId);
  trimInPlace(cfg.token);
  if (!validDeviceId(cfg.deviceId)) return false;
  if (cfg.token[0] == '\0' || std::strlen(cfg.token) > sizeof(cfg.token) - 1) return false;
  if (!normalizeApiBase(cfg.apiBase)) return false;
  if (std::strstr(cfg.caPem, "BEGIN CERTIFICATE") == nullptr ||
      std::strstr(cfg.caPem, "END CERTIFICATE") == nullptr) return false;
  return true;
}

bool loadConfig(Config& cfg) {
  std::memset(&cfg, 0, sizeof(cfg));
  if (preferences.getString("deviceId", cfg.deviceId, sizeof(cfg.deviceId)) == 0) return false;
  preferences.getString("apiBase", cfg.apiBase, sizeof(cfg.apiBase));
  preferences.getString("token", cfg.token, sizeof(cfg.token));
  preferences.getString("caPem", cfg.caPem, sizeof(cfg.caPem));
  cfg.pollSeconds = preferences.getULong("pollSeconds", 300);
  cfg.tzOffsetSeconds = preferences.getLong("tzOffsetSeconds", 0);
  return validateConfig(cfg);
}

void saveConfig(const Config& cfg) {
  preferences.putString("deviceId", cfg.deviceId);
  preferences.putString("apiBase", cfg.apiBase);
  preferences.putString("token", cfg.token);
  preferences.putString("caPem", cfg.caPem);
  preferences.putULong("pollSeconds", cfg.pollSeconds);
  preferences.putLong("tzOffsetSeconds", cfg.tzOffsetSeconds);
}

// ---------------------------------------------------------------------------
// OLED primitives
// ---------------------------------------------------------------------------

void showMessage(const char* line1, const char* line2 = nullptr,
                 const char* line3 = nullptr) {
  if (!displayReady) return;
  char l1[kDetailLineChars + 1];
  char l2[kDetailLineChars + 1];
  char l3[kDetailLineChars + 1];
  UiModel::copyTruncated(l1, sizeof(l1), line1);
  UiModel::copyTruncated(l2, sizeof(l2), line2);
  UiModel::copyTruncated(l3, sizeof(l3), line3);

  display.clearDisplay();
  display.fillRect(0, 0, kScreenWidth, 11, WHITE);
  display.setTextSize(1);
  display.setTextColor(BLACK);
  display.setCursor(2, 2);
  display.print("ENERGIA");
  display.setTextColor(WHITE);
  display.setCursor(0, 16);
  display.print(l1);
  display.setCursor(0, 28);
  display.print(l2);
  display.setCursor(0, 42);
  display.print(l3);
  display.display();
}

void showToast(const char* line1, const char* line2, uint32_t durationMs) {
  UiModel::copyTruncated(toastLine1, sizeof(toastLine1), line1);
  UiModel::copyTruncated(toastLine2, sizeof(toastLine2), line2);
  toastUntilMs = millis() + durationMs;
  hasDrawn = false;
}

uint32_t hashToast(const char* a, const char* b, bool displayOn) {
  uint32_t hash = 2166136261u;
  auto mix = [&hash](unsigned char byte) {
    hash ^= static_cast<uint32_t>(byte);
    hash *= 16777619u;
  };
  for (const char* p = a; *p != '\0'; ++p) mix(static_cast<unsigned char>(*p));
  mix(0);
  for (const char* p = b; *p != '\0'; ++p) mix(static_cast<unsigned char>(*p));
  mix(displayOn ? 1 : 0);
  return hash;
}

void drawHeader(const char* title, const char* right) {
  display.fillRect(0, 0, kScreenWidth, 11, WHITE);
  display.setTextSize(1);
  display.setTextColor(BLACK);
  display.setCursor(2, 2);
  display.print(title);
  if (right != nullptr && right[0] != '\0') {
    char bounded[10];
    UiModel::copyTruncated(bounded, sizeof(bounded), right);
    const int x = kScreenWidth - static_cast<int>(std::strlen(bounded)) * 6 - 2;
    display.setCursor(x > 0 ? x : 0, 2);
    display.print(bounded);
  }
  display.setTextColor(WHITE); // teal-equivalent outline/status glyphs are ASCII
}

void drawListPage() {
  char right[10];
  std::snprintf(right, sizeof(right), "%u", static_cast<unsigned>(ui.count()));
  drawHeader(ui.stale() ? "DEPTS!" : "DEPTS", right);

  if (ui.count() == 0) {
    display.setTextColor(WHITE);
    display.setCursor(0, 24);
    display.print("NO DATA");
    display.setCursor(0, 38);
    display.print("WAIT SYNC");
    return;
  }

  // EN-ESP-002: selected row uses an inverted bar (lime accent equivalent)
  // and a '>' marker; the window scrolls with the selection.
  for (uint8_t row = 0; row < UiModel::kVisibleRows; ++row) {
    const size_t index = ui.windowStart() + row;
    const int y = 14 + static_cast<int>(row) * 12;
    if (index >= ui.count()) break;

    char label[kListRowChars];
    UiModel::formatListRow(*ui.item(index), label, sizeof(label));
    const bool selected = index == ui.selectedIndex();
    if (selected) {
      display.fillRect(0, y, kScreenWidth, 11, WHITE);
      display.setTextColor(BLACK);
      display.setCursor(5, y + 2);
      display.print("> ");
    } else {
      display.setTextColor(WHITE);
      display.setCursor(17, y + 2); // align with the highlighted row's label
    }
    display.print(label);
  }
}

void drawDetailPage() {
  const DepartmentSummary* item = ui.selected();
  drawHeader("DETAIL", ui.stale() ? "OFFLINE" : "ONLINE");
  if (item == nullptr) {
    display.setTextColor(WHITE);
    display.setCursor(0, 28);
    display.print("NO DATA");
    return;
  }

  char line[kDetailLineChars + 1];

  // EN-ESP-003: name, server score, status label, last update; no arithmetic.
  UiModel::formatListRow(*item, line, sizeof(line));
  display.setCursor(0, 14);
  display.print(line);

  UiModel::formatScoreLine(*item, line, sizeof(line));
  display.setCursor(0, 25);
  display.print(line);

  UiModel::formatStatusLine(*item, ui.stale(), line, sizeof(line));
  display.setCursor(0, 36);
  display.print(line);

  UiModel::formatUpdatedLine(*item, nowEpoch(), config.tzOffsetSeconds,
                             ui.stale(), line, sizeof(line));
  display.setCursor(0, 47);
  display.print(line);

  // EN-ESP-004: approved alias + score, or a privacy-neutral message.
  UiModel::formatTopLine(*item, ui.recognitionEnabled(), line, sizeof(line));
  display.setCursor(0, 57);
  display.print(line);
}

void applyDisplayPower() {
  if (!displayReady) return;
  displayPowerOn = ui.displayOn();
  display.ssd1306_command(displayPowerOn ? SSD1306_DISPLAYON : SSD1306_DISPLAYOFF);
  if (displayPowerOn) hasDrawn = false; // force a full redraw after waking the panel
}

void renderDisplay(uint32_t nowMs, uint32_t epoch) {
  if (!displayReady) return;
  (void)nowMs;

  const bool toastActive = toastUntilMs != 0 && millis() < toastUntilMs;
  const uint32_t signature = toastActive
      ? hashToast(toastLine1, toastLine2, displayPowerOn)
      : ui.signature(epoch, config.tzOffsetSeconds);

  if (displayPowerOn && hasDrawn && signature == lastDrawnSignature) return;
  lastDrawnSignature = signature;
  hasDrawn = true;
  if (!displayPowerOn) return;

  display.clearDisplay();
  if (toastActive) {
    display.fillRect(0, 0, kScreenWidth, 11, WHITE);
    display.setTextSize(1);
    display.setTextColor(BLACK);
    display.setCursor(2, 2);
    display.print("ENERGIA");
    display.setTextColor(WHITE);
    display.setCursor(0, 24);
    display.print(toastLine1);
    display.setCursor(0, 38);
    display.print(toastLine2);
  } else if (ui.page() == UiPage::List) {
    drawListPage();
  } else {
    drawDetailPage();
  }
  display.display();
}

// ---------------------------------------------------------------------------
// EN-ESP-001: provisioning and configuration
// ---------------------------------------------------------------------------

void copyParam(char* dst, size_t capacity, const char* value) {
  UiModel::copyTruncated(dst, capacity, value);
  trimInPlace(dst);
}

bool factoryResetRequested() {
  const uint32_t start = millis();
  bool navLong = false;
  bool powerLong = false;
  navButton.reset(false, start);
  powerButton.reset(false, start);

  while (millis() - start < 2500) {
    const uint32_t now = millis();
    if (navButton.update(digitalRead(kNavButtonPin) == LOW, now) == PressEvent::Long)
      navLong = true;
    if (powerButton.update(digitalRead(kPowerButtonPin) == LOW, now) == PressEvent::Long)
      powerLong = true;
    if (navLong && powerLong && navButton.isPressed() && powerButton.isPressed())
      return true;
    delay(10);
  }
  navButton.reset(false, millis());
  powerButton.reset(false, millis());
  return false;
}

void runProvisioning(Config& cfg, bool haveValidConfig) {
  // Static buffers avoid a multi-kilobyte setup() stack footprint; the
  // WiFiManagerParameter objects keep pointers to these for their lifetime.
  static char deviceId[sizeof(cfg.deviceId)];
  static char apiBase[sizeof(cfg.apiBase)];
  static char token[sizeof(cfg.token)];
  static char caPem[sizeof(cfg.caPem)];
  static char pollBuffer[12];
  static char tzBuffer[16];

  std::memset(deviceId, 0, sizeof(deviceId));
  std::memset(apiBase, 0, sizeof(apiBase));
  std::memset(token, 0, sizeof(token));
  std::memset(caPem, 0, sizeof(caPem));
  UiModel::copyTruncated(deviceId, sizeof(deviceId), cfg.deviceId);
  UiModel::copyTruncated(apiBase, sizeof(apiBase), cfg.apiBase);
  UiModel::copyTruncated(token, sizeof(token), cfg.token);
  if (!escapePemForPortal(cfg.caPem, caPem, sizeof(caPem)))
    UiModel::copyTruncated(caPem, sizeof(caPem), cfg.caPem);
  std::snprintf(pollBuffer, sizeof(pollBuffer), "%lu",
                static_cast<unsigned long>(cfg.pollSeconds));
  std::snprintf(tzBuffer, sizeof(tzBuffer), "%ld",
                static_cast<long>(cfg.tzOffsetSeconds / 60));

  WiFiManager wm;
  wm.setConfigPortalTimeout(kWifiConnectTimeoutS);

  WiFiManagerParameter pDevice("deviceId", "Device ID", deviceId,
                               static_cast<int>(sizeof(deviceId)));
  WiFiManagerParameter pApi("apiBase", "HTTPS API base URL", apiBase,
                            static_cast<int>(sizeof(apiBase)));
  WiFiManagerParameter pToken("token", "Device token", token,
                              static_cast<int>(sizeof(token)));
  WiFiManagerParameter pPoll("poll", "Poll seconds (60..86400)", pollBuffer,
                             static_cast<int>(sizeof(pollBuffer)));
  WiFiManagerParameter pTz("tz", "UTC offset minutes", tzBuffer,
                           static_cast<int>(sizeof(tzBuffer)));
  WiFiManagerParameter pCa("caPem", "Root CA PEM (use \\n escapes)",
                           caPem, static_cast<int>(sizeof(caPem)));
  wm.addParameter(&pDevice);
  wm.addParameter(&pApi);
  wm.addParameter(&pToken);
  wm.addParameter(&pPoll);
  wm.addParameter(&pTz);
  wm.addParameter(&pCa);

  showMessage("WIFI SETUP", "AP energia-esp32", "192.168.4.1");

  // With no valid config, force the captive portal even if WiFi credentials
  // happen to be stored. No extra physical button is needed (EN-ESP-001).
  const bool connected = haveValidConfig
      ? wm.autoConnect("energia-esp32")
      : wm.startConfigPortal("energia-esp32");

  if (!connected) {
    Serial.println("[provision] portal timeout / WiFi connect failed");
    showMessage("SETUP FAILED", "RESTART TO RETRY");
    delay(3000);
    ESP.restart();
  }

  copyParam(cfg.deviceId, sizeof(cfg.deviceId), pDevice.getValue());
  copyParam(cfg.apiBase, sizeof(cfg.apiBase), pApi.getValue());
  copyParam(cfg.token, sizeof(cfg.token), pToken.getValue());
  std::memset(cfg.caPem, 0, sizeof(cfg.caPem));
  if (!normalizePem(cfg.caPem, sizeof(cfg.caPem), pCa.getValue())) {
    showMessage("BAD ROOT CA", "REPROVISION");
    delay(3000);
    ESP.restart();
  }
  cfg.pollSeconds = parsePollSeconds(pPoll.getValue());
  cfg.tzOffsetSeconds = parseTzOffset(pTz.getValue());

  if (!validateConfig(cfg)) {
    showMessage("BAD CONFIG", "CHECK PORTAL");
    delay(3000);
    ESP.restart();
  }

  saveConfig(cfg);
  Serial.printf("[provision] ready: id=%s base=%s poll=%lus\n",
                cfg.deviceId, cfg.apiBase,
                static_cast<unsigned long>(cfg.pollSeconds));
  showMessage("CONFIG OK", cfg.deviceId, "CONNECTING");
}

// ---------------------------------------------------------------------------
// EN-ESP-001: compact read-only payload parsing + NVS cache
// ---------------------------------------------------------------------------

const char* jsonText(JsonObject object, const char* key) {
  if (!object.containsKey(key) || object[key].isNull()) return "";
  return object[key] | "";
}

const char* scoreStateText(ScoreState state) {
  switch (state) {
    case ScoreState::Final: return "final";
    case ScoreState::Provisional: return "provisional";
    case ScoreState::Unavailable: return "unavailable";
    default: return "unavailable";
  }
}

void cacheSanitizedPayload(const DepartmentSummary* items, size_t count,
                           bool recognitionEnabled, uint32_t generatedAt) {
  // Only allow-listed fields are written to NVS; no user IDs or evidence.
  JsonDocument out;
  out["schema"] = "energia.department-summaries.v1";
  out["generatedAt"] = generatedAt;
  out["recognitionEnabled"] = recognitionEnabled;
  JsonArray departments = out.createNestedArray("departments");

  for (size_t i = 0; i < count; ++i) {
    const DepartmentSummary& item = items[i];
    JsonObject department = departments.createNestedObject();
    department["name"] = item.name;
    department["statusLabel"] = item.statusLabel;
    department["scoreState"] = scoreStateText(item.scoreState);
    if (item.hasScore) {
      department["score"] = item.score;
    } else {
      department["score"].clear(); // serialize as null
    }
    department["lastUpdate"] = item.lastUpdateEpoch;
    if (item.top.eligible) {
      JsonObject top = department.createNestedObject("topEmployee");
      top["displayName"] = item.top.displayName;
      top["score"] = item.top.score;
      top["eligible"] = true;
    } else {
      department["topEmployee"].clear(); // serialize as null
    }
  }

  static char buffer[kCacheCapacity + 1];
  const size_t length = serializeJson(out, buffer, sizeof(buffer));
  if (length > 0 && length < sizeof(buffer)) {
    preferences.putString("lastJson", buffer);
    preferences.putULong("cacheAt", generatedAt);
  } else {
    Serial.println("[cache] sanitized JSON too large; cache skipped");
  }
}

bool parsePayload(const char* json, bool writeCache) {
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, json);
  if (error) {
    Serial.printf("[json] parse error: %s\n", error.c_str());
    return false;
  }

  // Strict schema check: device only accepts the agreed compact contract.
  if (std::strcmp(doc["schema"] | "", "energia.department-summaries.v1") != 0) {
    Serial.println("[json] unsupported schema");
    return false;
  }
  JsonArray departments = doc["departments"];
  if (departments.isNull()) {
    Serial.println("[json] departments missing");
    return false;
  }

  const uint32_t generatedAt = doc["generatedAt"] | 0UL;
  const bool recognitionEnabled = doc["recognitionEnabled"] | false;

  DepartmentSummary items[kMaxDepartments];
  size_t count = 0;
  for (JsonVariant value : departments) {
    if (count >= kMaxDepartments) break; // bounded memory, extra rows ignored
    if (!value.is<JsonObject>()) continue;
    JsonObject department = value.as<JsonObject>();

    DepartmentSummary item;
    UiModel::copyTruncated(item.name, sizeof(item.name), jsonText(department, "name"));
    UiModel::copyTruncated(item.statusLabel, sizeof(item.statusLabel),
                           jsonText(department, "statusLabel"));

    const char* state = jsonText(department, "scoreState");
    if (std::strcmp(state, "provisional") == 0) {
      item.scoreState = ScoreState::Provisional;
    } else if (std::strcmp(state, "unavailable") == 0) {
      item.scoreState = ScoreState::Unavailable;
    } else if (std::strcmp(state, "final") == 0) {
      item.scoreState = ScoreState::Final;
    } else {
      // Unknown state is treated as unavailable rather than guessing.
      item.scoreState = ScoreState::Unavailable;
    }

    // EN-ESP-003: copy the server score verbatim. Never compute or adjust it.
    if (department.containsKey("score") && !department["score"].isNull()) {
      const int score = department["score"] | -1;
      if (score >= 0 && score <= 100) {
        item.hasScore = true;
        item.score = static_cast<uint8_t>(score);
      }
    }
    if (item.scoreState == ScoreState::Unavailable) {
      item.hasScore = false;
      item.score = 0;
    }

    item.lastUpdateEpoch = generatedAt;
    if (department.containsKey("lastUpdate") && !department["lastUpdate"].isNull()) {
      const unsigned long valueEpoch = department["lastUpdate"] | 0UL;
      if (valueEpoch != 0) item.lastUpdateEpoch = static_cast<uint32_t>(valueEpoch);
    }

    // EN-ESP-004: read only the approved display alias and its score.
    if (department.containsKey("topEmployee") &&
        department["topEmployee"].is<JsonObject>()) {
      JsonObject top = department["topEmployee"].as<JsonObject>();
      const bool eligible = top["eligible"] | false;
      const int topScore = top["score"] | -1;
      if (eligible && topScore >= 0 && topScore <= 100) {
        item.top.eligible = true;
        item.top.score = static_cast<uint8_t>(topScore);
        UiModel::copyTruncated(item.top.displayName, sizeof(item.top.displayName),
                               jsonText(top, "displayName"));
        if (item.top.displayName[0] == '\0') item.top.eligible = false;
      }
    }

    items[count++] = item;
  }

  if (!ui.setDepartments(items, count)) return false;
  ui.setRecognitionEnabled(recognitionEnabled);
  ui.setGeneratedAt(generatedAt);

  if (writeCache) cacheSanitizedPayload(items, count, recognitionEnabled, generatedAt);
  return true;
}

void loadCachedSummary() {
  static char buffer[kCacheCapacity + 1];
  const size_t length = preferences.getString("lastJson", buffer, sizeof(buffer));
  if (length == 0) return;
  buffer[length] = '\0';
  if (parsePayload(buffer, false)) {
    // A rebooted cache is never presented as current (EN-ESP-005).
    ui.setStale(true);
    networkStale = true;
    Serial.println("[cache] loaded last valid JSON (marked stale)");
  } else {
    preferences.remove("lastJson");
  }
}

void syncClock(int32_t tzOffsetSeconds) {
  configTime(tzOffsetSeconds, 0, "pool.ntp.org", "time.google.com");
  const uint32_t deadline = millis() + 8000;
  while (nowEpoch() < 1700000000UL && millis() < deadline) delay(100);
  if (nowEpoch() == 0) Serial.println("[time] NTP unavailable; using ages only");
}

// ---------------------------------------------------------------------------
// EN-ESP-001 / EN-ESP-005: bounded HTTPS polling
// ---------------------------------------------------------------------------

struct FetchResult {
  bool ok;
  int httpCode;
  bool authRejected;
};

FetchResult fetchOnce() {
  FetchResult result{false, 0, false};
  if (WiFi.status() != WL_CONNECTED) return result;

  char url[256];
  const int written = std::snprintf(
      url, sizeof(url), "%s/api/v1/devices/%s/department-summaries",
      config.apiBase, config.deviceId);
  if (written < 0 || written >= static_cast<int>(sizeof(url))) return result;

  char authorization[sizeof(config.token) + 16];
  std::snprintf(authorization, sizeof(authorization), "Bearer %s", config.token);

  WiFiClientSecure client;
  // HTTPS with real certificate validation; no setInsecure() fallback.
  // The root CA PEM is provisioned by the operator, never compiled into firmware.
  client.setCACert(config.caPem);

  HTTPClient http;
  http.begin(client, url);
  http.setUserAgent("energia-esp32/1.0");
  http.addHeader("Accept", "application/json");
  http.addHeader("Authorization", authorization);
  http.setConnectTimeout(4000);
  http.setTimeout(5000);
  http.setReuse(false);

  const int code = http.GET();
  feedWatchdog();
  result.httpCode = code;

  if (code == 200) {
    const String body = http.getString();
    http.end();
    result.ok = parsePayload(body.c_str(), true);
  } else {
    http.end();
    result.authRejected = (code == 401 || code == 403);
    Serial.printf("[http] status %d\n", code);
  }
  return result;
}

void refreshStaleFlag(uint32_t nowMs, uint32_t epoch) {
  bool stale = !everFetched || networkStale;
  if (everFetched) {
    const uint32_t thresholdSeconds =
        config.pollSeconds > 300 ? config.pollSeconds * 3U : 900U;
    if (epoch > 0 && lastSuccessEpoch > 0 && epoch >= lastSuccessEpoch) {
      if (epoch - lastSuccessEpoch > thresholdSeconds) stale = true;
    } else if (lastSuccessMs > 0 && nowMs >= lastSuccessMs &&
               nowMs - lastSuccessMs > thresholdSeconds * 1000UL) {
      stale = true;
    }
  }
  ui.setStale(stale);
}

void pollIfDue(uint32_t nowMs, uint32_t epoch) {
  if (tokenRevoked || pollState.authBlocked) return; // 401/403: wait for re-provision
  if (nowMs < pollState.nextAttemptMs) return;

  if (WiFi.status() != WL_CONNECTED) {
    networkStale = true;
    WiFi.reconnect();
    pollState.attempts = 0;
    pollState.nextAttemptMs = nowMs + 10000;
    return;
  }

  const FetchResult result = fetchOnce();
  if (result.ok) {
    networkStale = false;
    everFetched = true;
    lastSuccessEpoch = epoch > 0 ? epoch : 0;
    lastSuccessMs = nowMs;
    pollState.attempts = 0;
    pollState.nextAttemptMs = nowMs + config.pollSeconds * 1000UL;
    Serial.println("[poll] summaries updated");
    return;
  }

  if (result.authRejected) {
    // EN-ESP-001: honor revocation immediately; no further calls with that token.
    tokenRevoked = true;
    pollState.authBlocked = true;
    networkStale = true;
    showToast("TOKEN REVOKED", "REPROVISION", 60000);
    return;
  }

  if (pollState.attempts < kMaxFetchAttempts - 1) {
    const uint32_t delayMs = kRetryDelaysMs[pollState.attempts];
    ++pollState.attempts;
    pollState.nextAttemptMs = nowMs + delayMs;
  } else {
    // Bounded retries: after the last attempt, wait a full poll interval.
    pollState.attempts = 0;
    networkStale = true;
    pollState.nextAttemptMs = nowMs + config.pollSeconds * 1000UL;
  }
}

// ---------------------------------------------------------------------------
// EN-ESP-005: deep sleep on the same power GPIO
// ---------------------------------------------------------------------------

void enterDeepSleep() {
  showMessage("SLEEP", "WAKE: POWER BTN");
  if (displayReady) display.ssd1306_command(SSD1306_DISPLAYOFF);
  if (watchdogActive) (void)esp_task_wdt_delete(NULL);
  watchdogActive = false;

  // Wake level LOW matches the active-low button. We only reach here after the
  // release has debounced, so the pin is HIGH when sleep starts (no instant wake).
  (void)esp_sleep_enable_ext0_wakeup(kPowerButtonPin, 0);
  delay(50);
  esp_deep_sleep_start();
}

PressKind kindFrom(PressEvent event) {
  return event == PressEvent::Long ? PressKind::Long : PressKind::Short;
}

void handleUiEvent(UiEvent event) {
  if (event == UiEvent::DisplayToggled) applyDisplayPower();
  if (event != UiEvent::None) hasDrawn = false;
}

} // namespace

// ---------------------------------------------------------------------------
// Arduino entry points
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(kNavButtonPin, INPUT_PULLUP);
  pinMode(kPowerButtonPin, INPUT_PULLUP);
  Wire.begin(kSdaPin, kSclPin);

  if (!display.begin(SSD1306_SWITCHCAPVCC, kOledAddress))
    Serial.println("[oled] SSD1306 init failed; continuing headless");
  display.clearDisplay();
  display.display();
  displayReady = true;

  preferences.begin("energia", false);
  showMessage("ENERGIA", "BOOTING");

  // Optional recovery path using only the two existing buttons: hold both for
  // ~2s during boot to clear Preferences/WiFi settings and force provisioning.
  if (factoryResetRequested()) {
    preferences.clear();
    WiFiManager wm;
    wm.resetSettings();
    showMessage("FACTORY RESET", "RESTARTING");
    delay(1000);
    ESP.restart();
  }

  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    showMessage("WAKE", "POWER BUTTON");
  }

  Config cfg;
  const bool haveValidConfig = loadConfig(cfg);
  runProvisioning(cfg, haveValidConfig);

  syncClock(cfg.tzOffsetSeconds);
  loadCachedSummary();
  ui.setDisplayOn(true); // a wake/power cycle always presents a visible display
  displayPowerOn = true;
  navButton.reset(false, millis());
  powerButton.reset(false, millis());

  // Watchdog starts only after blocking setup/portal work (EN-ESP-005).
  (void)esp_task_wdt_init(kWatchdogTimeoutS, true);
  (void)esp_task_wdt_add(NULL);
  watchdogActive = true;

  pollState.nextAttemptMs = millis();
  showMessage("SYNCING", cfg.deviceId);
  hasDrawn = false;
}

void loop() {
  feedWatchdog();

  const uint32_t nowMs = millis();
  const uint32_t epoch = nowEpoch();

  const PressEvent navEvent = navButton.update(digitalRead(kNavButtonPin) == LOW, nowMs);
  const PressEvent powerEvent = powerButton.update(digitalRead(kPowerButtonPin) == LOW, nowMs);

  if (navEvent != PressEvent::None)
    handleUiEvent(ui.handleButton(Button::Nav, kindFrom(navEvent)));
  if (powerEvent == PressEvent::Short)
    handleUiEvent(ui.handleButton(Button::Power, PressKind::Short));
  if (powerEvent == PressEvent::Long) sleepPending = true;

  // Wait for physical release before arming ext0 wake (see enterDeepSleep).
  if (sleepPending && !powerButton.isPressed()) enterDeepSleep();

  pollIfDue(nowMs, epoch);
  refreshStaleFlag(nowMs, epoch);
  renderDisplay(nowMs, epoch);
}
