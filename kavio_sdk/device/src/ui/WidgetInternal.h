#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <kavio/ui/Widget.h>

namespace kavio::ui::detail {

struct WidgetNode {
    explicit WidgetNode(WidgetType widget_type) : type(widget_type) {}

    WidgetType type = WidgetType::kContainer;
    Style style;
    std::string text;
    std::string source;
    // RGB565 pixels are owned by the widget; no caller-owned buffer is retained.
    std::vector<uint16_t> rgb565;
    uint32_t image_width = 0;
    uint32_t image_height = 0;
    std::vector<std::shared_ptr<WidgetNode>> children;
    std::function<void()> on_click;
    std::function<void(int32_t)> on_value;
    std::function<void(bool)> on_checked;
    int32_t minimum = 0;
    int32_t maximum = 100;
    int32_t value = 0;
    bool checked = false;

    // Backend-owned mount state. UI object mutation calls refresh only while
    // this node is mounted by a backend.
    void* native = nullptr;
    std::function<void(WidgetNode&)> refresh;
};

inline void Refresh(WidgetNode& node)
{
    if (node.refresh) node.refresh(node);
}

}  // namespace kavio::ui::detail
