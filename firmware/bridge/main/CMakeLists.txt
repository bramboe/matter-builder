if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/secrets.h")
    message(FATAL_ERROR "Copy main/secrets.example.h to main/secrets.h and fill in Wi-Fi and token")
endif()

idf_component_register(
    SRCS "app_main.cpp" "matter_bridge.cpp" "net.cpp"
    INCLUDE_DIRS "."
    REQUIRES octo_core esp_matter nvs_flash esp_wifi esp_netif esp_event
)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
