// Copy to secrets.h (not in git) and fill in.
#pragma once

// The Wi-Fi network is set on the settings page (on the setup access point
// of a new bridge), never here.

// The password of the settings page is chosen on the page the first time.
// A bridge built with an OCTO_HTTP_TOKEN here keeps accepting it until then.

// Bridge on an ESP32-C6 only, optional: the Thread network is normally pasted
// on the settings page (in configuration mode on Wi-Fi). This one is used
// until then, as the operational dataset TLVs in hex. In Home Assistant:
// Settings > Devices & services > Thread > Configure, the (i) of the
// preferred network.
#define OCTO_THREAD_DATASET ""
