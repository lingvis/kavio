#pragma once

/** Base types for retained UI widgets and their shared lifetime. */

#include <functional>
#include <memory>
#include <string>

#include <kavio/ui/Style.h>
#include <kavio/ui/UiApi.h>

namespace kavio::ui {
namespace detail { struct WidgetNode; }
class LvglBackend;

enum class WidgetType {
    kContainer,
    kColumn,
    kRow,
    kLabel,
    kButton,
    kImage,
    kSlider,
    kSwitch,
};

/** Retained, backend-independent UI object used by Kavio Widget backends. */
class KAVIO_UI_API Widget {
public:
    Widget();
    virtual ~Widget();

    Widget(const Widget&) = default;
    Widget& operator=(const Widget&) = default;
    Widget(Widget&&) noexcept = default;
    Widget& operator=(Widget&&) noexcept = default;

    /** Returns false for a default-constructed widget without a UI node. */
    bool IsValid() const noexcept;
    WidgetType Type() const noexcept;

    /** Applies appearance properties to this widget. */
    Widget& SetStyle(const Style& style);
    const Style& GetStyle() const;
    /** Attaches a child; the underlying node stays alive while referenced. */
    Widget& Add(const Widget& child);

protected:
    explicit Widget(WidgetType type);
    std::shared_ptr<detail::WidgetNode> node_;

private:
    friend class LvglBackend;
};

}  // namespace kavio::ui
