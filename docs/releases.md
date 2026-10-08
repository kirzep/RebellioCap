# Release policy and packaging

[Documentation](README.md) · [Testing](testing.md) · [Licensing](../LICENSE.md)

The public repository currently publishes development source. Desktop manifests declare **0.1.7**; CMake and vcpkg declare **0.1.0** for the engine/native package. These are existing component versions, not a unified verified release tag. Check [GitHub Releases](https://github.com/kirzep/RebellioCap/releases) for explicitly published binary artifacts.

## Build a local package

Prepare the [development environment](getting-started.md), then build and stage an optimized engine:

```powershell
./scripts/invoke-dev.ps1 -Command @('cmake', '--preset', 'windows-release')
./scripts/invoke-dev.ps1 -Command @('cmake', '--build', '--preset', 'windows-release')
./scripts/stage-desktop-engine.ps1 -BuildDirectory build/windows-release
npm run desktop:build
```

The Tauri build hook stages resources again and prefers the newest release engine, including `windows-hardware-release` when present. Verify build identity and the staged resource manifest before distributing a package.

The configured target is a **per-user NSIS installer**, with Russian/English installer language selection, engine/runtime resources, and bundled license files. Standard output is under `apps/desktop/src-tauri/target/release/bundle/nsis/`; an overridden Cargo target directory changes that location.

If WebView2 is missing, the installer downloads its bootstrapper. That requires internet access. See upstream [Windows installer documentation](https://v2.tauri.app/distribute/windows-installer/). Configured installer behavior is not proof of successful clean-machine install/uninstall acceptance.

## Distribution gates

Record a source commit, dependency lockfiles, toolchain, staged hashes, and test evidence. Evaluate these gates separately:

| Gate | Evidence |
| --- | --- |
| Software | C++, Rust, frontend/bundle, browser, and crash-coverage checks; state skipped checks explicitly. |
| Desktop runtime | Actual WebView2, first launch, IPC, tray/hotkeys, recording/replay, playback and export. |
| Hardware/long sessions | Named GPU, driver, audio/display configuration; disconnect/recovery behavior, stream validity and memory behavior. |
| Performance | Named hardware/workload/settings with baseline and capture runs, using committed collectors. |
| Installer | Clean install, prerequisites, launch, upgrade/uninstall, retained data and failures. |
| Redistribution | Project permission, dependency notices, FFmpeg corresponding-source/relinking provisions, and artifact checksums. |
| Signing | Explicitly record whether artifacts are signed and what was verified; no signing guarantee is made here. |

See [testing](testing.md), [Windows toolchain](development/windows-toolchain.md), and [third-party notices](licenses/third-party.md). Checked-in notices/source archives do not certify every future binary bundle.

## Release notes

Describe user-visible changes, migration concerns, exact artifacts, accepted environments, and known limitations. Version/date entries must refer to actual releases. Planned features and skipped hardware checks must not appear as shipped capabilities or passing results. The [changelog](../CHANGELOG.md) records the current source snapshot without inventing release history.
