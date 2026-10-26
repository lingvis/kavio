#pragma once

/*
 * File: FrameShm.h
 * Brief: Double-buffer POSIX shared memory wrapper for latest-only read/write.
 * Scope: Device SDK internal SHM layout and lifecycle/read-write implementation.
 */

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <kavio/Types.h>
#include "ShmBase.h"

namespace kavio {

enum class ShmType {
    kUnknown = 0,
    kRx,
    kTx,
};

/* ============================================================================
 * FrameShm 设计
 * ----------------------------------------------------------------------------
 * FrameShm 是 Device SDK 内部的双 buffer POSIX SHM 封装。
 *
 * 职责：
 * 1. ShmBase 负责 shm_open / mmap / close / unlink 和公共映射状态；
 * 2. FrameShm 负责 header / meta / payload 的固定二进制布局；
 * 3. FrameShm 提供 latest-only 的非阻塞读写模型。
 *
 * 固定布局：
 *   [SharedMemoryHeader]
 *   [ShmBufferMeta x 2]
 *   [payload0]
 *   [payload1]
 *
 * 并发模型：
 * - 写路径按单生产者模型设计；
 * - 读路径允许单读者或多读者；
 * - 没有安全可写 buffer 时允许丢帧，不阻塞生产者；
 * - 双 buffer 与读引用计数用于避免读写撕裂；
 * - 多生产者场景需要在写侧增加独立仲裁机制。
 * ========================================================================== */

/* =========================
 * 原子辅助函数
 *
 * 说明：
 * - 跨线程 / 跨进程共享内存场景里，直接依赖 std::atomic<T> 的对象模型，
 *   往往会让人对“对象构造/布局/ABI”边界产生顾虑
 * - 统一使用 GCC/Clang 的 __atomic builtins 完成原子访问
 * - 更贴近底层共享内存语义，也更适合 Linux 嵌入式环境
 * ========================= */

inline uint32_t AtomicLoadU32(const uint32_t* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

inline void AtomicStoreU32(uint32_t* p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

inline uint64_t AtomicLoadU64(const uint64_t* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

inline void AtomicStoreU64(uint64_t* p, uint64_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

inline uint32_t AtomicFetchAddU32(uint32_t* p, uint32_t v) {
    return __atomic_fetch_add(p, v, __ATOMIC_ACQ_REL);
}

inline uint32_t AtomicFetchSubU32(uint32_t* p, uint32_t v) {
    return __atomic_fetch_sub(p, v, __ATOMIC_ACQ_REL);
}

inline void AtomicThreadFenceRelease() {
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

inline void AtomicThreadFenceAcquire() {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

/* =========================
 * 常量定义
 * ========================= */

constexpr uint32_t kShmMagic = 0x56495348;
constexpr uint32_t kShmBufferCount = 2;

/* =========================
 * SHM Buffer 元数据
 *
 * 设计说明：
 * - 这里描述的是“某一个 buffer 当前已发布内容”的元信息
 * - 每个 buffer 同时保存当前帧的 width / height / stride / pixel_format
 * - payload 与这些帧参数在 WriteLatest 的发布动作中作为同一份快照生效
 * - header.width / header.height / header.stride 定义整个 SHM buffer 的固定容量，创建后保持不变
 * - 固定布局把“源区域”和“目标位置”定义为两套独立语义：
 *   1) valid_rect_* 表示 SHM 内部的有效子区域，也就是源裁剪区域
 *   2) dest_pos_* 表示 SHM 逻辑画布贴到 TX 时的目标锚点
 * - 平台按下面的模型解释：
 *   1) 使用 valid_rect_* 在 SHM 固定画布内选出当前有效源区域
 *   2) 将整块 SHM 逻辑画布锚定在 dest_pos_*
 *   3) 最终可见内容位于 TX 的 dest_pos + valid_rect.pos 对应位置
 *
 * 这个模型支持两类典型用法：
 * - 小 SHM：valid_rect 通常覆盖整块小 SHM，调用方只需要调整 dest_pos_* 来移动目标位置
 * - 大 SHM：dest_pos_* 固定不动，调用方在大 SHM 内不同位置绘制，并通过 valid_rect_* 控制当前可见窗口
 *
 * 额外约定：
 * - 写侧不校验 valid_rect_* 边界，由消费/合成阶段按自身策略解释
 * - valid_rect 为空时，语义上表示“本次发布没有可见源区域”
 * - SDK 的高级封装可以选择把空 rect 自动补成整幅区域，但底层 SHM 接口本身不强制这样做
 * - 读侧使用当前借出 buffer 的帧参数解析 payload，不使用异步控制消息推断帧格式
 * ========================= */

struct ShmBufferMeta {
    // 调试/观测维度上的帧标识，由写侧随每次发布一起更新。
    uint64_t frame_id = 0;

    // 帧时间戳，通常由写侧生产该帧时填写，读侧按原样读取。
    uint64_t timestamp_ns = 0;

    // 当前发布帧的实际图像参数。
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t pixel_format = 0;

    // 当前 buffer 中已写入的 payload 字节数。
    // 注意：它描述的是整块 SHM buffer 内已有多少原始像素数据，
    // 不是 valid_rect 对应裁剪后区域的字节数。
    uint32_t payload_size = 0;

    // 写侧发布序号。每成功写入一次就递增，便于调试观察 buffer 是否更新过。
    uint32_t write_seq = 0;

    // SHM 内部有效区域的左上角坐标，坐标原点是“本 buffer 自身左上角”。
    // 该字段只属于源坐标系；TX 目标位置由 dest_pos_* 表示。
    // 写侧不会在写入时校验它是否越界；消费侧按自己的规则解释这组值。
    int32_t valid_rect_pos_x = 0;
    int32_t valid_rect_pos_y = 0;

    // SHM 内部有效区域的宽高。
    // 它们描述当前真正应该参与显示的源区域尺寸，而不是整个 SHM 的逻辑尺寸。
    // 当宽或高为 0 时，约定这次发布没有可见区域。
    uint32_t valid_rect_width = 0;
    uint32_t valid_rect_height = 0;

    // 当前 buffer 是否已有一份可被读侧消费的已发布内容。
    uint32_t valid = 0;

    // 当前正在读取这个 buffer 的读者数量。
    // 写侧会用它避免覆盖仍被读侧借出的 buffer。
    uint32_t read_in_progress = 0;

    // 整块 SHM 逻辑画布贴到 TX 时的目标锚点。
    // 它只负责“目标平面上的放置起点”；真正显示出来的内容仍由 valid_rect_* 决定。
    // 因此平台侧实际显示位置可理解为：dest_pos + valid_rect.pos。
    int32_t dest_pos_x = 0;
    int32_t dest_pos_y = 0;

    // 布局预留/对齐字段，当前未使用。
    uint64_t reserved1 = 0;
};

/* =========================
 * SharedMemoryHeader
 *
 * header 描述整个 SHM 对象的固定布局和容量。
 * width / height / stride / pixel_format 是创建该 SHM 时采用的容量参数，
 * 不表示 RX 当前帧参数；当前帧参数保存在对应的 ShmBufferMeta 中。
 * ========================= */

struct SharedMemoryHeader {
    // 用于识别这块共享内存是否属于 FrameShm 布局。
    uint32_t magic = kShmMagic;

    // 固定头结构大小。布局通过 magic + size 校验。
    uint32_t header_size = 0;

    // 固定单 buffer 元数据结构大小。
    uint32_t buffer_meta_size = 0;

    // 固定布局预留字段。
    uint32_t reserved_layout = 0;

    // 共享内存用途类型，例如 RX 或 TX。
    uint32_t shm_type = 0;

    // 创建 SHM 容量时使用的像素格式。RX 当前帧格式以 ShmBufferMeta 为准。
    uint32_t pixel_format = 0;

    // 所属 session 标识。
    uint32_t session_id = 0;

    // 当前 session 内的 shm 标识。
    uint32_t shm_id = 0;

    // buffer 数量，当前固定为双 buffer。
    uint32_t buffer_count = kShmBufferCount;

    // 创建 SHM 容量时使用的宽度。
    uint32_t width = 0;

    // 创建 SHM 容量时使用的高度。
    uint32_t height = 0;

    // 创建 SHM 容量时使用的最大行字节跨度。
    uint32_t stride = 0;

    // 单个 buffer 的最大 payload 容量。
    uint32_t payload_capacity = 0;

    // 当前整个 SHM 对象映射总大小。
    uint32_t total_shm_size = 0;

    // 最新已发布 buffer 的索引。
    uint32_t published_index = 0;

    // 最近一次成功借出给读侧的 buffer 索引，主要用于观测和调试。
    uint32_t consumer_index = 0;

    // 布局预留/对齐字段，当前未使用。
    uint64_t reserved0 = 0;
    uint64_t reserved1 = 0;
};

static_assert(sizeof(ShmBufferMeta) == 80, "Unexpected ShmBufferMeta layout");
static_assert(sizeof(SharedMemoryHeader) == 80, "Unexpected SharedMemoryHeader layout");

/* =========================
 * FrameShm
 * ========================= */

class FrameShm final : public ShmBase {
public:
    /* ------------------------------------------------------------------------
     * ReadView
     *
     * 说明：
     * - 是一次 BeginReadLatest 成功后借出的只读快照
     * - payload 指向共享内存中的最新 buffer，不拥有数据本身
     * - 使用方必须在消费结束后调用 EndRead(index) 归还读引用
     * - valid_rect / dest_pos 的语义与 ShmBufferMeta 保持一致：
     *   1) valid_rect 描述 SHM 内部的源区域
     *   2) dest_pos 描述整块 SHM 逻辑画布在目标平面的锚点
     * - 如果 valid_rect 为空，说明这次发布没有可见区域；消费方可以直接视为 no-op
     * ---------------------------------------------------------------------- */
    struct ReadView {
        // 当前借出的 buffer 索引；EndRead 需要原样传回。
        uint32_t index = 0;

        // 指向共享内存中该 buffer payload 起始位置的只读指针。
        const uint8_t* payload = nullptr;

        // 该 buffer 当前发布帧的实际图像参数。
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t stride = 0;
        PixelFormat pixel_format = PixelFormat::kUnknown;

        // 该 buffer 当前已发布的 payload 字节数。
        uint32_t payload_size = 0;

        // 帧标识，按写侧发布值原样返回。
        uint64_t frame_id = 0;

        // 帧时间戳，按写侧发布值原样返回。
        uint64_t timestamp_ns = 0;

        // 目标平面上的锚点坐标。
        int32_t dest_pos_x = 0;
        int32_t dest_pos_y = 0;

        // SHM 内部当前真正有效的源区域。
        Rect valid_rect;

        // 指向底层元数据与头部的只读指针，便于高级调用方在必要时直接观察底层状态。
        const ShmBufferMeta* meta = nullptr;
        const SharedMemoryHeader* header = nullptr;
    };

public:
    struct CreateConfig {
        std::string name;
        ShmType shm_type = ShmType::kRx;
        uint32_t session_id = 0;
        uint32_t shm_id = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t stride = 0;
        PixelFormat pixel_format = PixelFormat::kUnknown;
    };

    struct OpenConfig {
        std::string name;
    };

    FrameShm();
    ~FrameShm() override;

    FrameShm(const FrameShm&) = delete;
    FrameShm& operator=(const FrameShm&) = delete;

    FrameShm(FrameShm&& other) noexcept;
    FrameShm& operator=(FrameShm&& other) noexcept;

    static std::unique_ptr<FrameShm> Create(
        const CreateConfig& config,
        std::string* out_error = nullptr);

    static std::unique_ptr<FrameShm> Open(
        const OpenConfig& config,
        std::string* out_error = nullptr);

    SharedMemoryHeader* Header() { return header_; }
    const SharedMemoryHeader* Header() const { return header_; }

    ShmBufferMeta* Meta(uint32_t index);
    const ShmBufferMeta* Meta(uint32_t index) const;

    uint8_t* Payload(uint32_t index);
    const uint8_t* Payload(uint32_t index) const;

    // 发布最新一帧/一块数据到当前可写 buffer。
    // 说明：
    // - valid_rect 默认为空，表示本次发布没有可见区域
    // - 底层接口不校验 valid_rect 是否越界，由消费/合成阶段解释
    // - SDK 高层包装可以把空 rect 补成整幅区域；TxSurface::WriteFrame 使用该规则
    bool WriteLatest(const void* src,
                     uint32_t src_size,
                     uint64_t frame_id,
                     uint64_t timestamp_ns,
                     uint32_t width,
                     uint32_t height,
                     uint32_t stride,
                     PixelFormat pixel_format,
                     int32_t dest_pos_x,
                     int32_t dest_pos_y,
                     const Rect& valid_rect = Rect{});

    bool BeginReadLatest(ReadView* out_view);
    void EndRead(uint32_t index);
    void ClearAllBuffers();

    static constexpr size_t MinimumHeaderSize() {
        return sizeof(SharedMemoryHeader) + sizeof(ShmBufferMeta) * kShmBufferCount;
    }

    // stride 是每一行实际占用的字节数，已经包含像素宽度以及可能存在的行对齐填充。
    // 因此共享内存单帧容量按 height * stride 计算即可，不需要再额外乘 width。
    static size_t ComputePayloadCapacity(uint32_t height, uint32_t stride) {
        return static_cast<size_t>(height) * static_cast<size_t>(stride);
    }

    static size_t ComputeTotalSize(uint32_t payload_capacity) {
        return MinimumHeaderSize() +
               static_cast<size_t>(payload_capacity) * kShmBufferCount;
    }

private:
    bool CreateInternal(const CreateConfig& config,
                        std::string* out_error);

    bool OpenInternal(const OpenConfig& config,
                      std::string* out_error);

    bool AttachLayout(void* base, size_t size, std::string* out_error);
    bool ValidateLayout(std::string* out_error) const;

    bool InitializeLayout(ShmType shm_type,
                          PixelFormat pixel_format,
                          uint32_t session_id,
                          uint32_t shm_id,
                          uint32_t width,
                          uint32_t height,
                          uint32_t stride,
                          uint32_t payload_capacity,
                          uint32_t total_shm_size,
                          std::string* out_error);

    int ChooseWritableBuffer() const;
    void ResetLayout() noexcept override;

private:
    SharedMemoryHeader* header_ = nullptr;
    ShmBufferMeta* meta_[kShmBufferCount] = {nullptr, nullptr};
};

}  // namespace kavio
