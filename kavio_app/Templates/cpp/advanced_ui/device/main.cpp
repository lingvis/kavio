#include <cmath>
#include <stdexcept>
#include <string>

#include <kavio/App.h>
#include <kavio/AppContext.h>
#include <kavio/ui/Layout.h>
#include <kavio/ui/LvglBackend.h>
#include <kavio/ui/Widgets.h>

#include <lvgl.h>

namespace {

void Require(const kavio::Status& status, const char* operation)
{
    if (status.ok()) return;
    throw std::runtime_error(
        std::string(operation) + " failed: " + status.name() +
        (status.message.empty() ? "" : " - " + status.message));
}

}  // namespace

int main()
{
    kavio::App app;
    kavio::ui::LvglBackend backend;
    lv_obj_t* chart = nullptr;
    lv_chart_series_t* series = nullptr;
    int phase = 0;

    app.on_ready([&](kavio::AppContext& context) {
        Require(backend.Initialize(context.presentation()), "Initialize LVGL backend");

        kavio::ui::Column public_widgets;
        public_widgets.Add(kavio::ui::Label("Public Widget + Native LVGL"));
        Require(backend.SetContent(public_widgets), "Set public widget content");
        backend.MakeDefault();

        lv_obj_t* screen = backend.Screen();
        if (!screen) {
            throw std::runtime_error("LVGL backend did not expose an active screen");
        }

        lv_obj_t* title = lv_label_create(screen);
        lv_label_set_text(title, "Advanced UI / Native LVGL");
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

        chart = lv_chart_create(screen);
        lv_obj_set_size(chart, 900, 420);
        lv_obj_align(chart, LV_ALIGN_CENTER, 0, 30);
        lv_chart_set_point_count(chart, 80);
        series = lv_chart_add_series(
            chart,
            lv_palette_main(LV_PALETTE_BLUE),
            LV_CHART_AXIS_PRIMARY_Y);
        for (int i = 0; i < 80; ++i) {
            lv_chart_set_next_value(chart, series, 50);
        }

        lv_obj_t* button = lv_button_create(screen);
        lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -28);
        lv_obj_t* label = lv_label_create(button);
        lv_label_set_text(label, "Native LVGL control");
        lv_obj_center(label);
    });

    app.on_tick([&](kavio::AppContext&) {
        if (chart && series) {
            const int value = 50 + static_cast<int>(35.0 * std::sin(phase * 0.12));
            lv_chart_set_next_value(chart, series, value);
            ++phase;
        }
        backend.Process();
    });

    app.on_stopping([&](kavio::AppContext&) {
        backend.Shutdown();
        chart = nullptr;
        series = nullptr;
    });

    return app.run();
}
