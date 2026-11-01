idf_component_register(
    SRCS "app_main.cpp" "io.cpp" "matter_device.cpp" "ir.cpp" "ble_scan.cpp"
    INCLUDE_DIRS "."
    REQUIRES mb_core esp_matter nvs_flash esp_driver_gpio esp_driver_ledc esp_driver_i2c esp_driver_rmt
             esp_timer openthread bt
)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")

# The settings page, sent gzip-compressed: over Thread every kilobyte counts
set(page "${CMAKE_CURRENT_SOURCE_DIR}/../web/generic.html")
set(page_gz "${CMAKE_CURRENT_BINARY_DIR}/generic.html.gz")
add_custom_command(OUTPUT "${page_gz}"
    COMMAND gzip -9 -n -c "${page}" > "${page_gz}"
    DEPENDS "${page}"
    VERBATIM)
add_custom_target(generic_page_gz DEPENDS "${page_gz}")
target_add_binary_data(${COMPONENT_LIB} "${page_gz}" BINARY DEPENDS generic_page_gz)

# A project generated from a spec (tools/mb_generate.py) carries the spec
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/spec.yaml")
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/spec.yaml" TEXT)
endif()
