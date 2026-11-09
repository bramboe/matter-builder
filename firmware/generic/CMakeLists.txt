cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "Set ESP_MATTER_PATH to the esp-matter checkout (release/v1.6)")
endif()

set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

# The chip (-D IDF_TARGET=..., the ESP32-C6 by default) and its network
# (-D MB_NETWORK=thread|wifi). Thread only where the chip has it; Wi-Fi
# everywhere else. Each combination is its own firmware with its own name,
# so an update never crosses chips or networks:
#   mb_generic          ESP32-C6, Thread
#   mb_generic_wifi     ESP32-C6, Wi-Fi
#   mb_generic_<chip>   any other chip (Wi-Fi; Thread on the ESP32-H2)
if(NOT DEFINED IDF_TARGET)
    if(DEFINED ENV{IDF_TARGET})
        set(IDF_TARGET $ENV{IDF_TARGET})
    else()
        set(IDF_TARGET esp32c6)
    endif()
endif()
set(MB_THREAD_CHIPS esp32c6 esp32h2)
set(MB_WIFI_CHIPS esp32 esp32c3 esp32s3 esp32c6)
if(NOT DEFINED MB_NETWORK)
    if(IDF_TARGET IN_LIST MB_THREAD_CHIPS)
        set(MB_NETWORK thread)
    else()
        set(MB_NETWORK wifi)
    endif()
endif()
if(MB_NETWORK STREQUAL "thread" AND NOT IDF_TARGET IN_LIST MB_THREAD_CHIPS)
    message(FATAL_ERROR "${IDF_TARGET} has no Thread radio: use MB_NETWORK=wifi")
endif()
if(MB_NETWORK STREQUAL "wifi" AND NOT IDF_TARGET IN_LIST MB_WIFI_CHIPS)
    message(FATAL_ERROR "${IDF_TARGET} is not supported on Wi-Fi")
endif()
if(MB_NETWORK STREQUAL "wifi")
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.wifi")
endif()
if(IDF_TARGET STREQUAL "esp32c6")
    set(MB_PROJECT mb_generic)
    if(MB_NETWORK STREQUAL "wifi")
        set(MB_PROJECT mb_generic_wifi)
    endif()
else()
    set(MB_PROJECT mb_generic_${IDF_TARGET})
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# mb_core: next to this project in the repository, or wherever MB_COMPONENTS
# says (a package source building against a matter-builder checkout)
if(DEFINED ENV{MB_COMPONENTS})
    set(MB_COMPONENTS $ENV{MB_COMPONENTS})
else()
    set(MB_COMPONENTS "${CMAKE_CURRENT_LIST_DIR}/../../components")
endif()

set(EXTRA_COMPONENT_DIRS
    "${MB_COMPONENTS}"
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components")

# Shown to controllers as the software version. PROJECT_VER_NUMBER goes into
# the Matter OTA image header and must equal CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER,
# the version the device reports: an update is only taken when it is higher.
set(PROJECT_VER "0.1.8")
set(PROJECT_VER_NUMBER 108)
# Only what main needs: the other components next to mb_core stay out
set(COMPONENTS main)
project(${MB_PROJECT})

if(NOT CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER EQUAL PROJECT_VER_NUMBER)
    message(FATAL_ERROR "PROJECT_VER_NUMBER ${PROJECT_VER_NUMBER} differs from "
        "CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER ${CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER}")
endif()

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
