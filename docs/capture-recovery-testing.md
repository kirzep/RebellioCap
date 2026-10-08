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

Device removal, resolution changes requiring a rebuilt video pipeline, encoder,
audio and unrelated errors still follow normal terminal error handling. There
is no unconditional process restart which would hide these failures.

Manual release checks (do not suspend a machine during an active automated run):

1. Start replay; turn off monitors, restore them, then save a replay.
2. Start a recording; lock/unlock Windows, then stop and play the recording.
3. Repeat with Windows sleep/resume and the same selected output/resolution.
4. Check logs for `capture.suspended`, sampled `capture.retry`, and
   `capture.restored`; confirm engine PID stays unchanged.
5. Confirm stop/quit remains responsive while the monitor is unavailable.
6. Confirm a genuine device/encoder failure is still reported as an error.

Automated capture-recovery tests inject access loss and secure-desktop failures,
verify retry pacing, one hour of simulated unavailability, restoration, and
propagation of unrelated/fatal errors. They do not prove real hardware sleep
behaviour on every GPU or audio driver.
