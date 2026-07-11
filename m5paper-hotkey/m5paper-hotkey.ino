// Garage Monitor hotkey pad - M5Stack PaperS3
//
// Renders a configurable grid of buttons (orientation, columns, rows, up
// to 12 slots) whose content is entirely data-driven from Home Assistant:
// input_text.garage_monitor_layout_config (orientation/cols/rows) and
// input_text.garage_monitor_slot_0..11 (one per grid cell), all edited via
// the "Garage Monitor Hotkey Layout" card on the HA dashboard.
//
// E-ink refresh is full-quality only (epd_quality) on every redraw - no
// partial refresh - to avoid ghosting, per explicit preference. Icons are
// drawn from a curated set of pre-rasterized MDI bitmaps (icons.h) with a
// text-only fallback for anything not in that set - MDI has ~7000 icons,
// bundling/rendering all of them live isn't reasonable on this hardware.
//
// Power: uses light sleep (not deep sleep) between actions. The GT911
// touch controller's INT line is GPIO48, which is outside the ESP32-S3's
// RTC-GPIO domain (GPIO0-21) and so cannot be used as a deep-sleep wake
// source - this is a hardware limitation of how the pin is wired, not a
// software choice. Light sleep supports GPIO wake on any pin and preserves
// RAM/peripheral state (no full re-init needed on wake), at the cost of
// less dramatic power savings than deep sleep would have given.
//
// OTA: HTTP-pull self-update (HTTPUpdate) checked once per timer wake.
// Compares FIRMWARE_VERSION against a small version.txt hosted on the HA
// Pi's www folder; downloads and flashes the .bin on a mismatch. Not
// push-based ArduinoOTA, since a device that's mostly asleep can't be
// continuously listening for a push.

#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include "icons.h"
#include "secrets.h" // WIFI_SSID, WIFI_PASSWORD, HA_TOKEN - copy secrets.h.example, gitignored

// ---- Config ----
const char* HA_HOST = "192.168.7.1";
const uint16_t HA_PORT = 8123;
const char* MEDIA_PLAYER = "media_player.garage_garage_monitor_ls49cg954snxza";
// The samsungtv integration never reports a current-source attribute on the
// media_player entity (confirmed 2026-07-11 - only a static source_list),
// so the actual displayed input can't be read back from HA directly. This
// input_select is self-tracked instead: each input-changing HA script sets
// it as its last step, so it reflects anything done via HA/this pad, but
// not changes made through other means (e.g. the physical remote).
const char* SOURCE_ENTITY = "input_select.garage_monitor_source";
const char* LAYOUT_ENTITY = "input_text.garage_monitor_layout_config";

const int MAX_SLOTS = 12;
const uint64_t TIMER_WAKE_INTERVAL_US = 180ULL * 1000000ULL; // background sync every 3 min
#define TOUCH_INT_PIN GPIO_NUM_48 // GT911 INT, active-low, NOT RTC-capable (deep sleep wake unusable)

const char* FIRMWARE_VERSION = "5";
const char* OTA_VERSION_URL = "http://192.168.7.1:8123/local/m5paper-hotkey/version.txt";
const char* OTA_BIN_URL = "http://192.168.7.1:8123/local/m5paper-hotkey/firmware.bin";

uint32_t COL_BLACK;
uint32_t COL_WHITE;

struct Slot {
  String label;
  String icon;      // e.g. "mdi:video-input-hdmi" - looked up in ICON_TABLE
  String domain;    // "" => refresh-only, no HA service call
  String service;
  String entity;
  String matchType; // "source" | "state" | "none"
  String matchValue;
  int16_t x, y, w, h;
};

Slot g_slots[MAX_SLOTS];
String g_slotRaw[MAX_SLOTS];
int g_activeCount = 6;
String g_orientation = "landscape";
int g_cols = 3;
int g_rows = 2;
int g_fontSize = 2; // M5GFX setTextSize() multiplier for button labels, 1-4
String g_layoutRaw = "";

String g_state = "";
String g_source = "";

void applyOrientation() {
  // Empirically: rotation(0) is native/portrait on this panel, rotation(1)
  // is landscape (960x540) - confirmed by the working landscape layout
  // used throughout earlier development.
  M5.Display.setRotation(g_orientation == "portrait" ? 0 : 1);
}

const int HEADER_H = 60; // fixed/compact, was height*0.16 (86px landscape, 154px portrait)
int16_t g_refreshBtnX, g_refreshBtnY, g_refreshBtnW, g_refreshBtnH;

void layoutSlots() {
  applyOrientation();
  int W = M5.Display.width();
  int H = M5.Display.height();
  int margin = 14;
  int cols = max(1, g_cols);
  int rows = max(1, g_rows);
  int gridTop = HEADER_H + margin;
  int gridH = H - gridTop - margin;
  int cellW = (W - margin * (cols + 1)) / cols;
  int cellH = (gridH - margin * (rows - 1)) / rows;
  for (int i = 0; i < g_activeCount; i++) {
    int col = i % cols;
    int row = i / cols;
    g_slots[i].x = margin + col * (cellW + margin);
    g_slots[i].y = gridTop + row * (cellH + margin);
    g_slots[i].w = cellW;
    g_slots[i].h = cellH;
  }

  g_refreshBtnW = 48;
  g_refreshBtnH = 48;
  g_refreshBtnX = W - g_refreshBtnW - 8;
  g_refreshBtnY = 6;
}

void ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  // WiFi.reconnect() only works from "still in STA mode but disconnected".
  // drawUI() calls WiFi.mode(WIFI_OFF) to quiet the radio during the
  // e-paper refresh and never restores it, so by the time the next action
  // needs the network, the radio may be fully powered off - reconnect()
  // silently does nothing from that state. Explicitly restore STA mode and
  // re-supply credentials so this works regardless of prior radio state.
  if (WiFi.getMode() != WIFI_STA) {
    WiFi.mode(WIFI_STA);
  }
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(200);
  }
}

bool haGetEntityState(const String &entityId, String &stateOut) {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  String url = String("http://") + HA_HOST + ":" + HA_PORT + "/api/states/" + entityId;
  http.begin(url);
  http.addHeader("Authorization", String("Bearer ") + HA_TOKEN);
  http.setTimeout(5000);
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    String payload = http.getString();
    // Parse the outer envelope with a real JSON parser rather than naive
    // string-scraping - slot/layout entities hold a JSON string AS their
    // state value, so the outer payload contains escaped quotes that a
    // hand-rolled quote-scanner can't handle correctly.
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err && doc["state"].is<const char*>()) {
      stateOut = String((const char*)doc["state"]);
      ok = true;
    }
  }
  http.end();
  return ok;
}

bool haGetState(String &stateOut, String &sourceOut) {
  bool ok1 = haGetEntityState(MEDIA_PLAYER, stateOut);
  String src;
  bool ok2 = haGetEntityState(SOURCE_ENTITY, src);
  if (ok2) sourceOut = src;
  return ok1;
}

void parseSlotJson(int i, const String &raw) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, raw);
  if (err) return; // keep previous content on parse failure
  g_slots[i].label = String((const char*)(doc["label"] | ""));
  g_slots[i].icon = String((const char*)(doc["icon"] | ""));
  g_slots[i].domain = String((const char*)(doc["domain"] | ""));
  g_slots[i].service = String((const char*)(doc["service"] | ""));
  g_slots[i].entity = String((const char*)(doc["entity"] | ""));
  g_slots[i].matchType = String((const char*)(doc["matchType"] | "none"));
  g_slots[i].matchValue = String((const char*)(doc["match"] | ""));
}

// Fetches layout config + all active slots. Returns true if anything
// actually changed (content or grid shape), and recomputes geometry when
// it does.
bool refreshLayoutAndSlots() {
  bool changed = false;

  String rawLayout;
  if (haGetEntityState(LAYOUT_ENTITY, rawLayout) && rawLayout != g_layoutRaw) {
    g_layoutRaw = rawLayout;
    JsonDocument doc;
    if (!deserializeJson(doc, rawLayout)) {
      g_orientation = String((const char*)(doc["orientation"] | "landscape"));
      g_cols = doc["cols"] | 3;
      g_rows = doc["rows"] | 2;
      g_fontSize = doc["fontSize"] | 2;
      if (g_fontSize < 1) g_fontSize = 1;
      if (g_fontSize > 4) g_fontSize = 4;
    }
    changed = true;
  }

  int wantCount = g_cols * g_rows;
  if (wantCount < 1) wantCount = 1;
  if (wantCount > MAX_SLOTS) wantCount = MAX_SLOTS;
  if (wantCount != g_activeCount) {
    g_activeCount = wantCount;
    changed = true;
  }

  for (int i = 0; i < g_activeCount; i++) {
    String entity = "input_text.garage_monitor_slot_" + String(i);
    String raw;
    if (haGetEntityState(entity, raw) && raw != g_slotRaw[i]) {
      g_slotRaw[i] = raw;
      parseSlotJson(i, raw);
      changed = true;
    }
  }

  if (changed) layoutSlots();
  return changed;
}

bool haCallService(const String &domain, const String &service, const String &entityId) {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  String url = String("http://") + HA_HOST + ":" + HA_PORT + "/api/services/" + domain + "/" + service;
  http.begin(url);
  http.addHeader("Authorization", String("Bearer ") + HA_TOKEN);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);
  String body = "{\"entity_id\":\"" + entityId + "\"}";
  int code = http.POST(body);
  http.end();
  return code >= 200 && code < 300;
}

void checkOTA() {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(OTA_VERSION_URL);
  http.setTimeout(5000);
  int code = http.GET();
  String remoteVersion;
  if (code == 200) {
    remoteVersion = http.getString();
    remoteVersion.trim();
  }
  http.end();
  if (remoteVersion.length() && remoteVersion != String(FIRMWARE_VERSION)) {
    WiFiClient client;
    httpUpdate.rebootOnUpdate(true);
    httpUpdate.update(client, OTA_BIN_URL); // reboots automatically on success
  }
}

const uint8_t* findIcon(const String &name) {
  for (int i = 0; i < ICON_TABLE_SIZE; i++) {
    if (name.equalsIgnoreCase(ICON_TABLE[i].name)) return ICON_TABLE[i].data;
  }
  return nullptr;
}

bool slotIsActive(const Slot &s) {
  if (s.matchType == "source") return g_source.length() && g_source == s.matchValue;
  if (s.matchType == "state") return g_state.length() && g_state == s.matchValue;
  return false;
}

void drawSlot(const Slot &s, bool active) {
  auto &d = M5.Display;
  uint32_t fill = active ? COL_BLACK : COL_WHITE;
  uint32_t text = active ? COL_WHITE : COL_BLACK;
  d.fillRoundRect(s.x, s.y, s.w, s.h, 10, fill);
  d.drawRoundRect(s.x, s.y, s.w, s.h, 10, COL_BLACK);

  d.setTextSize(g_fontSize);
  String label = s.label.length() ? s.label : "(empty)";

  bool roomForIcon = (s.h >= ICON_SIZE + 30);
  const uint8_t* iconBmp = findIcon(s.icon);
  if (roomForIcon && iconBmp) {
    int iconX = s.x + (s.w - ICON_SIZE) / 2;
    int iconY = s.y + 6;
    d.drawBitmap(iconX, iconY, iconBmp, ICON_SIZE, ICON_SIZE, text, fill);
    d.setTextColor(text, fill);
    d.setTextDatum(top_center);
    d.drawString(label, s.x + s.w / 2, iconY + ICON_SIZE + 4);
  } else {
    d.setTextColor(text, fill);
    d.setTextDatum(middle_center);
    d.drawString(label, s.x + s.w / 2, s.y + s.h / 2);
  }
}

// The physical e-paper refresh triggered by display() is a multi-second,
// multi-pass, timing-sensitive software-driven waveform sequence (epdiy).
// Something preempting the CPU mid-refresh (WiFi radio activity is the
// leading suspect, matching a documented issue with this exact driver
// stack) can desync it, producing a partial/gradient-corrupted refresh:
// confirmed via photo, tiles drawn earlier in the scan render perfectly,
// later ones fade through a gray gradient with no borders/icons - i.e. a
// partially-applied waveform, not a logic bug - and it's intermittent
// rather than deterministic (a genuine logic bug would corrupt every time
// the same way; this doesn't).
//
// Mitigations: WiFi is fully torn down before display() (reconnected
// lazily by the next haGetEntityState/haCallService via ensureWiFi()), and
// the whole draw+refresh sequence runs twice - each pass starts with a
// clean fillScreen(WHITE), so a second pass overwrites any corruption left
// by a bad first pass. This doesn't fix the underlying cause, but turns an
// intermittent single-pass failure into something that would need to fail
// twice in a row to be visible.
void drawUIOnce() {
  auto &d = M5.Display;
  Serial.printf("[drawUI] start free_heap=%u activeCount=%d cols=%d rows=%d orientation=%s\n",
                ESP.getFreeHeap(), g_activeCount, g_cols, g_rows, g_orientation.c_str());
  d.setEpdMode(epd_mode_t::epd_quality);
  d.fillScreen(COL_WHITE);

  int W = d.width();
  d.drawFastHLine(0, HEADER_H - 1, W, COL_BLACK);
  d.drawFastHLine(0, HEADER_H - 2, W, COL_BLACK);

  d.setTextColor(COL_BLACK, COL_WHITE);
  d.setTextDatum(top_left);
  d.setTextSize(2);
  d.setCursor(16, 6);
  d.print("Garage Monitor");

  d.setCursor(16, 28);
  String stateStr = g_state.length() ? g_state : "unknown";
  String srcStr = g_source.length() ? g_source : "-";
  d.printf("Power: %s   Source: %s", stateStr.c_str(), srcStr.c_str());

  const uint8_t* refreshIcon = findIcon("mdi:refresh");
  if (refreshIcon) {
    d.drawBitmap(g_refreshBtnX, g_refreshBtnY, refreshIcon, ICON_SIZE, ICON_SIZE, COL_BLACK, COL_WHITE);
  }

  for (int i = 0; i < g_activeCount; i++) {
    drawSlot(g_slots[i], slotIsActive(g_slots[i]));
  }

  Serial.printf("[drawUI] before display() free_heap=%u\n", ESP.getFreeHeap());
  d.display();
  Serial.printf("[drawUI] done free_heap=%u\n", ESP.getFreeHeap());
}

void drawUI() {
  bool wifiWasOn = (WiFi.status() == WL_CONNECTED);
  if (wifiWasOn) {
    WiFi.mode(WIFI_OFF);
    delay(300);
  }
  // Drawing content twice back-to-back was tried and made things worse: if
  // display() returns before the panel's hardware refresh has *fully*
  // settled, starting a second draw can interrupt/corrupt the first one -
  // and since only the last pass is ever visible, that's a straight
  // downgrade, not "at least one good attempt". A separate clearDisplay()
  // (a real full black/white/black ghost-clearing cycle, more thorough
  // than a plain fillScreen) with generous settling delays around each
  // physical refresh targets the actual symptoms instead: gradient/partial
  // corruption (settling time) and the persistent line ghosting artifact
  // (clearDisplay).
  auto &d = M5.Display;
  d.setEpdMode(epd_mode_t::epd_quality);
  d.clearDisplay();
  delay(800);
  drawUIOnce();
  delay(800);
}

void showBootMessage(const char* msg) {
  auto &d = M5.Display;
  d.setEpdMode(epd_mode_t::epd_fast);
  d.fillScreen(COL_WHITE);
  d.setTextColor(COL_BLACK, COL_WHITE);
  d.setTextDatum(middle_center);
  d.setTextSize(2);
  d.drawString(msg, d.width() / 2, d.height() / 2);
  d.display();
}

void connectWiFi() {
  showBootMessage("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
  }
}

void enterLightSleepAndWait() {
  gpio_wakeup_enable(TOUCH_INT_PIN, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_timer_wakeup(TIMER_WAKE_INTERVAL_US);
  esp_light_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("[setup] booting");
  auto cfg = M5.config();
  M5.begin(cfg);

  COL_BLACK = M5.Display.color888(0, 0, 0);
  COL_WHITE = M5.Display.color888(255, 255, 255);

  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    haGetState(g_state, g_source);
    refreshLayoutAndSlots();
    checkOTA();
  } else {
    g_state = "wifi-fail";
    layoutSlots();
  }
  drawUI();
}

void loop() {
  M5.update();

  bool handledTouch = false;
  if (M5.Touch.getCount()) {
    auto t = M5.Touch.getDetail(0);
    if (t.wasReleased()) {
      if (t.x >= g_refreshBtnX && t.x <= g_refreshBtnX + g_refreshBtnW &&
          t.y >= g_refreshBtnY && t.y <= g_refreshBtnY + g_refreshBtnH) {
        Serial.println("[loop] touch hit header refresh button");
        haGetState(g_state, g_source);
        refreshLayoutAndSlots();
        drawUI();
        handledTouch = true;
      }
      for (int i = 0; !handledTouch && i < g_activeCount; i++) {
        Slot &s = g_slots[i];
        if (t.x >= s.x && t.x <= s.x + s.w && t.y >= s.y && t.y <= s.y + s.h) {
          Serial.printf("[loop] touch hit slot %d ('%s')\n", i, s.label.c_str());
          if (s.domain.length() && s.service.length() && s.entity.length()) {
            haCallService(s.domain, s.service, s.entity);
            delay(1500); // let HA/monitor settle before re-polling state
          }
          haGetState(g_state, g_source);
          Serial.printf("[loop] refreshLayoutAndSlots free_heap_before=%u\n", ESP.getFreeHeap());
          refreshLayoutAndSlots();
          Serial.printf("[loop] refreshLayoutAndSlots free_heap_after=%u\n", ESP.getFreeHeap());
          drawUI();
          handledTouch = true;
          break;
        }
      }
    }
  }

  if (!handledTouch && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
    String newState, newSource;
    bool stateChanged = false;
    if (haGetState(newState, newSource)) {
      stateChanged = (newState != g_state || newSource != g_source);
      g_state = newState;
      g_source = newSource;
    }
    bool layoutChanged = refreshLayoutAndSlots();
    if (stateChanged || layoutChanged) drawUI();
    checkOTA();
  }

  enterLightSleepAndWait();
}
