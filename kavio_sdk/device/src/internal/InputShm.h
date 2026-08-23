#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "ShmBase.h"

namespace kavio {

/**
 * InputShm
 *
 * 平台级输入状态 SHM。
 *
 * 该 SHM 保存“标准状态”，不保存事件队列：
 * 1. 一个统一的大按键 bitmap；
 * 2. 鼠标状态：x / y / buttons；
 * 3. update_counter 仅用于调试观察是否有更新，不参与同步；
 * 4. 不使用 seqlock。
 *
 * key code 建议使用 Linux input-event-codes.h 中的 EV_KEY code。
 * 例如：
 *   KEY_W / KEY_A / KEY_S / KEY_D
 *   KEY_1 / KEY_5
 *   KEY_J / KEY_K / KEY_U / KEY_I / KEY_O / KEY_P
 *   BTN_LEFT / BTN_RIGHT / BTN_MIDDLE
 *   BTN_A / BTN_B / BTN_X / BTN_Y
 *
 * 数字按键类输入统一进入 key_bitmap。
 * 鼠标位置由于不是按键，需要单独字段。
 */
class InputShm final : public ShmBase {
public:
    static constexpr uint32_t kMagic = 0x4B494E50u; // 'KINP'
    static constexpr uint32_t kKeyBits = 1024;
    static constexpr uint32_t kKeyWords = kKeyBits / 32;

    static constexpr const char* kDefaultShmName = "/kavio_input_state";

    enum MouseButton : uint32_t {
        MOUSE_BUTTON_LEFT   = 1u << 0,
        MOUSE_BUTTON_RIGHT  = 1u << 1,
        MOUSE_BUTTON_MIDDLE = 1u << 2,
        MOUSE_BUTTON_BACK   = 1u << 3,
        MOUSE_BUTTON_FORWARD= 1u << 4,
    };

    struct MouseState {
        int32_t x = 0;
        int32_t y = 0;

        // 鼠标按键状态。
        // 当前状态布局不包含滚轮字段。
        uint32_t buttons = 0;

        uint32_t reserved[5] = {};
    };

    struct SharedState {
        uint32_t magic = 0;
        uint32_t struct_size = 0;

        // 固定布局预留字段。
        uint32_t reserved_layout = 0;

        // 不做同步控制，只用于调试观察是否有输入更新。
        uint32_t update_counter = 0;

        // 输入管理程序最后一次更新的时间。
        // 单位：微秒，建议使用 CLOCK_MONOTONIC。
        uint64_t timestamp_us = 0;

        // 统一按键状态。
        // code 使用 Linux EV_KEY code 或平台统一 code。
        uint32_t key_bitmap[kKeyWords] = {};

        MouseState mouse;

        uint8_t reserved[512] = {};
    };

public:
    struct CreateConfig {
        std::string name = kDefaultShmName;
    };

    struct OpenConfig {
        std::string name = kDefaultShmName;
        ShmAccess access = ShmAccess::ReadOnly;
    };

    InputShm();
    ~InputShm() override;

    InputShm(const InputShm&) = delete;
    InputShm& operator=(const InputShm&) = delete;

    InputShm(InputShm&&) = delete;
    InputShm& operator=(InputShm&&) = delete;

    /**
     * 创建并初始化输入 SHM。
     *
     * 严格策略：
     * 1. shm_name 必须以 '/' 开头；
     * 2. 创建后固定大小为 sizeof(SharedState)；
     * 3. 会初始化 magic/struct_size；
     * 4. 如果 shm 已存在，会打开并重置为当前结构。
     */
    static std::unique_ptr<InputShm> Create(
        const CreateConfig& config,
        std::string* error = nullptr);

    /**
     * 打开已有输入 SHM。
     *
     * 严格策略：
     * 1. shm_name 必须以 '/' 开头；
     * 2. magic/struct_size 不匹配直接失败；
     * 3. 固定布局由 magic、struct_size 和映射大小校验。
     */
    static std::unique_ptr<InputShm> Open(
        const OpenConfig& config,
        std::string* error = nullptr);

    static size_t RequiredSize() {
        return sizeof(SharedState);
    }

    static bool IsValidKeyCode(uint32_t code) {
        return code < kKeyBits;
    }

    /**
     * 仅写侧使用。
     * 清空按键和鼠标状态。
     */
    bool ResetState(std::string* error = nullptr);

    /**
     * 仅写侧使用。
     * 设置某个标准按键状态。
     */
    bool SetKey(uint32_t code, bool pressed);

    /**
     * 读侧/写侧均可使用。
     * 查询某个标准按键是否按下。
     */
    bool IsKeyDown(uint32_t code) const;

    /**
     * 读出某个 bitmap word。
     * 用于 game_emulator 这种应用一次性读取多个键。
     */
    uint32_t GetKeyWord(uint32_t word_index) const;

    /**
     * 仅写侧使用。
     * 设置鼠标状态。
     *
     * buttons 使用 MouseButton bit。
     * 当前状态布局不包含滚轮字段。
     */
    bool SetMouseState(int32_t x, int32_t y, uint32_t buttons);

    /**
     * 读侧/写侧均可使用。
     */
    MouseState GetMouseState() const;

    uint32_t GetUpdateCounter() const;

    uint64_t GetTimestampUs() const;

    const SharedState* RawState() const {
        return state_;
    }

    SharedState* MutableRawState() {
        return IsWritable() ? state_ : nullptr;
    }

    /**
     * 按名称删除 SHM 对象。实例生命周期统一使用 ShmBase::Unlink/CloseAndUnlink。
     */
    static bool Remove(const std::string& shm_name = kDefaultShmName,
        std::string* error = nullptr);

private:
    bool CreateInternal(const CreateConfig& config,
        std::string* error);

    bool OpenInternal(const OpenConfig& config,
        std::string* error);

    void ResetLayout() noexcept override;

    static bool ValidateShmName(const std::string& shm_name,
        std::string* error);


    static uint64_t NowMonotonicUs();

    bool ValidateHeader(std::string* error) const;

    bool RequireOpen(std::string* error = nullptr) const;

    bool RequireWritable(std::string* error = nullptr) const;

private:
    SharedState* state_ = nullptr;
};

} // namespace kavio