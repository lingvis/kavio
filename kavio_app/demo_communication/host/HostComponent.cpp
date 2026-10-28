/*
 * demo_communication Windows HostComponent
 *
 * 收到Device Component 的查询结构体后，现场采集 Windows 总体 CPU、物理内存以及
 * 固定磁盘容量，并把完整响应结构体返回。第一次 CPU 采样没有前值时返回 0。
 */

#define KAVIO_HOST_IMPLEMENTATION
#include <kavio/KavioHost.h>
#include <KLogger.h>
#include "../common/DemoMetricsProtocol.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <new>
#include <string>

namespace {

uint64_t FileTimeValue(const FILETIME& value) {
    ULARGE_INTEGER number;
    number.LowPart = value.dwLowDateTime;
    number.HighPart = value.dwHighDateTime;
    return number.QuadPart;
}

// 一个 KavioAppHost 进程可以承载一个Host Component 实例；所有跨回调共享状态都放在这里。
// Host Component 只负责 Windows 系统指标采样并回复 Device 查询；不参与 TX 绘制。
// Windows Host 回调可能跨线程进入，修改共享采样状态时必须遵循已有互斥约束。
struct DemoHostComponent {
    const KavioHostApi* host = nullptr;
    std::atomic<bool> running{false};
    bool has_cpu_sample = false;
    uint64_t previous_idle = 0;
    uint64_t previous_kernel = 0;
    uint64_t previous_user = 0;
};

// GetSystemTimes 返回累计时间，必须保存上一次采样后做差才能得到区间 CPU 使用率。
// GetSystemTimes 给出累计 CPU 时间，使用相邻采样差值计算利用率；首次采样只建立基线。
void ReadCpu(DemoHostComponent& component, double& usage) {
    usage = 0.0;
    FILETIME idle = {}, kernel = {}, user = {};
    if (!::GetSystemTimes(&idle, &kernel, &user)) return;
    const uint64_t next_idle = FileTimeValue(idle);
    const uint64_t next_kernel = FileTimeValue(kernel);
    const uint64_t next_user = FileTimeValue(user);
    if (component.has_cpu_sample && next_kernel >= component.previous_kernel &&
        next_user >= component.previous_user && next_idle >= component.previous_idle) {
        const uint64_t idle_delta = next_idle - component.previous_idle;
        const uint64_t total_delta = (next_kernel - component.previous_kernel) +
                                     (next_user - component.previous_user);
        if (total_delta > 0 && idle_delta <= total_delta)
            usage = 100.0 * static_cast<double>(total_delta - idle_delta) /
                    static_cast<double>(total_delta);
    }
    component.previous_idle = next_idle;
    component.previous_kernel = next_kernel;
    component.previous_user = next_user;
    component.has_cpu_sample = true;
}

// 返回总物理内存与已使用量（字节），不要在协议层换算为 KB/MB。
void ReadMemory(demo_metrics::ResponseMessage& response) {
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    if (!::GlobalMemoryStatusEx(&status)) return;
    response.memory_total_bytes = status.ullTotalPhys;
    response.memory_used_bytes = status.ullTotalPhys - status.ullAvailPhys;
}

// 仅枚举 DRIVE_FIXED；网络盘、光驱和可移动盘不属于这个 Demo 的统计范围。
// 枚举固定磁盘并受协议 kMaxDisks 限制，避免响应长度随机器卷数无限增长。
void ReadDisks(demo_metrics::ResponseMessage& response) {
    wchar_t drives[512] = {};
    const DWORD length = ::GetLogicalDriveStringsW(511, drives);
    if (length == 0 || length >= 511) return;
    for (const wchar_t* drive = drives; *drive && response.disk_count < demo_metrics::kMaxDisks;
         drive += wcslen(drive) + 1) {
        if (::GetDriveTypeW(drive) != DRIVE_FIXED) continue;
        ULARGE_INTEGER free_available = {}, total = {}, free_total = {};
        if (!::GetDiskFreeSpaceExW(drive, &free_available, &total, &free_total)) continue;
        demo_metrics::DiskUsage& disk = response.disks[response.disk_count++];
        const char letter = drive[0] >= L'a' && drive[0] <= L'z'
            ? static_cast<char>(drive[0] - L'a' + 'A') : static_cast<char>(drive[0]);
        disk.name[0] = letter;
        disk.name[1] = '\0';
        disk.total_bytes = total.QuadPart;
        disk.used_bytes = total.QuadPart - free_total.QuadPart;
    }
}

}  // namespace

extern "C" {


KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostCreate(
    const KavioHostCreateParams* params, KavioHostHandle* component) {
    if (!params || !component || !params->host ||
        params->struct_size < KAVIO_HOST_CREATE_PARAMS_BASE_SIZE ||
        params->host->struct_size < KAVIO_HOST_API_BASE_SIZE ||
        !params->host->send) return KavioResult_InvalidArgument;
    DemoHostComponent* instance = new (std::nothrow) DemoHostComponent();
    if (!instance) return KavioResult_InternalError;
    instance->host = params->host;
    *component = instance;
    return KavioResult_Success;
}

// Host 生命周期从此入口开始；设备是否已就绪由通信回调和 SDK 状态决定。
KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostStart(KavioHostHandle handle) {
    DemoHostComponent* component = static_cast<DemoHostComponent*>(handle);
    if (!component) return KavioResult_InvalidArgument;
    component->running.store(true);
    KLOGI("demo_communication started");
    return KavioResult_Success;
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostPrepareStop(KavioHostHandle handle) {
    DemoHostComponent* component = static_cast<DemoHostComponent*>(handle);
    if (!component) return KavioResult_InvalidArgument;
    component->running.store(false);
    return KavioResult_Success;
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostStop(KavioHostHandle handle) {
    DemoHostComponent* component = static_cast<DemoHostComponent*>(handle);
    if (!component) return KavioResult_InvalidArgument;
    component->running.store(false);
    return KavioResult_Success;
}

KAVIO_HOST_ENTRY void KAVIO_HOST_CALL KavioHostDestroy(KavioHostHandle handle) {
    delete static_cast<DemoHostComponent*>(handle);
}

// Device Component 的每个 QueryMessage 都对应一次现场采样和一次完整 ResponseMessage。
KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostOnData(
    KavioHostHandle handle, const void* data, size_t size) {
    DemoHostComponent* component = static_cast<DemoHostComponent*>(handle);
    if (!component || !data || size != sizeof(demo_metrics::QueryMessage))
        return KavioResult_InvalidArgument;
    if (!component->running.load()) return KavioResult_NotReady;

    demo_metrics::QueryMessage query;
    std::memcpy(&query, data, sizeof(query));
    if (!demo_metrics::IsValid(query.header, demo_metrics::MessageType::Query,
                               sizeof(query))) return KavioResult_InvalidArgument;

    demo_metrics::ResponseMessage response;
    demo_metrics::Initialize(response, query.sequence);
    ReadCpu(*component, response.cpu_usage_percent);
    ReadMemory(response);
    ReadDisks(response);
    return component->host->send(component->host->context,
                                   &response, sizeof(response));
}

// Channel ready 只表示双端业务通道可用；真正的数据处理仍在 KavioHostOnData 中完成。
KAVIO_HOST_ENTRY void KAVIO_HOST_CALL KavioHostOnDeviceState(
    KavioHostHandle handle, uint32_t ready) {
    (void)handle;
    KLOGI("{}", ready ? "device channel ready" : "device channel unavailable");
}

}  // extern "C"
