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
    LANGUAGES CXX
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

if(TARGET libprotobuf)
    set(KAVIO_PROTOBUF_LIB_TARGET libprotobuf)
elseif(TARGET protobuf::libprotobuf)
    set(KAVIO_PROTOBUF_LIB_TARGET protobuf::libprotobuf)
else()
    message(FATAL_ERROR "libprotobuf target not found")
endif()

if(TARGET protobuf::protoc)
    set(KAVIO_PROTOC_TARGET protobuf::protoc)
elseif(TARGET protoc)
    set(KAVIO_PROTOC_TARGET protoc)
else()
    message(FATAL_ERROR "protoc target not found")
endif()

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
add_dependencies(kavio_sdk kavio_sdk_proto_py)

target_include_directories(kavio_sdk
    PUBLIC
        "${KAVIO_SDK_INC_DIR}"
    PRIVATE
        "${KAVIO_SDK_SRC_DIR}"
)

target_link_libraries(kavio_sdk
    PUBLIC
    kavio_sdk_shared
        Threads::Threads
        rt
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
        "-DKAVIO_SHARED_LIBRARY=$<TARGET_FILE:kavio_sdk_shared>"
        "-DKAVIO_PROTO_LIBRARY=$<TARGET_FILE:kavio_sdk_proto>"
        "-DKAVIO_PROTOBUF_LIBRARY=$<TARGET_FILE:${KAVIO_PROTOBUF_LIB_TARGET}>"
        "-DKAVIO_INCLUDE_DIR=${KAVIO_SDK_INC_DIR}"
        "-DKAVIO_PROTO_INCLUDE_DIR=${KAVIO_SDK_PROTO_GEN_DIR}"
        "-DKAVIO_PROTOBUF_SOURCE_INCLUDE_DIR=${KAVIO_PROTOBUF_DIR}/src"
        -P "${KAVIO_DEVICE_SDK_CONFIG_GENERATOR}"
    DEPENDS
        kavio_sdk
        "${KAVIO_DEVICE_SDK_CONFIG_GENERATOR}"
    COMMENT "Generating KavioDeviceSdkConfig.cmake for Native Apps"
    VERBATIM
)
