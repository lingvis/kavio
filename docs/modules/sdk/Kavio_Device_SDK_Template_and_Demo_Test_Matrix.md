# Kavio Device SDK Template and KavioDemo Test Matrix

## 1. Purpose

There are two different validation targets:

1. **Templates** — source starting points consumed by `./kavio new`; not buildable projects themselves.
2. **KavioDemo** — three real integrated buildable projects used for SDK capability/regression validation.

## 2. Template matrix

| Route | C++ | Python | Lua |
| --- | ---: | ---: | ---: |
| Direct FrameBuffer | `Templates/cpp/framebuffer` | `Templates/python/framebuffer` | `Templates/lua/framebuffer` |
| Kavio Widget | `Templates/cpp/kavio_widget` | `Templates/python/kavio_widget` | `Templates/lua/kavio_widget` |
| Advanced raw-LVGL UI | `Templates/cpp/advanced_ui` | — | — |

Every template must contain `kavio_project.json.template` and must **not** contain `kavio_project.json`.

## 3. Template instantiation tests

For every template:

1. `./kavio new <temporary-project> --template <key>` succeeds.
2. Exactly one `kavio_project.json` is created in the new project.
3. No `kavio_project.json.template` remains in the generated project.
4. The generated project appears in `./kavio list`.
5. App ID/name/Device Entry tokens are replaced.
6. Generated projects do not contain `sdk` or `sdk.modules` declarations.
7. Generated C++ CMake uses the unique generated Device Entry target.
8. Python/Lua source syntax checks pass in the real SDK environment.
9. Build/package dependency closure is inferred without developer-maintained internal `.so` lists.

## 4. Route-specific validation

### FrameBuffer

Validate Acquire/Present/Discard, BGRA8888 width/height/stride, dirty rectangles, raw touch, script buffer lifetime, and absence of UI/LVGL package dependencies.

### Kavio Widget

Validate layouts, Label/Button/Image/Slider/Switch/State, callbacks on the App run thread, touch/visibility, and one SDK-owned LVGL backend/AppView/session.

### C++ Advanced UI

Validate use of the SDK `LvglBackend`, raw LVGL only after backend initialization, Chart/Canvas/custom draw on the same LVGL runtime, no `lv_init()`/second display/AppView, and clean backend shutdown before Presentation teardown.

Python/Lua do not have this route until the public SDK intentionally exposes a raw LVGL binding.

## 5. KavioDemo matrix

Buildable integrated projects:

```text
KavioDemoCpp
KavioDemoPython
KavioDemoLua
```

All three validate Runtime Data, Presentation/UI, HDMI RX/TX, TX partial valid rect, Keyboard, Mouse, USB App Channel, H.264, App Configuration, Device errors, ConfigChanged, H.264 invalidation and Demo Protocol v1.

## 6. Discovery tests

Repository baseline `./kavio list` must contain real projects only. No `Templates/...` key may appear.

`./kavio new --list` must list exactly the seven supported template keys.

## 7. Host/package tests

- Host DLL is built only on Windows PC.
- `build` never builds/checks Host DLL.
- `pack` rejects a declared but missing Host artifact.
- `.kap` never includes `kavio_project.json`, `kavio_project.json.template`, source build caches, `__pycache__`, or `*.pyc`.
- `manifest.json` must parse against the application manifest schema.

## 8. Release validation

Static validation cannot replace a real SDK build and VS680 execution. Final release validation must include real C++ compilation, Python native bindings, Lua 5.4 Runner, package installation, App start/stop/re-run, touch, LVGL partial rendering, RX/TX, USB, H.264, configuration, and Host integration.
