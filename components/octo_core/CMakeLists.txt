# The Octo part of the bridge: bed links over BLE, the bed commands and the
# settings page. What all apps share is in mb_core.
idf_component_register(
    SRCS "bed_link.cpp" "octo_commands.cpp"
    INCLUDE_DIRS "include"
    REQUIRES mb_core octo_protocol bt nvs_flash esp_timer
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
