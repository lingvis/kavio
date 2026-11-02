cmake_minimum_required(VERSION 3.16)

project(kavio_app_frame_device
    VERSION 1.0.0
    DESCRIPTION "Minimal Kavio Device App template"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)
get_filename_component(KAVIO_PROJECT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(KAVIO_REPOSITORY_ROOT "${KAVIO_PROJECT_DIR}/../.." ABSOLUTE)
set(KAVIO_DEVICE_SDK_DIR "${KAVIO_REPOSITORY_ROOT}/kavio_sdk/device/build" CACHE PATH
    "Directory containing KavioDeviceSdkConfig.cmake")

if(NOT EXISTS "${KAVIO_DEVICE_SDK_DIR}/KavioDeviceSdkConfig.cmake")
    message(FATAL_ERROR
        "Prebuilt Kavio Device SDK was not found: ${KAVIO_DEVICE_SDK_DIR}/KavioDeviceSdkConfig.cmake\n"
        "Build kavio_sdk/device first with the LVGL AppView adapter enabled.")
endif()

find_package(KavioDeviceSdk CONFIG REQUIRED
    PATHS "${KAVIO_DEVICE_SDK_DIR}"
    NO_DEFAULT_PATH
)

if(NOT TARGET Kavio::DeviceSdkLvgl)
    message(FATAL_ERROR
        "Kavio::DeviceSdkLvgl is not available. Configure the SDK with\n"
        "-DKAVIO_BUILD_LVGL_ADAPTER=ON and valid KAVIO_LVGL_ROOT / KAVIO_LV_CONF_PATH.")
endif()

add_executable(kavio_app_frame main.cpp)
set_target_properties(kavio_app_frame PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_link_libraries(kavio_app_frame PRIVATE Kavio::DeviceSdkLvgl Threads::Threads rt)
target_compile_options(kavio_app_frame PRIVATE -Wall -Wextra -Wno-deprecated-declarations)
