# Getting started

[Documentation](README.md) · [Requirements](system-requirements.md) · [Troubleshooting](troubleshooting.md)

This guide builds the development source on Windows x64 and walks through the first recording. For published downloads, check [GitHub Releases](https://github.com/kirzep/RebellioCap/releases). Building a local package does not by itself establish a verified release; see [release policy](releases.md).

## 1. Prepare the machine

Install these development tools:

- Visual Studio / Build Tools with **Desktop development with C++**, x64 MSVC, the Windows SDK, and bundled CMake/Ninja.
- Node.js **24** with npm, matching [CI](../.github/workflows/desktop-ci.yml).
- Rust **1.90 or newer**, with the `x86_64-pc-windows-msvc` toolchain.
- Microsoft Edge **WebView2 Runtime** for the native desktop interface.
- Git and PowerShell.

Recording also requires a compatible NVIDIA GPU and NVENC driver. Review [system requirements](system-requirements.md). The upstream [Tauri Windows prerequisites](https://v2.tauri.app/start/prerequisites/#windows) cover C++ Build Tools and WebView2; this capture engine adds hardware requirements beyond Tauri itself.

## 2. Restore source and dependencies

```powershell
git clone https://github.com/kirzep/RebellioCap.git
Set-Location RebellioCap
./scripts/verify-prerequisites.ps1
./scripts/bootstrap-vcpkg.ps1
npm ci
```

Run commands sequentially and stop if one fails. The prerequisite script produces JSON and a nonzero exit code for missing requirements. It checks the native development/capture environment, including NVIDIA; it is not a frontend-only prerequisite check.

Bootstrap restores a pinned vcpkg checkout. Native dependencies use the committed [manifest](../vcpkg.json) and [baseline](../vcpkg-configuration.json); npm uses the root lockfile. The first native build compiles dependencies and can take much longer than later builds. Dependency downloads require network access.

## 3. Build and stage the engine

```powershell
./scripts/invoke-dev.ps1 -Command @('cmake', '--preset', 'windows-debug')
./scripts/invoke-dev.ps1 -Command @('cmake', '--build', '--preset', 'windows-debug')
./scripts/stage-desktop-engine.ps1 -BuildDirectory build/windows-debug
```

The wrapper imports the Visual Studio x64 environment, so a preconfigured developer terminal is unnecessary. Staging verifies and copies the engine, FFmpeg tools, required DLLs, and app-local release CRT files into the desktop resource directory with a hash manifest.

For optimized local use, configure/build `windows-release` instead, then stage `build/windows-release`. Hardware tests use the separate `windows-hardware-release` preset. See the [toolchain reference](development/windows-toolchain.md).

## 4. Start the application

```powershell
npm run desktop:dev
```

This starts Vite and the Tauri/Rust desktop host. The development hook stages resources again: when several native builds exist, it prefers the newest release engine before falling back to debug. Keep builds current and check the resource manifest when investigating version mismatches.

On the first run:

1. Start setup and run the initial compatibility check.
2. Choose the display and video profile.
3. Select system audio and, optionally, a microphone.
4. Set replay duration, memory policy, output folder, and hotkeys.
5. Run the test recording, review its preview, and apply the configuration.

The interface is currently in Russian. A microphone is optional; a disabled source is an intentional configuration choice.

## 5. Record and save a replay

The shared [contract](../contracts/recording-settings.v1.json) defines these defaults:

| Action | Shortcut |
| --- | --- |
| Save instant replay | **Alt + F10** |
| Toggle continuous recording | **Ctrl + Shift + R** |

On **Обзор**, confirm replay is running (start it if stopped), wait for some history, then save using the button or hotkey. Applying setup starts the engine with replay enabled. Toggle recording to begin/end a continuous file. Both features can run together. Saved files appear in the output folder and **Клипы** library.

Replay duration is a target bounded by accumulated history, keyframes, and memory. A save immediately after capture starts or resumes can be shorter than the requested window.

## 6. Edit a clip

Open **Редактор клипов**, import media or add a recording from the library, and arrange linked video/audio items. Trim or split, add image/text layers, adjust framing and audio gain, then export. Save a project to keep the edit structure; autosave recovery is an additional safeguard, not a substitute for keeping source files and backups.

File-size targets tune export settings for a chosen budget. They are not a promise about another service's current upload policy.

## Frontend-only development

UI rendering and deterministic tests do not need a native capture build:

```powershell
npm ci
npm run dev -w @rebelliocap/desktop
```

Open the local URL printed by Vite. A plain browser has no Tauri host: native recording, filesystem operations, and export are unavailable. Browser acceptance fixtures inject a test host; see [testing](testing.md) and [screenshot details](screenshots/README.md).

## Verify changes

```powershell
./scripts/verify-desktop.ps1
npx playwright install chromium
npm run test:e2e -w @rebelliocap/desktop
npm run test:crash-coverage
```

If Rust compilation reports missing staged resources, stage the engine as in step 3 and rerun the verifier. See [testing](testing.md) for test boundaries and [release policy](releases.md) for binary packaging.
