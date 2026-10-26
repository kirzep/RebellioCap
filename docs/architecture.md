# Architecture

RebellioCap separates desktop interaction from the Windows capture pipeline.
React renders the interface, the Tauri Rust host owns configuration and native
operations, and a C++20 child process captures and encodes media. The engine is
also usable through its CLI, which lets tests exercise recording without a
WebView.

## Process and data flow

```mermaid
flowchart TB
    UI["React / TypeScript interface"]
    Host["Tauri Rust host<br/>configuration, clips, editor, tray"]
    Session["C++ engine session<br/>commands and snapshots"]
    Video["DXGI Desktop Duplication<br/>D3D11 conversion → NVENC H.264"]
    Audio["WASAPI capture<br/>PCM normalization / mixing → AAC"]
    Packets["Timestamped encoded packets<br/>shared ownership and memory budget"]
    Replay["Replay ring"]
    Save["Replay save worker"]
    Continuous["Continuous recording writer"]
    Files["MP4 / MKV files"]
    Editor["FFmpeg / ffprobe child processes<br/>preview preparation and edited export"]

    UI <-->|"typed host bridge"| Host
    Host <-->|"versioned NDJSON over pipes"| Session
    Session --> Video
    Session --> Audio
    Video --> Packets
    Audio --> Packets
    Packets --> Replay
    Replay -->|"immutable save snapshot"| Save
    Packets --> Continuous
    Save --> Files
    Continuous --> Files
    Host --> Editor
    Files --> Editor
```

The recording path uses FFmpeg libraries for muxing already encoded packets.
The editor uses the staged FFmpeg executables for media processing. These are
separate paths: replay saving does not re-encode the captured video.

## Responsibilities

| Layer | Responsibilities | Entry points |
| --- | --- | --- |
| Desktop interface | Onboarding, recording controls, gallery, playback, editor interaction | [host bridge](../apps/desktop/src/bridge/host.ts), [editor model](../apps/desktop/src/editor/model.ts) |
| Rust host | Validate and persist settings; own engine and editor processes; mediate file access; expose commands | [commands](../apps/desktop/src-tauri/src/commands.rs), [engine supervisor](../apps/desktop/src-tauri/src/engine/supervisor.rs), [editor](../apps/desktop/src-tauri/src/editor.rs) |
| C++ session | Serialize control commands, publish state, correlate save completions | [session controller](../src/engine/session_controller.cpp), [session loop and hardware factory](../src/engine/hardware_pipeline_factory.cpp) |
| Recording engine | Schedule video, pump audio, dispatch hotkeys, maintain replay, queue saves and continuous writes | [recorder engine](../src/engine/recorder_engine.cpp), [continuous recording](../src/engine/continuous_recording.cpp) |
| Platform adapters | Capture devices, GPU conversion, hardware encoding, containers | [capture](../src/capture/dxgi_capture_source.cpp), [audio](../src/audio/wasapi_capture_source.cpp), [NVENC](../src/video/nvenc_encoder.cpp), [muxers](../src/mux/ffmpeg_continuous_muxer.cpp) |

The engine depends on clock, pipeline, hotkey and muxer interfaces through
[RecorderEngineDependencies](../src/engine/recorder_engine.h). Tests substitute
those dependencies to simulate delayed encoding, blocked disk writes and
device outages without adding production fault-control switches.

## Control boundary

The host launches the canonical engine executable with explicit arguments and
places it in a Windows Job Object before resuming it. Closing the job terminates
owned processes. The supervisor serializes requests through an actor and waits
for events or request deadlines; it does not periodically poll an idle session.

The [C++ protocol](../src/engine/session_protocol.h) and
[Rust protocol](../apps/desktop/src-tauri/src/engine/protocol.rs) use protocol
version 1, request IDs and newline-delimited JSON frames limited to 64 KiB.
Malformed frames, incompatible versions and invalid response sequences are
errors. A replay save has two distinct events: acceptance (`command_result`)
and completion (`clip_saved` or an error). Acceptance alone does not establish
that a file exists. See the [protocol tests](../apps/desktop/src-tauri/tests/protocol.rs)
and [supervisor tests](../apps/desktop/src-tauri/tests/supervisor.rs).

Recording ranges and default hotkeys come from one
[JSON contract](../contracts/recording-settings.v1.json), consumed by C++, Rust
and TypeScript. The Rust host validates settings before turning them into
engine configuration. Its [configuration store](../apps/desktop/src-tauri/src/config/store.rs)
tracks onboarding drafts separately from active settings, binds test-recording
evidence to the tested configuration, and maintains a backup during replacement.
The [configuration tests](../apps/desktop/src-tauri/tests/config.rs) cover stale
evidence, corrupt files, future schemas and failed replacements.

## Media timing and ownership

Video remains in D3D11 textures through capture, scaling, rotation and conversion
to NV12 before NVENC submission. A latest-frame GPU cache allows the scheduled
encoder to reuse unchanged desktop content. Frame leases keep textures alive
while GPU work still references them; the [DXGI lifetime tests](../tests/unit/dxgi_frame_lifetime_test.cpp)
and [NVENC state tests](../tests/unit/nvenc_state_machine_test.cpp) exercise those
ownership transitions. If driver completion cannot be established during
shutdown, NVENC retains unsafe-to-release resources and requires a process
restart instead of freeing in-flight GPU resources. The tradeoff is retained
resources until process exit; see [shutdown tests](../tests/unit/nvenc_shutdown_test.cpp).

Audio and video share the QueryPerformanceCounter (QPC) timeline. Video deadlines
are calculated from a frame index and origin, so rounding does not accumulate
through repeated sleeps. Actual capture work uses the current clock; missed
deadlines are counted and skipped. Audio is normalized and aligned in bounded
PCM windows before AAC encoding. With both sources enabled, files contain a
mixed track plus separate system and microphone tracks. Source tracks are
encoded before their copies enter the bounded mixing queue, so a mixing overrun
does not discard the independent source audio.

[EncodedPacket](../src/media/encoded_packet.h) carries presentation time, decode
time, duration, keyframe status and a capture epoch. The
[replay ring](../src/replay/replay_ring.cpp) starts saves at a preceding keyframe
and retains the decode references needed by reordered pictures, even when a
reference has a later presentation time than the requested clip end. Snapshots
share immutable compressed payloads and own their packet metadata. New capture
epochs keep a resumed session from combining incompatible buffered intervals.
See [replay tests](../tests/unit/replay_ring_test.cpp) and
[scheduler tests](../tests/unit/engine_scheduler_test.cpp).

## Storage, backpressure and idle behavior

Capture workers hand packets to the replay ring and continuous writer. Replay
saves run on a separate worker; their destination category and naming preset
are frozen at the request boundary. The default limit is 16 outstanding saves,
including the active write. Continuous recording has its own writer and a
4,096-packet queue. Slow storage can therefore produce a visible queue or memory
error without making capture wait for the disk operation.

One [packet memory budget](../src/media/packet_memory_budget.h) accounts for
retained compressed allocations across replay, pending snapshots and continuous
recording. Shared payloads are charged once; snapshot metadata is also reserved.
The [budget policy](../src/engine/packet_budget_policy.h) selects an automatic
limit from available RAM or validates a manual limit. Under pressure, replay
evicts complete GOPs and may retain less than the requested duration; continuous
recording stops with an error if it cannot accept more packets. This budget is
not a process-memory cap: GPU surfaces, codecs, PCM and other allocations are
outside it.

When replay and continuous recording are both off, capture workers flush and
release their pipelines while control and already queued saves remain alive.
Resuming creates fresh hardware sessions and requests a keyframe. This avoids
keeping capture resources active solely to display the desktop controls.

Muxers create sibling temporary files without replacing an existing destination.
Successful finalization flushes and renames the file. Continuous MP4 uses
keyframe fragments; a failed or interrupted write may leave a `.partial` file
containing completed media. This is a recovery opportunity, not a guarantee
that the final fragment or every interrupted file is playable. See
[continuous recording tests](../tests/unit/continuous_recording_test.cpp) and
[crash verification](../scripts/verify-crash-media.mjs).

## Recovery and operational limits

Expected display transitions recreate Desktop Duplication with paced retries
for the selected monitor identity. During unavailability the engine emits no
new video packets and discards cached content; restoration requests a keyframe.
Input geometry and rotation can rebuild the converter while keeping configured
output dimensions and the encoder stable. Recoverable audio endpoint loss
retries the pinned device and supplies timestamped silence while unavailable.
Device removal, encoder failures and unrelated errors remain visible failures.
See [capture recovery](capture-recovery-testing.md) for the manual checks.

The host grants asset access to specific prepared playback and editor files,
then revokes it when their owner releases them. Editor imports and exports have
deadlines, cancellation and owned child processes; preview lifetime is separate
from original media and export lifetime. Tests cover cancelled publication,
scope rollback and cleanup in the [editor implementation](../apps/desktop/src-tauri/src/editor.rs).

This design targets Windows x64 and NVIDIA encoding. Deterministic tests cannot
establish compatibility with every display, audio driver, HDR configuration or
game. [Testing](testing.md) distinguishes those tests from real hardware and
long-session acceptance.

## Optimization instrumentation and admission

NVENC admission collects completed packets before the converter leases an output
surface. A saturated queue skips that input tick without losing a pending IDR;
completed packets still reach fanout when no image is available. Internal pool
experiments accept 6–12 encoder slots and reserve two additional converter
surfaces. The default remains 6/8. P2–P5, B=0/1/2, explicit multipass and bounded
VBR quality/CQP configurations are experiments; the decode timeline uses the
requested reorder depth. They have not selected new product profiles.

Optional `PipelineTelemetry` records fixed atomic latency histograms without
allocation on updates. Percentiles are bucket upper bounds; overflow reports
the observed maximum. Concurrent snapshots are approximate; quiescent snapshots
are exact. CPU-side scopes measure host elapsed time, including waits inside
an API, rather than GPU execution or per-thread CPU consumption. NVENC
submit-to-collection includes B-frame delay and polling. The legacy latency
histogram remains capped at 1.024 seconds; the optional stage histogram has its
own wider bound and overflow count. TraceLogging is disabled by default.

Continuous admission publishes a whole batch under one queue lock. The worker
drains up to 32 packets / 1 MiB by default and permits an oversized first packet.
Queue byte/age metrics include in-flight work; age is measured from enqueue QPC,
not media DTS. Age limits are opt-in and remain active while draining on Stop.
The shared budget accounts encoded owners by allocation capacity; logical queue
bytes are a separate measure. The copied FFmpeg buffer retains the encoded
budget owner through interleaving. FFmpeg's additional copy is not a total RAM
hard-cap guarantee. AVIO defaults to 32 KiB and scheduling remains unchanged.

A missing NVENC completion beyond the internal 30-second deadline is terminal;
it does not authorize reuse of driver-owned textures. Uninterruptible vendor
calls remain behind the engine process boundary. The host bounds stdin writes
and stale validated-snapshot waits, terminating the whole owned process job
before joining its pipe worker. Device removal remains an explicit failure;
seamless new-device/container continuation is not implemented.

The optional direct NV12 experiment owns a canonical, unregistered NV12 texture per converter generation. Only free leased pool textures are copied from it and submitted to NVENC. Static ticks do not access previous Desktop Duplication inputs. Up to four input views retain COM identities; reconfiguration invalidates the canonical image/views while preserving registered output surfaces and their leases. The initial unsupported input view can fall back to BGRA before any encode submission; device/resource failures remain terminal. The fallback BGRA allocation is retained in this prototype and must be included in memory comparisons.

Normalized stereo48k PCM passes through a per-source `PcmClockAligner` before
source AAC, epoch bridging and mixing. It compares capture QPC with emitted
sample cadence, smooths phase over two seconds and applies bounded soft
libswresample compensation once per second (at most2000ppm). Ordinary clock
drift changes sample count smoothly; it is not repaired by relabeling AAC PTS
or by per-packet silence/overlap cuts. A discontinuity drains the old resampler
before a fresh segment, retaining the existing bridge recovery-gap policy.
Under64 startup frames are retained untouched until enough filter support is
available, or published directly on final drain. Input/output allocations and
drain work are bounded. Long-session hardware acceptance is still pending.

The stable desktop identifier `com.rebelliocap.desktop` retains its established
LocalAppData/RebellioCap configuration directory. Separately packaged variants
select Tauri's per-app local directory before ConfigStore appends RebellioCap,
so a test package does not reuse stable settings merely because the executable
and UI retain the same name.
