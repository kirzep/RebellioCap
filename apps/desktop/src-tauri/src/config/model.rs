use super::{ConfigError, Result};
use crate::model::*;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::path::PathBuf;

pub const CONFIG_SCHEMA_VERSION: u32 = 1;

fn is_auto_memory_limit(value: &u32) -> bool { *value == 0 }

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct OnboardingProgress {
    pub completed: bool,
    pub last_completed_step: u8,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct OnboardingDraft {
    pub monitor_id: Option<String>,
    pub system_audio: Option<AudioSelection>,
    pub microphone: Option<AudioSelection>,
    pub width: Option<u32>,
    pub height: Option<u32>,
    pub fps: Option<u32>,
    pub bitrate: Option<u32>,
    pub replay_seconds: Option<u32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub replay_memory_limit_mb: Option<u32>,
    pub replay_mode: Option<ReplayMode>,
    pub container: Option<Container>,
    pub output_directory: Option<PathBuf>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub save_without_game_folders: Option<bool>,
    pub save_replay_hotkey: Option<Hotkey>,
    pub toggle_recording_hotkey: Option<Hotkey>,
    pub preferences: Option<Preferences>,
    pub continuous_recording_enabled: Option<bool>,
}

impl OnboardingDraft {
    /// Hash every selection, including explicit disabled audio and preferences.
    /// Struct field serialization order is stable; no unordered maps are used.
    pub fn fingerprint(&self) -> Result<String> {
        let mut normalized = self.clone();
        if normalized.replay_memory_limit_mb == Some(0) { normalized.replay_memory_limit_mb = None; }
        if normalized.save_without_game_folders == Some(false) { normalized.save_without_game_folders = None; }
        Ok(format!(
            "{:x}",
            Sha256::digest(serde_json::to_vec(&(CONFIG_SCHEMA_VERSION, &normalized))?)
        ))
    }

    pub fn to_active(&self) -> Result<ActiveConfig> {
        fn required<T: Clone>(value: &Option<T>, field: &'static str) -> Result<T> {
            value.clone().ok_or(ConfigError::Invalid(field))
        }
        Ok(ActiveConfig {
            onboarding_completed: true,
            monitor_id: required(&self.monitor_id, "monitor_id")?,
            system_audio: required(&self.system_audio, "system_audio")?,
            microphone: required(&self.microphone, "microphone")?,
            width: required(&self.width, "width")?,
            height: required(&self.height, "height")?,
            fps: required(&self.fps, "fps")?,
            bitrate: required(&self.bitrate, "bitrate")?,
            replay_seconds: required(&self.replay_seconds, "replay_seconds")?,
            replay_memory_limit_mb: self.replay_memory_limit_mb.unwrap_or(0),
            replay_mode: required(&self.replay_mode, "replay_mode")?,
            container: required(&self.container, "container")?,
            output_directory: required(&self.output_directory, "output_directory")?,
            save_without_game_folders: self.save_without_game_folders.unwrap_or(false),
            save_replay_hotkey: required(&self.save_replay_hotkey, "save_replay_hotkey")?,
            toggle_recording_hotkey: required(
                &self.toggle_recording_hotkey,
                "toggle_recording_hotkey",
            )?,
            preferences: required(&self.preferences, "preferences")?,
            continuous_recording_enabled: required(
                &self.continuous_recording_enabled,
                "continuous_recording_enabled",
            )?,
        })
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ActiveConfig {
    pub onboarding_completed: bool,
    pub monitor_id: String,
    pub system_audio: AudioSelection,
    pub microphone: AudioSelection,
    pub width: u32,
    pub height: u32,
    pub fps: u32,
    pub bitrate: u32,
    pub replay_seconds: u32,
    #[serde(default, skip_serializing_if = "is_auto_memory_limit")]
    pub replay_memory_limit_mb: u32,
    pub replay_mode: ReplayMode,
    pub container: Container,
    pub output_directory: PathBuf,
    #[serde(default)]
    pub save_without_game_folders: bool,
    pub save_replay_hotkey: Hotkey,
    pub toggle_recording_hotkey: Hotkey,
    pub preferences: Preferences,
    pub continuous_recording_enabled: bool,
}

impl ActiveConfig {
    pub fn as_draft(&self) -> OnboardingDraft {
        OnboardingDraft {
            monitor_id: Some(self.monitor_id.clone()),
            system_audio: Some(self.system_audio.clone()),
            microphone: Some(self.microphone.clone()),
            width: Some(self.width),
            height: Some(self.height),
            fps: Some(self.fps),
            bitrate: Some(self.bitrate),
            replay_seconds: Some(self.replay_seconds),
            replay_memory_limit_mb: (self.replay_memory_limit_mb != 0).then_some(self.replay_memory_limit_mb),
            replay_mode: Some(self.replay_mode.clone()),
            container: Some(self.container.clone()),
            output_directory: Some(self.output_directory.clone()),
            save_without_game_folders: self.save_without_game_folders.then_some(true),
            save_replay_hotkey: Some(self.save_replay_hotkey.clone()),
            toggle_recording_hotkey: Some(self.toggle_recording_hotkey.clone()),
            preferences: Some(self.preferences.clone()),
            continuous_recording_enabled: Some(self.continuous_recording_enabled),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct StoredState {
    pub schema_version: u32,
    pub onboarding: OnboardingProgress,
    pub draft: OnboardingDraft,
    pub active: Option<ActiveConfig>,
    pub last_successful_test: Option<RecordingTestSummary>,
}

impl Default for StoredState {
    fn default() -> Self {
        Self {
            schema_version: CONFIG_SCHEMA_VERSION,
            onboarding: Default::default(),
            draft: Default::default(),
            active: None,
            last_successful_test: None,
        }
    }
}
