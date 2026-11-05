# {{APP_NAME}} — Lua Kavio Widget Template

## Recommended Lua starting point

Choose this template for standard interactive UI. Lua code uses the public `kavio.ui` Widget model while the SDK owns LVGL, AppView, touch routing and presentation.

```text
Lua App -> Kavio Widget -> SDK LVGL backend -> Presentation
```

## Create your app

```bash
./kavio new MyApp --template lua/kavio_widget --app-id com.example.my-app
./kavio build MyApp
./kavio pack MyApp
```

This template declares only the public optional module:

```json
"sdk": { "modules": ["ui"] }
```

The tool infers Lua runtime, Lua binding, UI runtime and Lua UI binding automatically.

Use `lua/framebuffer` when another renderer owns the complete frame. Raw LVGL C APIs are not part of the current public Lua SDK.
