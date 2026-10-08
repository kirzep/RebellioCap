# System requirements

[Documentation](README.md) · [Getting started](getting-started.md)

These are requirements of the current implementation and its documented development baseline. They do not certify every matching PC. First-run checks and an actual test recording establish whether the selected devices/settings work together.

## Running capture

| Component | Requirement / boundary |
| --- | --- |
| OS | **Windows x64**. Documented baseline: Windows 10 22H2, build **19045**, or later. Windows 11 is within this baseline; cross-version acceptance still requires testing. |
| GPU | **NVIDIA**, supporting the requested asynchronous NVENC H.264 High-profile / NV12 configuration with two B-frames. A GPU vendor name alone does not establish compatibility. |
| Driver | The prerequisite checker requires **570.00+**. The engine separately negotiates **NVENC API 13.0** and validates encoder capabilities. Runtime checks are authoritative for the selected configuration. |
| Display | An available DXGI Desktop Duplication output on the capture adapter. Everything visible on the selected display can enter the recording. |
| Desktop runtime | Microsoft Edge **WebView2 Runtime**. The NSIS configuration downloads a bootstrapper if it is missing; this installation step needs internet access. |
| Audio | Available WASAPI render/capture endpoints for enabled system/microphone sources, and Media Foundation AAC support. Sources can be disabled. |
| RAM | Enough **available** memory for replay packets, retained exports, native/GPU resources, and the UI. No universal minimum or recommended system-RAM figure has been validated. |
| Storage | A canonical writable output directory, free capacity, and sustained write throughput. Source media, preview/cache files, and exports need additional capacity. |
| Permissions | Normal user access to selected devices/files. Packaging uses per-user installation; the application does not require a permanently elevated session. |

The engine loads NVENC from Windows System32; the NVIDIA driver is not bundled. See [NVENC loading](../src/video/nvenc_api.cpp), [encoder validation](../src/video/nvenc_encoder.cpp), and [system checks](../apps/desktop/src-tauri/src/native/system_check.rs).

## Size the recording workload

```text
video bytes ≈ bitrate in bits/second × duration in seconds ÷ 8
```

At **12 Mbit/s**, video payload is approximately **90 MB per minute** or **5.4 GB per hour**, using decimal units. Add audio and container overhead. These are arithmetic estimates, not measured files or benchmarks.

Replay stores **encoded packets**, not raw desktop frames. Automatic budgets are derived from system RAM; pending saves and continuous recording share the compressed-data budget. A compressed-data cap does not represent total process or GPU memory; see [architecture](architecture.md).

First-run memory/free-space preflight uses `bitrate / 8 × replay_seconds × 2 + 256 MiB`. Passing that conservative check does not guarantee that an arbitrarily long recording or layered edit will fit.

## Configuration bounds

The shared [contract](../contracts/recording-settings.v1.json) accepts even dimensions from 64 to 16384, 1–240 FPS, 1–200 Mbit/s, replay duration of 1–3600 seconds, and manual replay budgets of 64–8192 MiB (`0` selects automatic budgeting).

These are **validation bounds**, not verified recording performance. Hardware, drivers, memory, and storage can reject or constrain a combination. The balanced onboarding default fits the display within 1920×1080 without upscaling, at 60 FPS and 12 Mbit/s; it is a default, not a benchmark.

## Development tools

| Tool | Project boundary |
| --- | --- |
| C++ | MSVC x64, C++20, Windows SDK; other native toolchains are rejected by CMake. |
| Build system | CMake **3.28+** and Ninja, resolved through the Visual Studio environment wrapper. |
| Rust | **1.90+**, MSVC target; dependencies recorded in `Cargo.lock`. |
| Node.js / npm | Node **24**, matching CI; `npm ci` restores the committed lockfile. |
| Native dependencies | Pinned vcpkg baseline/manifest, restored by `bootstrap-vcpkg.ps1`. |
| Browser tests | Playwright Chromium; installed Edge can be selected with `PLAYWRIGHT_CHANNEL=msedge`. |
| Performance tooling | Pinned PresentMon and an explicitly specified workload; not required for ordinary UI work. |

See [getting started](getting-started.md) and [Windows toolchain](development/windows-toolchain.md).

## Unsupported cases

macOS/Linux capture, ARM64 builds, AMD/Intel capture encoders, HDR fidelity/tone mapping, injected game capture, and per-process audio isolation are not implemented. The editor's Media Foundation export fallback does not add another capture encoder. Locked/secure desktop intervals cannot provide recoverable imagery.
