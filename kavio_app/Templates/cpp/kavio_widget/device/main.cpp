#include <stdexcept>
#include <string>

#include <kavio/App.h>
#include <kavio/AppContext.h>
#include <kavio/ui/Layout.h>
#include <kavio/ui/LvglBackend.h>
#include <kavio/ui/State.h>
#include <kavio/ui/Widgets.h>

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
    kavio::ui::State<std::string> state("Ready");
    kavio::ui::Label status(state.Get());
    kavio::ui::Slider slider(0, 100, 50);
    kavio::ui::Switch toggle(false);
    kavio::ui::Button button("Press me");

    state.Observe([&](const std::string& value) {
        status.SetText(value);
    });
    button.OnClick([&] {
        state.Set("Button clicked");
    });
    slider.OnChange([&](int value) {
        state.Set("Slider: " + std::to_string(value));
    });
    toggle.OnChange([&](bool enabled) {
        state.Set(enabled ? "Switch: on" : "Switch: off");
    });

    kavio::ui::Column root;
    root.Add(kavio::ui::Label("{{APP_NAME}}"))
        .Add(status)
        .Add(button)
        .Add(slider)
        .Add(toggle);

    app.on_ready([&](kavio::AppContext& context) {
        Require(backend.Initialize(context.presentation()), "Initialize LVGL backend");
        Require(backend.SetContent(root), "Set widget content");
    });
    app.on_tick([&](kavio::AppContext&) {
        backend.Process();
    });
    app.on_stopping([&](kavio::AppContext&) {
        backend.Shutdown();
    });

    return app.run();
}
