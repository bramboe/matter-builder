cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "Set ESP_MATTER_PATH to the esp-matter checkout (release/v1.6)")
endif()

set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

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
set(PROJECT_VER "0.1.1")
set(PROJECT_VER_NUMBER 101)
# Only what main needs: the other components next to mb_core stay out
set(COMPONENTS main)
project(mb_generic)

if(NOT CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER EQUAL PROJECT_VER_NUMBER)
    message(FATAL_ERROR "PROJECT_VER_NUMBER ${PROJECT_VER_NUMBER} differs from "
        "CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER ${CONFIG_DEVICE_SOFTWARE_VERSION_NUMBER}")
endif()

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
