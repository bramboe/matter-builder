if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/secrets.h")
    message(FATAL_ERROR "Copy main/secrets.example.h to main/secrets.h and fill in Wi-Fi and token")
endif()

# Wi-Fi on the classic ESP32, Thread on the ESP32-C6
if(CONFIG_OPENTHREAD_ENABLED)
    set(net_src "net_thread.cpp")
    set(net_requires openthread)
else()
    set(net_src "net.cpp")
    set(net_requires esp_wifi)
endif()

idf_component_register(
    SRCS "app_main.cpp" "matter_bridge.cpp" ${net_src}
    INCLUDE_DIRS "."
    REQUIRES octo_core esp_matter nvs_flash esp_netif esp_event ${net_requires}
)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
