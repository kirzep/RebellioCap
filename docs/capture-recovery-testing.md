# Desktop capture recovery

The engine recreates Desktop Duplication after access loss, a secure desktop,
session disconnection, or temporary disappearance of the selected monitor.
It retains the same engine, encoder, recording and replay configuration.
Retries happen at most once per second; repeated waiting logs are sampled
every 30 attempts. Suspension and the first fresh frame after restoration are
logged through engine stderr into the bounded application log.

During suspension, video packets are not produced and the cached desktop image
is discarded. The first resumed frame requests a keyframe. The unavailable
interval cannot be captured. Recording timestamps retain the elapsed time.
The monitor is matched by its Windows name and adapter, rather than silently
switching to another output after enumeration changes.

When the selected monitor returns with different input dimensions or rotation,
the engine rebuilds the capture-sized GPU cache and converter input. Configured
output dimensions, the NVENC session and container descriptors remain stable;
the next fresh frame requests a keyframe. Graphics device removal, failed
converter rebuilding, encoder failures and unrelated capture errors still
follow normal terminal error handling. There is no unconditional process restart
which would hide these failures.

Audio recovery is independent. Expected endpoint loss, temporary endpoint
unavailability and unreliable timestamps retry the pinned endpoint at most
once per second. The audio source supplies timestamped silence during recovery
and marks resumed audio as discontinuous. It also supplies silence when idle
loopback stops delivering packets, after allowing for normal delivery latency.
The source timeline limits catch-up after a scheduler stall to 500 ms. It does
not silently switch to another endpoint; unrelated audio and AAC encoder errors
remain failures. See [audio recovery](../src/audio/recovering_audio_source.h) and
[hardware pipeline integration](../src/engine/hardware_pipeline_factory.cpp).

Manual release checks (do not suspend a machine during an active automated run):

1. Start replay; turn off monitors, restore them, then save a replay.
2. Start a recording; lock/unlock Windows, then stop and play the recording.
3. Repeat with Windows sleep/resume and the same selected output/resolution.
4. Check logs for `capture.suspended`, sampled `capture.retry`, and
   `capture.restored`; confirm engine PID stays unchanged.
5. Confirm stop/quit remains responsive while the monitor is unavailable.
6. Change the selected monitor's resolution or rotation; confirm captured content
   adapts while the recording's configured output dimensions stay unchanged.
7. Disconnect/reconnect a selected audio endpoint; confirm it is marked
   unavailable, silence covers the outage, and the same endpoint resumes.
8. Confirm a genuine device/encoder failure is still reported as an error.

Automated [capture-recovery tests](../tests/unit/capture_recovery_test.cpp) inject
access loss and secure-desktop failures, verify retry pacing, one hour of
simulated unavailability, restoration, and propagation of unrelated/fatal
errors. [Audio recovery tests](../tests/unit/recovering_audio_source_test.cpp)
exercise endpoint retry, silent intervals, discontinuities and fatal errors.
These tests do not prove real hardware sleep behavior on every GPU or audio
driver. See [testing boundaries](testing.md) for the wider verification workflow.
