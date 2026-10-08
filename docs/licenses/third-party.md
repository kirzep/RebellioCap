# Third-party licenses and clean-room boundary

RebellioCap is a clean-room implementation. It may use the public APIs and
redistributable binaries recorded below, but it does not copy source, code,
effects, presets, assets, or runtime behavior from other capture applications.

| Dependency | Pinned boundary | License |
| --- | --- | --- |
| vcpkg | baseline `118bba14b94bc040c098c0c15e63c142148c05ca` | MIT |
| Catch2 | manifest minimum `3.16.0` | Boost Software License 1.0 |
| FFmpeg | `avformat`, `avfilter`, `swscale`, `swresample`, `ffmpeg`, `ffprobe`, `nvcodec`, `zlib`; default features disabled | LGPL-compatible build required |
| NVIDIA NVENC declarations (`ffnvcodec`) | vcpkg `13.0.19.0`, upstream `FFmpeg/nv-codec-headers` tag `n13.0.19.0` | MIT |
| NVIDIA NVAPI headers | official `NVIDIA/nvapi`, commit `70d337db9186e968eab622f7e786de7e437faf3d`; local read-only DRS lookup | MIT; `NVIDIA-NVAPI-MIT.txt` |
| nlohmann/json | manifest minimum `3.12.0` | MIT |
| PresentMon | `2.5.1` | MIT |
| React | `19.3.0` | MIT |
| thinking-orbs | `0.3.2`; composing indicator during onboarding test recording | MIT; copyright 2026 Jakub Antalik |
| Tauri | `2.11.5` | Apache-2.0 / MIT |
| Playwright | `1.63.0` | Apache-2.0 |
| Manrope Variable via Fontsource | `@fontsource-variable/manrope` `5.3.0`, publish hash `5ea7462ba57d9fdb` | SIL Open Font License 1.1 |
| Golos Text Variable via Fontsource | `@fontsource-variable/golos-text` `5.3.0`, publish hash `2982edfd482e13b0` | SIL Open Font License 1.1 |
| shadcn/ui component sources | Generated with CLI `4.21.0`, new-york / Radix registry, 2026-09-30 | MIT; `shadcn-MIT.txt` |
| tweakcn Claymorphism theme | `https://tweakcn.com/r/themes/claymorphism.json`, retrieved 2026-09-30 | Apache-2.0; `tweakcn-Apache-2.0.txt` |
| Radix UI | `radix-ui` `1.6.7` | MIT |
| cn | `0.4.0` | MIT |
| class-variance-authority | `0.7.1` | Apache-2.0 |
| Tailwind CSS / Vite integration | `4.3.3` | MIT |
| tw-animate-css | `1.4.0` | MIT |

The vcpkg manifest must never enable FFmpeg `gpl`, `all-gpl`, `nonfree`, `x264`,
`x265`, or `fdk-aac` features. Recording uses direct NVENC and Media Foundation.
The clip editor uses the shared LGPL FFmpeg CLI for filtering, H.264 export via
NVENC or Media Foundation, and AAC encoding. PNG imports and text layers use zlib.

The renderer bundles Manrope and Golos Text from the Fontsource packages above;
it does not fetch fonts at runtime or install them into Windows. The exact WOFF2
payload hashes used by the current package versions are recorded in
`docs/licenses/fonts.sha256`. The upstream copyright notices and full SIL Open
Font License 1.1 text shipped with the project are recorded in
`docs/licenses/fonts-OFL-1.1.txt`.

The Claymorphism color, radius and shadow tokens are bundled in the renderer's
global stylesheet. Font declarations are adapted to the locally bundled Cyrillic
fonts above. No runtime connection to tweakcn or shadcn is required. Copied
component sources are locally adapted for Russian labels, desktop sizing, focus
management and disabling the sidebar keyboard shortcut during hotkey editing.

The `ffnvcodec` package is header-only. RebellioCap compiles against
`include/ffnvcodec/nvEncodeAPI.h` (NVENC API 13.0 declarations; NVIDIA header
copyright 2010-2024) from the pinned upstream tag above. The vcpkg port records
the upstream archive SHA-512 as
`103381914daf92ae11a409b2c9d0a9036bd40e3f7f244fa05202ed19c863f0630818c72e09e829b336754d727672b75d2789978a5875b355c3bc107fa9ca3ec6`.
The header carries the MIT permission notice.

At runtime RebellioCap loads `nvEncodeAPI64.dll` only from the Windows system
directory, resolves `NvEncodeAPIGetMaxSupportedVersion` and
`NvEncodeAPICreateInstance`, and negotiates API 13.0. The DLL is supplied by the
installed NVIDIA display driver: RebellioCap neither links an NVENC import
library nor bundles or redistributes the driver DLL. No NVIDIA sample
implementation and no Vice source or implementation were copied.

Release packaging must use an LGPL-compatible dynamically linked FFmpeg build,
include the applicable notices and source-offer mechanism, and preserve users'
relinking rights. This document is a boundary record, not a substitute for a
release-time legal review.

The standalone `PresentMon-2.5.1-x64.exe` comes only from the official
GameTechDev release URL recorded in `scripts/dependencies.json`; bootstrap
accepts it only when its SHA-256 is
`9bec3083069f58f911e6a512f4806db51a27bd096103087bc1d05ef54c80a191`.

Task 10 links dynamically to the pinned FFmpeg 9.0.1 libraries
`avformat-63.dll`, `avcodec-63.dll`, and `avutil-61.dll`. The codec library
supplies packet allocation/rescaling used by muxing; no FFmpeg encoder is
opened or invoked. Video remains direct NVENC and audio remains Media Foundation
AAC. The runtime-copy helper copies those exact pinned DLLs beside the mux test.
The installed ffprobe reports LGPL 2.1-or-later, shared linkage, and no enabled
GPL/nonfree flags. Its strict decode probes are test validation only. Release
packaging must carry the corresponding FFmpeg license and source provisions
specified above; copying test DLLs is not a completed release distribution.
