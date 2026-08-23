# Device Demo 链接预先构建的 Kavio Device SDK。Overlay 使用轻量 BGRA 像素绘制，
# 不依赖 OpenCV/TBB 等额外图像库。
cmake_minimum_required(VERSION 3.16)

project(demo_rx_tx_device
    VERSION 1.0.0
    DESCRIPTION "Kavio RX/TX shared-memory Device Component demo"
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
        "Prebuilt Kavio Device SDK was not found:
  ${KAVIO_DEVICE_SDK_DIR}/KavioDeviceSdkConfig.cmake
"
        "Build kavio_sdk/device first.")
endif()

find_package(KavioDeviceSdk CONFIG REQUIRED
    PATHS "${KAVIO_DEVICE_SDK_DIR}"
    NO_DEFAULT_PATH
)

set(KAVIO_RX_TX_DEMO_TARGET demo_rx_tx)
add_executable(${KAVIO_RX_TX_DEMO_TARGET}
    main.cpp
)

set_target_properties(${KAVIO_RX_TX_DEMO_TARGET} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

target_link_libraries(${KAVIO_RX_TX_DEMO_TARGET}
    PRIVATE
        Kavio::DeviceSdk
        Threads::Threads
        ${CMAKE_DL_LIBS}
        rt
        m
)
