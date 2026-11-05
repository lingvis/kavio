#pragma once

#include <variant>

#include <kavio/ui/Layout.h>
#include <kavio/ui/Widgets.h>

struct KavioWidget {
    using Value = std::variant<
        kavio::ui::Page,
        kavio::ui::Column,
        kavio::ui::Row,
        kavio::ui::Label,
        kavio::ui::Button,
        kavio::ui::Image,
        kavio::ui::Slider,
        kavio::ui::Switch>;

    explicit KavioWidget(Value value) : value(std::move(value)) {}
    Value value;
};

inline kavio::ui::Widget& KavioWidgetBase(KavioWidget& widget)
{
    return std::visit([](auto& value) -> kavio::ui::Widget& { return value; }, widget.value);
}
