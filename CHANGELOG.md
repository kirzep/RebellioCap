# Changelog

This file describes public source changes. Tags, dates, binary artifacts, and compatibility evidence are recorded when actually published. See [release policy](docs/releases.md).

## 0.1.18

### Added

- A separate full-screen overlay editor window lets you move the recording status, replay status, and messages anywhere on the preview. Use `Esc` to discard changes or `Ctrl+Enter` to save; the editor confirms the result on screen.
- Compact recording and replay indicators show their current state while you play. Messages report when capture starts, stops, saves a replay, or encounters an error.
- Overlay and notification options now have a dedicated settings page. Choose custom notification sounds and place each overlay item to suit your screen.
- Add optional capture diagnostics and performance tools for investigating frame timing, audio alignment, replay memory, and recording queues.

### Improved

- Keep the overlay hidden when recording and replay are both off, so it does not cover the desktop while idle.
- Keep replay responsive when the encoder is busy, and preserve keyframe requests while frames are skipped under load.
- Gradually correct audio clock drift to improve long-recording sync. Long-session hardware acceptance is still pending.
- Make recording work more resilient under load and report stalled encoder sessions clearly. Optional capture experiments remain off by default.

### Fixed

- Create the overlay editor outside the Windows synchronous command dispatcher and match the shared WebView2 browser arguments to prevent hangs and window creation errors. Keep the main window size and position intact.
- Close the editor before showing saved or discarded feedback on a transparent desktop overlay, then restore settings. Keep Escape available if editor settings fail to load.
- Save overlay placement as screen-relative positions so the editor preview and desktop overlay use the same layout.
- Replace corner buttons that could cover overlay items with keyboard shortcuts, and show clear saved or discarded feedback after closing the editor.
- Keep separate packaged app variants from reusing the stable installation's settings folder.
- Keep benchmark controls responsive while waiting for a game scene, and reject runs if the game loses focus during timing.

## 0.1.17

- Add “Save recordings without game subfolders” in recording settings. Replay clips and continuous recordings can be saved directly to the selected folder; the choice persists across restarts and defaults off.

## 0.1.16

- Show the actual Windows language flag even when the application uses a different language.
- Open the clip editor inside the main workspace with an automatically compact sidebar. Keep the montage and undo history when switching sections.
- Remember declined recovery prompts and stop offering empty projects. Preserve declined snapshots locally; new edits make them eligible for recovery again.

## 0.1.15

- Style the language dropdown consistently with the application and add Russian / English flags.
- Render update release notes as Markdown instead of showing raw formatting markers.
- Use a neutral download progress bar matching the interface.
- Install subsequent updates silently for the current user and restart the application automatically. Updating from an older build still uses that build's installer mode.

## 0.1.14

- Move the interface language selector above Settings in the sidebar, including access while collapsed.
- Save persistent display identities and resolve current adapter addresses when starting or recovering capture. Migrate older saved addresses when they still identify a connected display.
- Add audio gap/reset recovery integration coverage; the reported older-version audio epoch failure is already handled by the current engine.

## Unreleased

### Added

- Signed Windows installer builds in the public repository, with published releases and update manifests, plus identical assets mirrored to the private repository.
- Background update checks and an update button above Settings, with confirmation, download progress and installation from the application.
- Signature and package-version checks before recording shutdown; updates preserve configuration and saved clips and require the editor to be closed.

### Fixed

- Milestone evidence uses the CMake executable recorded by the configured build instead of requiring an optional vcpkg-downloaded tool cache.

### Documentation and repository setup

- English/Russian overviews, current UI screenshots, and a documentation index.
- Requirements, source-build walkthrough, architecture, testing, troubleshooting, privacy, and packaging guides.
- Contribution/support/security guidance and issue/pull-request templates.
- Explicit project licensing status alongside preserved third-party notices.

## Initial public source snapshot

Initial public commit: [7525685](https://github.com/kirzep/RebellioCap/commit/75256856b83b3edd1736dd66fda5c9125c67177b). Desktop manifests declare **0.1.7**; this is not a binary release announcement.

The source includes Windows capture using direct NVENC H.264, WASAPI audio and Media Foundation AAC, bounded replay, continuous MP4/MKV recording, capture/audio recovery, a Rust/Tauri host, a React interface, a local clip library, and timeline editing/export/recovery.

Native, host, frontend, browser, and script tests are included. Broad hardware compatibility, installer acceptance, HDR fidelity, and published performance results are not established by this entry.
