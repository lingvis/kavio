# {{APP_NAME}} — Python FrameBuffer Template

## When to choose this template

Use this template when Python coordinates a renderer that should draw directly into the Kavio AppView framebuffer. Typical cases are image processing, video visualization, a native extension, or another renderer that can consume a Python buffer/memoryview.

For ordinary UI, use `python/kavio_widget`.

## Rendering path

```text
Python App -> Presentation -> FrameLease -> writable memoryview -> renderer -> Present
```

The buffer protocol exposes the SDK framebuffer directly; do not create a second full-frame Python image unless an external library requires it. Avoid high-frequency per-pixel Python loops. Prefer row/bulk operations or a native renderer.

## Create your app

```bash
./kavio new MyApp --template python/framebuffer --app-id com.example.my-app
./kavio build MyApp
./kavio pack MyApp
```

## What to learn from `main.py`

1. Run the App through the native Runtime.
2. Acquire a `FrameLease`.
3. Access the writable buffer view.
4. Respect stride and BGRA8888 layout.
5. Release views before presenting.
6. Use dirty rectangles when only part of the frame changes.

The project does not request the optional `ui` module.
