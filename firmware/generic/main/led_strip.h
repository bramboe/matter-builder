// Addressable LED strips (WS2812/WS2811/WS2815 and SK6812 RGBW) as Matter
// colour lights: connection types "led_strip" and "led_strip_rgbw", set on
// the page like any other (a pin and the number of LEDs). Registered like a
// component, so stored connections keep their type.

#pragma once

namespace led_strip {

void register_types();

}  // namespace led_strip
