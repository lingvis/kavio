# KavioDemoPython

Independent Python Device and Windows Host demo application.

## Layout

```text
KavioDemoPython/
├── kavio_project.json
├── device/
├── host/
│   ├── KavioDemoHost.sln
│   └── HostComponent.cpp
└── common/
    └── DemoProtocol.h
```

## Build and package

From the repository root:

```bash
./kavio build KavioDemoPython
./kavio pack KavioDemoPython
```

Build `host/KavioDemoHost.sln` directly in Visual Studio, or build it from a Windows Developer Command Prompt:

```bat
msbuild host\KavioDemoHost.sln /p:Configuration=Release /p:Platform=x64
```

The Host project uses paths relative to its `.vcxproj` to locate this demo's `common/` folder and the repository's `kavio_sdk/` headers.

Then copy its complete `host/build/host/` output folder to `build/apps/KavioDemoPython/host/` on Linux. The `host.dll` file must be directly inside that folder.

Place optional Device runtime dependencies under `build/apps/KavioDemoPython/device/` and preserve their intended relative paths. Device build outputs remain in `build/apps/KavioDemoPython/debug/device/` or `release/device/`. The resulting package is written beneath `build/apps/KavioDemoPython/dist/`.

This project has its own Host source and Demo Protocol files. It has no dependency on sibling demo directories.

## Source guide

The Device entrypoint is `device/main.py`.
The Windows Host entrypoint is `host/HostComponent.cpp`, and the shared
wire protocol is documented in `common/DemoProtocol.h`. The Device and Host
must use the same message identifiers and page order.

The main demonstration covers LVGL presentation, touch controls, App Channel
handshakes/transfer tests, HDMI RX image reading, HDMI TX surfaces, virtual
keyboard/mouse events, H.264 capture, and App configuration.

### RX preview

On **HDMI RX**, use **Read latest RX frame** for a single sample or press
**START LIVE** to attempt an RX frame on every application tick. Press
**STOP LIVE** to pause. No fixed refresh interval is imposed by the demo;
actual speed depends on frame availability, the SDK receive sampling rate,
pixel conversion, and UI processing. The FPS label counts distinct frames
successfully presented during a measurement window of approximately one
second; it is **preview FPS**, not the HDMI source refresh rate. Lua reports
an approximate one-second-bucket rate due to its available wall-clock API.
Preview automatically stops when navigating away from the RX page. No
image-data pointer is retained after its owning native buffer becomes invalid.

### Resource and callback rules

- Build and replace LVGL widgets from the application loop, not from arbitrary
  transport worker callbacks.
- Unsubscribe H.264 and release TX surfaces during shutdown.
- Treat RX frames and Host packets as bounded data; validate sizes and formats.
- Host transfer speed tests are for demonstration, not a guaranteed throughput
  measurement under all device configurations.

### Comparable HDMI RX measurements

- The RX sampler requests every available frame (`rx_frame_sample_interval=1`).
- All three device demos use SDK Image conversion to RGB565, with a 620 x 360 pixel maximum (aspect ratio preserved; no upscaling).
- **START LIVE** attempts one latest-frame read per application tick, without a fixed timer delay.
- A repeated nonzero frame ID is skipped; only successfully submitted, distinct frames count toward FPS.
- FPS is updated about once per elapsed second using a monotonic clock. The value is **preview submission FPS**, not HDMI source FPS or guaranteed display-flush FPS.
- Stop live preview before changing pages. Tests should use the same HDMI input and board build.
