# Kavio App Projects and Templates

`kavio_app/` contains two different kinds of content:

```text
kavio_app/
├── Templates/      # source templates used by `./kavio new`; not buildable projects
├── KavioDemoCpp/    # independent C++ demo
├── KavioDemoPython/ # independent Python demo
├── KavioDemoLua/    # independent Lua demo
├── tools/          # project schema and package builder
└── <YourApp>/      # projects created by developers
```

## 1. What is a buildable Kavio project?

A directory is a buildable Kavio project **only when it contains a file named exactly**:

```text
kavio_project.json
```

The root `./kavio` tool recursively scans:

```text
kavio_app/**/kavio_project.json
```

The directory containing that file becomes one project. Its project key is the path relative to `kavio_app/`.

For example:

```text
kavio_app/
├── MyApp/kavio_project.json
├── KavioDemoCpp/kavio_project.json
├── KavioDemoPython/kavio_project.json
└── KavioDemoLua/kavio_project.json
```

is discovered as:

```text
MyApp
KavioDemoCpp
KavioDemoPython
KavioDemoLua
```

Directory names such as `cpp`, `python`, `lua`, `Templates`, or `KavioDemo` do not make a project buildable. **Only `kavio_project.json` does.**

Use:

```bash
./kavio list
```

to list discovered projects.

## 2. Templates are intentionally not projects

Source templates live under:

```text
kavio_app/Templates/
```

A template contains:

```text
kavio_project.json.template
```

instead of `kavio_project.json`.

Therefore templates are never returned by `./kavio list` and cannot accidentally be built or packaged as installed Apps.

The template scanner is separate from the project scanner. List templates with:

```bash
./kavio new --list
```

Current templates:

```text
C++
  cpp/kavio_widget      recommended for normal application UI
  cpp/framebuffer       application-owned/custom renderer
  cpp/advanced_ui       raw LVGL specialized UI on the SDK backend

Python
  python/kavio_widget
  python/framebuffer

Lua
  lua/kavio_widget
  lua/framebuffer
```

Python/Lua do not have `advanced_ui`: the current public script bindings do not expose raw LVGL C APIs, and their custom-renderer path is already represented by `framebuffer`.

## 3. Recommended first step

Most applications should start from Kavio Widget:

```bash
./kavio new MyApp \
  --template cpp/kavio_widget \
  --app-id com.example.my-app
```

Then:

```bash
./kavio build MyApp  # build only
./kavio pack MyApp   # build, then package
```

`new` copies the selected template, replaces template tokens, and converts:

```text
kavio_project.json.template
```

into the real project descriptor:

```text
kavio_project.json
```

The new project immediately becomes visible in `./kavio list`.

## 4. Which template should I choose?

| Requirement | Template |
| --- | --- |
| Normal text/buttons/images/forms/settings UI | `*/kavio_widget` |
| Game, video, OpenCV, Godot, existing/custom renderer | `*/framebuffer` |
| C++ Chart/Canvas/custom draw/raw LVGL specialized controls | `cpp/advanced_ui` |
| See all SDK capabilities and Host integration together | `KavioDemoCpp`, `KavioDemoPython`, `KavioDemoLua` |

### `kavio_widget`

The recommended high-level UI route:

```text
App -> Kavio Widget -> SDK LVGL backend -> Presentation
```

Applications program against Kavio Widget. LVGL is an SDK backend detail for ordinary Widget projects.

### `framebuffer`

The direct-render route:

```text
App -> Presentation -> FrameLease -> BGRA framebuffer -> your renderer -> Present
```

No Kavio Widget UI module is required.

### `advanced_ui` (C++ only)

For C++ applications that need raw LVGL features not represented by Kavio Widget:

```text
App -> SDK LvglBackend -> same SDK-owned LVGL runtime -> raw LVGL objects
```

It reuses the SDK backend. It must not call `lv_init()` or create a second LVGL display/AppView.

## 5. `kavio_project.json`

Every real project maintains one project descriptor. Example C++ Widget project:

```json
{
  "projectVersion": 1,
  "language": "cpp",
  "sdk": {
    "modules": ["ui"]
  },
  "package": {
    "appId": "com.example.my-app",
    "version": "1.0.0",
    "name": "My App",
    "description": "My Kavio App.",
    "device": {
      "runtime": "native",
      "entry": "kavio_my_app",
      "arguments": []
    }
  }
}
```

The formal schema is:

```text
kavio_app/tools/kavio_project.schema.json
```

Project files do not need a relative `$schema` path. The Kavio tool always applies the repository's fixed project schema/rules.

## 6. `sdk.modules`

`sdk.modules` declares **optional public SDK modules**, not individual Device APIs or internal shared libraries.

Current supported values:

```text
ui
```

No `sdk.modules` field (or an empty list) means the application uses only the core App/Device/Presentation/FrameBuffer capabilities.

```json
"sdk": {
  "modules": ["ui"]
}
```

enables Kavio Widget UI. The tool automatically deploys the required UI runtime, LVGL backend, and language-specific UI binding. Developers do not list internal dependencies such as `ui_core`, `python_ui_binding`, or `lua_ui_binding`.

Do not add `rx`, `tx`, `h264`, `usb`, `config`, `framebuffer`, or `lvgl` as modules. Those are either core Device/Presentation capabilities or internal implementation details.

## 7. Project layout

Recommended generated project:

```text
MyApp/
├── kavio_project.json
├── icon.png
├── schema_description.json
├── README.md
└── device/
    └── ...
```

`icon.png` and `schema_description.json` use fixed names.

## 8. Package metadata

`kavio_project.json.package` is the source of truth for install/runtime metadata. It stays explicit so the package manifest can be derived directly without reconstructing runtime semantics from build fields.

The project descriptor itself is **not** included in `.kap`.

The package contains runtime artifacts such as:

```text
manifest.json
icon.png
schema_description.json
device/
host/              # only when declared
signature.json      # when signing is enabled
```

Build-only information such as source directories, SDK module selection and CMake configuration is not shipped.

## 9. KavioDemo

`KavioDemoCpp`, `KavioDemoPython`, and `KavioDemoLua` are independent buildable projects. They are integrated SDK capability/regression applications, not starter templates.

Use templates to create your own App. Use KavioDemo to validate how Device APIs, UI, USB, RX/TX, H.264, configuration and Host integration work together.

See the `README.md` within each demo project.

## Batch operations

```bash
./kavio build_all [--release] [--jobs 4]
./kavio pack_all [--release] [--jobs 4]
```

Both commands discover projects recursively by `kavio_project.json`, continue after an invalid project or individual build failure, and return a nonzero exit status if any project fails. `pack_all` builds each Device App before packaging it. Host DLLs and host-side dependencies must still be built on Windows and copied to each project's own `build/apps/<project-key>/host/` directory.
