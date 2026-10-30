# Bed links, boot guard, console commands and the HTTP API of the Matter
# bridge. The app provides net.cpp (see include/net.h).
idf_component_register(
    SRCS "bed_link.cpp" "boot_guard.cpp" "commands.cpp" "http_api.cpp"
    INCLUDE_DIRS "include"
    REQUIRES octo_protocol bt nvs_flash console esp_timer app_update esp_app_format esp_http_server
)

# The settings page is sent gzip-compressed: over Thread every kilobyte counts
set(page "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html")
set(page_gz "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
add_custom_command(OUTPUT "${page_gz}"
    COMMAND gzip -9 -n -c "${page}" > "${page_gz}"
    DEPENDS "${page}"
    VERBATIM)
add_custom_target(octo_page_gz DEPENDS "${page_gz}")
target_add_binary_data(${COMPONENT_LIB} "${page_gz}" BINARY DEPENDS octo_page_gz)
