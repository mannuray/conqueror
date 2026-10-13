#!/usr/bin/env bash
# App + source archive over WiFi (bridge LLD R2.5). Only works while nobody drives the tank.
# The OTA password is never printed: espota's debug output and command echoes are filtered.
set -euo pipefail
cd "$(dirname "$0")/.."
: "${CQ_OTA_PASSWORD:?export CQ_OTA_PASSWORD (the same value as in include/secrets.h)}"
host="${CQ_OTA_HOST:-conqueror-bridge.local}"
# Resolve mDNS once: espota's own lookup sometimes misses a slow mDNS answer ("Host ... Not Found").
ip="$(python3 -c 'import socket,sys; print(socket.gethostbyname(sys.argv[1]))' "$host")"
echo "ota: $host = $ip"
scrub() { sed -E "s/(auth[=:] ?['\"]?)[^'\" ,)]*/\1***/g"; }
pio run -e bridge_ota -t upload --upload-port "$ip" 2>&1 | scrub
echo "waiting 20 s for the bridge to reboot into the new slot and rejoin WiFi..."
sleep 20
CQ_OTA_HOST="$ip" pio run -e bridge_ota -t upload_source_ota 2>&1 | scrub
