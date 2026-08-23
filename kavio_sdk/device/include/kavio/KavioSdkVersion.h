#pragma once
/*
 * KavioSdkVersion.h
 *
 * Kavio SDK 版本定义。
 *
 * 版本规则：
 * 1. SDK 版本固定为 major.minor.patch 三段式；
 * 2. 三段版本仅用于形成可排序的 SDK 版本号，不赋予 major 特殊的不兼容语义；
 * 3. 运行时 SDK 版本只在 IPC Session 创建阶段由 Platform 校验一次，且只判断是否低于最低接入版本；
 * 4. Host ABI 与各 SHM 布局使用各自的结构和尺寸校验机制，不使用 Device SDK 版本号作为布局版本；
 * 5. KAVIO_SDK_VERSION_STRING 由三个数值字段自动生成，不单独维护版本字符串。
 */

#include <stdint.h>

#define KAVIO_SDK_VERSION_MAJOR 3
#define KAVIO_SDK_VERSION_MINOR 0
#define KAVIO_SDK_VERSION_PATCH 0

#define KAVIO_SDK_STRINGIFY_IMPL(value) #value
#define KAVIO_SDK_STRINGIFY(value) KAVIO_SDK_STRINGIFY_IMPL(value)

#define KAVIO_SDK_VERSION_STRING \
    KAVIO_SDK_STRINGIFY(KAVIO_SDK_VERSION_MAJOR) "." \
    KAVIO_SDK_STRINGIFY(KAVIO_SDK_VERSION_MINOR) "." \
    KAVIO_SDK_STRINGIFY(KAVIO_SDK_VERSION_PATCH)

typedef struct KavioSdkVersion
{
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} KavioSdkVersion;

static inline int KavioSdkVersionCompare(KavioSdkVersion lhs, KavioSdkVersion rhs)
{
    if (lhs.major != rhs.major) return lhs.major < rhs.major ? -1 : 1;
    if (lhs.minor != rhs.minor) return lhs.minor < rhs.minor ? -1 : 1;
    if (lhs.patch != rhs.patch) return lhs.patch < rhs.patch ? -1 : 1;
    return 0;
}
