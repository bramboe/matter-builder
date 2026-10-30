// Copy to secrets.h (not in git) and fill in.
#pragma once

#define OCTO_WIFI_SSID "your-ssid"
#define OCTO_WIFI_PASSWORD "your-wifi-password"

// Sent as header "X-Octo-Token" on every HTTP request. Generate one with:
//   openssl rand -hex 16
#define OCTO_HTTP_TOKEN "change-me"

// Bridge on an ESP32-C6 only: the Thread network to join, as the operational
// dataset TLVs in hex. In Home Assistant: Settings > Devices & services >
// Thread > Configure, the (i) of the preferred network.
#define OCTO_THREAD_DATASET ""
