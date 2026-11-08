# What every Matter Builder app shares: settings page server, HTTP API,
# program tokens, boot guard, console, pairing code, and on Thread chips the
# network with configuration mode and the setup access point (mb_core.h).
set(srcs "mb_core.cpp" "boot_guard.cpp" "commands.cpp" "http_api.cpp" "program_access.cpp" "setup_code.cpp"
    "matter_info.cpp" "log_buffer.cpp")
set(requires nvs_flash console esp_timer app_update esp_app_format esp_http_server esp_driver_gpio mbedtls
    esp_matter esp_netif esp_event)
if(CONFIG_OPENTHREAD_ENABLED)
    list(APPEND srcs "net_thread.cpp" "config_mode.cpp" "thread_dataset.cpp" "setup_dns.cpp")
    list(APPEND requires openthread esp_wifi mdns)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES ${requires}
)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
