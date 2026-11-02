# Kavio SDK Showcase

`Kavio SDK Showcase` is the primary integrated example for the public Kavio SDK.

- Project directory: `kavio_app/sdk_showcase`
- App ID: `com.kavio.demo.sdk-showcase`
- Device executable: `kavio_sdk_showcase`
- Host Component: `host.dll`
- Host Panel: enabled
- Standard App Configuration: enabled

The example intentionally keeps App-specific shared code under:

```text
kavio_app/sdk_showcase/common/
```

`common/DemoProtocol.h` is used **only** by this Demo's Device Component and Host Component. It is private to this Demo and is not part of the public SDK contract.

## Demonstrated capabilities

| Page / component | Public capability |
| --- | --- |
| Home | vertically scrollable single-column capability navigation and SDK status overview |
| Render | `AppViewSurface`, LVGL adapter, touch and continuous local/partial rendering |
| Transfer | bidirectional USB App Channel, ping, packet integrity, configurable `1..1023 KB` payloads, Host submit rate and device-confirmed throughput measurement |
| HDMI RX | read one UYVY / YUYV / BGRA frame, convert it and scale it into an LVGL preview area |
| HDMI TX | create up to three TX surfaces; normal surfaces move at about 30 Hz and the Partial case demonstrates `ImageFrame.valid_rect` |
| Keyboard | A-Z, 0-9, navigation, modifier, system and F1-F12 key output through `SendInputEvent()` |
| Mouse | touch-pad style absolute pointer output plus left/right click and wheel tests through `SendInputEvent()` |
| H.264 Capture | subscribe to the H.264 stream, read Annex-B packets from SHM and save them under `$KAVIO_APP_DATA_DIR/cache/` without decoding or playback |
| Schema Update | demonstrate `GetAppConfig()`, `UpdateAppConfig()` and `ResetAppConfigToDefaults()` against the static `schema_description.json` contract |
| Runtime | session state and runtime statistics |
| Standard App Settings | `schema_description.json` / Device SDK App Configuration |
| Windows App Panel | Host SDK C ABI, device navigation, ping, configurable transfer payloads, Host submit rate, device-confirmed throughput, stable scroll/selection behavior and consistent native styling |

Video playback is intentionally not included. The H.264 case only subscribes, reads and saves raw Annex-B packets. Video playback is outside the scope of this Demo.

## Build the Device Component

Build the Device SDK first with the LVGL AppView adapter enabled, then:

```bash
cd kavio_app/sdk_showcase/device
./build.sh --release
```

Output:

```text
kavio_app/sdk_showcase/output/device/kavio_sdk_showcase
```

## Build the Host Component

Open:

```text
kavio_app/sdk_showcase/host/KavioSdkShowcaseHost.sln
```

Build `Release | x64` with Visual Studio 2015 or later.

Output:

```text
kavio_app/sdk_showcase/output/host/host.dll
```

The Host project uses only:

- `kavio_sdk/host/include`
- `kavio_sdk/common/logging` (required by the Host SDK lifecycle logging exports)
- this Demo's own `common/DemoProtocol.h`

It uses only public SDK headers.

## Validate and package

From `kavio_app/`:

```bash
python3 kavio_app.py status sdk_showcase
python3 kavio_app.py package sdk_showcase
```

The App icon uses the fixed path `package/icon.png`; there is no `icon` field in `kavio_package.json`. The package builder requires it to be a valid PNG with:

- width in `1..512` pixels;
- height in `1..512` pixels;
- file size no greater than `512 KiB`.

## App Configuration vs App Panel

The example deliberately demonstrates both without mixing their responsibilities.

`schema_description.json` contains persistent Device-side settings such as the default page and Render FPS. These values are managed through the standard App Configuration flow.

The Host Panel is runtime UI owned by the Host Component. It navigates the Device UI, performs ping and transfer tests, accepts any payload size from `1..1023 KB`, and separates benchmark results into Host submit rate and device-confirmed Host-to-Device throughput. `QueueFull` retry counts are also shown to help distinguish Host/AppHost backpressure from downstream device throughput. It does not read or modify the standard App Configuration.

The HDMI TX test keeps each created `TxSurface` alive while it moves. The public SDK currently has no separate move-only API, so the normal case updates `dest_pos_x` / `dest_pos_y` through `TxSurface::WriteFrame()` at about 30 Hz. The `PARTIAL` case creates a 320x200 Surface and moves a 96x72 `valid_rect` inside it. This is a partial publish/composition demonstration; `WriteFrame()` still copies the full Surface payload into SHM. The UI `CLEAR` action destroys the Demo-created surfaces and releases their resources.

The App-local Host/Device demo protocol intentionally uses protocol version **1**. New demo pages do not change this private version unless the actual datagram wire format becomes incompatible.
