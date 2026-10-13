// Template for include/secrets.h (git-ignored). Copy, then fill in:
//   cp include/secrets.example.h include/secrets.h
// Never commit secrets.h. pack_source.py fails the build if these values appear in the source archive.
#pragma once

#define CQ_WIFI_SSID "your-home-ssid"
#define CQ_WIFI_PASSWORD "your-home-password"
// Fallback hotspot "conqueror-bridge" (WPA2): at least 8 characters.
#define CQ_AP_PASSWORD "change-me-too"
// ArduinoOTA password (also export it as CQ_OTA_PASSWORD for `pio run -e bridge_ota -t upload`).
// The build refuses "change-me".
#define CQ_OTA_PASSWORD "change-me"
