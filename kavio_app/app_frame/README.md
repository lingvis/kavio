# Kavio App Frame

`app_frame` is the minimal native C++ starting template for a Kavio App. It intentionally contains no Host Component, USB business protocol, HDMI RX/TX logic, H.264 capture, Input demo, App Configuration fields, or App Panel.

Its only goal is to provide a known-good SDK integration skeleton that can:

1. start `KavioClient`;
2. establish a Device App Session;
3. asynchronously create an AppView surface;
4. initialize LVGL on that surface;
5. display a minimal UI;
6. shut down the AppView and SDK cleanly.

For feature examples, use `../sdk_showcase` instead.

## Directory layout

```text
app_frame/
├── README.md
├── device/
│   ├── CMakeLists.txt
│   ├── build.sh
│   └── main.cpp
└── package/
    ├── icon.png
    ├── kavio_package.json
    └── schema_description.json
```

There is deliberately no `common/` or `host/` directory. Add them only when the App needs Host/Device communication or a Windows Host Component.

## Before using this as your own App

Copy the entire `app_frame` directory, then change at least:

- the directory name;
- `package/kavio_package.json`: `appId`, `name`, `version`, and `description`;
- replace `package/icon.png` with your App icon (PNG, up to 512x512 pixels and 512 KiB);
- `device/CMakeLists.txt`: project/target name if desired;
- `device/build.sh`: `TARGET_NAME` if the executable was renamed;
- `device/main.cpp`: `client_name` and the UI/business logic.

`com.kavio.template.app-frame` is only the template ID. Do not ship multiple Apps with this same `appId`.

## Build

Build the public Device SDK first:

```bash
cd kavio_sdk/device
./build.sh -r
```

Then build the template App:

```bash
cd kavio_app/app_frame/device
./build.sh -r
```

For cross compilation:

```bash
./build.sh -r --toolchain /path/to/aarch64-toolchain.cmake
```

The staged executable is:

```text
kavio_app/app_frame/output/device/kavio_app_frame
```

## Check and package

```bash
cd kavio_app
python3 kavio_app.py status app_frame
python3 kavio_app.py package app_frame
```

The package is written to `app_frame/dist/`.

## Runtime flow

`device/main.cpp` keeps the minimum asynchronous lifecycle visible in one file:

```text
KavioClient::Start()
        ↓
MSG_P2A_RSP_CREATE_SESSION
        ↓
LvglAppViewAdapter::Initialize()
        ↓
AppView create response
        ↓
LvglAppViewAdapter::ProcessPending()
        ↓
CreateUi()
        ↓
lv_timer_handler()
        ↓
Shutdown / Stop
```

Keep `OnMessage()` short. SDK business requests are asynchronous; add request/response handling there, but move expensive work to the App's own worker/UI thread.

## Empty App Configuration

`schema_description.json` contains:

```json
{}
```

This is intentional. The file is mandatory even when an App has no configurable fields. Add a real Schema only when the App needs persistent configuration.

## Adding features later

Use the public SDK headers rather than copying Platform implementation code. Typical additions are:

- RX: `KavioClient::ReadLatestRxFrame()`;
- TX: `CreateTxSurface()` / `OpenTxSurface()` / `TxSurface::WriteFrame()`;
- Input: `SendInputEvent()`;
- Host communication: `SendCommChannelData()` plus an optional `host/` component;
- H.264: `SubscribeH264Stream()` / `ReadH264Packet()`;
- App Configuration: `GetAppConfig()` / `UpdateAppConfig()` / `ResetAppConfigToDefaults()`.

See `../sdk_showcase` for working examples of those capabilities.
