if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/secrets.h")
    message(FATAL_ERROR "Copy main/secrets.example.h to main/secrets.h and fill in what applies")
endif()

# The network (Thread or Wi-Fi) comes with mb_core
idf_component_register(
    SRCS "app_main.cpp" "matter_bridge.cpp"
    INCLUDE_DIRS "."
    REQUIRES octo_core mb_core esp_matter nvs_flash esp_netif esp_event esp_app_format esp_wifi
)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")

# A project generated from a spec (tools/mb_generate.py) carries the spec
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/spec.yaml")
    target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_SOURCE_DIR}/spec.yaml" TEXT)
endif()
