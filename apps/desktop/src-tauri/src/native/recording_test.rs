use crate::{
    config::{ActiveConfig, OnboardingDraft},
    engine::{supervisor::run_once, Result},
    model::*,
};
use serde::Deserialize;
use serde_json::{json, Value};
use std::{
    io::Write,
    path::{Path, PathBuf},
    time::Duration,
};

fn hotkey(key: &Hotkey) -> Result<String> {
    let mut parts = Vec::<String>::new();
    if key.ctrl {
        parts.push("Ctrl".into());
    }
    if key.alt {
        parts.push("Alt".into());
    }
    if key.shift {
        parts.push("Shift".into());
    }
    if key.win {
        return Err("hotkey.windows_reserved".into());
    }
    parts.push(match key.key {
        0x30..=0x39 | 0x41..=0x5a => char::from_u32(u32::from(key.key)).unwrap().to_string(),
        0x70..=0x87 => format!("F{}", key.key - 0x70 + 1),
        0x09 => "Tab".into(),
        0x1b => "Escape".into(),
        0x2e => "Delete".into(),
        0x20 => "Space".into(),
        _ => return Err("hotkey.unsupported".into()),
    });
    Ok(parts.join("+"))
}
pub fn engine_config(config: &ActiveConfig, test_output: Option<&Path>) -> Result<Value> {
    if config.width < 64 || config.height < 64 {
        return Err("video.minimum_64_pixels".into());
    }
    if config.replay_mode != ReplayMode::Ram {
        return Err("replay.disk_not_supported_by_engine".into());
    }
    fn audio(value: &AudioSelection) -> (bool, &str) {
        match value {
            AudioSelection::Disabled => (false, ""),
            AudioSelection::Endpoint(id) => (true, id),
        }
    }
    let system = audio(&config.system_audio);
    let microphone = audio(&config.microphone);
    let mut value = json!({"protocolVersion":1,"monitorId":config.monitor_id,"width":config.width,"height":config.height,"fps":config.fps,"bitrate":config.bitrate,"replaySeconds":config.replay_seconds,"replayMemoryLimitMb":config.replay_memory_limit_mb,"clipSeconds":config.replay_seconds,"container":config.container,"outputDirectory":config.output_directory,"saveWithoutGameFolders":config.save_without_game_folders,"systemAudioEnabled":system.0,"systemAudioId":system.1,"microphoneEnabled":microphone.0,"microphoneId":microphone.1,"saveReplayHotkey":hotkey(&config.save_replay_hotkey)?,"toggleRecordingHotkey":hotkey(&config.toggle_recording_hotkey)?,"continuousRecordingEnabled":config.continuous_recording_enabled});
    if let Some(path) = test_output {
        value["durationSeconds"] = 5.into();
        value["outputPath"] = json!(path);
    }
    Ok(value)
}
pub fn write_config(
    root: &Path,
    config: &ActiveConfig,
    test_output: Option<&Path>,
) -> Result<tempfile::NamedTempFile> {
    let bytes =
        serde_json::to_vec(&engine_config(config, test_output)?).map_err(|e| e.to_string())?;
    if bytes.len() > 65536 {
        return Err("config.too_large".into());
    }
    let mut file = tempfile::Builder::new()
        .prefix("RebellioCap-engine-")
        .suffix(".json")
        .tempfile_in(root)
        .map_err(|e| e.to_string())?;
    file.write_all(&bytes)
        .and_then(|()| file.as_file().sync_all())
        .map_err(|e| e.to_string())?;
    Ok(file)
}
#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct TestResult {
    protocol_version: u32,
    #[serde(rename = "type")]
    kind: String,
    passed: bool,
    initialization_stages: Vec<String>,
    duration_seconds: u32,
    video_packets: u64,
    audio_packets: u64,
    missed_frames: u64,
    dropped_frames: u64,
    mux_completed: bool,
    file_size: u64,
    output_path: PathBuf,
    metrics: EngineMetrics,
}
pub fn validate_test_result(
    bytes: &[u8],
    exited_successfully: bool,
    draft: &OnboardingDraft,
    expected_output: &Path,
) -> Result<RecordingTestSummary> {
    if let Ok(event) = crate::engine::protocol::decode_event(bytes) {
        if let Some(error) = event.error {
            return Err(format!("{}: {}", error.code, error.message));
        }
    }
    let result: TestResult = serde_json::from_slice(bytes).map_err(|e| e.to_string())?;
    let stages = [
        "configuration",
        "monitor",
        "nvenc",
        "audio",
        "recorder",
        "capture",
        "mux",
        "file_validation",
    ];
    if !exited_successfully
        || result.protocol_version != 1
        || result.kind != "test_result"
        || !result.passed
        || !result.mux_completed
        || result.duration_seconds != 5
        || result.video_packets == 0
        || result.file_size == 0
        || result.initialization_stages != stages
        || result.metrics.completed_saves != 1
        || result.metrics.failed_saves != 0
        || result.metrics.pipeline_errors != 0
        || result.metrics.video_packets != result.video_packets
        || result.metrics.audio_packets != result.audio_packets
        || result.missed_frames != result.metrics.missed_video_deadlines
        || result.dropped_frames != result.missed_frames
    {
        return Err("test.invalid_native_evidence".into());
    }
    let audio = [&draft.system_audio, &draft.microphone]
        .into_iter()
        .any(|a| matches!(a, Some(AudioSelection::Endpoint(_))));
    if audio && result.audio_packets == 0 {
        return Err("test.audio_missing".into());
    }
    let res_clean = crate::model::strip_verbatim_prefix(&result.output_path);
    let exp_clean = crate::model::strip_verbatim_prefix(expected_output);
    let res_parent = result.output_path.parent().map(crate::model::strip_verbatim_prefix);
    let draft_parent = draft.output_directory.as_deref().map(crate::model::strip_verbatim_prefix);
    if res_clean != exp_clean || res_parent != draft_parent {
        return Err("test.unexpected_output".into());
    }
    let canonical = result
        .output_path
        .canonicalize()
        .map_err(|e| e.to_string())?;
    let canonical_clean = crate::model::strip_verbatim_prefix(&canonical);
    if canonical_clean != exp_clean
        || !canonical.is_file()
        || canonical.metadata().map_err(|e| e.to_string())?.len() != result.file_size
    {
        return Err("test.invalid_file".into());
    }
    Ok(RecordingTestSummary {
        fingerprint: draft.fingerprint().map_err(|e| e.to_string())?,
        succeeded: true,
        clip_path: canonical_clean,
        video_packets: result.video_packets,
        audio_packets: result.audio_packets,
    })
}
pub fn run(
    executable: &Path,
    root: &Path,
    draft: &OnboardingDraft,
) -> Result<RecordingTestSummary> {
    let config = draft.to_active().map_err(|e| e.to_string())?;
    let suffix = match config.container {
        Container::Mp4 => ".mp4",
        Container::Mkv => ".mkv",
    };
    // Reserve a random name then relinquish it; the engine refuses existing destinations.
    let reserved = tempfile::Builder::new()
        .prefix("RebellioCap-test-")
        .suffix(suffix)
        .tempfile_in(&config.output_directory)
        .map_err(|e| e.to_string())?;
    let output = reserved.path().to_owned();
    reserved.close().map_err(|e| e.to_string())?;
    let file = write_config(root, &config, Some(&output))?;
    let (bytes, status) = run_once(
        executable,
        &[
            "test-json".into(),
            "--config".into(),
            file.path().as_os_str().into(),
        ],
        Duration::from_secs(45),
    )?;
    validate_test_result(&bytes, status, draft, &output)
}
