# Garage Monitor PBP Automation

Home Assistant automation for a garage workstation with two Samsung G95SC monitors (Upper/Lower), plus a physical M5Stack PaperS3 e-paper hotkey pad for switching sources without touching a keyboard.

## What's here

- **`homeassistant/packages/garage_monitor.yaml`** - HA package: `input_select` helpers that track each monitor's active source (the Samsung integration doesn't expose one), and scripts for power and source switching (Mac Mini / Laptop / SBS picture-by-picture) on both monitors. Source switching is done via `remote.send_command` key navigation, not `media_player.select_source` - the Samsung integration's source list doesn't include DisplayPort.
- **`homeassistant/www/garage-monitor-hotkey-card.js`** - a vanilla-JS Lovelace card for visually building the hotkey pad's button grid (layout, and a domain -> entity -> service action picker per button). No build step, no LitElement.
- **`m5paper-hotkey/`** - the pad's firmware (Arduino/`arduino-cli`, M5Unified + M5GFX). Fully data-driven from the HA entities above; the physical grid, icons, and button actions are all configured from the Lovelace card, not hardcoded in firmware.
- **`scripts/purge-ha-app-cache.sh`** - clears the HA macOS companion app's WebKit Service Worker/cache, since it doesn't reliably pick up Lovelace/dashboard changes on a plain reload.

## Hotkey pad architecture

The pad is a PaperS3 (ESP32-S3, 960x540 e-paper, GT911 touch), normally in light sleep and woken by touch. Two independent paths to fire a button's action, in order:

1. **BLE** (primary) - the pad briefly advertises and notifies a "garage_hotkey_ble" HA-side BLE proxy coordinator (lives in HA's `custom_components/`, not in this repo) via a Seeed XIAO ESP32S3 running ESPHome's `bluetooth_proxy`. Fast, no WiFi needed.
2. **WiFi/HTTP fallback** - if BLE doesn't connect within ~4s, the pad brings up WiFi and calls the HA REST API directly.

Layout/button config lives in `input_text.garage_monitor_layout_config` and `input_text.garage_monitor_slot_0..11` (a 12-slot pool - HA's 255-char `input_text` limit is why it's sharded instead of one JSON blob). Icons are a curated set of ~48 pre-rasterized MDI bitmaps baked into `icons.h`; anything outside that set falls back to text-only.

OTA updates are HTTP-pull, not push: the pad checks `http://<HA host>:8123/local/m5paper-hotkey/version.txt` against its own compiled-in version only when the header refresh button is tapped (no periodic background wake - the pad is otherwise fully asleep except when touched).

### Building and publishing firmware

```
cd m5paper-hotkey
cp secrets.h.example secrets.h   # fill in WIFI_SSID / WIFI_PASSWORD / HA_TOKEN
arduino-cli compile --fqbn "m5stack:esp32:m5stack_papers3:CDCOnBoot=cdc" --export-binaries .
```

The `CDCOnBoot=cdc` board option matters: without it, `Serial` output is silently routed to the physical UART0 pins instead of the USB-C port, and a serial monitor attached to `/dev/cu.usbmodem*` will just sit at zero bytes forever with no error.

To publish an update, bump `FIRMWARE_VERSION` in the `.ino`, then push the binary and version file to the HA instance's `www/m5paper-hotkey/` folder (served at `/local/m5paper-hotkey/`):

```
scp -O build/m5stack.esp32.m5stack_papers3/m5paper-hotkey.ino.bin ha-pi:/tmp/firmware.bin
ssh ha-pi "sudo mv /tmp/firmware.bin /homeassistant/www/m5paper-hotkey/firmware.bin"
echo "<new version>" | ssh ha-pi "sudo tee /homeassistant/www/m5paper-hotkey/version.txt"
```

Always verify the served bytes match the local build (`curl` + `diff`) before considering a publish done - and note the pad only checks for updates on a manual refresh-button tap, so a fresh publish won't reach the device until then.

### Hardware notes

- The GT911 touch controller's INT line (GPIO48) is outside the ESP32-S3's RTC-GPIO domain, so true deep-sleep wake-on-touch isn't possible on this board - the pad uses light sleep instead.
- No separate BOOT button - long-press the side button until the LED flashes red for download/bootloader mode. `esptool`'s normal auto-reset doesn't reliably work once the pad has been through a light-sleep cycle.
- The bootloader has app-rollback protection enabled; firmware calls `esp_ota_mark_app_valid_cancel_rollback()` at boot so an OTA'd image doesn't stay "pending verify" indefinitely (which risks a silent rollback to the last-confirmed image if another reset happens before it's confirmed).

## Home Assistant access

Standard access details (SSH, REST API, config paths) aren't included here since this is a personal setup - see local notes if you have access to the HA instance this targets.
