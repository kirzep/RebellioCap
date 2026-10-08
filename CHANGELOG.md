# Changelog

This file describes public source changes. Tags, dates, binary artifacts, and compatibility evidence are recorded when actually published. See [release policy](docs/releases.md).

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
