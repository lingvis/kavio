# {{APP_NAME}} — C++ Kavio Widget Template

## Recommended starting point

This is the default template for most Kavio applications. Choose it for text, buttons, images, sliders, switches, forms, settings pages, navigation, and normal interactive UI.

The public programming model is the Kavio Widget API. The SDK currently renders those widgets through its LVGL backend, but ordinary application code should treat LVGL as an implementation detail.

## UI path

```text
App -> Kavio Widget -> SDK LVGL backend -> Presentation -> AppView
```

The template links the public UI target in its CMakeLists. The build tool detects the resulting runtime dependency and includes the UI libraries automatically. No SDK module declaration is required.

## Create your app

```bash
./kavio new MyApp --template cpp/kavio_widget --app-id com.example.my-app
./kavio build MyApp
./kavio pack MyApp
```

## Project structure

```text
MyApp/
├── kavio_project.json
├── icon.png
├── schema_description.json
├── README.md
└── device/
    ├── CMakeLists.txt
    └── main.cpp
```

## What to learn from `main.cpp`

The template demonstrates the normal application flow:

1. Create `kavio::App`.
2. Create Kavio Widget objects such as `Column`, `Label`, `Button`, `Slider` and `Switch`.
3. Use `State` or callbacks to update visible state.
4. Let the App/Widget runtime own Session, AppView, LVGL display setup, touch routing and frame presentation.

For a normal Widget application you should **not** call `lv_init()`, create an LVGL display, create another AppView, or create another `KavioClient`.

## When this template is not enough

If a specialized C++ screen needs raw LVGL objects such as Chart/Canvas/custom draw, start from `cpp/advanced_ui`. If another renderer owns the entire frame, start from `cpp/framebuffer`.
