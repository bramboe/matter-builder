idf_component_register(
    SRCS "app_main.cpp" "bed_link.cpp" "console_cmds.cpp"
    INCLUDE_DIRS "."
    REQUIRES octo_protocol bt nvs_flash console esp_timer
)
