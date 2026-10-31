if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/secrets.h")
    message(FATAL_ERROR "Copy main/secrets.example.h to main/secrets.h and fill in what applies")
endif()

# Wi-Fi on the classic ESP32, Thread on the ESP32-C6
if(CONFIG_OPENTHREAD_ENABLED)
    # Plus Wi-Fi for the configuration mode (config_mode.h)
    set(net_src "net_thread.cpp" "config_mode.cpp" "thread_dataset.cpp")
    set(net_requires openthread esp_wifi mdns)
else()
    set(net_src "net.cpp")
    set(net_requires esp_wifi)
endif()

idf_component_register(
    SRCS "app_main.cpp" "matter_bridge.cpp" "setup_code.cpp" ${net_src}
    INCLUDE_DIRS "."
    REQUIRES octo_core esp_matter nvs_flash esp_netif esp_event esp_app_format ${net_requires}
)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
