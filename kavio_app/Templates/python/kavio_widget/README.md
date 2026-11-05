# {{APP_NAME}} — Python Kavio Widget Template

## Recommended Python starting point

Choose this for normal Python applications with labels, buttons, images, sliders, switches, settings pages, and other standard interactive UI.

```text
Python App -> Kavio Widget -> SDK LVGL backend -> Presentation
```

You program against `kavio.ui`; the LVGL backend is owned by the SDK. Python does not call raw LVGL C APIs.

## Create your app

```bash
./kavio new MyApp --template python/kavio_widget --app-id com.example.my-app
./kavio build MyApp
./kavio pack MyApp
```

## `sdk.modules`

This template contains:

```json
"sdk": { "modules": ["ui"] }
```

The tool automatically deploys `app_runtime`, Python core binding, UI runtime, LVGL backend and Python UI binding. Do not list those internal dependencies manually.

## What to learn from `main.py`

Use `App`, `ui.State`, layouts and widgets. Widget/device callbacks are dispatched on the App run thread. Do not create a second IPC Client or manipulate LVGL from another thread.

If your application owns the whole rendering pipeline instead, choose `python/framebuffer`.
