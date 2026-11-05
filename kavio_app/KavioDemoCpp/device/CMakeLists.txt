cmake_minimum_required(VERSION 3.16)

project(kavio_demo_cpp_device
    VERSION 1.0.0
    DESCRIPTION "KavioDemo C++ Device Component"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)
# Paths are provided by the application build driver; no source-tree traversal.
foreach(required_var KAVIO_DEVICE_SDK_DIR KAVIO_APP_PROJECT_ROOT)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} must be provided by the App build driver")
    endif()
endforeach()

if(NOT EXISTS "${KAVIO_DEVICE_SDK_DIR}/KavioDeviceSdkConfig.cmake")
    message(FATAL_ERROR
        "Prebuilt Kavio Device SDK was not found: ${KAVIO_DEVICE_SDK_DIR}/KavioDeviceSdkConfig.cmake\n"
        "Build the Device SDK first from the repository root.")
endif()

find_package(KavioDeviceSdk CONFIG REQUIRED
    PATHS "${KAVIO_DEVICE_SDK_DIR}"
    NO_DEFAULT_PATH
)

if(NOT TARGET Kavio::App OR NOT TARGET Kavio::UI::LVGL OR NOT TARGET Kavio::DeviceSdk)
    message(FATAL_ERROR
        "Kavio::App, Kavio::UI::LVGL and Kavio::DeviceSdk are required by KavioDemo C++")
endif()

add_executable(kavio_demo_cpp main.cpp)
set_target_properties(kavio_demo_cpp PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_include_directories(kavio_demo_cpp PRIVATE "${KAVIO_APP_PROJECT_ROOT}/common")
target_link_libraries(kavio_demo_cpp PRIVATE
    Kavio::App
    Kavio::UI::LVGL
    Kavio::DeviceSdk
    Threads::Threads
    rt)
target_compile_options(kavio_demo_cpp PRIVATE -Wall -Wextra -Wno-deprecated-declarations)
