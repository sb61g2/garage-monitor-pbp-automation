#!/bin/bash
# Purges the Home Assistant macOS companion app's WebKit frontend cache -
# for when dashboard/frontend changes don't show up despite reboots and
# Cmd+R, and the in-app Settings > cache-clear option isn't clickable.
#
# The app (io.robbie.HomeAssistant) is a WKWebView wrapper; a plain reload
# doesn't reliably bust its Service Worker / Cache Storage data the way
# Cmd+Shift+R does in a real browser, so this clears the underlying WebKit
# storage directly. Login/server config is stored in Keychain and
# Preferences, not touched here, so you won't be signed out.

set -euo pipefail

CONTAINER="$HOME/Library/Containers/io.robbie.HomeAssistant/Data"

if [ ! -d "$CONTAINER" ]; then
  echo "Home Assistant app container not found at $CONTAINER - is the app installed?" >&2
  exit 1
fi

echo "Quitting Home Assistant..."
osascript -e 'quit app "Home Assistant"' 2>/dev/null || true
# Give it a moment to release its file handles before we delete out from under it.
for i in $(seq 1 20); do
  pgrep -f "Home Assistant.app/Contents/MacOS/Home Assistant" > /dev/null 2>&1 || break
  sleep 0.5
done

echo "Purging WebKit frontend/service-worker cache..."
rm -rf "$CONTAINER/Library/WebKit/WebsiteData/Default"
rm -rf "$CONTAINER/Library/Caches/WebKit"
rm -rf "$CONTAINER/Library/Caches/com.apple.WebKit.Networking"

echo "Relaunching Home Assistant..."
open -a "Home Assistant"
echo "Done."
