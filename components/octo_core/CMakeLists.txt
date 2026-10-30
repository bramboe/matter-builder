# Bed links, boot guard, console commands and the HTTP API of the Matter
# bridge. The app provides net.cpp (see include/net.h).
idf_component_register(
    SRCS "bed_link.cpp" "boot_guard.cpp" "commands.cpp" "http_api.cpp"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "web/index.html"
    REQUIRES octo_protocol bt nvs_flash console esp_timer app_update esp_app_format esp_http_server
)
