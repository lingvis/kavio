cmake_minimum_required(VERSION 3.16)

project(demo_virtual_hdmi_device
    VERSION 2.0.0
    DESCRIPTION "Kavio native virtual HDMI stream receiver"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)
get_filename_component(KAVIO_PROJECT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(KAVIO_REPOSITORY_ROOT "${KAVIO_PROJECT_DIR}/../.." ABSOLUTE)
set(KAVIO_DEVICE_SDK_DIR "${KAVIO_REPOSITORY_ROOT}/kavio_sdk/device/build" CACHE PATH
    "Directory containing KavioDeviceSdkConfig.cmake")

if(NOT EXISTS "${KAVIO_DEVICE_SDK_DIR}/KavioDeviceSdkConfig.cmake")
    message(FATAL_ERROR
        "Prebuilt Kavio Device SDK was not found:\n"
        "  ${KAVIO_DEVICE_SDK_DIR}/KavioDeviceSdkConfig.cmake\n"
        "Build kavio_sdk/device first.")
endif()

find_package(KavioDeviceSdk CONFIG REQUIRED
    PATHS "${KAVIO_DEVICE_SDK_DIR}"
    NO_DEFAULT_PATH
)

set(TARGET_NAME demo_virtual_hdmi)
add_executable(${TARGET_NAME}
    main.cpp
    FrameReceiver.cpp
    FrameReceiver.h
)
set_target_properties(${TARGET_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_include_directories(${TARGET_NAME} PRIVATE
    "${KAVIO_PROJECT_DIR}/common")
target_link_libraries(${TARGET_NAME} PRIVATE
    Kavio::DeviceSdk Threads::Threads ${CMAKE_DL_LIBS} rt)
