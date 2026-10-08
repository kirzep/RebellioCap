# Troubleshooting

[Documentation](README.md) · [Getting started](getting-started.md) · [Support](../SUPPORT.md)

Start with the visible error and the selected configuration. For a reproducible issue, record the application version/commit, Windows version, GPU/driver, display, audio sources, and recording settings. Share reviewed error excerpts rather than a complete unreviewed archive.

## Build or launch

| Symptom | What to check |
| --- | --- |
| MSVC, SDK, CMake or Ninja is missing | Install the C++ desktop workload and use `invoke-dev.ps1`. Run `verify-prerequisites.ps1` and read its JSON checks. |
| vcpkg toolchain/packages are missing | Run `bootstrap-vcpkg.ps1` from the repository root, then configure the chosen preset again. First-time dependency downloads need network access. |
| Frontend dependency or type errors | Use `npm ci` at the root so the workspace and lockfile match. Check the Node version against CI. Do not mix unrelated lockfiles. |
| Rust resources or engine executable are missing | Build the native engine, then run `stage-desktop-engine.ps1 -BuildDirectory build/windows-debug` before native desktop compilation. |
| The app seems to use an older engine | The Tauri hooks prefer the newest release build over debug. Rebuild the intended preset and inspect the staged resource manifest; see [packaging](releases.md). |
| WebView2 cannot start | Confirm the WebView2 Runtime is installed. A configured installer can download its bootstrapper; source development requires the runtime already available. |
| A plain browser cannot record or open files | The Vite browser frontend has no Tauri host. Use `npm run desktop:dev` for native functions. Browser fixtures are for UI checks. |
| Playwright cannot find its browser | Run `npx playwright install chromium` from the repository root. Alternatively use installed Edge: `$env:PLAYWRIGHT_CHANNEL = 'msedge'`, then run E2E. |

## Capture and recording

**NVENC or native hardware checks fail.** Check [requirements](system-requirements.md): the GPU must support the requested H.264 configuration and the driver must expose the required API. The prerequisite driver threshold alone does not prove encoder compatibility. On hybrid/multi-adapter systems, also check that the chosen display is on a compatible capture adapter. Keep the exact diagnostic code in a report.

**No output / insufficient space.** Choose an existing, writable local output folder and ensure enough free space for the requested bitrate and duration. First-run preflight is not a quota for an entire long session. Free-space and throughput failures can affect ongoing writes even when initial checks pass.

**Replay is shorter than requested.** Wait for enough history after start/resume. Keyframe boundaries and memory pressure affect the available interval. Lower bitrate or shorten the requested window; review the replay budget. A manual packet budget is not a whole-process RAM cap.

**A save or continuous recording reports pressure.** Saves and continuous writes have bounded queues. Let pending saves complete and check storage throughput/free space. Repeated saves against slow storage can exhaust the permitted queue or retained-packet budget. The error should remain visible; increasing every limit is not a substitute for finding the constrained resource.

**Lock, sleep or display changes interrupt capture.** Supported access-loss transitions retry the same display and request a keyframe on restoration. They cannot capture the unavailable interval. Fatal GPU/encoder failures remain terminal. Check [capture recovery](capture-recovery-testing.md) and report the exact transition, whether the display returned, and whether capture resumed.

**A crash leaves `.partial` files.** Nonempty orphaned MP4/MKV partial files can appear as recovery candidates in the library. Keep the original while checking playback/recovery. Recovery depends on completed media fragments; the last fragment or an incompletely initialized file may be unusable. Do not rename, overwrite, or delete your only copy to troubleshoot it.

## Audio and playback

**Missing microphone/system sound.** Check the selected endpoint and whether the source is disabled. Confirm device availability and Windows microphone permissions. A disconnected pinned endpoint is retried; the engine does not silently switch to another microphone. Silence during an outage is intentional recovery behavior.

**Several audio tracks are present.** With system audio and microphone enabled, the recording includes mixed and separate source tracks. Choose the intended track(s) in the player. Playing a mix and its source tracks simultaneously can duplicate sound.

**A recording plays differently in another player.** RebellioCap prepares playback media and separate audio resources for its own player. External applications differ in container/codec and multi-track handling. Include a small neutral sample and the player/version when reporting a compatibility issue; avoid a full personal recording.

**Recorded colors differ from an HDR display.** The recording path currently outputs SDR BT.709 without HDR fidelity or tone mapping. This is a known boundary, not a setting that enables HDR recording.

## Editor and export

**A project is missing media.** Projects reference source paths. Restore/move the original source files back to accessible locations or replace the missing media. Packaged projects can copy sources into an adjacent `assets` folder; keep that folder with the project.

**Import or export fails or times out.** Check source access, temporary/output space, the staged FFmpeg runtime, and the exact error. Start with a small neutral file to isolate a media-specific failure. Large or complex compositions have bounded processing and can exceed deadlines.

**A size-targeted export cannot fit.** Shorten the composition or choose a larger budget. Lower resolution/frame rate when appropriate. Budget presets are application targets, not an authoritative list of another service's upload limits.

**An edit was interrupted.** Check editor recovery/autosaves. Save a normal project after recovering and keep source media separately. Recovery data lives in WebView local storage; clearing it can remove unsaved work.

## Diagnostics and reporting

Application logs are local under `%LOCALAPPDATA%\RebellioCap\logs`; settings and separate system-check JSON files are under `%LOCALAPPDATA%\RebellioCap`. The developer log-export control creates a local ZIP; it does not submit a report. Read [privacy](privacy.md) for archive contents and redaction limits.

If the guides do not resolve a reproducible problem, open an [issue](https://github.com/kirzep/RebellioCap/issues/new/choose) with the failing step, error, environment and reviewed evidence. Report vulnerabilities through [Security](../SECURITY.md), separately from ordinary support.
