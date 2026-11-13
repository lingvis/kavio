# Kavio App Templates

`Templates/` contains **source templates**, not buildable App projects. A template contains `kavio_project.json.template`; the project scanner only recognizes `kavio_project.json`, so templates never appear in `./kavio list`.

Create a real App with `./kavio new`.

## Choose a template

| Need | C++ | Python | Lua |
| --- | --- | --- | --- |
| Standard application UI | `cpp/kavio_widget` | `python/kavio_widget` | `lua/kavio_widget` |
| Existing/custom renderer owns the full frame | `cpp/framebuffer` | `python/framebuffer` | `lua/framebuffer` |
| Raw LVGL specialized UI beyond Kavio Widget | `cpp/advanced_ui` | — | — |

For most applications, start with **Kavio Widget**.

```bash
./kavio new MyApp --template cpp/kavio_widget --app-id com.example.my-app
./kavio build MyApp
./kavio pack MyApp
```

List available templates:

```bash
./kavio new --list
```

## Templates versus KavioDemo

- `Templates/` are the supported starting points for creating your own App. They are intentionally not scanned as projects.
- `KavioDemoCpp/`, `KavioDemoPython/`, and `KavioDemoLua/` are independent, buildable SDK capability/regression applications.

Do not copy KavioDemo as an application starter.

## UI development guide

For the rendering architecture, available UI controls, interactive screen examples, raw LVGL, and FrameBuffer drawing, see [`Kavio_UI_Development_Guide.md`](../../docs/modules/sdk/Kavio_UI_Development_Guide.md).
