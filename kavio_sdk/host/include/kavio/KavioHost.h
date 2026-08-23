#pragma once
/*
 * KavioHost.h
 *
 * Kavio Host Component 对外 C ABI。
 *
 * Host Component 开发者只需要包含这个头文件，不需要链接 Host SDK 库。
 * KavioAppHost 负责 DLL 加载、进程管理和与 KavioWin 的通信；组件负责实现
 * 本文件末尾声明的导出入口，并通过 KavioHostApi 调用宿主能力。
 *
 * ABI 约束：
 * 1. Host ABI 不使用 Device SDK 版本号，通过 struct_size 判断结构体字段可用性。
 * 2. ABI 结构体只能在尾部追加可选字段，不能改变已有字段含义或顺序。
 * 3. 只能访问 struct_size 已覆盖的字段。
 * 4. ABI 边界不传递 C++ STL 对象、异常或跨运行库分配/释放的内存。
 * 5. KavioAppHost 以 KAVIO_HOST_TYPES_ONLY 包含本文件，只取得 ABI 类型，不声明
 *    组件需要实现的导出入口；DLL Loader 的函数指针类型也只存在于 KavioAppHost 内部。
 * 6. 实现 Host Component 导出入口的 .cpp 必须在包含本文件前定义
 *    KAVIO_HOST_IMPLEMENTATION。这样头文件只提供 ABI 类型，不预先声明导出入口，
 *    避免 MSVC 将普通声明与 __declspec(dllexport) 定义判定为不同链接属性。
 *
 * 必须实现：
 *   KavioHostCreate / KavioHostStart / KavioHostStop /
 *   KavioHostDestroy / KavioHostOnData
 *
 * 按需实现：
 *   KavioHostPrepareStop / KavioHostOnDeviceState / KavioHostOpenPanel
 *
 * 实现导出函数的推荐形式：
 *   #define KAVIO_HOST_IMPLEMENTATION
 *   #include <kavio/KavioHost.h>
 *
 *   KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL
 *   KavioHostStart(KavioHostHandle host) { ... }
 */

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define KAVIO_HOST_CALL __cdecl
#define KAVIO_HOST_VISIBILITY __declspec(dllexport)
#else
#define KAVIO_HOST_CALL
#define KAVIO_HOST_VISIBILITY __attribute__((visibility("default")))
#endif

#if defined(KAVIO_HOST_TYPES_ONLY) && defined(KAVIO_HOST_IMPLEMENTATION)
#error KAVIO_HOST_TYPES_ONLY and KAVIO_HOST_IMPLEMENTATION cannot be used together
#endif

#ifdef KAVIO_HOST_IMPLEMENTATION
#ifdef __cplusplus
#define KAVIO_HOST_ENTRY extern "C" KAVIO_HOST_VISIBILITY
#else
#define KAVIO_HOST_ENTRY KAVIO_HOST_VISIBILITY
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void* KavioHostHandle;

typedef enum KavioResult
{
    KavioResult_Success = 0,
    KavioResult_InvalidArgument = 1,
    KavioResult_NotReady = 2,
    KavioResult_TransportError = 3,
    KavioResult_InternalError = 4,
    KavioResult_NotSupported = 5,
    KavioResult_QueueFull = 6
} KavioResult;

typedef enum KavioHostLogLevel
{
    KavioHostLogLevel_Trace = 0,
    KavioHostLogLevel_Debug = 1,
    KavioHostLogLevel_Info = 2,
    KavioHostLogLevel_Warning = 3,
    KavioHostLogLevel_Error = 4
} KavioHostLogLevel;

/* 向当前 App 唯一对应的 Device Component 发送一条非空完整数据报。 */
typedef KavioResult(KAVIO_HOST_CALL* KavioHostSendFn)(
    void* context,
    const void* data,
    size_t size);

/* 对端 Device Endpoint 在线且当前通道可双向通信时返回非 0。 */
typedef uint32_t(KAVIO_HOST_CALL* KavioHostIsDeviceOnlineFn)(void* context);

/* 返回当前 App 由 app_id 稳定计算得到的 ChannelID。 */
typedef uint32_t(KAVIO_HOST_CALL* KavioHostGetChannelIdFn)(void* context);

/* 请求 KavioWin 停止并重新启动当前 Host Component。 */
typedef KavioResult(KAVIO_HOST_CALL* KavioHostRestartFn)(
    void* context,
    uint32_t reason);

/* 将一条日志写入 KavioWin 统一日志系统。 */
typedef void(KAVIO_HOST_CALL* KavioHostLogFn)(
    void* context,
    uint32_t level,
    const char* message);

/* 返回当前 App Host Component 可写的私有数据目录。 */
typedef const char*(KAVIO_HOST_CALL* KavioHostGetDataPathFn)(void* context);

/*
 * KavioAppHost 提供给 Host Component 的宿主能力表。
 * 该指针从 KavioHostCreate 开始，到 KavioHostDestroy 返回前始终有效。
 */
typedef struct KavioHostApi
{
    uint32_t struct_size;
    void* context;

    KavioHostSendFn send;
    KavioHostIsDeviceOnlineFn is_device_online;
    KavioHostRestartFn restart;
    KavioHostLogFn log;
    KavioHostGetDataPathFn get_data_path;
    KavioHostGetChannelIdFn get_channel_id;
} KavioHostApi;

#define KAVIO_HOST_API_BASE_SIZE \
    ((uint32_t)(offsetof(KavioHostApi, get_channel_id) + \
                sizeof(((KavioHostApi*)0)->get_channel_id)))

#define KAVIO_HOST_API_HAS_FIELD(api, field) \
    ((api) != NULL && \
     (api)->struct_size >= (uint32_t)(offsetof(KavioHostApi, field) + \
                                      sizeof(((KavioHostApi*)0)->field)))

typedef struct KavioHostCreateParams
{
    uint32_t struct_size;

    /* 当前 Kavio App 的唯一 app_id。 */
    const char* app_id;

    /* KavioAppHost 提供的宿主能力表。有效期见 KavioHostApi。 */
    const KavioHostApi* host;
} KavioHostCreateParams;

#define KAVIO_HOST_CREATE_PARAMS_BASE_SIZE \
    ((uint32_t)(offsetof(KavioHostCreateParams, host) + \
                sizeof(((KavioHostCreateParams*)0)->host)))

#if !defined(KAVIO_HOST_TYPES_ONLY) && !defined(KAVIO_HOST_IMPLEMENTATION)

/*
 * 必须实现：创建一个 Host Component 实例。
 * 这里只做轻量初始化；成功时把组件私有对象写入 out_host，随后会调用 KavioHostStart。
 */
KavioResult KAVIO_HOST_CALL KavioHostCreate(
    const KavioHostCreateParams* params,
    KavioHostHandle* out_host);

/* 必须实现：启动组件业务逻辑及工作线程。 */
KavioResult KAVIO_HOST_CALL KavioHostStart(KavioHostHandle host);

/*
 * 可选实现：在 KavioHostStop 前停止接受新任务，并尽量让在途任务收敛。
 * DLL 未导出该符号时，KavioAppHost 直接跳过该阶段。
 */
KavioResult KAVIO_HOST_CALL KavioHostPrepareStop(KavioHostHandle host);

/* 必须实现：停止业务工作。返回后不应再主动发送新的 Device 数据。 */
KavioResult KAVIO_HOST_CALL KavioHostStop(KavioHostHandle host);

/* 必须实现：释放由 KavioHostCreate 创建的组件实例。 */
void KAVIO_HOST_CALL KavioHostDestroy(KavioHostHandle host);

/*
 * 必须实现：接收当前 App 的 Device Component 发来的一条完整原始数据报。
 * data 由 KavioAppHost 持有，只在本次调用期间有效；需要异步使用时必须自行拷贝。
 */
KavioResult KAVIO_HOST_CALL KavioHostOnData(
    KavioHostHandle host,
    const void* data,
    size_t size);

/*
 * 可选实现：对端 Device Endpoint 上下线通知。
 * online 非 0 表示当前已具备双向通信条件；为 0 时不得假定 Device Component 可接收数据。
 */
void KAVIO_HOST_CALL KavioHostOnDeviceState(
    KavioHostHandle host,
    uint32_t online);

/*
 * 可选实现：打开 Host Component 自己提供的 App Panel。
 * DLL 未导出该符号时，KavioAppHost 认为该 App 没有 Host Panel。
 */
KavioResult KAVIO_HOST_CALL KavioHostOpenPanel(KavioHostHandle host);

#endif  /* !KAVIO_HOST_TYPES_ONLY && !KAVIO_HOST_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif
