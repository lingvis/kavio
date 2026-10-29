#pragma once

#include <cstdint>
#include <array>
#include <deque>
#include <mutex>
#include <vector>

#include <lvgl.h>
#include <kavio/KavioClient.h>
#include <kavio/detail/AppViewSurfaceShared.h>

namespace kavio {

/**
 * @brief LVGL 9.4 adapter that flushes a Device App's UI to AppViewSurface.
 *
 * KavioClient::OnMessage runs on the SDK dispatch thread. HandleMessage() only
 * stores the asynchronous create reply; ProcessPending() must be called from
 * the App's LVGL/UI thread to map the surface and create the LVGL display.
 */
class LvglAppViewAdapter {
public:
    explicit LvglAppViewAdapter(KavioClient& client);
    ~LvglAppViewAdapter();

    LvglAppViewAdapter(const LvglAppViewAdapter&) = delete;
    LvglAppViewAdapter& operator=(const LvglAppViewAdapter&) = delete;

    /** Submit an asynchronous request to create the full-screen AppView surface. */
    Status Initialize();

    /** Forward IPC messages here before the App handles other messages. Returns true if consumed. */
    bool HandleMessage(const kavio::ipc::proto::IPCPackagePb& message);

    /** Call on the LVGL/UI thread to finish an asynchronous surface creation. */
    Status ProcessPending();

    lv_display_t* Display() const noexcept { return display_; }
    /** Background Apps keep rendering if they choose, but only the foreground gets input. */
    bool IsForeground() const;
    void MakeDefault();
    Status LastStatus() const;
    void Shutdown();

private:
    static void FlushCallback(lv_display_t* display, const lv_area_t* area, uint8_t* px_map);
    static void ReadPointerCallback(lv_indev_t* indev, lv_indev_data_t* data);
    void ReadPointer(lv_indev_data_t* data);
    void Flush(lv_display_t* display, const lv_area_t* area, const uint8_t* px_map);
    void SetLastStatus(const Status& status);

    KavioClient* client_ = nullptr; // caller must destroy this adapter before KavioClient
    AppViewSurface surface_;
    lv_display_t* display_ = nullptr;
    lv_indev_t* pointer_device_ = nullptr;
    std::vector<uint8_t> draw_buffer_;
    std::array<AppViewRect, detail::kAppViewMaxDirtyRects> dirty_rects_{};
    uint32_t dirty_rect_count_ = 0;
    bool dirty_overflow_ = false;
    // UI thread only; diagnostics aggregate 200 submitted LVGL frames.
    struct DirtyPerf {
        uint64_t frames = 0;
        uint64_t flushes = 0;
        uint64_t raw_pixels = 0;
        uint64_t output_pixels = 0;
        uint64_t output_rects = 0;
        uint64_t full_submits = 0;
        uint64_t near_full_flushes = 0;
    } dirty_perf_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t stride_ = 0;
    bool frame_write_locked_ = false; // LVGL UI thread only; spans all partial flushes in one refresh.
    Status frame_copy_status_ = Status::Ok();

    mutable std::mutex mutex_;
    bool creating_ = false;
    bool pending_ready_ = false;
    bool pending_has_info_ = false;
    uint64_t create_request_id_ = 0;
    kavio::ipc::proto::AppViewSurfaceInfoPb pending_info_;
    Status pending_status_ = Status::Ok();
    Status last_status_ = Status::Ok();
    struct PointerSample {
        int32_t x = 0;
        int32_t y = 0;
        bool pressed = false;
    };
    std::deque<PointerSample> pointer_queue_; // mutex_; SDK IPC dispatch -> LVGL UI thread
    PointerSample last_pointer_{};           // UI thread only
    uint32_t active_surface_id_ = 0;          // mutex_
    uint64_t last_pointer_sequence_ = 0;     // mutex_
    int32_t last_received_x_ = 0;            // mutex_
    int32_t last_received_y_ = 0;            // mutex_
    bool foreground_ = false;                // mutex_
    uint32_t pending_visible_surface_id_ = 0; // mutex_
    bool pending_visible_ = false;           // mutex_
};

} // namespace kavio
