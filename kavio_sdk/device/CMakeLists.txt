cmake_minimum_required(VERSION 3.16)

# KavioSdkVersion.h 是 SDK 版本唯一来源。CMake project 版本从该头文件解析，
# 避免构建版本与SDK 版本分别维护后发生漂移。
set(KAVIO_SDK_VERSION_HEADER_SOURCE "${CMAKE_CURRENT_LIST_DIR}/include/kavio/KavioSdkVersion.h")
file(READ "${KAVIO_SDK_VERSION_HEADER_SOURCE}" KAVIO_SDK_VERSION_HEADER_TEXT)
foreach(component MAJOR MINOR PATCH)
    string(REGEX MATCH
        "#define[ \t]+KAVIO_SDK_VERSION_${component}[ \t]+([0-9]+)U?"
        _version_match
        "${KAVIO_SDK_VERSION_HEADER_TEXT}")
    if(_version_match STREQUAL "")
        message(FATAL_ERROR "Cannot parse KAVIO_SDK_VERSION_${component} from ${KAVIO_SDK_VERSION_HEADER_SOURCE}")
    endif()
    set(KAVIO_SDK_VERSION_${component} "${CMAKE_MATCH_1}")
endforeach()
set(KAVIO_SDK_VERSION
    "${KAVIO_SDK_VERSION_MAJOR}.${KAVIO_SDK_VERSION_MINOR}.${KAVIO_SDK_VERSION_PATCH}")

project(kavio_sdk
    VERSION ${KAVIO_SDK_VERSION}
    DESCRIPTION "kavio sdk project"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KAVIO_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        "$<$<CONFIG:Debug>:-O0;-g3;-ggdb;-fno-omit-frame-pointer>"
        "$<$<CONFIG:Release>:-O3;-DNDEBUG>"
    )
endif()

find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(KAVIO_SDK_DEVICE_DIR            ${CMAKE_CURRENT_LIST_DIR})
set(KAVIO_3RDPARTY_DIR              ${KAVIO_SDK_DEVICE_DIR}/3rdParty)
set(KAVIO_SDK_INC_DIR               ${KAVIO_SDK_DEVICE_DIR}/include)
set(KAVIO_LOGGING_DIR               ${KAVIO_SDK_DEVICE_DIR}/../common/logging)
set(KAVIO_SDK_SRC_DIR               ${KAVIO_SDK_DEVICE_DIR}/src)
set(KAVIO_SDK_PROTO_DIR             ${KAVIO_SDK_DEVICE_DIR}/proto)
set(KAVIO_SDK_PROTO_GEN_DIR         ${CMAKE_CURRENT_BINARY_DIR}/sdk_proto_src)
set(KAVIO_SDK_PY_DIR                ${KAVIO_SDK_DEVICE_DIR}/py)
set(KAVIO_SDK_PY_PACKAGE_DIR        ${KAVIO_SDK_PY_DIR}/kavio_sdk)
set(KAVIO_SDK_PROTO_PY_DIR          ${KAVIO_SDK_PY_PACKAGE_DIR}/proto_py)
set(KAVIO_SDK_VERSION_HEADER        ${KAVIO_SDK_INC_DIR}/kavio/KavioSdkVersion.h)
set(KAVIO_SDK_PY_VERSION_FILE       ${KAVIO_SDK_PY_PACKAGE_DIR}/sdk_version.py)
set(KAVIO_SDK_PY_VERSION_GENERATOR  ${KAVIO_SDK_DEVICE_DIR}/tools/generate_python_sdk_version.py)
set(KAVIO_PROTOBUF_DIR              ${KAVIO_3RDPARTY_DIR}/protobuf-3.21.12)
function(kavio_collect_cpp_sources out_var)
    set(all_sources)
    foreach(dir IN LISTS ARGN)
        if(NOT IS_DIRECTORY "${dir}")
            message(FATAL_ERROR "Source directory not found: ${dir}")
        endif()
        file(GLOB_RECURSE dir_sources CONFIGURE_DEPENDS
            "${dir}/*.cpp"
            "${dir}/*.cc"
            "${dir}/*.cxx"
        )
        list(APPEND all_sources ${dir_sources})
    endforeach()
    list(REMOVE_DUPLICATES all_sources)
    set(${out_var} ${all_sources} PARENT_SCOPE)
endfunction()

if(NOT EXISTS "${KAVIO_PROTOBUF_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Bundled protobuf source not found:\n  ${KAVIO_PROTOBUF_DIR}")
endif()

file(MAKE_DIRECTORY "${KAVIO_SDK_PROTO_GEN_DIR}")

set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_PROTOC ON CACHE BOOL "" FORCE)
add_subdirectory("${KAVIO_PROTOBUF_DIR}" protobuf_build EXCLUDE_FROM_ALL)

set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_PKGCONFIG_SUPPORT OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_CMAKE_PACKAGE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_OBJECT_LIBS OFF CACHE BOOL "" FORCE)
set(KAVIO_JSONCPP_DIR "${KAVIO_3RDPARTY_DIR}/jsoncpp-1.9.5")
add_subdirectory("${KAVIO_JSONCPP_DIR}" jsoncpp_build EXCLUDE_FROM_ALL)

# Bundled protobuf 3.21.12 exposes libprotobuf and protoc.
set(KAVIO_PROTOBUF_LIB_TARGET libprotobuf)
set(KAVIO_PROTOC_TARGET protoc)

file(GLOB KAVIO_SDK_PROTO_FILES CONFIGURE_DEPENDS
    "${KAVIO_SDK_PROTO_DIR}/*.proto"
)
list(SORT KAVIO_SDK_PROTO_FILES)

set(KAVIO_SDK_PROTO_GENERATED_SRCS)
set(KAVIO_SDK_PROTO_GENERATED_HDRS)
foreach(proto_file IN LISTS KAVIO_SDK_PROTO_FILES)
    get_filename_component(proto_name "${proto_file}" NAME_WE)
    list(APPEND KAVIO_SDK_PROTO_GENERATED_SRCS
        "${KAVIO_SDK_PROTO_GEN_DIR}/${proto_name}.pb.cc")
    list(APPEND KAVIO_SDK_PROTO_GENERATED_HDRS
        "${KAVIO_SDK_PROTO_GEN_DIR}/${proto_name}.pb.h")
endforeach()

add_custom_command(
    OUTPUT ${KAVIO_SDK_PROTO_GENERATED_SRCS}
           ${KAVIO_SDK_PROTO_GENERATED_HDRS}
    COMMAND $<TARGET_FILE:${KAVIO_PROTOC_TARGET}>
            --cpp_out=${KAVIO_SDK_PROTO_GEN_DIR}
            --proto_path=${KAVIO_SDK_PROTO_DIR}
            ${KAVIO_SDK_PROTO_FILES}
    DEPENDS ${KAVIO_PROTOC_TARGET}
            ${KAVIO_SDK_PROTO_FILES}
    COMMENT "Generating sdk protobuf C++ sources"
    VERBATIM
)

set(KAVIO_SDK_PROTO_GENERATED_PYS)
foreach(proto_file IN LISTS KAVIO_SDK_PROTO_FILES)
    get_filename_component(proto_name "${proto_file}" NAME_WE)
    list(APPEND KAVIO_SDK_PROTO_GENERATED_PYS
        "${KAVIO_SDK_PROTO_PY_DIR}/${proto_name}_pb2.py")
endforeach()

add_custom_command(
    OUTPUT ${KAVIO_SDK_PROTO_GENERATED_PYS}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${KAVIO_SDK_PROTO_PY_DIR}"
    COMMAND $<TARGET_FILE:${KAVIO_PROTOC_TARGET}>
            --python_out=${KAVIO_SDK_PROTO_PY_DIR}
            --proto_path=${KAVIO_SDK_PROTO_DIR}
            ${KAVIO_SDK_PROTO_FILES}
    DEPENDS ${KAVIO_PROTOC_TARGET}
            ${KAVIO_SDK_PROTO_FILES}
    COMMENT "Generating sdk protobuf Python sources"
    VERBATIM
)

add_custom_command(
    OUTPUT ${KAVIO_SDK_PY_VERSION_FILE}
    COMMAND ${Python3_EXECUTABLE}
            ${KAVIO_SDK_PY_VERSION_GENERATOR}
            --header ${KAVIO_SDK_VERSION_HEADER}
            --output ${KAVIO_SDK_PY_VERSION_FILE}
    DEPENDS ${KAVIO_SDK_VERSION_HEADER}
            ${KAVIO_SDK_PY_VERSION_GENERATOR}
    COMMENT "Generating Python SDK version module"
    VERBATIM
)

add_custom_target(kavio_sdk_python_version
    DEPENDS ${KAVIO_SDK_PY_VERSION_FILE}
)

add_custom_target(kavio_sdk_proto_py
    DEPENDS ${KAVIO_SDK_PROTO_GENERATED_PYS}
)
add_dependencies(kavio_sdk_proto_py kavio_sdk_python_version)

add_library(kavio_sdk_proto STATIC
    ${KAVIO_SDK_PROTO_GENERATED_SRCS}
    ${KAVIO_SDK_PROTO_GENERATED_HDRS}
)

target_include_directories(kavio_sdk_proto
    PUBLIC
        "${KAVIO_SDK_PROTO_GEN_DIR}"
        "${KAVIO_PROTOBUF_DIR}/src"
)

target_link_libraries(kavio_sdk_proto
    PUBLIC
        ${KAVIO_PROTOBUF_LIB_TARGET}
)

add_library(kavio_sdk_shared STATIC
    "${KAVIO_SDK_SRC_DIR}/internal/ShmBase.cpp"
    "${KAVIO_SDK_SRC_DIR}/internal/FrameShm.cpp"
)

target_include_directories(kavio_sdk_shared
    PUBLIC
        "${KAVIO_SDK_INC_DIR}"
    PRIVATE
        "${KAVIO_SDK_SRC_DIR}"
)

target_link_libraries(kavio_sdk_shared
    PUBLIC
        kavio_sdk_proto
        rt
)

kavio_collect_cpp_sources(KAVIO_SDK_SOURCES ${KAVIO_SDK_SRC_DIR})
list(REMOVE_ITEM KAVIO_SDK_SOURCES
    "${KAVIO_SDK_SRC_DIR}/internal/ShmBase.cpp"
    "${KAVIO_SDK_SRC_DIR}/internal/FrameShm.cpp"
)

add_library(kavio_sdk STATIC ${KAVIO_SDK_SOURCES})

target_include_directories(kavio_sdk
    PUBLIC
        "${KAVIO_SDK_INC_DIR}"
        "${KAVIO_LOGGING_DIR}"
    PRIVATE
        "${KAVIO_SDK_SRC_DIR}"
)

target_link_libraries(kavio_sdk
    PUBLIC
    kavio_sdk_shared
        Threads::Threads
        rt
        jsoncpp_static
)

target_compile_options(kavio_sdk
    PRIVATE
        -Wall
        -Wno-deprecated-declarations
        -frtti
)

if(KAVIO_WARNINGS_AS_ERRORS)
    target_compile_options(kavio_sdk PRIVATE -Werror)
endif()

set_target_properties(kavio_sdk PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)


# Optional LVGL adapter for full-screen AppView rendering. The core Device SDK
# remains buildable without LVGL; AppView does not use TX Overlay surfaces.
option(KAVIO_BUILD_LVGL_APPVIEW_ADAPTER
    "Build the optional LVGL 9.4 AppView adapter when LVGL source/config are available" ON)

set(_KAVIO_LVGL_DEFAULT_ROOT "${KAVIO_SDK_DEVICE_DIR}/3rdParty/lvgl-9.4.0")
if(NOT EXISTS "${_KAVIO_LVGL_DEFAULT_ROOT}/CMakeLists.txt" AND
   EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../kavio_platform/3rdParty/lvgl-9.4.0/CMakeLists.txt")
    set(_KAVIO_LVGL_DEFAULT_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/../../../kavio_platform/3rdParty/lvgl-9.4.0")
endif()
# Keep the Device SDK independent from Platform source directories.
# AppView's optional LVGL adapter uses the SDK-local FreeType dependency only.
set(_KAVIO_FREETYPE_DEFAULT_ROOT "${KAVIO_SDK_DEVICE_DIR}/3rdParty/freetype-2.10.0")
if(NOT DEFINED KAVIO_FREETYPE_ROOT OR KAVIO_FREETYPE_ROOT STREQUAL "")
    set(KAVIO_FREETYPE_ROOT "${_KAVIO_FREETYPE_DEFAULT_ROOT}" CACHE PATH
        "Path to bundled FreeType source used by LVGL AppView" FORCE)
else()
    set(KAVIO_FREETYPE_ROOT "${KAVIO_FREETYPE_ROOT}" CACHE PATH
        "Path to bundled FreeType source used by LVGL AppView")
endif()
if(NOT DEFINED KAVIO_LVGL_ROOT OR KAVIO_LVGL_ROOT STREQUAL "")
    set(KAVIO_LVGL_ROOT "${_KAVIO_LVGL_DEFAULT_ROOT}" CACHE PATH
        "Path to LVGL 9.4.0 source used by the AppView adapter" FORCE)
else()
    set(KAVIO_LVGL_ROOT "${KAVIO_LVGL_ROOT}" CACHE PATH
        "Path to LVGL 9.4.0 source used by the AppView adapter")
endif()

# Prefer the SDK-owned LVGL configuration; do not silently inherit Platform config.
set(_KAVIO_LV_CONF_DEFAULT "${KAVIO_SDK_DEVICE_DIR}/lv_conf.h")
if(NOT DEFINED KAVIO_LV_CONF_PATH OR KAVIO_LV_CONF_PATH STREQUAL "")
    set(KAVIO_LV_CONF_PATH "${_KAVIO_LV_CONF_DEFAULT}" CACHE FILEPATH
        "LVGL configuration used by the AppView adapter" FORCE)
else()
    set(KAVIO_LV_CONF_PATH "${KAVIO_LV_CONF_PATH}" CACHE FILEPATH
        "LVGL configuration used by the AppView adapter")
endif()

if(KAVIO_BUILD_LVGL_APPVIEW_ADAPTER AND
   EXISTS "${KAVIO_LVGL_ROOT}/CMakeLists.txt" AND
   EXISTS "${KAVIO_LV_CONF_PATH}" AND
   EXISTS "${KAVIO_FREETYPE_ROOT}/CMakeLists.txt")
    get_filename_component(_KAVIO_LV_CONF_DIR "${KAVIO_LV_CONF_PATH}" DIRECTORY)
    set(LV_CONF_PATH "${KAVIO_LV_CONF_PATH}" CACHE FILEPATH "LVGL lv_conf.h path" FORCE)
    # This SDK uses LVGL as a library only. Do not configure examples, demos,
    # or LVGL's own tests into the normal SDK build, including stale cache values.
    set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "Build LVGL examples" FORCE)
    set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "Build LVGL demos" FORCE)
    set(LV_BUILD_TESTS OFF CACHE BOOL "Build LVGL tests" FORCE)

    # FreeType 2.10.0 treats FT_WITH_* = OFF as "optional", not "disabled";
    # it still auto-detects installed PNG/ZLIB/BZip2/HarfBuzz packages. That
    # can compile optional code into libfreetype without exporting those static
    # link dependencies to consuming Apps. Disable package discovery only while
    # configuring FreeType; AppView only needs ordinary outline-font rendering.
    set(FT_WITH_ZLIB OFF CACHE BOOL "Require system zlib for FreeType" FORCE)
    set(FT_WITH_BZIP2 OFF CACHE BOOL "Require bzip2 for FreeType" FORCE)
    set(FT_WITH_PNG OFF CACHE BOOL "Require PNG for FreeType" FORCE)
    set(FT_WITH_HARFBUZZ OFF CACHE BOOL "Require HarfBuzz for FreeType" FORCE)
    set(CMAKE_DISABLE_FIND_PACKAGE_ZLIB TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_PNG TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_BZip2 TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_HarfBuzz TRUE)
    if(NOT TARGET freetype AND NOT TARGET Freetype::Freetype)
        add_subdirectory("${KAVIO_FREETYPE_ROOT}" freetype_appview_build EXCLUDE_FROM_ALL)
    endif()
    unset(CMAKE_DISABLE_FIND_PACKAGE_ZLIB)
    unset(CMAKE_DISABLE_FIND_PACKAGE_PNG)
    unset(CMAKE_DISABLE_FIND_PACKAGE_BZip2)
    unset(CMAKE_DISABLE_FIND_PACKAGE_HarfBuzz)
    if(TARGET freetype)
        set(_KAVIO_FREETYPE_TARGET freetype)
    elseif(TARGET Freetype::Freetype)
        set(_KAVIO_FREETYPE_TARGET Freetype::Freetype)
    else()
        message(FATAL_ERROR "FreeType source was found, but no freetype target was created")
    endif()

    if(NOT TARGET lvgl AND NOT TARGET lvgl::lvgl)
        add_subdirectory("${KAVIO_LVGL_ROOT}" lvgl_appview_build EXCLUDE_FROM_ALL)
    endif()
    if(TARGET lvgl::lvgl)
        set(_KAVIO_LVGL_TARGET lvgl::lvgl)
    elseif(TARGET lvgl)
        set(_KAVIO_LVGL_TARGET lvgl)
    else()
        message(FATAL_ERROR "LVGL source was found, but no lvgl target was created")
    endif()
    if(TARGET lvgl)
        target_include_directories(lvgl PUBLIC
            "${_KAVIO_LV_CONF_DIR}"
            "${KAVIO_LVGL_ROOT}/src/libs/freetype")
        target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
        target_link_libraries(lvgl PUBLIC ${_KAVIO_FREETYPE_TARGET})
    endif()

    add_library(kavio_sdk_lvgl_appview STATIC
        "${KAVIO_SDK_DEVICE_DIR}/lvgl_app_view/LvglAppViewAdapter.cpp")
    target_include_directories(kavio_sdk_lvgl_appview
        PUBLIC
            "${KAVIO_SDK_INC_DIR}"
            "${KAVIO_LVGL_ROOT}/src"
            "${KAVIO_LVGL_ROOT}/src/libs/freetype"
            "${KAVIO_FREETYPE_ROOT}/include"
            "${_KAVIO_LV_CONF_DIR}")
    target_compile_definitions(kavio_sdk_lvgl_appview PUBLIC LV_CONF_INCLUDE_SIMPLE)
    target_link_libraries(kavio_sdk_lvgl_appview PUBLIC kavio_sdk ${_KAVIO_LVGL_TARGET} ${_KAVIO_FREETYPE_TARGET})
    target_compile_options(kavio_sdk_lvgl_appview PRIVATE -Wall -Wno-deprecated-declarations -frtti)
    if(KAVIO_WARNINGS_AS_ERRORS)
        target_compile_options(kavio_sdk_lvgl_appview PRIVATE -Werror)
    endif()
else()
    message(STATUS
        "Skipping optional kavio_sdk_lvgl_appview target: LVGL 9.4 source, lv_conf.h, or FreeType source not found. "
        "Set KAVIO_LVGL_ROOT, KAVIO_LV_CONF_PATH, and KAVIO_FREETYPE_ROOT to enable it.")
endif()

# Generate the build-tree CMake package consumed by Native Apps.
#
# Important: exported include directories must only contain headers that are
# actually part of the SDK consumption surface. protobuf_build/* is an
# internal CMake build directory and must never leak into Kavio::DeviceSdk.
# Native Apps need only:
#   1. public Kavio SDK headers;
#   2. generated SDK protobuf headers;
#   3. protobuf public headers from the bundled 3rdParty source tree.
set(KAVIO_DEVICE_SDK_CONFIG_FILE "${CMAKE_BINARY_DIR}/KavioDeviceSdkConfig.cmake")
set(KAVIO_DEVICE_SDK_CONFIG_GENERATOR
    "${KAVIO_SDK_DEVICE_DIR}/tools/GenerateCppSdkConfig.cmake")

set(_KAVIO_SDK_PACKAGE_OPTIONAL_ARGS)
set(_KAVIO_SDK_PACKAGE_OPTIONAL_DEPS)
if(TARGET kavio_sdk_lvgl_appview)
    list(APPEND _KAVIO_SDK_PACKAGE_OPTIONAL_ARGS
        "-DKAVIO_LVGL_APPVIEW_LIBRARY=$<TARGET_FILE:kavio_sdk_lvgl_appview>"
        "-DKAVIO_LVGL_LIBRARY=$<TARGET_FILE:${_KAVIO_LVGL_TARGET}>"
        "-DKAVIO_LVGL_INCLUDE_DIR=${KAVIO_LVGL_ROOT}/src"
        "-DKAVIO_LV_CONF_DIR=${_KAVIO_LV_CONF_DIR}"
        "-DKAVIO_FREETYPE_LIBRARY=$<TARGET_FILE:${_KAVIO_FREETYPE_TARGET}>"
        "-DKAVIO_FREETYPE_INCLUDE_DIR=${KAVIO_FREETYPE_ROOT}/include")
    list(APPEND _KAVIO_SDK_PACKAGE_OPTIONAL_DEPS
        kavio_sdk_lvgl_appview ${_KAVIO_LVGL_TARGET} ${_KAVIO_FREETYPE_TARGET})
endif()

add_custom_target(kavio_device_sdk_package
    COMMAND ${CMAKE_COMMAND}
        "-DKAVIO_CONFIG_OUTPUT=${KAVIO_DEVICE_SDK_CONFIG_FILE}"
        "-DKAVIO_MAIN_LIBRARY=$<TARGET_FILE:kavio_sdk>"
        "-DKAVIO_SHARED_LIBRARY=$<TARGET_FILE:kavio_sdk_shared>"
        "-DKAVIO_PROTO_LIBRARY=$<TARGET_FILE:kavio_sdk_proto>"
        "-DKAVIO_PROTOBUF_LIBRARY=$<TARGET_FILE:${KAVIO_PROTOBUF_LIB_TARGET}>"
        "-DKAVIO_PROTOC_EXECUTABLE=$<TARGET_FILE:${KAVIO_PROTOC_TARGET}>"
        "-DKAVIO_JSONCPP_LIBRARY=$<TARGET_FILE:jsoncpp_static>"
        "-DKAVIO_JSONCPP_INCLUDE_DIR=${KAVIO_JSONCPP_DIR}/include"
        "-DKAVIO_INCLUDE_DIR=${KAVIO_SDK_INC_DIR}"
        "-DKAVIO_LOGGING_DIR=${KAVIO_LOGGING_DIR}"
        "-DKAVIO_PROTO_INCLUDE_DIR=${KAVIO_SDK_PROTO_GEN_DIR}"
        "-DKAVIO_PROTOBUF_SOURCE_INCLUDE_DIR=${KAVIO_PROTOBUF_DIR}/src"
        ${_KAVIO_SDK_PACKAGE_OPTIONAL_ARGS}
        -P "${KAVIO_DEVICE_SDK_CONFIG_GENERATOR}"
    DEPENDS
        kavio_sdk
        kavio_sdk_proto_py
        ${_KAVIO_SDK_PACKAGE_OPTIONAL_DEPS}
        "${KAVIO_DEVICE_SDK_CONFIG_GENERATOR}"
    COMMENT "Generating KavioDeviceSdkConfig.cmake for Native Apps"
    VERBATIM
)
add_dependencies(kavio_device_sdk_package
    kavio_sdk
    kavio_sdk_proto_py
    ${KAVIO_PROTOC_TARGET})
