# Testing and verification

RebellioCap tests the engine, desktop host and interface separately, then uses
opt-in checks for their integration with real Windows devices. A passing unit
or browser run establishes only the behavior exercised by that suite. This
document describes the checks in source; it is not a claim that a particular
commit, installer or hardware configuration has passed them.

## Test layers

| Layer | What it exercises | Boundary |
| --- | --- | --- |
| C++ and script unit tests | Scheduling, replay snapshots, packet budgets, protocol parsing, recovery, audio alignment, resource ownership and verification tooling | Windows build required; injected pipelines and software D3D checks do not validate a real NVIDIA driver or capture device |
| Rust host tests | Protocol correlation, child-process ownership, configuration persistence, path policy, editor lifecycle and rollback | Most tests use fixtures; real engine and media suites are explicitly ignored by default |
| Frontend unit tests | State transitions, controls, validation, editor model, autosave, playback errors and accessibility behavior | Vitest with a simulated DOM and host substitutes |
| Browser E2E | Onboarding/resume, settings, naming, gallery, playback, editor interaction and keyboard navigation | Chromium with host fixtures; does not exercise Tauri, Windows shell integration or NVENC |
| Hardware/native integration | DXGI, NVENC, conversion, audio endpoints/AAC, muxed output and real engine control | Interactive Windows session and applicable devices required; unavailable checks are not successful hardware evidence |
| Native WebView2 smoke | Actual desktop UI, Rust commands, engine, playback/editor and overlay behavior | Separate development-host runner; does not establish installer lifecycle or cross-machine compatibility |
| Soak, crash and performance collection | Long-running capture, interrupted recording, media inspection and measured host behavior | Opt-in experiments whose results depend on the recorded binary, configuration, hardware and workload |

CTest labels are defined in [tests/CMakeLists.txt](../tests/CMakeLists.txt).
`unit` is the normal deterministic gate; `hardware` requires real platform
resources; `performance` marks measured or extended checks. A test can have
both `hardware` and `performance` labels. The normal debug/release presets
exclude both, while `windows-hardware-release` enables and includes them.
Several hardware tests use exit code 77 to report a skip. Inspect skip status
as well as the overall CTest exit code.

## Routine development gate

Install the toolchain and dependencies described in
[Windows development](development/windows-toolchain.md), then run from the
repository root:

```powershell
./scripts/verify-desktop.ps1
```

The script stops at the first failed command. It verifies project naming,
configures/builds the debug C++ engine, runs the default CTest preset, runs Rust
tests, then type-checks, tests and builds the frontend. It also checks that the
Tauri CSP contains `default-src 'self'`. That check is a configuration assertion,
not a complete security audit. The workspace's `lint` task currently runs the
TypeScript compiler, so it does not represent a separate style-linter pass.

Run the browser suite separately after installing its Chromium runtime:

```powershell
npx playwright install chromium
npm run test:e2e -w @rebelliocap/desktop
```

[Playwright configuration](../apps/desktop/playwright.config.ts) starts the Vite
server on port 1420 and runs one Chromium worker. The tests inject host fixtures;
some use real media files to exercise browser decoding, while native import,
save and export operations are substituted. The
[first-run fixture](../apps/desktop/e2e/fixtures/first-run.tsx) makes this boundary
explicit.

Useful focused commands are already used by the repository's scripts and CI:

```powershell
./scripts/invoke-dev.ps1 -Command @('ctest', '--preset', 'windows-debug', '--output-on-failure')
./scripts/invoke-dev.ps1 -Command @('cargo', 'test', '--locked', '--manifest-path', 'apps/desktop/src-tauri/Cargo.toml')
npm run desktop:test
npm run test:crash-coverage
```

CTest assumes the selected build has been configured and compiled. The final
command tests the crash-media coverage analyzer using synthetic frame coverage;
it does not crash or record the engine.

## Review paths for difficult behavior

These tests are useful entry points when reviewing the engineering decisions:

| Behavior | Representative evidence in source |
| --- | --- |
| Frame-index scheduling, missed deadlines, late capture timestamps and idle/resume | [engine_scheduler_test.cpp](../tests/unit/engine_scheduler_test.cpp) |
| Keyframe boundaries, reordered PTS/DTS, outage expiry and immutable save snapshots | [replay_ring_test.cpp](../tests/unit/replay_ring_test.cpp) |
| Shared packet accounting, outstanding-save pressure and bounded continuous writes | [packet_budget_policy_test.cpp](../tests/unit/packet_budget_policy_test.cpp), [continuous_recording_test.cpp](../tests/unit/continuous_recording_test.cpp) |
| DXGI frame leases and unsafe NVENC shutdown paths | [dxgi_frame_lifetime_test.cpp](../tests/unit/dxgi_frame_lifetime_test.cpp), [nvenc_shutdown_test.cpp](../tests/unit/nvenc_shutdown_test.cpp) |
| Recoverable outages and fatal-error propagation | [capture_recovery_test.cpp](../tests/unit/capture_recovery_test.cpp), [recovering_audio_source_test.cpp](../tests/unit/recovering_audio_source_test.cpp) |
| QPC audio alignment, silence, discontinuity and bounded mixing | [wasapi_timeline_test.cpp](../tests/unit/wasapi_timeline_test.cpp), [pcm_windowizer_test.cpp](../tests/unit/pcm_windowizer_test.cpp) |
| Strict framing, separate save acceptance/completion, timeouts and process cleanup | [Rust protocol tests](../apps/desktop/src-tauri/tests/protocol.rs), [Rust supervisor tests](../apps/desktop/src-tauri/tests/supervisor.rs) |
| Configuration recovery, future schemas and evidence tied to tested settings | [Rust configuration tests](../apps/desktop/src-tauri/tests/config.rs) |
| Editor ownership, cancellation, project round trips and media export | [editor.rs tests](../apps/desktop/src-tauri/src/editor.rs), [frontend editor tests](../apps/desktop/src/editor/ClipEditor.test.tsx) |

Virtual long-duration scheduling and injected outages cover algorithmic
invariants quickly. They do not replace wall-clock soak runs. The replay
concurrency test exercises simultaneous append and snapshot operations, but
explicitly has no MSVC ThreadSanitizer coverage.

## Hardware and native desktop checks

On a compatible interactive Windows/NVIDIA host:

```powershell
./scripts/verify-desktop.ps1 -Hardware -EvidenceRoot artifacts/desktop-first-run
```

This adds the hardware release build and CTest preset, stages the engine runtime,
then runs [test-native-desktop.ps1](../scripts/test-native-desktop.ps1). The native
runner enables the normally ignored Rust engine and editor tests, verifies
staged resource hashes, and records logs and build identity in a fresh evidence
directory. The full hardware preset includes extended performance-labeled
checks; it is not a quick smoke run.

The [native engine tests](../apps/desktop/src-tauri/tests/native_engine.rs) check
real catalogs, doctor, session control, saving and endpoint metering. The ignored
editor tests process actual media. These checks still do not operate the Tauri
WebView. The separate [test-webview2-desktop.mjs](../scripts/test-webview2-desktop.mjs)
runner requires a debug desktop executable, staged engine, installed WebView2
and the Vite server at `127.0.0.1:1420`. It launches an isolated test instance
with a temporary configuration and records screenshots and results. It uses
the real bridge rather than the browser fixtures.

Manual lock/unlock, monitor-off and sleep/resume procedures are described in
[capture recovery](capture-recovery-testing.md). Repeat them for the intended
monitor and audio devices. Automated access-loss injection does not establish
sleep behavior for every driver.

## Long-session and release evidence

The repository includes separate collectors with different scopes:

| Tool | Output and interpretation |
| --- | --- |
| [test-engine-soak.mjs](../scripts/test-engine-soak.mjs) | Combined replay/continuous recording run; requires a validated native config through `REBELLIOCAP_SOAK_CONFIG`; records events, samples and clip inspection |
| [test-continuous-crash.mjs](../scripts/test-continuous-crash.mjs) | Deliberately terminates its own recording child; requires `REBELLIOCAP_CRASH_CONFIG`; inspects retained MP4/MKV media with [verify-crash-media.mjs](../scripts/verify-crash-media.mjs) |
| [collect-replay-memory.ps1](../scripts/collect-replay-memory.ps1) | Samples process memory and packet-budget metrics; uncontrolled desktop content does not prove the configured bitrate or worst-case memory behavior |
| [collect-performance.ps1](../scripts/collect-performance.ps1) | Host-specific baseline/capture measurements with a declared game workload and pinned PresentMon; results need the same settings and source identity |
| [verify-milestone-1.ps1](../scripts/verify-milestone-1.ps1) | Collects or verifies the defined stability/stream/performance evidence bundle; missing, failed or `not-run` evidence remains incomplete |

The detailed performance collection sequence is in
[Windows development](development/windows-toolchain.md). Keep binary hashes,
source identity, configuration, environment and workload with results. Generated
logs, media, traces and screenshots can contain local paths or captured desktop
content; keep them separate from the public source tree.

A successful decode and sane timestamps establish media-level properties, not
physical lip-sync or playback in every external player. Packet-budget checks
exclude GPU, codec and PCM allocations and cannot be read as a process RSS cap.
Crash retention does not establish disk-full recovery or preservation of the
last incomplete fragment. Installer installation, upgrade, uninstall, HDR and
cross-device acceptance require their own evidence.

## Continuous integration

[Desktop checks](../.github/workflows/desktop-ci.yml) defines two Windows jobs:

- The frontend job runs type checking, the compiler-backed lint task, Vitest,
  production build, browser E2E and crash-coverage analyzer tests.
- The native job bootstraps pinned vcpkg dependencies, builds the debug engine,
  runs the default CTest preset, stages runtime resources and runs locked Rust
  tests.

The workflow does not run the hardware preset, ignored native media suites,
actual WebView2 UI, long-session collectors or installer acceptance. Use the
workflow result for the exact commit under review; the presence of this file
alone is not a passing-build claim.
