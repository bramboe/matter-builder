# Bed links, boot guard and the console commands, shared by the F0 spike and
# the Matter bridge. Each app provides its own net.cpp (see include/net.h).
idf_component_register(
    SRCS "bed_link.cpp" "boot_guard.cpp" "commands.cpp" "http_api.cpp"
    INCLUDE_DIRS "include"
    REQUIRES octo_protocol bt nvs_flash console esp_timer app_update esp_app_format esp_http_server
)
