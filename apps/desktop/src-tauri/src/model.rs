use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

pub fn strip_verbatim_prefix(path: &Path) -> PathBuf {
    let s = path.to_string_lossy();
    if let Some(stripped) = s.strip_prefix(r"\\?\UNC\") {
        PathBuf::from(format!(r"\\{}", stripped))
    } else if let Some(stripped) = s.strip_prefix(r"\\?\") {
        PathBuf::from(stripped)
    } else {
        path.to_path_buf()
    }
}

pub fn is_canonical_path(path: &Path) -> bool {
    if let Ok(canonical) = path.canonicalize() {
        let c_norm = strip_verbatim_prefix(&canonical);
        let p_norm = strip_verbatim_prefix(path);
        #[cfg(windows)]
        {
            c_norm.to_string_lossy().eq_ignore_ascii_case(&p_norm.to_string_lossy())
        }
        #[cfg(not(windows))]
        {
            c_norm == p_norm
        }
    } else {
        false
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum AudioSelection {
    Disabled,
    Endpoint(String),
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ReplayMode {
    Ram,
    Disk,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Container {
    Mp4,
    Mkv,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Hotkey {
    pub key: u16,
    pub ctrl: bool,
    pub alt: bool,
    pub shift: bool,
    pub win: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Preferences {
    // Read-only compatibility input. NotificationSettings owns the active flag.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub overlay_enabled: Option<bool>,
    pub start_with_windows: bool,
}

/// Only the native recording-test controller may supply this result to the store.
/// It must validate mux completion, enabled audio sources and the resulting clip.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RecordingTestSummary {
    pub fingerprint: String,
    pub succeeded: bool,
    pub clip_path: PathBuf,
    pub video_packets: u64,
    pub audio_packets: u64,
}

#[derive(Debug, Clone, Default)]
pub struct DeviceInventory {
    pub monitor_ids: Vec<String>,
    pub system_audio_ids: Vec<String>,
    pub microphone_ids: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum EngineLifecycle {
    Starting,
    Ready,
    #[default]
    Stopped,
    Recovering,
    Degraded,
    Blocked,
    Failed,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct EngineMetrics {
    pub video_ticks: u64,
    pub missed_video_deadlines: u64,
    pub video_packets: u64,
    pub audio_packets: u64,
    #[serde(default)]
    pub audio_mixing_dropped_frames: u64,
    #[serde(default)]
    pub audio_unavailable_sources: u32,
    pub save_requests: u64,
    pub completed_saves: u64,
    pub failed_saves: u64,
    pub rejected_saves: u64,
    pub pipeline_errors: u64,
    pub continuous_packets: u64,
    pub continuous_failures: u64,
    #[serde(default)]
    pub continuous_recovery_path: String,
    pub continuous_recording_active: bool,
    pub last_hotkey_save_latency_ticks: i64,
    pub replay_bytes: u64,
    #[serde(default)]
    pub budget_bytes: u64,
    #[serde(default)]
    pub buffered_bytes: u64,
    #[serde(default)]
    pub replay_retained_seconds: f64,
    #[serde(default)]
    pub replay_memory_limited: bool,
    #[serde(default)]
    pub replay_memory_drops: u64,
}

#[cfg(test)]
mod memory_metrics_tests {
    use super::EngineMetrics;
    #[test]
    fn audio_recovery_survives_contract_and_defaults_for_older_engines() {
        let mut value = serde_json::to_value(EngineMetrics::default()).unwrap();
        value["audioUnavailableSources"] = 3.into();
        let metrics: EngineMetrics = serde_json::from_value(value.clone()).unwrap();
        assert_eq!(metrics.audio_unavailable_sources, 3);
        value.as_object_mut().unwrap().remove("audioUnavailableSources");
        let older: EngineMetrics = serde_json::from_value(value).unwrap();
        assert_eq!(older.audio_unavailable_sources, 0);
    }

    #[test]
    fn mixing_drops_survive_contract_and_default_for_older_engines() {
        let mut value = serde_json::to_value(EngineMetrics::default()).unwrap();
        value["audioMixingDroppedFrames"] = 24000.into();
        let metrics: EngineMetrics = serde_json::from_value(value.clone()).unwrap();
        assert_eq!(metrics.audio_mixing_dropped_frames, 24000);
        value.as_object_mut().unwrap().remove("audioMixingDroppedFrames");
        let older: EngineMetrics = serde_json::from_value(value).unwrap();
        assert_eq!(older.audio_mixing_dropped_frames, 0);
    }

    #[test]
    fn memory_metrics_survive_strict_native_contract() {
        let mut value = serde_json::to_value(EngineMetrics::default()).unwrap();
        value["budgetBytes"] = 8192.into();
        value["bufferedBytes"] = 5000.into();
        value["replayRetainedSeconds"] = 12.5.into();
        value["replayMemoryLimited"] = true.into();
        value["replayMemoryDrops"] = 16.into();
        value["continuousRecoveryPath"] = "C:/Clips/recovered.mp4.partial".into();
        let metrics: EngineMetrics = serde_json::from_value(value).unwrap();
        assert_eq!(metrics.budget_bytes, 8192);
        assert_eq!(metrics.buffered_bytes, 5000);
        assert_eq!(metrics.replay_retained_seconds, 12.5);
        assert!(metrics.replay_memory_limited);
        assert_eq!(metrics.replay_memory_drops, 16);
        assert_eq!(metrics.continuous_recovery_path, "C:/Clips/recovered.mp4.partial");
    }

    #[test]
    fn older_metrics_without_memory_fields_keep_compatibility() {
        let mut value = serde_json::to_value(EngineMetrics::default()).unwrap();
        let fields = value.as_object_mut().unwrap();
        fields.remove("continuousRecoveryPath");
        for field in ["budgetBytes", "bufferedBytes", "replayRetainedSeconds", "replayMemoryLimited", "replayMemoryDrops"] {
            fields.remove(field);
        }
        let metrics: EngineMetrics = serde_json::from_value(value).unwrap();
        assert_eq!(metrics.budget_bytes, 0);
        assert!(!metrics.replay_memory_limited);
        assert!(metrics.continuous_recovery_path.is_empty());
    }
}
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct NativeError {
    pub code: String,
    pub message: String,
    pub hresult: Option<i64>,
}
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct EngineSnapshot {
    pub revision: u64,
    pub lifecycle: EngineLifecycle,
    pub replay_active: bool,
    pub continuous_recording_active: bool,
    pub replay_seconds: u32,
    pub metrics: EngineMetrics,
    pub last_error: Option<NativeError>,
}
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct MonitorChoice {
    pub id: String,
    #[serde(default, rename = "legacyId", skip_serializing_if = "Option::is_none")]
    pub legacy_id: Option<String>,
    pub name: String,
    pub width: u32,
    pub height: u32,
    pub primary: bool,
}
#[derive(Debug, Clone, Serialize)]
pub struct AudioChoice {
    pub id: String,
    pub name: String,
    pub is_default: bool,
    pub available: bool,
}
#[derive(Debug, Clone, Serialize)]
pub struct AudioCatalog {
    pub system_audio: Vec<AudioChoice>,
    pub microphones: Vec<AudioChoice>,
}
#[derive(Debug, Clone, Serialize)]
pub struct SystemCheckStage {
    pub id: String,
    pub passed: bool,
    pub message: String,
}
#[derive(Debug, Clone, Serialize)]
pub struct SystemCheckResult {
    pub stages: Vec<SystemCheckStage>,
    pub passed: bool,
    pub diagnostics_path: Option<String>,
}
#[derive(Debug, Clone, Serialize)]
pub struct AudioLevel {
    pub timestamp_ms: u64,
    pub level: f32,
}
#[derive(Debug, Clone, Serialize)]
pub struct AudioMeasurement {
    pub samples: Vec<AudioLevel>,
    pub cancelled: bool,
}
