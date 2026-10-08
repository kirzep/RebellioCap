# RebellioCap

RebellioCap is a Windows desktop application for recording and saving recent
gameplay. The native C++ engine handles capture, audio, hardware encoding and
the replay buffer; the desktop interface uses React and a Tauri Rust host.

This repository contains development source. Publishing the source does not
establish installer, hardware compatibility or long-session performance acceptance.

## Build requirements

- Windows x64 with Visual Studio C++ Build Tools and the Windows SDK.
- Visual Studio's CMake and Ninja, Rust/Cargo, and Node.js with npm.
- An NVIDIA GPU and a compatible NVENC driver for hardware capture tests.

Bootstrap native dependencies, install frontend dependencies and run checks:

```powershell
./scripts/bootstrap-vcpkg.ps1
npm ci
./scripts/verify-desktop.ps1
npm run test:e2e -w @rebelliocap/desktop
```

Build and stage the native engine before starting the desktop application:

```powershell
./scripts/invoke-dev.ps1 -Command @('cmake', '--preset', 'windows-debug')
./scripts/invoke-dev.ps1 -Command @('cmake', '--build', '--preset', 'windows-debug')
./scripts/stage-desktop-engine.ps1 -BuildDirectory build/windows-debug
npm run desktop:dev
```

See [Windows development](docs/development/windows-toolchain.md) for the
toolchain, test labels and hardware verification workflow. See
[third-party licenses](docs/licenses/third-party.md) for dependencies and notices.
No project-wide open-source license is granted by this publication; third-party
materials retain their respective licenses.
