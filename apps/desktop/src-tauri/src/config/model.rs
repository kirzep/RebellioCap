use super::{ConfigError, Result};
use crate::model::*;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::path::PathBuf;

pub const CONFIG_SCHEMA_VERSION: u32 = 1;

fn is_auto_memory_limit(value: &u32) -> bool { *value == 0 }

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum CaptureMode { #[default] Desktop, Game }

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum RateControl { #[default] Vbr, Cqp }
fn default_cqp() -> u32 { 23 }
fn default_rate(value: &RateControl) -> bool { *value == RateControl::Vbr }
fn default_qp(value: &u32) -> bool { *value == 23 }
fn disabled(value: &bool) -> bool { !*value }

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct OnboardingProgress {
    pub completed: bool,
    pub last_completed_step: u8,
}

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct OnboardingDraft {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub capture_mode: Option<CaptureMode>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub game_executable: Option<String>,
    pub monitor_id: Option<String>,
    pub system_audio: Option<AudioSelection>,
    pub microphone: Option<AudioSelection>,
    pub width: Option<u32>,
    pub height: Option<u32>,
    pub fps: Option<u32>,
    pub bitrate: Option<u32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub rate_control: Option<RateControl>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub cqp: Option<u32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub recording_priority: Option<bool>,
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
        if normalized.rate_control == Some(RateControl::Vbr) { normalized.rate_control = None; }
        if normalized.cqp == Some(23) { normalized.cqp = None; }
        if normalized.recording_priority == Some(false) { normalized.recording_priority = None; }
        if normalized.capture_mode == Some(CaptureMode::Desktop) { normalized.capture_mode = None; }
        if normalized.game_executable.as_deref() == Some("") { normalized.game_executable = None; }
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
            capture_mode: self.capture_mode.unwrap_or_default(),
            game_executable: self.game_executable.clone().unwrap_or_default(),
            onboarding_completed: true,
            monitor_id: if self.capture_mode == Some(CaptureMode::Game) { self.monitor_id.clone().unwrap_or_default() } else { required(&self.monitor_id, "monitor_id")? },
            system_audio: required(&self.system_audio, "system_audio")?,
            microphone: required(&self.microphone, "microphone")?,
            width: required(&self.width, "width")?,
            height: required(&self.height, "height")?,
            fps: required(&self.fps, "fps")?,
            bitrate: required(&self.bitrate, "bitrate")?,
            rate_control: self.rate_control.unwrap_or_default(),
            cqp: self.cqp.unwrap_or(23),
            recording_priority: self.recording_priority.unwrap_or(false),
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
    #[serde(default)]
    pub capture_mode: CaptureMode,
    #[serde(default)]
    pub game_executable: String,
    pub onboarding_completed: bool,
    pub monitor_id: String,
    pub system_audio: AudioSelection,
    pub microphone: AudioSelection,
    pub width: u32,
    pub height: u32,
    pub fps: u32,
    pub bitrate: u32,
    #[serde(default, skip_serializing_if = "default_rate")]
    pub rate_control: RateControl,
    #[serde(default = "default_cqp", skip_serializing_if = "default_qp")]
    pub cqp: u32,
    #[serde(default, skip_serializing_if = "disabled")]
    pub recording_priority: bool,
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
            capture_mode: (self.capture_mode != CaptureMode::Desktop).then_some(self.capture_mode),
            game_executable: (!self.game_executable.is_empty()).then(|| self.game_executable.clone()),
            monitor_id: Some(self.monitor_id.clone()),
            system_audio: Some(self.system_audio.clone()),
            microphone: Some(self.microphone.clone()),
            width: Some(self.width),
            height: Some(self.height),
            fps: Some(self.fps),
            bitrate: Some(self.bitrate),
            rate_control: (self.rate_control != RateControl::Vbr).then_some(self.rate_control),
            cqp: (self.cqp != 23).then_some(self.cqp),
            recording_priority: self.recording_priority.then_some(true),
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
