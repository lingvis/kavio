/*
 * demo_panel_image - Windows Host Component
 *
 * Drop a PNG or GIF into the App Panel. Windows Imaging Component decodes the
 * image to BGRA. GIF frames are composed on the GIF logical canvas according to
 * frame offsets, transparency and disposal metadata before they are uploaded to
 * the Device Component. Dragging the image sends exact X/Y position updates to the App.
 *
 * The panel uses persistent double buffering. Painting never erases the client
 * background separately, so GIF playback and image dragging do not flicker.
 */
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <wincodec.h>
#include <propvarutil.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define KAVIO_HOST_IMPLEMENTATION
#include <kavio/KavioHost.h>
#include <KLogger.h>
#include "DemoImagePanelProtocol.h"

namespace {

template <typename T> void SafeRelease(T*& value) {
    if (value) { value->Release(); value = nullptr; }
}

struct DecodedFrame {
    uint32_t delay_ms = 100;
    std::vector<uint8_t> pixels;
};

// Host 负责 PNG/GIF 解码，统一输出可由 Device 直接消费的 BGRA8888 帧。
struct DecodedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<DecodedFrame> frames;
};

struct HostComponent {
    const KavioHostApi* host = nullptr;
    std::atomic<bool> running{false};
    std::atomic<bool> channel_ready{false};
    std::atomic<bool> device_ready{false};
    std::mutex panel_mutex;
    std::thread panel_thread;
    HWND panel_window = nullptr;
    HANDLE panel_ready = nullptr;
};

struct PanelState {
    HostComponent* component = nullptr;
    DecodedImage image;
    size_t frame_index = 0;
    int x = 0;
    int y = 0;
    bool dragging = false;
    int drag_offset_x = 0;
    int drag_offset_y = 0;
    ULONGLONG next_frame_tick = 0;
    uint64_t next_transfer_id = 1;

    // Persistent client-area back buffer. The checker background and image are
    // composed off-screen and copied to the real window in one BitBlt.
    HDC back_dc = nullptr;
    HBITMAP back_bitmap = nullptr;
    HGDIOBJ back_old_bitmap = nullptr;
    int back_width = 0;
    int back_height = 0;

    // Persistent bitmap for the currently displayed image frame. Moving a PNG
    // therefore does not recreate or premultiply the bitmap for every WM_MOUSEMOVE.
    HDC image_dc = nullptr;
    HBITMAP image_bitmap = nullptr;
    HGDIOBJ image_old_bitmap = nullptr;
    void* image_bits = nullptr;
    uint32_t image_bitmap_width = 0;
    uint32_t image_bitmap_height = 0;
    size_t cached_frame_index = static_cast<size_t>(-1);
};

// GIF 帧需要记录 disposal、位置和延迟，不能把局部更新帧当成完整画布。
struct GifFrameInfo {
    uint32_t left = 0;
    uint32_t top = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    uint32_t delay_ms = 100;
    uint32_t disposal = 0;
};

// Explorer normally runs at medium integrity. If KavioWin is started elevated
// (for example from an elevated Visual Studio), UIPI can block WM_DROPFILES and
// related shell drag/drop messages before they reach this window.
bool EnableShellFileDrop(HWND window) {
    if (!window) return false;

    // Resolve this API dynamically so the demo still compiles cleanly with the
    // Windows 8.1 SDK used by the VS2015 project even when the SDK headers expose
    // different _WIN32_WINNT gates for ChangeWindowMessageFilterEx.
    typedef BOOL (WINAPI *ChangeWindowMessageFilterExFn)(HWND, UINT, DWORD, void*);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    ChangeWindowMessageFilterExFn change_filter = user32 ?
        reinterpret_cast<ChangeWindowMessageFilterExFn>(
            GetProcAddress(user32, "ChangeWindowMessageFilterEx")) : nullptr;

    bool filter_ok = true;
    if (change_filter) {
        const DWORD kMsgfltAllow = 1;
        const UINT kWmCopyGlobalData = 0x0049;
        filter_ok = change_filter(window, WM_DROPFILES, kMsgfltAllow, nullptr) != FALSE &&
                    change_filter(window, kWmCopyGlobalData, kMsgfltAllow, nullptr) != FALSE;
    }

    DragAcceptFiles(window, TRUE);
    return filter_ok;
}

bool SendRaw(HostComponent* component, const void* data, size_t size) {
    if (!component || !component->host || !component->running.load() || !component->channel_ready.load() ||
        !component->device_ready.load() || !component->host->send) return false;
    // The KavioAppHost send queue is bounded. Large images are uploaded in chunks;
    // briefly wait when the queue is full instead of making developers add flow control.
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const KavioResult result = component->host->send(
            component->host->context, data, size);
        if (result == KavioResult_Success) return true;
        if (result != KavioResult_QueueFull) return false;
        Sleep(5);
    }
    return false;
}

template <typename T> bool SendMessage(HostComponent* component, const T& message) {
    return SendRaw(component, &message, sizeof(message));
}

bool PropVariantToUInt32(const PROPVARIANT& value, uint32_t& output) {
    switch (value.vt) {
    case VT_UI1: output = value.bVal; return true;
    case VT_UI2: output = value.uiVal; return true;
    case VT_UI4: output = value.ulVal; return true;
    case VT_I1:
        if (value.cVal < 0) return false;
        output = static_cast<uint32_t>(value.cVal); return true;
    case VT_I2:
        if (value.iVal < 0) return false;
        output = static_cast<uint32_t>(value.iVal); return true;
    case VT_I4:
        if (value.lVal < 0) return false;
        output = static_cast<uint32_t>(value.lVal); return true;
    default:
        return false;
    }
}

bool ReadMetadataUInt32(IWICMetadataQueryReader* reader,
                        const wchar_t* path,
                        uint32_t& output) {
    if (!reader || !path) return false;
    PROPVARIANT value;
    PropVariantInit(&value);
    const HRESULT hr = reader->GetMetadataByName(path, &value);
    const bool ok = SUCCEEDED(hr) && PropVariantToUInt32(value, output);
    PropVariantClear(&value);
    return ok;
}

uint32_t ReadFrameDelay(IWICMetadataQueryReader* reader) {
    uint32_t delay_10ms = 0;
    if (!ReadMetadataUInt32(reader, L"/grctlext/Delay", delay_10ms)) return 100;
    // Very small GIF delays are commonly treated as one display tick by players.
    // Keep a 20 ms floor to avoid spinning the demo at an unreasonable rate.
    return std::max<uint32_t>(20, delay_10ms * 10u);
}

// WIC 转换为 BGRA8888，统一 Alpha/stride，避免 Device 端携带多种图片解码器。
bool DecodeFrameBgra(IWICImagingFactory* factory,
                     IWICBitmapFrameDecode* frame,
                     uint32_t width,
                     uint32_t height,
                     std::vector<uint8_t>& pixels,
                     std::string& error) {
    if (!factory || !frame || width == 0 || height == 0) {
        error = "invalid image frame";
        return false;
    }

    const uint64_t byte_count = static_cast<uint64_t>(width) * height * 4ull;
    if (byte_count > 0xFFFFFFFFull) {
        error = "image frame is too large for WIC";
        return false;
    }

    IWICFormatConverter* converter = nullptr;
    HRESULT hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr)) {
        SafeRelease(converter);
        error = "cannot convert image frame to BGRA";
        return false;
    }

    pixels.resize(static_cast<size_t>(byte_count));
    hr = converter->CopyPixels(nullptr, width * 4,
                               static_cast<UINT>(pixels.size()), pixels.data());
    SafeRelease(converter);
    if (FAILED(hr)) {
        pixels.clear();
        error = "cannot copy decoded image frame";
        return false;
    }
    return true;
}

void ClearCanvasRect(std::vector<uint8_t>& canvas,
                     uint32_t canvas_width,
                     uint32_t canvas_height,
                     uint32_t left,
                     uint32_t top,
                     uint32_t width,
                     uint32_t height) {
    if (canvas.empty() || left >= canvas_width || top >= canvas_height) return;
    const uint32_t right = std::min<uint32_t>(canvas_width, left + width);
    const uint32_t bottom = std::min<uint32_t>(canvas_height, top + height);
    for (uint32_t y = top; y < bottom; ++y) {
        const size_t offset = (static_cast<size_t>(y) * canvas_width + left) * 4u;
        const size_t count = static_cast<size_t>(right - left) * 4u;
        std::memset(canvas.data() + offset, 0, count);
    }
}

// GIF 当前帧只覆盖它的子区域，合成必须在独立画布上处理透明像素。
void CompositeGifFrame(std::vector<uint8_t>& canvas,
                       uint32_t canvas_width,
                       uint32_t canvas_height,
                       const std::vector<uint8_t>& source,
                       uint32_t source_width,
                       uint32_t source_height,
                       uint32_t left,
                       uint32_t top) {
    if (canvas.empty() || source.empty() || left >= canvas_width || top >= canvas_height)
        return;

    const uint32_t copy_width = std::min<uint32_t>(source_width, canvas_width - left);
    const uint32_t copy_height = std::min<uint32_t>(source_height, canvas_height - top);
    for (uint32_t y = 0; y < copy_height; ++y) {
        for (uint32_t x = 0; x < copy_width; ++x) {
            const size_t src_offset = (static_cast<size_t>(y) * source_width + x) * 4u;
            const size_t dst_offset =
                (static_cast<size_t>(top + y) * canvas_width + left + x) * 4u;
            // GIF transparency is palette based and therefore binary. Transparent
            // source pixels leave the existing canvas pixel untouched.
            if (source[src_offset + 3] == 0) continue;
            canvas[dst_offset + 0] = source[src_offset + 0];
            canvas[dst_offset + 1] = source[src_offset + 1];
            canvas[dst_offset + 2] = source[src_offset + 2];
            canvas[dst_offset + 3] = source[src_offset + 3];
        }
    }
}

// 从 GIF 帧 metadata 提取延迟和 disposal；单位转换需与 Device 的 delay_ms 匹配。
bool ReadGifFrameInfo(IWICBitmapFrameDecode* frame,
                      GifFrameInfo& info,
                      std::string& error) {
    if (!frame) return false;

    UINT source_width = 0;
    UINT source_height = 0;
    if (FAILED(frame->GetSize(&source_width, &source_height)) ||
        source_width == 0 || source_height == 0) {
        error = "cannot read GIF frame size";
        return false;
    }

    info = GifFrameInfo{};
    info.source_width = source_width;
    info.source_height = source_height;
    info.width = source_width;
    info.height = source_height;

    IWICMetadataQueryReader* reader = nullptr;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&reader)) && reader) {
        ReadMetadataUInt32(reader, L"/imgdesc/Left", info.left);
        ReadMetadataUInt32(reader, L"/imgdesc/Top", info.top);
        ReadMetadataUInt32(reader, L"/imgdesc/Width", info.width);
        ReadMetadataUInt32(reader, L"/imgdesc/Height", info.height);
        ReadMetadataUInt32(reader, L"/grctlext/Disposal", info.disposal);
        info.delay_ms = ReadFrameDelay(reader);
    }
    SafeRelease(reader);

    if (info.width == 0) info.width = source_width;
    if (info.height == 0) info.height = source_height;
    return true;
}

// 按 GIF disposal 规则合成全尺寸帧；每一帧最终都可独立写入 TX Surface。
bool DecodeGif(IWICImagingFactory* factory,
               IWICBitmapDecoder* decoder,
               UINT frame_count,
               DecodedImage& output,
               std::string& error) {
    if (frame_count == 0) {
        error = "GIF contains no frames";
        return false;
    }
    if (frame_count > demo_image_panel::kMaxFrames) {
        error = "GIF contains too many frames for the safety limit";
        return false;
    }

    uint32_t canvas_width = 0;
    uint32_t canvas_height = 0;
    IWICMetadataQueryReader* decoder_reader = nullptr;
    if (SUCCEEDED(decoder->GetMetadataQueryReader(&decoder_reader)) && decoder_reader) {
        ReadMetadataUInt32(decoder_reader, L"/logscrdesc/Width", canvas_width);
        ReadMetadataUInt32(decoder_reader, L"/logscrdesc/Height", canvas_height);
    }
    SafeRelease(decoder_reader);

    std::vector<GifFrameInfo> frame_info;
    frame_info.resize(frame_count);
    uint64_t max_right = canvas_width;
    uint64_t max_bottom = canvas_height;

    for (UINT index = 0; index < frame_count; ++index) {
        IWICBitmapFrameDecode* frame = nullptr;
        const HRESULT hr = decoder->GetFrame(index, &frame);
        if (FAILED(hr) || !frame) {
            SafeRelease(frame);
            error = "cannot read GIF frame";
            return false;
        }
        if (!ReadGifFrameInfo(frame, frame_info[index], error)) {
            SafeRelease(frame);
            return false;
        }
        SafeRelease(frame);

        const GifFrameInfo& info = frame_info[index];
        max_right = std::max<uint64_t>(max_right,
            static_cast<uint64_t>(info.left) + info.width);
        max_bottom = std::max<uint64_t>(max_bottom,
            static_cast<uint64_t>(info.top) + info.height);
    }

    // Some real-world GIFs have incomplete logical-screen metadata but valid frame
    // rectangles. Use the full required extent rather than rejecting such files.
    canvas_width = static_cast<uint32_t>(std::min<uint64_t>(max_right, 0xFFFFFFFFull));
    canvas_height = static_cast<uint32_t>(std::min<uint64_t>(max_bottom, 0xFFFFFFFFull));
    if (canvas_width == 0 || canvas_height == 0 ||
        canvas_width > demo_image_panel::kMaxWidth ||
        canvas_height > demo_image_panel::kMaxHeight) {
        error = "GIF logical canvas size is outside the supported range";
        return false;
    }

    const uint64_t canvas_bytes = static_cast<uint64_t>(canvas_width) * canvas_height * 4ull;
    const uint64_t decoded_bytes = canvas_bytes * frame_count;
    if (decoded_bytes > demo_image_panel::kMaxDecodedBytes) {
        error = "decoded GIF exceeds the demo memory safety limit";
        return false;
    }

    DecodedImage image;
    image.width = canvas_width;
    image.height = canvas_height;
    image.frames.reserve(frame_count);

    std::vector<uint8_t> canvas(static_cast<size_t>(canvas_bytes), 0);
    std::vector<uint8_t> restore_canvas;
    GifFrameInfo previous_info;
    bool have_previous = false;

    for (UINT index = 0; index < frame_count; ++index) {
        // Disposal applies after the previous frame has been displayed and before
        // the current frame is composed.
        if (have_previous) {
            if (previous_info.disposal == 2) {  // restore to background
                ClearCanvasRect(canvas, canvas_width, canvas_height,
                                previous_info.left, previous_info.top,
                                previous_info.width, previous_info.height);
            }
            else if (previous_info.disposal == 3 &&
                     restore_canvas.size() == canvas.size()) {  // restore previous
                canvas = restore_canvas;
            }
        }

        const GifFrameInfo& info = frame_info[index];
        if (info.disposal == 3) {
            restore_canvas = canvas;
        }
        else {
            restore_canvas.clear();
        }

        IWICBitmapFrameDecode* frame = nullptr;
        HRESULT hr = decoder->GetFrame(index, &frame);
        if (FAILED(hr) || !frame) {
            SafeRelease(frame);
            error = "cannot decode GIF frame";
            return false;
        }

        std::vector<uint8_t> source;
        if (!DecodeFrameBgra(factory, frame, info.source_width, info.source_height,
                             source, error)) {
            SafeRelease(frame);
            return false;
        }
        SafeRelease(frame);

        uint32_t dest_left = info.left;
        uint32_t dest_top = info.top;
        // Some WIC GIF decoders expose a full-canvas bitmap for a frame while the
        // GIF metadata still describes the original sub-rectangle. In that case
        // the decoded pixels are already canvas-relative.
        if (info.source_width == canvas_width && info.source_height == canvas_height &&
            (info.left != 0 || info.top != 0 ||
             info.width != canvas_width || info.height != canvas_height)) {
            dest_left = 0;
            dest_top = 0;
        }

        CompositeGifFrame(canvas, canvas_width, canvas_height, source,
                          info.source_width, info.source_height, dest_left, dest_top);

        DecodedFrame decoded;
        decoded.delay_ms = info.delay_ms;
        decoded.pixels = canvas;
        image.frames.push_back(std::move(decoded));

        previous_info = info;
        have_previous = true;
    }

    output = std::move(image);
    return true;
}

bool DecodeStillImage(IWICImagingFactory* factory,
                      IWICBitmapDecoder* decoder,
                      DecodedImage& output,
                      std::string& error) {
    IWICBitmapFrameDecode* frame = nullptr;
    HRESULT hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
        SafeRelease(frame);
        error = "cannot decode image";
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    hr = frame->GetSize(&width, &height);
    if (FAILED(hr) || width == 0 || height == 0 ||
        width > demo_image_panel::kMaxWidth || height > demo_image_panel::kMaxHeight) {
        SafeRelease(frame);
        error = "image size is outside the supported range";
        return false;
    }

    const uint64_t bytes = static_cast<uint64_t>(width) * height * 4ull;
    if (bytes > demo_image_panel::kMaxDecodedBytes) {
        SafeRelease(frame);
        error = "decoded image exceeds the demo memory safety limit";
        return false;
    }

    DecodedFrame decoded;
    decoded.delay_ms = 100;
    if (!DecodeFrameBgra(factory, frame, width, height, decoded.pixels, error)) {
        SafeRelease(frame);
        return false;
    }
    SafeRelease(frame);

    DecodedImage image;
    image.width = width;
    image.height = height;
    image.frames.push_back(std::move(decoded));
    output = std::move(image);
    return true;
}

// 调用者只得到统一的完整 BGRA 帧集合，文件扩展名与图像解码细节留在 Host。
bool DecodeImage(const wchar_t* path, DecodedImage& output, std::string& error) {
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        error = "cannot create Windows Imaging Component";
        return false;
    }

    hr = factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) {
        SafeRelease(factory);
        error = "only valid PNG/GIF files are accepted";
        return false;
    }

    GUID container = GUID_NULL;
    hr = decoder->GetContainerFormat(&container);
    if (FAILED(hr) ||
        (!IsEqualGUID(container, GUID_ContainerFormatGif) &&
         !IsEqualGUID(container, GUID_ContainerFormatPng))) {
        SafeRelease(decoder);
        SafeRelease(factory);
        error = "only PNG and GIF files are accepted";
        return false;
    }

    UINT frame_count = 0;
    hr = decoder->GetFrameCount(&frame_count);
    if (FAILED(hr) || frame_count == 0) {
        SafeRelease(decoder);
        SafeRelease(factory);
        error = "image contains no decodable frames";
        return false;
    }

    bool ok = false;
    if (IsEqualGUID(container, GUID_ContainerFormatGif)) {
        ok = DecodeGif(factory, decoder, frame_count, output, error);
    }
    else {
        ok = DecodeStillImage(factory, decoder, output, error);
    }

    SafeRelease(decoder);
    SafeRelease(factory);
    return ok;
}

// ImageBegin/FrameBegin/FrameChunk/ImageEnd 是一次完整传输；数据按 kChunkBytes 分包，兼顾 IPC 数据报上限。
bool UploadImage(PanelState* state) {
    HostComponent* component = state->component;
    if (!component->channel_ready.load() || !component->device_ready.load()) {
        MessageBoxA(nullptr, "The Device Component is not ready.", "Kavio App Panel", MB_ICONWARNING);
        return false;
    }
    const uint64_t transfer = state->next_transfer_id++;
    demo_image_panel::ImageBeginMessage begin{};
    demo_image_panel::Initialize(begin, demo_image_panel::MessageType::ImageBegin);
    begin.transfer_id = transfer; begin.width = state->image.width; begin.height = state->image.height;
    begin.frame_count = static_cast<uint32_t>(state->image.frames.size());
    if (!SendMessage(component, begin)) return false;

    for (uint32_t index = 0; index < state->image.frames.size(); ++index) {
        const DecodedFrame& frame = state->image.frames[index];
        demo_image_panel::FrameBeginMessage frame_begin{};
        demo_image_panel::Initialize(frame_begin, demo_image_panel::MessageType::FrameBegin);
        frame_begin.transfer_id = transfer; frame_begin.frame_index = index;
        frame_begin.delay_ms = frame.delay_ms;
        frame_begin.data_size = static_cast<uint32_t>(frame.pixels.size());
        if (!SendMessage(component, frame_begin)) return false;

        uint32_t offset = 0;
        while (offset < frame.pixels.size()) {
            const uint32_t count = std::min<uint32_t>(demo_image_panel::kChunkBytes,
                static_cast<uint32_t>(frame.pixels.size() - offset));
            demo_image_panel::FrameChunkMessage chunk{};
            demo_image_panel::Initialize(chunk, demo_image_panel::MessageType::FrameChunk);
            chunk.header.size = static_cast<uint32_t>(sizeof(chunk) + count);
            chunk.transfer_id = transfer; chunk.frame_index = index;
            chunk.offset = offset; chunk.data_size = count;
            std::vector<uint8_t> packet(sizeof(chunk) + count);
            std::memcpy(packet.data(), &chunk, sizeof(chunk));
            std::memcpy(packet.data() + sizeof(chunk), frame.pixels.data() + offset, count);
            if (!SendRaw(component, packet.data(), packet.size())) return false;
            offset += count;
        }
    }
    demo_image_panel::ImageEndMessage end{};
    demo_image_panel::Initialize(end, demo_image_panel::MessageType::ImageEnd);
    end.transfer_id = transfer;
    if (!SendMessage(component, end)) return false;

    demo_image_panel::PositionMessage position{};
    demo_image_panel::Initialize(position, demo_image_panel::MessageType::Position);
    position.x = state->x; position.y = state->y;
    return SendMessage(component, position);
}

// 只发送拖动坐标，不重复上传像素；Device 通过 dest_pos 修改图层位置。
void SendPosition(PanelState* state) {
    demo_image_panel::PositionMessage message{};
    demo_image_panel::Initialize(message, demo_image_panel::MessageType::Position);
    message.x = state->x; message.y = state->y;
    SendMessage(state->component, message);
}

void DrawChecker(HDC dc, const RECT& rect) {
    const int size = 16;
    HBRUSH light = CreateSolidBrush(RGB(230,230,230));
    HBRUSH dark = CreateSolidBrush(RGB(195,195,195));
    for (int y = rect.top; y < rect.bottom; y += size) {
        for (int x = rect.left; x < rect.right; x += size) {
            RECT cell{x, y, (std::min)(x+size, (int)rect.right),
                      (std::min)(y+size, (int)rect.bottom)};
            FillRect(dc, &cell, (((x/size)+(y/size)) & 1) ? dark : light);
        }
    }
    DeleteObject(light);
    DeleteObject(dark);
}

void DestroyBackBuffer(PanelState* state) {
    if (!state) return;
    if (state->back_dc && state->back_old_bitmap) {
        SelectObject(state->back_dc, state->back_old_bitmap);
    }
    if (state->back_bitmap) DeleteObject(state->back_bitmap);
    if (state->back_dc) DeleteDC(state->back_dc);
    state->back_dc = nullptr;
    state->back_bitmap = nullptr;
    state->back_old_bitmap = nullptr;
    state->back_width = 0;
    state->back_height = 0;
}

// 只在窗口尺寸变化时重建双缓冲，避免每次 WM_PAINT 重复分配 GDI 位图。
bool EnsureBackBuffer(HDC reference_dc, PanelState* state, int width, int height) {
    if (!state || width <= 0 || height <= 0) return false;
    if (state->back_dc && state->back_bitmap &&
        state->back_width == width && state->back_height == height) return true;

    DestroyBackBuffer(state);
    state->back_dc = CreateCompatibleDC(reference_dc);
    state->back_bitmap = CreateCompatibleBitmap(reference_dc, width, height);
    if (!state->back_dc || !state->back_bitmap) {
        DestroyBackBuffer(state);
        return false;
    }
    state->back_old_bitmap = SelectObject(state->back_dc, state->back_bitmap);
    state->back_width = width;
    state->back_height = height;
    return true;
}

void DestroyImageBitmap(PanelState* state) {
    if (!state) return;
    if (state->image_dc && state->image_old_bitmap) {
        SelectObject(state->image_dc, state->image_old_bitmap);
    }
    if (state->image_bitmap) DeleteObject(state->image_bitmap);
    if (state->image_dc) DeleteDC(state->image_dc);
    state->image_dc = nullptr;
    state->image_bitmap = nullptr;
    state->image_old_bitmap = nullptr;
    state->image_bits = nullptr;
    state->image_bitmap_width = 0;
    state->image_bitmap_height = 0;
    state->cached_frame_index = static_cast<size_t>(-1);
}

void InvalidateImageBitmap(PanelState* state) {
    if (state) state->cached_frame_index = static_cast<size_t>(-1);
}

// 将当前 BGRA 帧转换为可绘制位图，并按帧代数复用，避免无变化时反复上传。
bool EnsureImageBitmap(HDC reference_dc, PanelState* state) {
    if (!state || state->image.frames.empty()) return false;
    const uint32_t width = state->image.width;
    const uint32_t height = state->image.height;
    if (width == 0 || height == 0) return false;

    if (!state->image_dc || !state->image_bitmap ||
        state->image_bitmap_width != width || state->image_bitmap_height != height) {
        DestroyImageBitmap(state);

        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(width);
        info.bmiHeader.biHeight = -static_cast<LONG>(height);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        state->image_dc = CreateCompatibleDC(reference_dc);
        state->image_bitmap = CreateDIBSection(reference_dc, &info, DIB_RGB_COLORS,
                                               &state->image_bits, nullptr, 0);
        if (!state->image_dc || !state->image_bitmap || !state->image_bits) {
            DestroyImageBitmap(state);
            return false;
        }
        state->image_old_bitmap = SelectObject(state->image_dc, state->image_bitmap);
        state->image_bitmap_width = width;
        state->image_bitmap_height = height;
    }

    const size_t current_index = state->frame_index % state->image.frames.size();
    if (state->cached_frame_index != current_index) {
        const DecodedFrame& frame = state->image.frames[current_index];
        uint8_t* target = static_cast<uint8_t*>(state->image_bits);
        // AlphaBlend expects premultiplied BGRA. Keep the source frames in straight
        // BGRA for device transfer and only premultiply the currently displayed frame.
        for (size_t i = 0; i < frame.pixels.size(); i += 4) {
            const uint32_t alpha = frame.pixels[i+3];
            target[i+0] = static_cast<uint8_t>((frame.pixels[i+0] * alpha) / 255u);
            target[i+1] = static_cast<uint8_t>((frame.pixels[i+1] * alpha) / 255u);
            target[i+2] = static_cast<uint8_t>((frame.pixels[i+2] * alpha) / 255u);
            target[i+3] = static_cast<uint8_t>(alpha);
        }
        state->cached_frame_index = current_index;
    }
    return true;
}

void DrawImage(HDC dc, PanelState* state) {
    if (!EnsureImageBitmap(dc, state)) return;
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(dc, state->x, state->y,
               static_cast<int>(state->image.width), static_cast<int>(state->image.height),
               state->image_dc, 0, 0,
               static_cast<int>(state->image.width), static_cast<int>(state->image.height), blend);
}

// 先绘制背景与当前图像，再叠加拖拽边框；需要双缓冲避免拖动闪烁。
void PaintPanel(HWND window, HDC dc, PanelState* state) {
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return;

    if (!EnsureBackBuffer(dc, state, width, height)) {
        DrawChecker(dc, client);
        DrawImage(dc, state);
        return;
    }

    RECT buffer_rect{0, 0, width, height};
    DrawChecker(state->back_dc, buffer_rect);
    DrawImage(state->back_dc, state);
    BitBlt(dc, 0, 0, width, height, state->back_dc, 0, 0, SRCCOPY);
}

void UpdateTitle(HWND window, const PanelState* state, const wchar_t* suffix = nullptr) {
    wchar_t title[256] = L"Kavio App Panel Image Demo - drop PNG/GIF";
    if (!state->image.frames.empty()) {
        swprintf_s(title, L"Kavio App Panel Image Demo - %ux%u, %u frame(s), position %d,%d",
                   state->image.width, state->image.height,
                   static_cast<unsigned>(state->image.frames.size()), state->x, state->y);
    }
    if (suffix) { wcscat_s(title, L" - "); wcscat_s(title, suffix); }
    SetWindowTextW(window, title);
}

LRESULT CALLBACK PanelWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    PanelState* state = reinterpret_cast<PanelState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<PanelState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(window, message, wparam, lparam);

    switch (message) {
    case WM_DROPFILES: {
        KLOGI("App Panel received a file drop");
        HDROP drop = reinterpret_cast<HDROP>(wparam);
        wchar_t path[MAX_PATH] = {};
        if (DragQueryFileW(drop, 0, path, MAX_PATH)) {
            DecodedImage image;
            std::string error;
            if (DecodeImage(path, image, error)) {
                state->image = std::move(image);
                state->frame_index = 0;
                state->x = 0;
                state->y = 0;
                InvalidateImageBitmap(state);
                state->next_frame_tick = GetTickCount64() +
                    std::max<uint32_t>(20, state->image.frames[0].delay_ms);
                if (UploadImage(state)) {
                    KLOGI("App Panel image decoded and uploaded successfully");
                    UpdateTitle(window, state);
                }
                else {
                    KLOGE("App Panel image upload failed");
                    UpdateTitle(window, state, L"upload failed");
                }
                InvalidateRect(window, nullptr, FALSE);
            }
            else {
                KLOGE("{}", error);
                MessageBoxA(window, error.c_str(), "Kavio App Panel", MB_ICONERROR);
            }
        }
        DragFinish(drop);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (!state->image.frames.empty()) {
            const int mx = GET_X_LPARAM(lparam);
            const int my = GET_Y_LPARAM(lparam);
            if (mx >= state->x && my >= state->y &&
                mx < state->x + static_cast<int>(state->image.width) &&
                my < state->y + static_cast<int>(state->image.height)) {
                state->dragging = true;
                state->drag_offset_x = mx - state->x;
                state->drag_offset_y = my - state->y;
                SetCapture(window);
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (state->dragging && (wparam & MK_LBUTTON)) {
            state->x = std::max(0, GET_X_LPARAM(lparam) - state->drag_offset_x);
            state->y = std::max(0, GET_Y_LPARAM(lparam) - state->drag_offset_y);
            SendPosition(state);
            UpdateTitle(window, state);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (state->dragging) {
            state->dragging = false;
            ReleaseCapture();
        }
        return 0;
    case WM_TIMER:
        if (state->image.frames.size() > 1) {
            const ULONGLONG now = GetTickCount64();
            if (now >= state->next_frame_tick) {
                state->frame_index = (state->frame_index + 1) % state->image.frames.size();
                InvalidateImageBitmap(state);
                state->next_frame_tick = now + std::max<uint32_t>(
                    20, state->image.frames[state->frame_index].delay_ms);
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        return 0;
    case WM_ERASEBKGND:
        // WM_PAINT draws the entire client area from the back buffer. Reporting the
        // erase as handled prevents the default background pass that causes flicker.
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        PaintPanel(window, dc, state);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_SIZE:
        // The next WM_PAINT recreates the back buffer to the new client dimensions.
        DestroyBackBuffer(state);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return 0;
    case WM_APP + 1:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        DestroyImageBitmap(state);
        DestroyBackBuffer(state);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

// App Panel 使用独立 UI 线程；窗口消息循环与 Host 通道回调分离。
void PanelThreadMain(HostComponent* component) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    PanelState state;
    state.component = component;

    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = PanelWindowProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
    cls.lpszClassName = L"KavioDemoAppPanelImage";
    // No class background brush: WM_PAINT owns the entire client background.
    cls.hbrBackground = nullptr;
    RegisterClassExW(&cls);

    HWND window = CreateWindowExW(WS_EX_ACCEPTFILES, cls.lpszClassName,
        L"Kavio App Panel Image Demo - drop PNG/GIF", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 650, nullptr, nullptr, cls.hInstance, &state);
    {
        std::lock_guard<std::mutex> lock(component->panel_mutex);
        component->panel_window = window;
    }
    if (window) {
        const bool filter_enabled = EnableShellFileDrop(window);
        if (!filter_enabled) {
            KLOGW("App Panel could not relax UIPI file-drop message filtering; "
                   "drag/drop may fail when KavioAppHost is elevated");
        }
        else {
            KLOGI("App Panel file-drop handling is ready");
        }
        SetTimer(window, 1, 16, nullptr);
    }
    SetEvent(component->panel_ready);

    MSG msg{};
    while (window && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    {
        std::lock_guard<std::mutex> lock(component->panel_mutex);
        component->panel_window = nullptr;
    }
    UnregisterClassW(cls.lpszClassName, cls.hInstance);
    CoUninitialize();
}

}  // namespace


KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostCreate(
    const KavioHostCreateParams* params, KavioHostHandle* output) {
    if (!params || !output || !params->host ||
        params->struct_size < KAVIO_HOST_CREATE_PARAMS_BASE_SIZE ||
        params->host->struct_size < KAVIO_HOST_API_BASE_SIZE) return KavioResult_InvalidArgument;
    HostComponent* component = new HostComponent();
    component->host = params->host;
    component->panel_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!component->panel_ready) { delete component; return KavioResult_InternalError; }
    *output = component;
    return KavioResult_Success;
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostStart(KavioHostHandle handle) {
    HostComponent* component = static_cast<HostComponent*>(handle);
    if (!component) return KavioResult_InvalidArgument;
    component->running.store(true);
    return KavioResult_Success;
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostPrepareStop(KavioHostHandle) {
    return KavioResult_Success;
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostStop(KavioHostHandle handle) {
    HostComponent* component = static_cast<HostComponent*>(handle);
    if (!component) return KavioResult_InvalidArgument;
    component->running.store(false);
    HWND window = nullptr;
    {
        std::lock_guard<std::mutex> lock(component->panel_mutex);
        window = component->panel_window;
    }
    if (window) PostMessageW(window, WM_APP + 1, 0, 0);
    if (component->panel_thread.joinable()) component->panel_thread.join();
    return KavioResult_Success;
}

KAVIO_HOST_ENTRY void KAVIO_HOST_CALL KavioHostDestroy(KavioHostHandle handle) {
    HostComponent* component = static_cast<HostComponent*>(handle);
    if (!component) return;
    KavioHostStop(component);
    if (component->panel_ready) CloseHandle(component->panel_ready);
    delete component;
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostOnData(
    KavioHostHandle handle, const void* data, size_t size) {
    HostComponent* component = static_cast<HostComponent*>(handle);
    if (!component || !data || size != sizeof(demo_image_panel::DeviceReadyMessage))
        return KavioResult_InvalidArgument;
    demo_image_panel::DeviceReadyMessage message{};
    std::memcpy(&message, data, sizeof(message));
    if (!demo_image_panel::IsValid(
            message.header, demo_image_panel::MessageType::DeviceReady, size))
        return KavioResult_InvalidArgument;
    component->device_ready.store(true);
    return KavioResult_Success;
}

KAVIO_HOST_ENTRY void KAVIO_HOST_CALL KavioHostOnDeviceState(
    KavioHostHandle handle, uint32_t ready) {
    HostComponent* component = static_cast<HostComponent*>(handle);
    if (component) {
        component->channel_ready.store(ready != 0);
        if (!ready) component->device_ready.store(false);
    }
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostOpenPanel(
    KavioHostHandle handle) {
    HostComponent* component = static_cast<HostComponent*>(handle);
    if (!component || !component->running.load()) return KavioResult_NotReady;
    HWND window = nullptr;
    {
        std::lock_guard<std::mutex> lock(component->panel_mutex);
        window = component->panel_window;
        if (!window && !component->panel_thread.joinable()) {
            ResetEvent(component->panel_ready);
            component->panel_thread = std::thread(PanelThreadMain, component);
        }
    }
    if (!window) {
        WaitForSingleObject(component->panel_ready, 3000);
        std::lock_guard<std::mutex> lock(component->panel_mutex);
        window = component->panel_window;
    }
    if (!window) return KavioResult_InternalError;
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    return KavioResult_Success;
}
