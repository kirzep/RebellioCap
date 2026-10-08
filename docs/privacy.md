# Privacy and local data

This document describes the behavior implemented in this repository. RebellioCap
records and edits media locally. The current source has no account service,
cloud upload, analytics SDK or automatic remote crash-report submission.
Local diagnostics are still collected; exporting them is a separate user action.

## What capture can contain

Capture reads the selected monitor through DXGI Desktop Duplication. Other
windows and notifications visible on that monitor can appear in a recording.
Game categorization changes folder names; it does not restrict capture to the
game window. Enabled system-audio capture reads the selected playback endpoint,
and enabled microphone capture reads the selected input endpoint. Voices and
other applications' audio can therefore be included.

The replay buffer retains encoded packets in memory until saved or retired.
Saved replays and continuous recordings are written to the chosen output
directory. A recording interrupted by a crash can leave a `.partial` media file.
The application does not provide encrypted media storage or secure erasure.
See the [capture implementation](../src/capture/dxgi_capture_source.cpp),
[audio implementation](../src/audio/wasapi_capture_source.cpp),
[replay buffer](../src/replay/replay_ring.cpp) and
[continuous recording](../src/engine/continuous_recording.cpp).

For local game categorization, the engine reads the foreground application's
executable path and the installed NVIDIA driver's predefined application
profiles. Category names and cached game icons can be written beneath the
recording directory. This does not fetch an online game catalog.
See [game categorization](../src/engine/game_category.cpp).

## Data stored on this computer

| Data | Location and contents |
| --- | --- |
| Application settings | `%LOCALAPPDATA%\RebellioCap`: `config.json` and `config.last-good.json` hold onboarding state, selected devices, output directory, hotkeys and recording/preferences settings. `recording-names.json` and `notifications.json` hold their respective preferences. |
| Recordings and exports | The output directory or export destination you choose. These files contain captured or edited media. |
| Editor projects | `.rebcap` files contain edit decisions, text and references to source media paths. Packaging a project copies source media into an adjacent `assets` directory. |
| Editor recovery and interface preferences | The desktop WebView's local storage holds editor autosaves, including project metadata and source paths, plus interface and export preferences. The browser preview uses its own browser local storage. |
| Temporary media | Windows temporary storage holds playback/editor previews, remuxed video/audio and intermediate export files. Normal resource release attempts cleanup; a crash or file access failure can leave data behind. |
| Logs and system checks | `%LOCALAPPDATA%\RebellioCap\logs` holds rolling application logs. System checks also save `RebellioCap-diagnostics-*.json` files directly in the application settings directory. |

Storage behavior is defined by the [configuration store](../apps/desktop/src-tauri/src/config/store.rs),
[recording-name settings](../apps/desktop/src-tauri/src/recording_names.rs),
[notification settings](../apps/desktop/src-tauri/src/notifications.rs),
[editor](../apps/desktop/src-tauri/src/editor.rs),
[autosaves](../apps/desktop/src/editor/autosave.ts) and
[playback commands](../apps/desktop/src-tauri/src/commands.rs).
These locations are separate: deleting a recording does not delete projects,
autosaves or logs that refer to it. Backups and file-synchronization services can
retain additional copies.

## Diagnostics and sharing

Application logs record timestamps, application/Windows versions, process IDs,
commands, engine state/metrics, warnings and errors. Renderer errors and engine
standard error are forwarded to the local logger. Logs rotate across up to four
files of 2 MiB each; initialization attempts to remove log files whose last
modified time is more than seven days old. This is not a deletion deadline for
every diagnostic or for exported archives.

The logger redacts fields whose names contain `path`, `directory` or `endpoint`
and replaces some known user/temp-directory prefixes in strings. The native
diagnostic writer also redacts standalone absolute paths and pointer-like values.
Device names, filenames, arbitrary error text and other details can remain.
Treat logs, system-check JSON, screenshots, recordings and project files as
potentially sensitive.

Developer-mode log export writes a ZIP to a directory you choose. The ZIP contains
`report.json` with version/platform information and the rolling application logs;
it does not bundle recordings, editor projects or the separate system-check JSON
files. It does not upload the archive. Review the contents before sharing.
See the [logger and export implementation](../apps/desktop/src-tauri/src/logging.rs),
[renderer logging](../apps/desktop/src/app/frontendLogging.ts) and
[native diagnostic writer](../src/core/diagnostic_writer.cpp).

## Network activity

- The production renderer uses bundled assets and local IPC/media protocols.
  The configured content security policy does not permit general Internet
  connections from the renderer. See [Tauri configuration](../apps/desktop/src-tauri/tauri.conf.json).
- The interface includes optional update-check/install hooks, but the current
  [native host bridge](../apps/desktop/src/bridge/host.ts) does not implement them.
  This source does not contact a release/version endpoint on startup.
- The Windows installer is configured with `downloadBootstrapper`; if WebView2 is
  missing, installation downloads and runs Microsoft's WebView2 bootstrapper and
  requires Internet access. See [Tauri's installer documentation](https://v2.tauri.app/distribute/windows-installer/#downloaded-bootstrapper).
  Windows, WebView2, GPU drivers and their update/diagnostic services have their
  own behavior and privacy settings.
- Development uses a loopback web server and downloads dependencies/tooling from
  their registries or upstream sources. This is separate from recording.
- Choosing network or synchronized storage can cause Windows or a sync client to
  transfer media. Attaching files to GitHub or sharing an export through another
  application also sends them outside RebellioCap.

The editor's Discord option prepares a local MP4 within a selected size limit;
it does not sign in to Discord or upload the result.

## Removing data

Close RebellioCap before manually removing its local settings or log files.
Deleting `%LOCALAPPDATA%\RebellioCap` resets the native settings and removes
diagnostics in that directory, but does not remove chosen media destinations or
WebView local-storage recovery data. Save needed projects and recordings first.
The current interface does not provide a single control to erase all stored
data. Removing files is ordinary filesystem deletion, not secure erasure.

Use [Support](../SUPPORT.md) for data-location questions and the private reporting
route in [Security](../SECURITY.md) for suspected information exposure.
