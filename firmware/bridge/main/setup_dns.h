// DNS for the setup access point: every name resolves to the bridge, so a
// phone that joins the access point opens the settings page by itself.

#pragma once

#include <cstdint>

namespace setup_dns {

// `ip` in network byte order, e.g. the access point's 192.168.4.1
void start(uint32_t ip);

}  // namespace setup_dns
