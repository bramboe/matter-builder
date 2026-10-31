// Copy to secrets.h (not in git) and fill in.
#pragma once

// Classic ESP32 only (its Matter network). The ESP32-C6 takes the Wi-Fi for
// configuration mode from the settings page, or its setup access point.
#define OCTO_WIFI_SSID "your-ssid"
#define OCTO_WIFI_PASSWORD "your-wifi-password"

// The password of the settings page is chosen on the page the first time.
// A bridge built with an OCTO_HTTP_TOKEN here keeps accepting it until then.

// Bridge on an ESP32-C6 only, optional: the Thread network is normally pasted
// on the settings page (in configuration mode on Wi-Fi). This one is used
// until then, as the operational dataset TLVs in hex. In Home Assistant:
// Settings > Devices & services > Thread > Configure, the (i) of the
// preferred network.
#define OCTO_THREAD_DATASET ""
