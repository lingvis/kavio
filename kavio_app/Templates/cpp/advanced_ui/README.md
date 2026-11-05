# {{APP_NAME}} — C++ Advanced UI Template

## When to choose this template

Choose this only when the public Kavio Widget abstraction cannot express a specialized C++ interface: LVGL Chart/Canvas, custom draw, a highly specialized control, or another feature that requires the native LVGL API.

For ordinary UI, use `cpp/kavio_widget`. For a completely independent renderer, use `cpp/framebuffer`.

## UI path

```text
App -> SDK LvglBackend -> the same SDK-owned LVGL runtime -> raw LVGL objects -> Presentation
```

The important rule is **reuse**: this template uses the LVGL runtime already owned by the SDK backend. It must not call `lv_init()`, create another LVGL display, or create another AppView.

## Create your app

```bash
./kavio new MyApp --template cpp/advanced_ui --app-id com.example.my-app
./kavio build MyApp
./kavio pack MyApp
```

## What to learn from `main.cpp`

1. Initialize the SDK `LvglBackend` from the App Presentation.
2. Use `Screen()`, `Display()` and `MakeDefault()` only after backend initialization.
3. Create raw LVGL objects on that existing screen/runtime.
4. Drive the backend from the App run thread.
5. Shut down the backend before Presentation teardown.

## Why Python and Lua do not have this template

The current public Python/Lua SDK does not expose raw `lv_obj_t*` or the LVGL C API. Their custom-renderer route is already covered by the FrameBuffer template, so duplicating an `advanced_ui` template would add no distinct capability.
