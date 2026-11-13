# {{APP_NAME}} — C++ FrameBuffer Template

## When to choose this template

Use this template when your application already owns its rendering pipeline or needs direct pixel access: games, video, OpenCV, Godot, a custom GUI engine, or another renderer that can draw into a BGRA framebuffer.

Do **not** choose this template for ordinary buttons, labels, forms, or settings pages. Use `cpp/kavio_widget` for those.

## Rendering path

```text
App -> Presentation -> FrameLease -> BGRA buffer -> your renderer -> Present
```


## Create your app

From the repository root:

```bash
./kavio new MyApp --template cpp/framebuffer --app-id com.example.my-app
./kavio build MyApp
./kavio pack MyApp
```

`new` copies this template, creates a real `kavio_project.json`, assigns the App ID/name, and creates a unique CMake target/device entry.

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

1. Create `kavio::App`.
2. Obtain `context.presentation()` in `on_ready`.
3. Call `AcquireFrame()`.
4. Respect `width`, `height` and `stride`; never assume tightly packed rows.
5. Write BGRA8888 pixels directly to the returned buffer.
6. Use `Present()` or dirty rectangles.
7. Use `Discard()` when an acquired frame should not be published.
8. Register raw touch input only if your renderer needs it.

## Extending the project

Keep rendering code separate from Kavio lifecycle code. If you integrate OpenCV or a game engine, prefer letting that renderer consume the SDK framebuffer directly. Add an extra full-frame copy only when the external renderer cannot target externally supplied memory.

## Next choice

If you later need ordinary UI controls, do not build a second UI system on top of this template. Consider creating the application from `cpp/kavio_widget` instead.
