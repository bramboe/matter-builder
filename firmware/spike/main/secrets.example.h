// Copy to secrets.h (not in git) and fill in.
#pragma once

#define OCTO_WIFI_SSID "your-ssid"
#define OCTO_WIFI_PASSWORD "your-wifi-password"

// Sent as header "X-Octo-Token" on every HTTP request. Generate one with:
//   openssl rand -hex 16
#define OCTO_HTTP_TOKEN "change-me"
