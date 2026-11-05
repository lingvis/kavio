# {{APP_NAME}} — Lua FrameBuffer Template

## When to choose this template

Use this when Lua coordinates a custom/native renderer that writes directly into the Kavio framebuffer. For ordinary UI, choose `lua/kavio_widget`.

```text
Lua App -> Presentation -> FrameLease -> BufferView -> renderer -> Present
```

Use BufferView bulk/native operations where possible; avoid treating Lua per-pixel loops as the recommended high-frame-rate renderer.

## Create your app

```bash
./kavio new MyApp --template lua/framebuffer --app-id com.example.my-app
./kavio build MyApp
./kavio pack MyApp
```

The generated project uses `kavio_lua_runner` as the package runtime entry and `main.lua` as its script argument. The build system deploys the Lua runtime/binding automatically.
