# The component's name is its type: the generator adds it to the firmware
idf_component_register(SRCS "pulse_relay.cpp"
                       REQUIRES mb_driver esp_driver_gpio esp_timer)
