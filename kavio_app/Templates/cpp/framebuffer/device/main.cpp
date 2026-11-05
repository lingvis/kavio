#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <kavio/App.h>
#include <kavio/AppContext.h>
#include <kavio/presentation/Presentation.h>

namespace {

void Require(const kavio::Status& status, const char* operation)
{
    if (status.ok()) return;
    throw std::runtime_error(
        std::string(operation) + " failed: " + status.name() +
        (status.message.empty() ? "" : " - " + status.message));
}

void DrawBackground(kavio::Presentation& presentation)
{
    kavio::FrameLease frame;
    Require(presentation.AcquireFrame(frame), "AcquireFrame");

    const kavio::FrameBufferInfo info = frame.Info();
    uint8_t* pixels = frame.Data();
    if (!pixels || info.pixel_format != kavio::PixelFormat::kBGRA8888) {
        (void)frame.Discard();
        throw std::runtime_error("AppView is not a writable BGRA8888 frame buffer");
    }

    for (uint32_t y = 0; y < info.height; ++y) {
        uint8_t* row = pixels + static_cast<size_t>(y) * info.stride;
        for (uint32_t x = 0; x < info.width; ++x) {
            const size_t offset = static_cast<size_t>(x) * 4U;
            row[offset + 0] = static_cast<uint8_t>(
                (x * 255U) / std::max(1U, info.width - 1U));
            row[offset + 1] = static_cast<uint8_t>(
                (y * 255U) / std::max(1U, info.height - 1U));
            row[offset + 2] = 48U;
            row[offset + 3] = 255U;
        }
    }

    Require(frame.Present(), "Present background");
}

void DrawTouchMarker(kavio::Presentation& presentation, int marker_x, int marker_y)
{
    kavio::FrameLease frame;
    Require(presentation.AcquireFrame(frame), "AcquireFrame");

    const kavio::FrameBufferInfo info = frame.Info();
    uint8_t* pixels = frame.Data();
    if (!pixels) {
        (void)frame.Discard();
        throw std::runtime_error("AppView frame buffer is not writable");
    }

    const int x0 = std::max(0, marker_x - 18);
    const int y0 = std::max(0, marker_y - 18);
    const int x1 = std::min<int>(static_cast<int>(info.width), marker_x + 18);
    const int y1 = std::min<int>(static_cast<int>(info.height), marker_y + 18);

    for (int y = y0; y < y1; ++y) {
        uint8_t* row = pixels + static_cast<size_t>(y) * info.stride;
        for (int x = x0; x < x1; ++x) {
            const size_t offset = static_cast<size_t>(x) * 4U;
            row[offset + 0] = 255U;
            row[offset + 1] = 255U;
            row[offset + 2] = 255U;
            row[offset + 3] = 255U;
        }
    }

    const kavio::Rect dirty{
        x0,
        y0,
        static_cast<uint32_t>(x1 - x0),
        static_cast<uint32_t>(y1 - y0),
    };
    Require(frame.Present(&dirty, 1U), "Present touch marker");
}

}  // namespace

int main()
{
    kavio::App app;
    app.on_ready([](kavio::AppContext& context) {
        // Demonstrate cancelling a write transaction before publishing the first custom frame.
        kavio::FrameLease discarded;
        Require(context.presentation().AcquireFrame(discarded), "Acquire discarded frame");
        Require(discarded.Discard(), "Discard frame");

        DrawBackground(context.presentation());
        context.presentation().SetTouchHandler(
            [&presentation = context.presentation()](const kavio::TouchFrame& frame) {
                for (const auto& point : frame.points) {
                    if (point.active && point.changed) {
                        DrawTouchMarker(presentation, point.x, point.y);
                        break;
                    }
                }
            });
    });

    return app.run();
}
