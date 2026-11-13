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

# The Device SDK only uses the explicitly vendored third-party source trees.
set(KAVIO_SDK_DEVICE_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(KAVIO_3RDPARTY_DIR "${KAVIO_SDK_DEVICE_DIR}/3rdParty")
set(KAVIO_SDK_INC_DIR "${KAVIO_SDK_DEVICE_DIR}/include")
set(KAVIO_LOGGING_DIR "${KAVIO_SDK_DEVICE_DIR}/../common/logging")
set(KAVIO_SDK_SRC_DIR "${KAVIO_SDK_DEVICE_DIR}/src")
set(KAVIO_SDK_PROTO_DIR "${KAVIO_SDK_DEVICE_DIR}/proto")
set(KAVIO_SDK_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/sdk_proto_src")
set(KAVIO_SDK_VERSION_HEADER "${KAVIO_SDK_INC_DIR}/kavio/KavioSdkVersion.h")
set(KAVIO_PROTOBUF_DIR "${KAVIO_3RDPARTY_DIR}/protobuf-3.21.12")
set(KAVIO_JSONCPP_DIR "${KAVIO_3RDPARTY_DIR}/jsoncpp-1.9.5")
set(KAVIO_LUA_SOURCE_DIR "${KAVIO_3RDPARTY_DIR}/lua-5.4.1")
set(KAVIO_LVGL_ROOT "${KAVIO_3RDPARTY_DIR}/lvgl-9.4.0")
set(KAVIO_FREETYPE_ROOT "${KAVIO_3RDPARTY_DIR}/freetype-2.10.0")
set(KAVIO_LV_CONF_PATH "${KAVIO_SDK_DEVICE_DIR}/lv_conf.h")

function(kavio_require_files label)
    foreach(required_file IN LISTS ARGN)
        if(NOT EXISTS "${required_file}")
            message(FATAL_ERROR "Missing ${label} dependency file: ${required_file}")
        endif()
    endforeach()
endfunction()

kavio_require_files("protobuf 3.21.12"
    "${KAVIO_PROTOBUF_DIR}/CMakeLists.txt"
    "${KAVIO_PROTOBUF_DIR}/src/google/protobuf/message.h")
kavio_require_files("JsonCpp 1.9.5"
    "${KAVIO_JSONCPP_DIR}/CMakeLists.txt"
    "${KAVIO_JSONCPP_DIR}/include/json/json.h")

kavio_require_files("Lua 5.4.1"
    "${KAVIO_LUA_SOURCE_DIR}/lua.h"
    "${KAVIO_LUA_SOURCE_DIR}/lapi.c"
    "${KAVIO_LUA_SOURCE_DIR}/lauxlib.c")

kavio_require_files("LVGL 9.4.0 / FreeType 2.10.0"
    "${KAVIO_LVGL_ROOT}/CMakeLists.txt"
    "${KAVIO_LVGL_ROOT}/lvgl.h"
    "${KAVIO_FREETYPE_ROOT}/CMakeLists.txt"
    "${KAVIO_FREETYPE_ROOT}/include/freetype/freetype.h"
    "${KAVIO_LV_CONF_PATH}")

find_package(Threads REQUIRED)
# The SDK native extension is built for the VS680 system CPython 3.8 ABI.
# Do not silently bind to another Python interpreter / development version.
set(Python3_EXECUTABLE "/usr/bin/python3.8" CACHE FILEPATH
    "Device SDK Python 3.8 interpreter" FORCE)
if(NOT EXISTS "${Python3_EXECUTABLE}")
    message(FATAL_ERROR "Required system Python interpreter not found: ${Python3_EXECUTABLE}")
endif()
# Use the exact target-system headers and ABI library, not another
# Python 3.8 installation found in /usr/local or a virtual environment.
set(Python3_INCLUDE_DIR "/usr/include/python3.8" CACHE PATH
    "VS680 Python 3.8 development headers" FORCE)
set(Python3_LIBRARY "/usr/lib/aarch64-linux-gnu/libpython3.8.so" CACHE FILEPATH
    "VS680 Python 3.8 ABI library" FORCE)
kavio_require_files("system Python 3.8 development"
    "${Python3_INCLUDE_DIR}/Python.h"
    "${Python3_LIBRARY}")
if(CMAKE_VERSION VERSION_LESS "3.18")
    find_package(Python3 3.8 EXACT COMPONENTS Interpreter Development REQUIRED)
else()
    find_package(Python3 3.8 EXACT COMPONENTS Interpreter Development.Module REQUIRED)
endif()
if(NOT Python3_VERSION MATCHES "^3\.8(\.|$)")
    message(FATAL_ERROR "Device SDK requires Python 3.8, found ${Python3_VERSION}")
endif()

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
# Protobuf 3.21.12 uses protobuf_BUILD_PROTOC_BINARIES. Do not rely on the
# upstream default: the SDK always builds and exports the exact bundled protoc
# so App-side proto generation uses the same version as the SDK runtime.
set(protobuf_BUILD_PROTOC_BINARIES ON CACHE BOOL "" FORCE)
set(protobuf_BUILD_LIBPROTOC ON CACHE BOOL "" FORCE)
add_subdirectory("${KAVIO_PROTOBUF_DIR}" protobuf_build EXCLUDE_FROM_ALL)

# v21.12's official cmake/protoc.cmake adds compiler/main.cc directly to the
# protoc target. Validate that invariant here so a modified/corrupt vendored
# CMake tree fails during configure instead of much later with an undefined
# reference to main at link time.
if(NOT TARGET protoc)
    message(FATAL_ERROR
        "Bundled protobuf 3.21.12 did not create the protoc target. "
        "Check ${KAVIO_PROTOBUF_DIR}/CMakeLists.txt and protobuf_BUILD_PROTOC_BINARIES.")
endif()
set(_KAVIO_PROTOC_MAIN
    "${KAVIO_PROTOBUF_DIR}/src/google/protobuf/compiler/main.cc")
if(NOT EXISTS "${_KAVIO_PROTOC_MAIN}")
    message(FATAL_ERROR
        "Bundled protobuf protoc entry point is missing:\n  ${_KAVIO_PROTOC_MAIN}")
endif()
get_target_property(_KAVIO_PROTOC_SOURCES protoc SOURCES)
set(_KAVIO_PROTOC_HAS_MAIN FALSE)
foreach(_kavio_protoc_source IN LISTS _KAVIO_PROTOC_SOURCES)
    if(_kavio_protoc_source STREQUAL "${_KAVIO_PROTOC_MAIN}" OR
       _kavio_protoc_source MATCHES "google/protobuf/compiler/main\\.cc$")
        set(_KAVIO_PROTOC_HAS_MAIN TRUE)
        break()
    endif()
endforeach()
if(NOT _KAVIO_PROTOC_HAS_MAIN)
    message(FATAL_ERROR
        "Bundled protobuf protoc target is malformed: compiler/main.cc is not "
        "one of its sources. Expected the official protobuf v21.12 target.\n"
        "protoc SOURCES = ${_KAVIO_PROTOC_SOURCES}\n"
        "Inspect: ${KAVIO_PROTOBUF_DIR}/cmake/protoc.cmake")
endif()
unset(_KAVIO_PROTOC_HAS_MAIN)
unset(_KAVIO_PROTOC_SOURCES)
unset(_KAVIO_PROTOC_MAIN)
unset(_kavio_protoc_source)

set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_PKGCONFIG_SUPPORT OFF CACHE BOOL "" FORCE)
set(JSONCPP_WITH_CMAKE_PACKAGE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_OBJECT_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory("${KAVIO_JSONCPP_DIR}" jsoncpp_build EXCLUDE_FROM_ALL)
set_target_properties(jsoncpp_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

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
set_target_properties(kavio_sdk_proto PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(${KAVIO_PROTOBUF_LIB_TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON)

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
set_target_properties(kavio_sdk_shared PROPERTIES POSITION_INDEPENDENT_CODE ON)

kavio_collect_cpp_sources(KAVIO_SDK_SOURCES ${KAVIO_SDK_SRC_DIR})
list(REMOVE_ITEM KAVIO_SDK_SOURCES
    "${KAVIO_SDK_SRC_DIR}/internal/ShmBase.cpp"
    "${KAVIO_SDK_SRC_DIR}/internal/FrameShm.cpp"
    "${KAVIO_SDK_SRC_DIR}/App.cpp"
    "${KAVIO_SDK_SRC_DIR}/AppContext.cpp"
)
list(FILTER KAVIO_SDK_SOURCES EXCLUDE REGEX "/runtime/")
list(FILTER KAVIO_SDK_SOURCES EXCLUDE REGEX "/presentation/")
list(FILTER KAVIO_SDK_SOURCES EXCLUDE REGEX "/src/device/")
list(FILTER KAVIO_SDK_SOURCES EXCLUDE REGEX "/ui/")
list(FILTER KAVIO_SDK_SOURCES EXCLUDE REGEX "/c_api/")

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
    POSITION_INDEPENDENT_CODE ON
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# High-level KavioApp runtime. It owns the Device Session, AppView Presentation
# and App-thread event loop, but intentionally has no LVGL dependency.
set(KAVIO_APP_RUNTIME_SOURCES
    "${KAVIO_SDK_SRC_DIR}/App.cpp"
    "${KAVIO_SDK_SRC_DIR}/AppContext.cpp"
    "${KAVIO_SDK_SRC_DIR}/device/Device.cpp"
    "${KAVIO_SDK_SRC_DIR}/runtime/AppRuntime.cpp"
    "${KAVIO_SDK_SRC_DIR}/runtime/DeviceRuntime.cpp"
    "${KAVIO_SDK_SRC_DIR}/runtime/EventRuntime.cpp"
    "${KAVIO_SDK_SRC_DIR}/presentation/FrameBuffer.cpp"
    "${KAVIO_SDK_SRC_DIR}/presentation/Presentation.cpp"
    "${KAVIO_SDK_SRC_DIR}/presentation/PresentationRuntime.cpp"
    "${KAVIO_SDK_SRC_DIR}/c_api/KavioC.cpp"
)
add_library(kavio_app_runtime SHARED ${KAVIO_APP_RUNTIME_SOURCES})
target_include_directories(kavio_app_runtime
    PUBLIC
        "${KAVIO_SDK_INC_DIR}"
    PRIVATE
        "${KAVIO_SDK_SRC_DIR}"
)
target_link_libraries(kavio_app_runtime
    PRIVATE
        kavio_sdk
        Threads::Threads
        rt
)
target_compile_definitions(kavio_app_runtime PRIVATE KAVIO_APP_RUNTIME_BUILD)
target_compile_options(kavio_app_runtime PRIVATE -Wall -Wextra -Wno-deprecated-declarations -frtti)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(kavio_app_runtime PRIVATE "-Wl,--exclude-libs,ALL")
endif()
if(KAVIO_WARNINGS_AS_ERRORS)
    target_compile_options(kavio_app_runtime PRIVATE -Werror)
endif()
set_target_properties(kavio_app_runtime PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    OUTPUT_NAME kavio_app_runtime
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)


# Backend-independent retained Widget model. This library has no LVGL dependency.
add_library(kavio_ui_core SHARED
    "${KAVIO_SDK_SRC_DIR}/ui/Widget.cpp"
    "${KAVIO_SDK_SRC_DIR}/ui/WidgetC.cpp")
target_include_directories(kavio_ui_core
    PUBLIC "${KAVIO_SDK_INC_DIR}"
    PRIVATE "${KAVIO_SDK_SRC_DIR}")
target_compile_definitions(kavio_ui_core PRIVATE KAVIO_UI_CORE_BUILD)
target_compile_options(kavio_ui_core PRIVATE -Wall -Wextra -Wno-deprecated-declarations -frtti)
set_target_properties(kavio_ui_core PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    BUILD_RPATH "$ORIGIN"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
if(KAVIO_WARNINGS_AS_ERRORS)
    target_compile_options(kavio_ui_core PRIVATE -Werror)
endif()

get_filename_component(_KAVIO_LV_CONF_DIR "${KAVIO_LV_CONF_PATH}" DIRECTORY)
set(LV_CONF_PATH "${KAVIO_LV_CONF_PATH}" CACHE FILEPATH "LVGL lv_conf.h path" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "Build LVGL examples" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "Build LVGL demos" FORCE)
set(LV_BUILD_TESTS OFF CACHE BOOL "Build LVGL tests" FORCE)

# The SDK only needs FreeType outline-font rendering. Keep optional external
# image/compression/shaping dependencies out of the SDK link interface.
set(FT_WITH_ZLIB OFF CACHE BOOL "Require system zlib for FreeType" FORCE)
set(FT_WITH_BZIP2 OFF CACHE BOOL "Require bzip2 for FreeType" FORCE)
set(FT_WITH_PNG OFF CACHE BOOL "Require PNG for FreeType" FORCE)
set(FT_WITH_HARFBUZZ OFF CACHE BOOL "Require HarfBuzz for FreeType" FORCE)
set(CMAKE_DISABLE_FIND_PACKAGE_ZLIB TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_PNG TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_BZip2 TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_HarfBuzz TRUE)
add_subdirectory("${KAVIO_FREETYPE_ROOT}" freetype_kavio_ui_build EXCLUDE_FROM_ALL)
unset(CMAKE_DISABLE_FIND_PACKAGE_ZLIB)
unset(CMAKE_DISABLE_FIND_PACKAGE_PNG)
unset(CMAKE_DISABLE_FIND_PACKAGE_BZip2)
unset(CMAKE_DISABLE_FIND_PACKAGE_HarfBuzz)
if(NOT TARGET freetype)
    message(FATAL_ERROR "Bundled FreeType 2.10.0 must provide the freetype target")
endif()
set_target_properties(freetype PROPERTIES POSITION_INDEPENDENT_CODE ON)
set(_KAVIO_FREETYPE_TARGET freetype)

add_subdirectory("${KAVIO_LVGL_ROOT}" lvgl_kavio_ui_build EXCLUDE_FROM_ALL)
if(NOT TARGET lvgl)
    message(FATAL_ERROR "Bundled LVGL 9.4.0 must provide the lvgl target")
endif()
set(_KAVIO_LVGL_TARGET lvgl)

set_target_properties(lvgl PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(TARGET lvgl_thorvg)
    set_target_properties(lvgl_thorvg PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

target_include_directories(lvgl PUBLIC
    "${_KAVIO_LV_CONF_DIR}"
    "${KAVIO_LVGL_ROOT}/src/libs/freetype")
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_link_libraries(lvgl PUBLIC ${_KAVIO_FREETYPE_TARGET})

add_library(kavio_ui_lvgl SHARED
    "${KAVIO_SDK_SRC_DIR}/ui/LvglBackend.cpp"
    "${KAVIO_SDK_SRC_DIR}/ui/LvglC.cpp")
target_include_directories(kavio_ui_lvgl
    PUBLIC
        "${KAVIO_SDK_INC_DIR}"
        "${KAVIO_LVGL_ROOT}/src"
        "${KAVIO_LVGL_ROOT}/src/libs/freetype"
        "${KAVIO_FREETYPE_ROOT}/include"
        "${_KAVIO_LV_CONF_DIR}"
    PRIVATE
        "${KAVIO_SDK_SRC_DIR}")
target_compile_definitions(kavio_ui_lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE PRIVATE KAVIO_UI_LVGL_BUILD)
target_link_libraries(kavio_ui_lvgl PRIVATE kavio_ui_core kavio_app_runtime)
# Kavio::UI::LVGL exposes a native C++ LVGL extension point. Embed the
# complete static LVGL archive in the backend so consumers use the same
# LVGL engine/state as the backend instead of linking a second copy.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_libraries(kavio_ui_lvgl PRIVATE
        "-Wl,--whole-archive" ${_KAVIO_LVGL_TARGET} "-Wl,--no-whole-archive"
        ${_KAVIO_FREETYPE_TARGET})
else()
    target_link_libraries(kavio_ui_lvgl PRIVATE ${_KAVIO_LVGL_TARGET} ${_KAVIO_FREETYPE_TARGET})
endif()
target_compile_options(kavio_ui_lvgl PRIVATE -Wall -Wextra -Wno-deprecated-declarations -frtti)
set_target_properties(kavio_ui_lvgl PROPERTIES
    C_VISIBILITY_PRESET default
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    BUILD_RPATH "$ORIGIN"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
if(KAVIO_WARNINGS_AS_ERRORS)
    target_compile_options(kavio_ui_lvgl PRIVATE -Werror)
endif()

# High-level Python binding. The core extension only depends on the language-neutral
# App Runtime C ABI. The LVGL extension is packaged separately so framebuffer-only
# Python Apps do not carry or load the Widget backend.
set(KAVIO_PYTHON_BINDING_SOURCE_DIR
    "${KAVIO_SDK_DEVICE_DIR}/bindings/python/package/kavio")
set(KAVIO_PYTHON_CORE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/python/core/kavio")
set(KAVIO_PYTHON_UI_OUTPUT_DIR "${CMAKE_BINARY_DIR}/python/ui/kavio")

add_library(kavio_python_native MODULE
    "${KAVIO_SDK_DEVICE_DIR}/bindings/python/native/kavio_native.cpp")
target_include_directories(kavio_python_native PRIVATE "${KAVIO_SDK_INC_DIR}")
target_link_libraries(kavio_python_native PRIVATE Python3::Module kavio_app_runtime)
target_compile_options(kavio_python_native PRIVATE
    -Wall -Wextra -Wno-deprecated-declarations
    -Wno-missing-field-initializers -Wno-cast-function-type)
if(KAVIO_WARNINGS_AS_ERRORS)
    target_compile_options(kavio_python_native PRIVATE -Werror)
endif()
set_target_properties(kavio_python_native PROPERTIES
    PREFIX ""
    OUTPUT_NAME "_native"
    LIBRARY_OUTPUT_DIRECTORY "${KAVIO_PYTHON_CORE_OUTPUT_DIR}"
    BUILD_RPATH "$ORIGIN/../../../lib;$ORIGIN/../lib")

add_custom_target(kavio_python_package
    COMMAND ${CMAKE_COMMAND} -E make_directory "${KAVIO_PYTHON_CORE_OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${KAVIO_PYTHON_BINDING_SOURCE_DIR}"
        "${KAVIO_PYTHON_CORE_OUTPUT_DIR}"
    DEPENDS kavio_python_native
    COMMENT "Staging high-level Kavio Python core package"
    VERBATIM)

add_library(kavio_python_ui_native MODULE
    "${KAVIO_SDK_DEVICE_DIR}/bindings/python/native/kavio_ui_native.cpp")
target_include_directories(kavio_python_ui_native PRIVATE "${KAVIO_SDK_INC_DIR}")
target_link_libraries(kavio_python_ui_native PRIVATE
    Python3::Module kavio_ui_core kavio_ui_lvgl)
target_compile_options(kavio_python_ui_native PRIVATE
    -Wall -Wextra -Wno-deprecated-declarations
    -Wno-missing-field-initializers -Wno-cast-function-type)
if(KAVIO_WARNINGS_AS_ERRORS)
    target_compile_options(kavio_python_ui_native PRIVATE -Werror)
endif()
set_target_properties(kavio_python_ui_native PROPERTIES
    PREFIX ""
    OUTPUT_NAME "_ui_native"
    LIBRARY_OUTPUT_DIRECTORY "${KAVIO_PYTHON_UI_OUTPUT_DIR}"
    BUILD_RPATH "$ORIGIN/../../../lib;$ORIGIN/../lib")
add_custom_target(kavio_python_ui_package DEPENDS kavio_python_ui_native)

# Lua 5.4 runtime and high-level binding. Lua Apps are packaged with the
# SDK-owned native runner, so the device App host continues to launch a native
# entry while the App implementation itself is written in Lua.
set(KAVIO_LUA_C_SOURCES
    "${KAVIO_LUA_SOURCE_DIR}/lapi.c"
    "${KAVIO_LUA_SOURCE_DIR}/lauxlib.c"
    "${KAVIO_LUA_SOURCE_DIR}/lbaselib.c"
    "${KAVIO_LUA_SOURCE_DIR}/lcode.c"
    "${KAVIO_LUA_SOURCE_DIR}/lcorolib.c"
    "${KAVIO_LUA_SOURCE_DIR}/lctype.c"
    "${KAVIO_LUA_SOURCE_DIR}/ldblib.c"
    "${KAVIO_LUA_SOURCE_DIR}/ldebug.c"
    "${KAVIO_LUA_SOURCE_DIR}/ldo.c"
    "${KAVIO_LUA_SOURCE_DIR}/ldump.c"
    "${KAVIO_LUA_SOURCE_DIR}/lfunc.c"
    "${KAVIO_LUA_SOURCE_DIR}/lgc.c"
    "${KAVIO_LUA_SOURCE_DIR}/linit.c"
    "${KAVIO_LUA_SOURCE_DIR}/liolib.c"
    "${KAVIO_LUA_SOURCE_DIR}/llex.c"
    "${KAVIO_LUA_SOURCE_DIR}/lmathlib.c"
    "${KAVIO_LUA_SOURCE_DIR}/lmem.c"
    "${KAVIO_LUA_SOURCE_DIR}/loadlib.c"
    "${KAVIO_LUA_SOURCE_DIR}/lobject.c"
    "${KAVIO_LUA_SOURCE_DIR}/lopcodes.c"
    "${KAVIO_LUA_SOURCE_DIR}/loslib.c"
    "${KAVIO_LUA_SOURCE_DIR}/lparser.c"
    "${KAVIO_LUA_SOURCE_DIR}/lstate.c"
    "${KAVIO_LUA_SOURCE_DIR}/lstring.c"
    "${KAVIO_LUA_SOURCE_DIR}/lstrlib.c"
    "${KAVIO_LUA_SOURCE_DIR}/ltable.c"
    "${KAVIO_LUA_SOURCE_DIR}/ltablib.c"
    "${KAVIO_LUA_SOURCE_DIR}/ltm.c"
    "${KAVIO_LUA_SOURCE_DIR}/lundump.c"
    "${KAVIO_LUA_SOURCE_DIR}/lutf8lib.c"
    "${KAVIO_LUA_SOURCE_DIR}/lvm.c"
    "${KAVIO_LUA_SOURCE_DIR}/lzio.c"
)
kavio_require_files("Lua 5.4.1 library sources" ${KAVIO_LUA_C_SOURCES})

add_library(kavio_lua_runtime SHARED ${KAVIO_LUA_C_SOURCES})
target_include_directories(kavio_lua_runtime PUBLIC "${KAVIO_LUA_SOURCE_DIR}")
target_compile_definitions(kavio_lua_runtime PRIVATE LUA_USE_LINUX)
target_link_libraries(kavio_lua_runtime PRIVATE dl m)
set_target_properties(kavio_lua_runtime PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME kavio_lua
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

set(KAVIO_LUA_NATIVE_DIR "${KAVIO_SDK_DEVICE_DIR}/bindings/lua/native")
set(KAVIO_LUA_PACKAGE_SOURCE_DIR "${KAVIO_SDK_DEVICE_DIR}/bindings/lua/package/kavio")
set(KAVIO_LUA_RUNTIME_OUTPUT_DIR "${CMAKE_BINARY_DIR}/lua/runtime/device")
set(KAVIO_LUA_CORE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/lua/core/kavio")
set(KAVIO_LUA_UI_OUTPUT_DIR "${CMAKE_BINARY_DIR}/lua/ui/kavio")

add_executable(kavio_lua_runner "${KAVIO_LUA_NATIVE_DIR}/kavio_lua_runner.cpp")
target_include_directories(kavio_lua_runner PRIVATE "${KAVIO_LUA_SOURCE_DIR}")
# On glibc 2.31, libstdc++'s std::thread checks weak pthread symbols.
# The runner loads its SDK runtime through a Lua module; without a direct
# DT_NEEDED entry for libpthread, thread creation reports EPERM.
target_link_libraries(kavio_lua_runner PRIVATE kavio_lua_runtime Threads::Threads)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(kavio_lua_runner PRIVATE "-Wl,--no-as-needed" pthread "-Wl,--as-needed")
endif()
target_compile_options(kavio_lua_runner PRIVATE -Wall -Wextra)
if(KAVIO_WARNINGS_AS_ERRORS)
    target_compile_options(kavio_lua_runner PRIVATE -Werror)
endif()
set_target_properties(kavio_lua_runner PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${KAVIO_LUA_RUNTIME_OUTPUT_DIR}"
    BUILD_RPATH "$ORIGIN/lib")

add_library(kavio_lua_native MODULE "${KAVIO_LUA_NATIVE_DIR}/kavio_lua_native.cpp")
target_include_directories(kavio_lua_native PRIVATE
    "${KAVIO_LUA_SOURCE_DIR}" "${KAVIO_SDK_INC_DIR}" "${KAVIO_LUA_NATIVE_DIR}")
target_link_libraries(kavio_lua_native PRIVATE kavio_lua_runtime kavio_app_runtime Threads::Threads)
target_compile_options(kavio_lua_native PRIVATE -Wall -Wextra -Wno-deprecated-declarations)
if(KAVIO_WARNINGS_AS_ERRORS)
    target_compile_options(kavio_lua_native PRIVATE -Werror)
endif()
set_target_properties(kavio_lua_native PROPERTIES
    PREFIX ""
    OUTPUT_NAME "native"
    LIBRARY_OUTPUT_DIRECTORY "${KAVIO_LUA_CORE_OUTPUT_DIR}"
    BUILD_RPATH "$ORIGIN/../lib;$ORIGIN/../../../lib")

add_custom_target(kavio_lua_runtime_package
    COMMAND ${CMAKE_COMMAND} -E make_directory "${KAVIO_LUA_RUNTIME_OUTPUT_DIR}/lib"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:kavio_lua_runtime>"
        "${KAVIO_LUA_RUNTIME_OUTPUT_DIR}/lib/$<TARGET_FILE_NAME:kavio_lua_runtime>"
    DEPENDS kavio_lua_runner kavio_lua_runtime
    COMMENT "Staging Kavio Lua runtime"
    VERBATIM)

add_custom_target(kavio_lua_package
    COMMAND ${CMAKE_COMMAND} -E make_directory "${KAVIO_LUA_CORE_OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${KAVIO_LUA_PACKAGE_SOURCE_DIR}"
        "${KAVIO_LUA_CORE_OUTPUT_DIR}"
    DEPENDS kavio_lua_native
    COMMENT "Staging high-level Kavio Lua core package"
    VERBATIM)

add_library(kavio_lua_ui_native MODULE "${KAVIO_LUA_NATIVE_DIR}/kavio_lua_ui_native.cpp")
target_include_directories(kavio_lua_ui_native PRIVATE
    "${KAVIO_LUA_SOURCE_DIR}" "${KAVIO_SDK_INC_DIR}" "${KAVIO_LUA_NATIVE_DIR}")
target_link_libraries(kavio_lua_ui_native PRIVATE
    kavio_lua_runtime kavio_ui_core kavio_ui_lvgl)
target_compile_options(kavio_lua_ui_native PRIVATE -Wall -Wextra -Wno-deprecated-declarations)
if(KAVIO_WARNINGS_AS_ERRORS)
    target_compile_options(kavio_lua_ui_native PRIVATE -Werror)
endif()
set_target_properties(kavio_lua_ui_native PROPERTIES
    PREFIX ""
    OUTPUT_NAME "ui_native"
    LIBRARY_OUTPUT_DIRECTORY "${KAVIO_LUA_UI_OUTPUT_DIR}"
    BUILD_RPATH "$ORIGIN/../lib;$ORIGIN/../../../lib")
add_custom_target(kavio_lua_ui_package DEPENDS kavio_lua_ui_native)

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


add_custom_target(kavio_device_sdk_package
    COMMAND ${CMAKE_COMMAND}
        "-DKAVIO_CONFIG_OUTPUT=${KAVIO_DEVICE_SDK_CONFIG_FILE}"
        "-DKAVIO_MAIN_LIBRARY=$<TARGET_FILE:kavio_sdk>"
        "-DKAVIO_APP_RUNTIME_LIBRARY=$<TARGET_FILE:kavio_app_runtime>"
        "-DKAVIO_UI_CORE_LIBRARY=$<TARGET_FILE:kavio_ui_core>"
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
        "-DKAVIO_LVGL_INCLUDE_DIR=${KAVIO_LVGL_ROOT}/src"
        "-DKAVIO_LV_CONF_DIR=${_KAVIO_LV_CONF_DIR}"
        "-DKAVIO_FREETYPE_INCLUDE_DIR=${KAVIO_FREETYPE_ROOT}/include"
        "-DKAVIO_UI_LVGL_LIBRARY=$<TARGET_FILE:kavio_ui_lvgl>"
        -P "${KAVIO_DEVICE_SDK_CONFIG_GENERATOR}"
    DEPENDS
        kavio_sdk
        kavio_app_runtime
        kavio_ui_core
        kavio_ui_lvgl
        kavio_python_package
        kavio_python_ui_package
        kavio_lua_runtime_package
        kavio_lua_package
        kavio_lua_ui_package
        "${KAVIO_DEVICE_SDK_CONFIG_GENERATOR}"
    COMMENT "Generating KavioDeviceSdkConfig.cmake for Native Apps"
    VERBATIM
)
add_dependencies(kavio_device_sdk_package
    kavio_sdk
    ${KAVIO_PROTOC_TARGET})
