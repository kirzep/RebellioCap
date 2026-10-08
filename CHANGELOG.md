# Changelog

This file describes public source changes. Tags, dates, binary artifacts, and compatibility evidence are recorded when actually published. See [release policy](docs/releases.md).

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
