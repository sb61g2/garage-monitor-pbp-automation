// Garage Monitor hotkey pad - M5Stack PaperS3
//
// Renders a configurable grid of buttons (orientation, columns, rows, up
// to 12 slots) whose content is entirely data-driven from Home Assistant:
// input_text.garage_monitor_layout_config (orientation/cols/rows) and
// input_text.garage_monitor_slot_0..11 (one per grid cell), all edited via
// the "Garage Monitor Hotkey Layout" card on the HA dashboard.
//
// Commands only, no state tracking: buttons fire an action and give a
// momentary "pressed" indication - there's no "currently active" highlight
// concept (the matchType/match comparison that used to drive one, and the
// media_player/input_select state polling it depended on, were removed as
// unnecessary overhead - the highlight round-trip was adding real latency
// for a feature that wasn't worth the wait).
//
// E-ink refresh uses fast/DU mode (single-pass, monochrome) for every
// redraw, main grid included - epd_quality's multi-pass GC16 waveform was
// tried first (to avoid ghosting) but in practice cost ~1.5-2s and several
// visible flash passes per redraw with no ghosting benefit worth the
// latency on this pad's mostly-text/icon content, so it was dropped in
// favor of snappier single-pass updates (2026-07-13). Revisit if ghosting
// becomes a visible problem in practice. Icons are drawn from a curated set
// of pre-rasterized MDI
// bitmaps (icons.h) with a text-only fallback for anything not in that set
// - MDI has ~7000 icons, bundling/rendering all of them live isn't
// reasonable on this hardware.
//
// Power: uses light sleep (not deep sleep) between actions. The GT911
// touch controller's INT line is GPIO48, which is outside the ESP32-S3's
// RTC-GPIO domain (GPIO0-21) and so cannot be used as a deep-sleep wake
// source - this is a hardware limitation of how the pin is wired, not a
// software choice. Light sleep supports GPIO wake on any pin and preserves
// RAM/peripheral state (no full re-init needed on wake), at the cost of
// less dramatic power savings than deep sleep would have given.
//
// WiFi is not used at all in normal operation - BLE (see below) is the
// primary and only path for a successful tap. WiFi only spins up lazily,
// on-demand, if BLE fails and the HTTP fallback actually runs, or on the
// periodic timer wake for layout/slot config sync and OTA checks. It is
// deliberately NOT connected at boot - boot draws whatever's already in
// memory (blank on a cold power-on) rather than spend several seconds
// connecting before the pad is usable. Real config populates on the first
// timer wake (or the first fallback-triggering tap) after power-on.
//
// OTA: HTTP-pull self-update (HTTPUpdate), checked once per timer wake (not
// at boot). Compares FIRMWARE_VERSION against a small version.txt hosted on
// the HA Pi's www folder; downloads and flashes the .bin on a mismatch. Not
// push-based ArduinoOTA, since a device that's mostly asleep (and mostly
// off WiFi entirely now) can't be continuously listening for a push.

#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "icons.h"
#include "secrets.h" // WIFI_SSID, WIFI_PASSWORD, HA_TOKEN - copy secrets.h.example, gitignored

// ---- Config ----
const char* HA_HOST = "192.168.7.1";
const uint16_t HA_PORT = 8123;
const char* LAYOUT_ENTITY = "input_text.garage_monitor_layout_config";

const int MAX_SLOTS = 12;
#define TOUCH_INT_PIN GPIO_NUM_48 // GT911 INT, active-low, NOT RTC-capable (deep sleep wake unusable)

const char* FIRMWARE_VERSION = "16";
const char* OTA_VERSION_URL = "http://192.168.7.1:8123/local/m5paper-hotkey/version.txt";
const char* OTA_BIN_URL = "http://192.168.7.1:8123/local/m5paper-hotkey/firmware.bin";

// BLE control path (see [[ble-hotkey-plan]]) - button presses go over this
// instead of the WiFi/HTTP path in ensureWiFi() et al, which stays as the
// automatic fallback (tryBleInteraction() returning false). USE_BLE is an
// OTA-deployable kill switch: flip to 0 and push a build if BLE ever
// destabilizes the "Garage BLE Proxy" ESP32 that routes it to HA, without
// needing a USB re-flash.
#define USE_BLE 1
#define BLE_SERVICE_UUID          "5b1e0000-0000-1000-8000-00805f9b0001"
#define BLE_CHAR_BUTTON_UUID      "5b1e0001-0000-1000-8000-00805f9b0001"
#define BLE_CHAR_CONFIGDIRTY_UUID "5b1e0003-0000-1000-8000-00805f9b0001"
#define BLE_CHAR_DEVICEINFO_UUID  "5b1e0004-0000-1000-8000-00805f9b0001"
#define BLE_SLOT_SENTINEL_REFRESH 0xFE // Button Event value meaning "header refresh button", not a grid slot

uint32_t COL_BLACK;
uint32_t COL_WHITE;

struct Slot {
  String label;
  String icon;      // e.g. "mdi:video-input-hdmi" - looked up in ICON_TABLE
  String domain;    // "" => refresh-only, no HA service call
  String service;
  String entity;
  int16_t x, y, w, h;
};

Slot g_slots[MAX_SLOTS];
String g_slotRaw[MAX_SLOTS];
int g_activeCount = 6;
String g_orientation = "landscape";
int g_cols = 3;
int g_rows = 2;
// When true, column 0 of each row is split into two half-height slots
// (On/Off) instead of one full-height slot - see layoutSlots(). Adds one
// extra slot per row on top of the normal cols*rows count.
bool g_splitFirstCol = false;
int g_fontSize = 2; // M5GFX setTextSize() multiplier for button labels, 1-4
String g_headerText = "Garage Monitor";
int g_headerFontSize = 2;
String g_layoutRaw = "";

BLEServer* g_bleServer = nullptr;
BLECharacteristic* g_buttonEventChar = nullptr;
BLECharacteristic* g_configDirtyChar = nullptr;
BLECharacteristic* g_deviceInfoChar = nullptr;
bool g_bleReady = false;             // bleSetup() has run
volatile bool g_bleConnected = false;
volatile bool g_bleSubscribed = false; // central has completed start_notify() - safe to notify() now
volatile bool g_bleAdvertising = false;

void applyOrientation() {
  // Empirically: rotation(0) is native/portrait on this panel, rotation(1)
  // is landscape (960x540) - confirmed by the working landscape layout
  // used throughout earlier development.
  M5.Display.setRotation(g_orientation == "portrait" ? 0 : 1);
}

const int HEADER_H = 60; // fixed/compact, was height*0.16 (86px landscape, 154px portrait)
int16_t g_refreshBtnX, g_refreshBtnY, g_refreshBtnW, g_refreshBtnH;

// Fixed position in the header, mirroring the refresh button on the
// opposite side - never overlaps header text (which starts to the right
// of it, see drawUIOnce()) regardless of header text length/font size.
const int16_t SLEEP_BADGE_X = 8;
const int16_t SLEEP_BADGE_Y = 6;

// Narrow gutter reserved on the left of the grid for "UP"/"LO" row labels
// (see drawUIOnce()) - only when the layout is exactly 2 rows, the one
// case with an established row meaning (top row = Upper monitor, bottom
// row = Lower monitor). Kept as small as legibly possible since it comes
// directly out of every button's width: 24px shrinks a 3-column landscape
// button by under 3%.
const int16_t ROW_LABEL_W = 40;
int16_t g_rowLabelCenterX = 0;
bool g_showRowLabels = false;
// Slot index of the first slot in row 1 - g_cols for a plain uniform grid,
// but shifted by 1 when column 0 is split (row 0 then holds cols+1 slots).
// Used by drawUIOnce() to find "the start of the bottom row" without
// hardcoding either layout shape.
int g_row1FirstSlotIndex = 0;
// Full (un-split) row height - needed because g_slots[0].h is only half a
// row's height when column 0 is split, so it can't be used directly to
// find a row's vertical center or bottom edge.
int g_rowFullH = 0;

void layoutSlots() {
  applyOrientation();
  int W = M5.Display.width();
  int H = M5.Display.height();
  int margin = 14;
  int cols = max(1, g_cols);
  int rows = max(1, g_rows);
  g_showRowLabels = (rows == 2);
  int gridLeft = margin + (g_showRowLabels ? ROW_LABEL_W : 0);
  g_rowLabelCenterX = margin + ROW_LABEL_W / 2;
  int gridTop = HEADER_H + margin;
  int gridH = H - gridTop - margin;
  int cellW = (W - gridLeft - margin * cols) / cols;
  int cellH = (gridH - margin * (rows - 1)) / rows;
  g_rowFullH = cellH;

  if (g_splitFirstCol) {
    // Column 0 of each row becomes two half-height slots (On/Off) instead
    // of one full-height slot; columns 1..cols-1 stay full height and
    // shift right by one slot index per row to make room.
    int splitGap = 4;
    int halfH = (cellH - splitGap) / 2;
    int slotsPerRow = cols + 1;
    g_row1FirstSlotIndex = slotsPerRow;
    for (int i = 0; i < g_activeCount; i++) {
      int row = i / slotsPerRow;
      int posInRow = i % slotsPerRow;
      int rowY = gridTop + row * (cellH + margin);
      if (posInRow == 0) { // On - top half of column 0
        g_slots[i].x = gridLeft;
        g_slots[i].y = rowY;
        g_slots[i].w = cellW;
        g_slots[i].h = halfH;
      } else if (posInRow == 1) { // Off - bottom half of column 0
        g_slots[i].x = gridLeft;
        g_slots[i].y = rowY + halfH + splitGap;
        g_slots[i].w = cellW;
        g_slots[i].h = halfH;
      } else {
        int col = posInRow - 1;
        g_slots[i].x = gridLeft + col * (cellW + margin);
        g_slots[i].y = rowY;
        g_slots[i].w = cellW;
        g_slots[i].h = cellH;
      }
    }
  } else {
    g_row1FirstSlotIndex = cols;
    for (int i = 0; i < g_activeCount; i++) {
      int col = i % cols;
      int row = i / cols;
      g_slots[i].x = gridLeft + col * (cellW + margin);
      g_slots[i].y = gridTop + row * (cellH + margin);
      g_slots[i].w = cellW;
      g_slots[i].h = cellH;
    }
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

void parseSlotJson(int i, const String &raw) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, raw);
  if (err) return; // keep previous content on parse failure
  g_slots[i].label = String((const char*)(doc["label"] | ""));
  g_slots[i].icon = String((const char*)(doc["icon"] | ""));
  g_slots[i].domain = String((const char*)(doc["domain"] | ""));
  g_slots[i].service = String((const char*)(doc["service"] | ""));
  g_slots[i].entity = String((const char*)(doc["entity"] | ""));
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
      g_splitFirstCol = doc["splitFirstCol"] | false;
      g_fontSize = doc["fontSize"] | 2;
      if (g_fontSize < 1) g_fontSize = 1;
      if (g_fontSize > 4) g_fontSize = 4;
      g_headerText = String((const char*)(doc["headerText"] | "Garage Monitor"));
      g_headerFontSize = doc["headerFontSize"] | 2;
      if (g_headerFontSize < 1) g_headerFontSize = 1;
      if (g_headerFontSize > 4) g_headerFontSize = 4;
    }
    changed = true;
  }

  int wantCount = g_cols * g_rows + (g_splitFirstCol ? g_rows : 0);
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

// ---- BLE control path ----
// GATT server (pad is peripheral) reached through the "Garage BLE Proxy"
// ESP32's active bluetooth_proxy - see homeassistant/custom_components/
// garage_hotkey_ble/ for the HA-side coordinator this talks to. Advertising
// is only ever on for the duration of a single tap's round-trip (started in
// tryBleInteraction(), stopped in bleTeardown()), never held open through
// light sleep - this keeps the pad's footprint on the proxy's shared
// connection budget minimal instead of constant.

class HotkeyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    g_bleConnected = true;
    Serial.println("[ble] connected");
  }
  void onDisconnect(BLEServer* server) override {
    g_bleConnected = false;
    g_bleSubscribed = false;
    Serial.println("[ble] disconnect");
  }
};

// Notifying immediately on connect races the central's own subscription
// (start_notify() requires its own round-trip GATT write to the CCCD
// descriptor) - a notify sent before that completes is silently dropped,
// with no error on either side. Confirmed this actually happened. Waiting
// for this explicit subscribe confirmation instead of firing blind on
// connect is the correct fix.
class ButtonEventCallbacks : public BLECharacteristicCallbacks {
  void onSubscribe(BLECharacteristic* c, ble_gap_conn_desc* desc, uint16_t subValue) override {
    if (subValue > 0) {
      g_bleSubscribed = true;
    }
  }
};

void bleSetup() {
  BLEDevice::init("GarageHotkeyPad");
  g_bleServer = BLEDevice::createServer();
  g_bleServer->setCallbacks(new HotkeyServerCallbacks());

  BLEService* service = g_bleServer->createService(BLE_SERVICE_UUID);

  g_buttonEventChar = service->createCharacteristic(BLE_CHAR_BUTTON_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  g_buttonEventChar->addDescriptor(new BLE2902());
  g_buttonEventChar->setCallbacks(new ButtonEventCallbacks());

  // Phase-2 stub: accepted but not yet acted on. Config/layout resync still
  // only happens via the existing timer-wake WiFi poll (refreshLayoutAndSlots())
  // until this is wired up to trigger an immediate resync.
  g_configDirtyChar = service->createCharacteristic(BLE_CHAR_CONFIGDIRTY_UUID, BLECharacteristic::PROPERTY_WRITE);

  g_deviceInfoChar = service->createCharacteristic(BLE_CHAR_DEVICEINFO_UUID, BLECharacteristic::PROPERTY_READ);
  g_deviceInfoChar->setValue(FIRMWARE_VERSION);

  service->start();

  // addServiceUUID() unconditionally push_back()s into a vector with no
  // dedup (confirmed in the library source) - calling it from
  // bleStartAdvertising() on every single tap silently duplicated the UUID
  // on each call. A 128-bit UUID is 16 bytes; two copies alone (32 bytes)
  // already exceed BLE's 31-byte legacy advertising payload limit, before
  // flags/name are even counted. Net effect: the first tap after boot
  // worked, and every tap after that had a corrupted/oversized advertising
  // payload - undiscoverable, indistinguishable from "the proxy failed".
  // Set the advertised UUID once here; bleStartAdvertising() only
  // starts/stops from then on.
  BLEDevice::getAdvertising()->addServiceUUID(BLE_SERVICE_UUID);

  g_bleReady = true;
  Serial.println("[ble] setup complete");
}

bool bleIsAdvertising() {
  return g_bleAdvertising;
}

void bleStartAdvertising() {
  if (!g_bleReady) return;
  g_bleAdvertising = true;
  BLEDevice::getAdvertising()->start();
  Serial.println("[ble] advertising start");
}

void bleTeardown() {
  if (!g_bleReady) return;
  if (g_bleAdvertising) {
    BLEDevice::getAdvertising()->stop();
    g_bleAdvertising = false;
  }
  if (g_bleConnected && g_bleServer) {
    g_bleServer->disconnect(g_bleServer->getConnId());
  }
  g_bleConnected = false;
  Serial.println("[ble] teardown");
}

// Bounded wait for a central (routed through the proxy) to connect AND
// finish subscribing to notifications, then notifies the button event.
// Waiting for the subscribe confirmation (not just the connection) avoids
// racing the central's own start_notify() - see ButtonEventCallbacks.
// Returns false on timeout - the caller falls back to the WiFi/HTTP path.
bool bleNotifyButtonPress(uint8_t slotIndexOrSentinel, uint32_t connectTimeoutMs) {
  unsigned long start = millis();
  while (!g_bleSubscribed && millis() - start < connectTimeoutMs) {
    if (!g_bleAdvertising && !g_bleConnected) break; // torn down elsewhere
    delay(50);
  }
  if (!g_bleSubscribed) return false;
  uint8_t payload = slotIndexOrSentinel;
  g_buttonEventChar->setValue(&payload, 1);
  g_buttonEventChar->notify();
  Serial.printf("[ble] notify slot=%d\n", slotIndexOrSentinel);
  return true;
}

// Single entry point for a tap: advertise -> connect -> subscribe -> notify,
// then done. Fire-and-forget - no waiting for any response from HA, since
// there's nothing left to wait for once the notify is sent (no highlight
// mask, no ack). Returns true if the notify was sent, false on timeout (the
// caller falls back to the WiFi/HTTP path). BLE is always torn down here
// before returning, so the caller's redraw never races an active connection.
bool tryBleInteraction(uint8_t slotIndexOrSentinel) {
#if USE_BLE
  bleStartAdvertising();
  bool ok = bleNotifyButtonPress(slotIndexOrSentinel, 4000);
  bleTeardown();
  return ok;
#else
  return false;
#endif
}

const uint8_t* findIcon(const String &name) {
  for (int i = 0; i < ICON_TABLE_SIZE; i++) {
    if (name.equalsIgnoreCase(ICON_TABLE[i].name)) return ICON_TABLE[i].data;
  }
  return nullptr;
}

// pressed=true is only ever used for the momentary flashPressedFeedback()
// pulse below - the persistent grid (drawUIOnce()) always renders neutral,
// no "currently active" concept at all.
void drawSlot(const Slot &s, bool pressed) {
  auto &d = M5.Display;
  uint32_t fill = pressed ? COL_BLACK : COL_WHITE;
  uint32_t text = pressed ? COL_WHITE : COL_BLACK;
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

// Draws text upright into a small offscreen sprite, then pushes it rotated
// 90 degrees so it reads vertically - the standard LovyanGFX technique for
// rotated text (M5Canvas + pushRotateZoom). 1-bit color depth keeps it hard
// black/white with no gray fringing from the rotation, matching the sharp
// look of everything else on this pad. Runs as part of the same full-panel
// refresh already under way in drawUIOnce(), so it needs no display()/
// settling logic of its own.
void drawRowLabel(const char* text, int16_t centerX, int16_t centerY) {
  M5Canvas sprite(&M5.Display);
  sprite.setColorDepth(1);
  sprite.createSprite(150, 36); // wide enough for "LOWER" (5 chars) at size 4
  sprite.fillSprite(COL_WHITE);
  sprite.setTextColor(COL_BLACK, COL_WHITE);
  sprite.setTextDatum(middle_center);
  sprite.setTextSize(4);
  sprite.drawString(text, 75, 18);
  sprite.pushRotateZoom(&M5.Display, centerX, centerY, 90, 1.0f, 1.0f);
  sprite.deleteSprite();
}

// display() in epd_fast mode is a single-pass monochrome (DU) waveform -
// much quicker and less timing-sensitive than the old multi-pass epd_quality
// (GC16) sequence, which was the source of an earlier intermittent
// gradient-corruption bug (confirmed via photo: tiles drawn earlier in the
// scan rendered perfectly, later ones faded through a gray gradient with no
// borders/icons - a partially-applied waveform interrupted mid-refresh, not
// a logic bug). WiFi is still torn down before every display() call
// regardless (reconnected lazily by the next haGetEntityState/haCallService
// via ensureWiFi()) since that mitigation is cheap and the underlying
// preemption risk isn't fully ruled out under DU mode either.
void drawUIOnce() {
  auto &d = M5.Display;
  Serial.printf("[drawUI] start free_heap=%u activeCount=%d cols=%d rows=%d orientation=%s\n",
                ESP.getFreeHeap(), g_activeCount, g_cols, g_rows, g_orientation.c_str());
  d.setEpdMode(epd_mode_t::epd_fast);
  d.fillScreen(COL_WHITE);

  int W = d.width();
  d.drawFastHLine(0, HEADER_H - 1, W, COL_BLACK);
  d.drawFastHLine(0, HEADER_H - 2, W, COL_BLACK);

  // Bold vector font for the header specifically - noticeably bigger/
  // heavier than the plain bitmap font used everywhere else on the pad.
  // Reset back to the default font right after so slot labels and row
  // labels below are unaffected.
  // g_headerFontSize (a 1-4 multiplier, still configurable from the
  // Same bitmap font as everything else on the pad (Font0), just a bigger
  // multiplier than the old g_headerFontSize default - size 5 lands at
  // roughly the same visual height the bold vector font had.
  d.setTextColor(COL_BLACK, COL_WHITE);
  d.setTextDatum(middle_left);
  d.setTextSize(5);
  d.drawString(g_headerText, SLEEP_BADGE_X + ICON_SIZE + 16, HEADER_H / 2);

  const uint8_t* refreshIcon = findIcon("mdi:refresh");
  if (refreshIcon) {
    d.drawBitmap(g_refreshBtnX, g_refreshBtnY, refreshIcon, ICON_SIZE, ICON_SIZE, COL_BLACK, COL_WHITE);
  }

  for (int i = 0; i < g_activeCount; i++) {
    drawSlot(g_slots[i], false); // always neutral - no persistent "active" state
  }

  // Row labels + divider: top row = Upper monitor, bottom row = Lower
  // monitor (see layoutSlots(), only shown for a 2-row layout). Uses
  // g_rowFullH (not g_slots[i].h, which is only a half-row when column 0
  // is split) to find each row's true vertical center/edges.
  if (g_showRowLabels && g_activeCount > g_row1FirstSlotIndex) {
    drawRowLabel("UPPER", g_rowLabelCenterX, g_slots[0].y + g_rowFullH / 2);
    drawRowLabel("LOWER", g_rowLabelCenterX, g_slots[g_row1FirstSlotIndex].y + g_rowFullH / 2);

    // Bold divider centered in the gap between the two rows, matching the
    // header's own 2-stacked-hline bold-line style but a little thicker
    // since this one is a deliberate row separator.
    int gapTop = g_slots[0].y + g_rowFullH;
    int gapBottom = g_slots[g_row1FirstSlotIndex].y;
    int lineThickness = 3;
    int lineY = gapTop + (gapBottom - gapTop - lineThickness) / 2;
    d.fillRect(0, lineY, W, lineThickness, COL_BLACK);
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
  // BLE and WiFi share the same 2.4GHz RF front end on the ESP32-S3, so
  // treat active BLE the same way as WiFi above: fully quiet before the
  // e-paper refresh. Unlike the WiFi mitigation (lines above, confirmed via
  // photo evidence), this hasn't been independently verified against BLE -
  // treated as the same class of risk by default pending its own check.
  if (g_bleConnected || bleIsAdvertising()) {
    bleTeardown();
    delay(300);
  }
  // The clearDisplay() ghost-clearing pass (a full black/white/black cycle)
  // and the generous settling delays around it were mitigations for
  // epd_quality's slow multi-pass GC16 refresh (see drawUIOnce()). Now that
  // every redraw is a single-pass DU update, they're dropped in favor of
  // raw speed - revisit if ghosting turns out to be a real problem in
  // practice under DU mode.
  drawUIOnce();
}

// Genuinely momentary "command received" feedback - a brief epd_fast pulse
// of just the tapped tile, immediately followed by a full drawUI() that
// repaints everything anyway, so any imprecision never persists. Always
// called after BLE teardown or with the WiFi call already complete, so it
// never races an active radio connection, matching every other display()
// call in this sketch. Not a state to track - the very next drawUI() call
// (always neutral, see drawUIOnce()) is what actually settles the screen.
void flashPressedFeedback(const Slot &s) {
  auto &d = M5.Display;
  d.setEpdMode(epd_mode_t::epd_fast);
  drawSlot(s, true);
  d.display();
}

// Small fixed-position "asleep" marker, the only visual difference between
// "pad is actually asleep, this tap both wakes it and fires the command"
// and "pad is already awake" - e-paper otherwise holds whatever was drawn
// last, so without this there's no way to tell the two states apart. A
// dead/crashed/powered-off pad can't draw anything at all, so in practice:
// moon visible = genuinely asleep and fine; moon absent on a screen that's
// unresponsive to touch = not properly asleep, check power/charge.
// Uses display(x, y, w, h) - a real windowed update scoped to just this
// icon (see Panel_EPDiy::display(), which only refreshes the accumulated
// modified-region rect, not the whole panel) - rather than the bare
// display() used everywhere else in this file, so showing/hiding it is a
// small partial refresh. Always fast/DU mode - safe now that setup() primes
// the panel with a one-time quality-mode clear before this is ever called.
// Always called before any radio activity starts in the same code path
// (right before entering light sleep, or as the very first thing on a
// touch wake, ahead of the BLE/WiFi call) so it never races the WiFi-
// interference corruption issue documented on drawUIOnce().
void drawSleepBadge(bool visible) {
  auto &d = M5.Display;
  d.setEpdMode(epd_mode_t::epd_fast);
  d.fillRect(SLEEP_BADGE_X, SLEEP_BADGE_Y, ICON_SIZE, ICON_SIZE, COL_WHITE);
  if (visible) {
    const uint8_t* moonIcon = findIcon("mdi:weather-night");
    if (moonIcon) {
      d.drawBitmap(SLEEP_BADGE_X, SLEEP_BADGE_Y, moonIcon, ICON_SIZE, ICON_SIZE, COL_BLACK, COL_WHITE);
    }
  }
  d.display(SLEEP_BADGE_X, SLEEP_BADGE_Y, ICON_SIZE, ICON_SIZE);
}

// GPIO-only wake, no periodic timer wake - this pad is a "dumb" remote
// with no background WiFi/BLE activity at all outside of an actual tap or
// the manual header refresh button, so there's nothing for a periodic wake
// to do. Removing it means the device sleeps indefinitely until touched,
// which is the real battery win (each wake/sleep cycle has its own
// overhead even when it does nothing).
void enterLightSleepAndWait() {
  gpio_wakeup_enable(TOUCH_INT_PIN, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
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

  // One-time quality-mode (GC16) clear to establish a clean reference frame
  // for the panel - every redraw after this uses fast/DU mode (see
  // drawUIOnce()/drawSleepBadge()), but DU updates starting from an unknown
  // post-power-on panel state have rendered wrong (inverted/half-drawn) in
  // testing. Paid once per boot, not per action, so it doesn't cost
  // anything against the snappiness this pad otherwise optimizes for.
  M5.Display.setEpdMode(epd_mode_t::epd_quality);
  M5.Display.clearDisplay();

#if USE_BLE
  bleSetup();
#endif

  // layoutSlots() lays out whatever's currently in g_slots (all blank on a
  // cold power-on) so the pad has something on screen immediately, then a
  // one-time WiFi pull fills in the real content right after - the pad has
  // no other automatic path to get real config now that the periodic
  // background sync is gone (manual refresh is the only recurring path),
  // so without this a cold boot would otherwise stay blank until someone
  // taps the header refresh button.
  layoutSlots();
  drawUI();
  if (refreshLayoutAndSlots()) {
    drawUI();
  }
}

void loop() {
  M5.update();

  bool handledTouch = false;
  if (M5.Touch.getCount()) {
    auto t = M5.Touch.getDetail(0);
    // wasPressed() (down-edge), not wasReleased() (up-edge): the touch that
    // wakes the device from light sleep is a press, and by the time loop()
    // resumes and polls here, the finger may still be down - waiting for a
    // release meant that first wake-triggering touch was frequently missed
    // entirely (code saw "not released yet", did nothing, went back to
    // sleep before the finger ever lifted), forcing a second tap to
    // register anything. Acting on press instead means a single deliberate
    // tap both wakes the pad and is treated as the command in one motion.
    if (t.wasPressed()) {
      // Clear the sleep badge immediately, before any BLE/WiFi call - this
      // is the fast, near-instant "I'm awake" confirmation, decoupled from
      // however long the actual command round-trip takes.
      drawSleepBadge(false);
      if (t.x >= g_refreshBtnX && t.x <= g_refreshBtnX + g_refreshBtnW &&
          t.y >= g_refreshBtnY && t.y <= g_refreshBtnY + g_refreshBtnH) {
        Serial.println("[loop] touch hit header refresh button");
        // Refresh is WiFi-only, deliberately, not routed through BLE at
        // all: it has no mapped HA service call, so a successful BLE round
        // trip for it would do nothing except redraw the same (possibly
        // stale) data - the whole point of this button is the WiFi resync
        // itself. This is also the only place a firmware update check
        // happens now - no periodic background polling, so "manually
        // triggered refresh" is the one moment WiFi comes up on its own.
        refreshLayoutAndSlots();
        drawUI();
        checkOTA();
        handledTouch = true;
      }
      for (int i = 0; !handledTouch && i < g_activeCount; i++) {
        Slot &s = g_slots[i];
        if (t.x >= s.x && t.x <= s.x + s.w && t.y >= s.y && t.y <= s.y + s.h) {
          Serial.printf("[loop] touch hit slot %d ('%s')\n", i, s.label.c_str());
          if (!tryBleInteraction((uint8_t)i)) {
            if (s.domain.length() && s.service.length() && s.entity.length()) {
              haCallService(s.domain, s.service, s.entity);
            }
            refreshLayoutAndSlots();
          }
          // Confirms the tap was received/handled, regardless of which path
          // (BLE or WiFi fallback) processed it - not tied to whether the
          // downstream HA action specifically succeeded. Always followed by
          // the normal neutral drawUI() below, so it never lingers.
          flashPressedFeedback(s);
          drawUI();
          handledTouch = true;
          break;
        }
      }
    }
  }

  drawSleepBadge(true);
  enterLightSleepAndWait();
}
