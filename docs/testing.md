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

For separate overlay editor windows and transparent desktop save/cancel feedback,
build the debug desktop host, stage the engine, start `npm run dev -w @rebelliocap/desktop`,
and run `node scripts/test-native-overlay.mjs` from another terminal. This uses real
WebView2 windows and an isolated settings profile. It checks save/cancel, unchanged
main-window geometry and feedback with notifications disabled; it does not validate
recording performance or the installed application.

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

## Optimization experiments

`collect-performance.ps1 -PlanOnly -ExperimentMode combined
-ExperimentOutputRoot build/optimization/fresh-id -SettingsDeclaration '...'
-ExperimentConfig host.json` generates an offline plan without starting a game,
engine or PresentMon. Modes are `game-only`, `replay-only`, `continuous-only`,
`combined`, and `save-burst`. Runtime requires an already running game, an
explicit repeatable scene and display/game settings, a fresh evidence root,
60-second warmup and at least three alternating pairs. Evidence inside the
repository must live in an ignored subtree so later source archives cannot
recursively contain earlier experiments. The 30-minute acceptance path is kept
separate; a short result does not imply acceptance PASS.

Short frame analysis retains raw CSV and selects the most populated process /
swap-chain trace, validates timestamp coverage and interval continuity, and
stores the selected rows separately without overwrite. Save deadlines begin at
each submission, including during measurement. Missing/truncated resource
evidence and late correlated save replies fail the run.

Internal host JSON experiments optionally accept `nvencPreset` (2–5),
`nvencBFrames` (0–2), `nvencSlotCount` (6–12), `nvencMultipass` (0–2),
`nvencConstantQp`, `nvencQpI/P/B` (0–51), `nvencTargetQuality` (1–51, zero
preserves the preset), `pipelineTelemetryEnabled`, `traceTelemetryEnabled`,
`duplicateOutput1Enabled`, and `continuousAvioBufferBytes` (4 KiB–1 MiB).
Omitting them preserves existing settings. Change one factor at a time; these
are not published UI quality profiles.

The CTest `nvenc_encode` test intentionally fixes its soak to 600 seconds.
For a short headless synthetic functional check, run the executable directly
with `REBELLIOCAP_NVENC_SOAK_SECONDS=5`; setting that variable before CTest does
not override the test property. A five-second soak is not long-session or game
acceptance. Modern duplication/fullscreen transitions, physical input latency,
A/V marker alignment and multi-device/driver validation remain separate gates.

Short game experiments require an interactive terminal (TTY). Redirected or closed stdin is rejected before the scene gates or evidence creation; PlanOnly and offline analysis remain usable without a terminal. Opt-in session metrics.diagnostics reports pipeline counters and accepted encoder settings. Short-run drops use counter deltas excluding warmup; latency histograms remain session cumulative and represent CPU submit-to-collection, not GPU execution time.

The internal directNv12CacheEnabled boolean enables the O09 canonical NV12 prototype. Baseline BGRA remains the default. Hardware converter checks include pixel readback only in tests, outstanding leases through resize, bounded input-view eviction, static repeat and all rotations. The audio/video lifetime executable also verifies three direct-backend idle/resume cycles. Three short Cyberpunk pairs showed worse game p99.9 on direct in every pair, so the experiment remains off. GPU timing, longer acceptance and other drivers remain separate gates.

The continuous recovery hardware fixture tests 32/128/256/512/1024 KiB AVIO buffers in MP4 and MKV with reordered video and three AAC tracks. Its offline NVENC generator waits for admission with a five-second deadline before conversion and requires every synthetic frame to be submitted and decoded.

For full decode validation of variable frame timestamps, preserve the demux time base: `ffmpeg -v error -xerror -err_detect explode -i clip.mp4 -map 0:v -map 0:a -fps_mode:v passthrough -enc_time_base:v demux -f null NUL`. Independently check per-stream packet DTS with ffprobe. A nominal 1/framerate output time base can round distinct source timestamps together in the null output. Keep decode logs; zero exit code alone does not establish empty diagnostics.

`scripts/measure-replay-snapshot.cpp` is a standalone metadata microbenchmark with its build command in the header. It measures 30-second/2-minute/5-minute/30-minute rings, short/full snapshots, sixteen retained snapshots and GOP-boundary appends. A shared tiny payload does not replace game contention, realistic memory pressure or long-session acceptance.

The hidden mux hardware case `[.quality-reference]` accepts
`REBELLIOCAP_QUALITY_INPUT` (exactly 240 BGRA frames at640×360) and
`REBELLIOCAP_QUALITY_OUTPUT` (a fresh, nonexistent directory). Run the mux
executable directly with that filter. It emits fourteen single-factor NVENC
profiles and test-only post-converter NV12 references. Validate identical
reference hashes, all240 decoded frames, per-stream DTS and aligned per-frame
metrics before comparing SSIM/PSNR. This tiny synthetic workload is not a
game-quality or throughput acceptance test. The production path has no CPU
upload/readback added by this fixture.

Continuous unit coverage includes failure partway through a drained batch,
shared-budget release, four producers racing stop with exact accepted-prefix
drain, and successive isolated batches making progress before stop supplies
any extra notification. These tests do not replace storage latency profiling.

An extended whole-pipeline soak passed full decode and DTS checks but did not
meet the 50 ms A/V marker limit (continuous recording 327 ms, final replay
320 ms). Treat this as a failed long-session sync gate; synthetic marker tests
do not replace hardware acceptance.

`pcm_clock_aligner` tests both signs of100/1000ppm drift, zero-drift frame
preservation, sine continuity with packet jitter, silence/recovery, irregular
batch sizes through recovery, invalid/unbounded input, sub-filter-length
segments and timestamp overflow. Hardware audio lifetime checks feed corrected
PCM through real AAC encoders and exercise buffered44.1k normalization. These
deterministic checks do not replace hardware marker and long-session acceptance.

`scripts/measure-audio-clock.cpp` is a read-only pinned-loopback clock probe;
its header contains build/run commands. It records only QPC/device-position
statistics, never PCM, and cannot establish microphone or other-device results.

Separately packaged app variants select their own local configuration profile;
the stable application keeps its established settings path. Unit and config
integration tests cover profile selection. Startup behavior still needs a
manual check in the packaged Windows application.

Interactive performance scene gates continue pumping engine protocol while
waiting for Enter. A bounded focus handoff precedes timing; focus loss during
measurement invalidates that run. Automated fixtures cover these paths.
