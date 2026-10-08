# Release policy and packaging

[Documentation](README.md) · [Testing](testing.md) · [Licensing](../LICENSE.md)

Desktop manifests declare **0.1.12**; CMake and vcpkg declare **0.1.0** for the engine/native package. Source versions do not establish a verified binary release. Check [GitHub Releases](https://github.com/kirzep/RebellioCap/releases) for explicitly published binary artifacts.

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

## GitHub installer builds and application updates

The installer is built only in the public `kirzep/RebellioCap` repository. The private repository mirrors the same published installer, signature, manifest and checksums; it does not rebuild or need the signing Secrets. It builds the exported source, including the release engine, FFmpeg, runtime DLLs and license files. Updates use the official Tauri updater with the public endpoint `https://github.com/kirzep/RebellioCap/releases/latest/download/latest.json`; no separate server or GitHub token in the installed application is required.

### One-time signing configuration

Create an updater signing key with the Tauri signer, keep the private key outside the repository, and back up the private key and its password securely. The public key in `apps/desktop/src-tauri/tauri.conf.json` must match it. Never replace this key casually: installations trust that public key for future updates.

In the **public repository**, open Settings → Secrets and variables → Actions and add these repository Secrets:

- `TAURI_SIGNING_PRIVATE_KEY`: the complete contents of the private key file, not its local path.
- `TAURI_SIGNING_PRIVATE_KEY_PASSWORD`: its password.

These Secrets are read only by the release build. They are not required for ordinary checks or pull requests. Missing Secrets stop the build before installer production. Local signed builds use the same values as environment variables; the CLI also accepts an absolute private-key path locally. Never commit the private key or password. Updater signatures verify update authenticity; Windows Authenticode signing is a separate capability and is not configured by this workflow.

### Release procedure

1. Set matching desktop versions in `apps/desktop/package.json`, `apps/desktop/src-tauri/Cargo.toml`, `apps/desktop/src-tauri/tauri.conf.json` and refresh the lockfiles. The engine's component version is independent.
2. Publish the reviewed source snapshot to this public repository using its normal export procedure.
3. Create a stable tag such as `v0.1.9` on the public commit and push that exact tag. Alternatively run **Windows installer release** manually with an existing stable tag. Tags with prerelease suffixes and mismatched desktop versions are rejected.
4. The public workflow runs software tests and produces the EXE installer, `.sig`, `latest.json` and `SHA256SUMS`. It uploads them to a draft, verifies checksums and manifest identity, then publishes the stable release automatically. Generated release notes are copied into the update manifest. Published versions are never overwritten.
5. Push a tag with the same version on the matching private source commit. Its workflow waits for the public release, verifies downloaded artifacts and publishes identical assets in the private repository using its own workflow token. Alternatively dispatch that workflow with the existing private tag. No cross-repository token is required.
6. The README download button opens the latest public release. The latest published stable release becomes the application update source. Drafts and prereleases are not offered to users.

A release tag authorizes publication after the automated software and artifact checks. Perform the installer, update and hardware acceptance below before tagging when those guarantees are required. CI does not establish GPU capture acceptance or the real installer lifecycle. Failed checks leave the release unpublished.

### Application behavior

Release builds check once in the background when the main screen starts. Development builds do not check or install updates. An unavailable endpoint does not block startup or recording. When a newer stable version exists, a button above Settings offers it with release notes and explicit confirmation.

After confirmation, the application downloads the full installer and verifies its signature **before stopping recording**. Download or signature failure leaves recording running and allows a retry. Installation saves the current recording, disables Replay and discards its unsaved buffer. Close the editor and finish exports before updating. The NSIS installer runs in passive mode and requests a restart of the application. Installer launch failure is shown in the application; recording can be started again. Configuration and saved clips remain outside the installed program files and are retained.

The first updater-enabled version is **0.1.12**. Existing 0.1.7 users need to install it manually once; subsequent published versions can be installed from the application. Full-package updates are supported; delta updates and automatic rollback are not included.

### Installer/update acceptance

Before claiming end-to-end update acceptance, install an updater-enabled signed version on Windows, publish a second version to the intended update endpoint, and verify check → consent → download → signature → recording completion → passive install → restart. Check retained settings and clips, single-instance and autostart behavior. Test network and signature failure while recording, postpone/retry behavior and an open editor/export. This requires installed builds and published update artifacts; unit and browser tests alone do not establish it.

## Distribution acceptance gates

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
