cmake_minimum_required(VERSION 3.16)
project({{DEVICE_ENTRY}}_device LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT KAVIO_DEVICE_SDK_DIR)
  message(FATAL_ERROR "KAVIO_DEVICE_SDK_DIR must be provided by ./kavio build")
endif()

find_package(KavioDeviceSdk CONFIG REQUIRED PATHS "${KAVIO_DEVICE_SDK_DIR}" NO_DEFAULT_PATH)
if(NOT TARGET Kavio::App OR NOT TARGET Kavio::UI::LVGL)
  message(FATAL_ERROR "Required Kavio SDK targets are unavailable")
endif()

add_executable({{DEVICE_ENTRY}} main.cpp)
set_target_properties({{DEVICE_ENTRY}} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
target_link_libraries({{DEVICE_ENTRY}} PRIVATE Kavio::App Kavio::UI::LVGL)
target_compile_options({{DEVICE_ENTRY}} PRIVATE -Wall -Wextra)
