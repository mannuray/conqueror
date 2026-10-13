#!/usr/bin/env bash
# App + source archive over WiFi (bridge LLD R2.5). Only works while nobody drives the tank.
set -euo pipefail
cd "$(dirname "$0")/.."
: "${CQ_OTA_PASSWORD:?export CQ_OTA_PASSWORD (the same value as in include/secrets.h)}"
pio run -e bridge_ota -t upload
echo "waiting 20 s for the bridge to reboot into the new slot and rejoin WiFi..."
sleep 20
pio run -e bridge_ota -t upload_source_ota
