# Windows x64 development toolchain

RebellioCap builds only on Windows 10 22H2 (build 19045) or later, using an x64
Visual Studio MSVC toolchain, the Windows SDK, NVIDIA drivers 570.00 or later,
and the Visual Studio-bundled CMake and Ninja. The reference host is an RTX 3060
Ti with driver 591.86, CMake 4.3.1, Ninja 1.13.2, and MSVC 14.51.

Run the prerequisite assertion before downloading or configuring dependencies:

```powershell
& .\scripts\verify-prerequisites.ps1
```

It emits exactly one JSON object. A nonzero exit means one or more mandatory
requirements are unavailable. This is a user-mode developer check; neither the
build nor the future runtime requires administrator privileges.

Bootstrap the pinned tool dependencies and run the baseline gate:

```powershell
& .\scripts\bootstrap-vcpkg.ps1
& .\scripts\bootstrap-presentmon.ps1
& .\scripts\invoke-dev.ps1 -Command cmake --preset windows-debug
& .\scripts\invoke-dev.ps1 -Command cmake --build --preset windows-debug
& .\scripts\invoke-dev.ps1 -Command ctest --preset windows-debug --output-on-failure
```

`invoke-dev.ps1 -Command <string[]>` resolves `vswhere.exe`, imports
`VsDevCmd.bat` with x64 host and target settings, then invokes the supplied
command array. It resolves bare `cmake`, `ctest`, and `ninja` to the bundled
Visual Studio executables and exits nonzero when the supplied command cannot
be found. Explicit paths are never substituted, and PowerShell commands keep
their normal invocation behavior. Both the documented token form and an
explicit array work:

```powershell
& .\scripts\invoke-dev.ps1 -Command cmake --preset windows-debug
& .\scripts\invoke-dev.ps1 -Command @('cmake', '--preset', 'windows-debug')
```

The supported presets are `windows-debug`, `windows-release`, and
`windows-hardware-release`. All use the manifest-mode vcpkg installation rooted
at `.tools/vcpkg_installed`; no user-global vcpkg installation is used.

CTest labels have strict meanings:

- `unit`: deterministic tests with no device dependency.
- `hardware`: tests requiring real capture, NVIDIA, monitor, microphone, or foreground input hardware.
- `performance`: measured performance gates on the designated host.

Normal debug and release test presets exclude `hardware` and `performance`.
An unavailable hardware test is reported as unavailable or not tested in its
evidence; it must never be represented as a passing hardware result.

## Milestone 1 evidence

Build the hardware configuration and run the non-interactive gates before an
operator benchmark:

```powershell
& .\scripts\invoke-dev.ps1 -Command @('cmake', '--build', '--preset', 'windows-hardware-release')
& .\scripts\invoke-dev.ps1 -Command @('ctest', '--preset', 'windows-hardware-release', '-L', 'unit', '--output-junit', 'artifacts/milestone-1/ctest-unit.xml', '--output-on-failure')
& .\scripts\invoke-dev.ps1 -Command @('ctest', '--preset', 'windows-hardware-release', '-L', 'hardware', '--output-junit', 'artifacts/milestone-1/ctest-hardware.xml', '--output-on-failure')
& .\build\windows-hardware-release\RebellioCap.Engine.exe doctor
& .\scripts\collect-performance.ps1 -PreflightOnly
```

First run the dedicated 30-minute stability and stream-contract collection:

```powershell
& .\scripts\verify-milestone-1.ps1 -UsePrimaryMonitor -DurationMinutes 30
```

Then run the performance collector. It requires the exact installed Cyberpunk
2077 executable and pinned PresentMon 2.5.1. Keep the same game preset,
resolution, display mode, scene, and route for all six benchmark runs, and
describe those settings explicitly:

```powershell
& .\scripts\collect-performance.ps1 `
  -SettingsDeclaration 'preset=High; resolution=1920x1080; mode=fullscreen; scene=built-in benchmark; route=default'
```

It records the top-level `capture-30m.ndjson`, asks for two F8 saves, writes
packet-level ffprobe evidence for the late-session MP4, and performs a separate
30-second MKV stream-contract smoke capture. The performance collector then
performs three baseline runs followed by three capture runs. It uses a separate
`performance/capture-engine.ndjson`, samples process and NVIDIA encode metrics,
asks for an early save, and schedules the second save at 29:30 so the resulting
clip proves late-session A/V boundaries. Existing evidence is never overwritten.

Finally run the strict verifier:

```powershell
& .\scripts\write-milestone-manifest.ps1
& .\scripts\verify-milestone-1.ps1
```

Any missing, failed, or `not-run` evidence keeps the milestone incomplete.
The SHA in `manifest.json` is the clean source commit used for collection. A
later evidence-only commit is allowed, but any source or tooling change after
that SHA invalidates the bundle. Both collection entry points refuse to start
when source changes are present.

## Desktop First-Run Verification

The desktop application (Tauri 2 + React 19) runs unified verification across the C++ engine, Rust supervisor host and frontend unit tests. Run Playwright E2E separately:

```powershell
& .\scripts\verify-desktop.ps1
npm run test:e2e -w @rebelliocap/desktop
```

For hardware acceptance and evidence manifest generation:

```powershell
& .\scripts\verify-desktop.ps1 -Hardware -EvidenceRoot artifacts\desktop-first-run
```
