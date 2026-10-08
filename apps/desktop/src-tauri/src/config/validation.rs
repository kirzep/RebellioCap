use super::{
    ActiveConfig, ConfigError, OnboardingDraft, Result, StoredState, CONFIG_SCHEMA_VERSION,
};
use crate::model::*;
use std::path::{Component, Path};

fn identity(value: &str) -> Result<()> {
    if value.trim().is_empty() || value.len() > 4096 || value.chars().any(char::is_control) {
        return Err(ConfigError::Invalid("device_identity"));
    }
    Ok(())
}

fn hotkey(value: &Hotkey) -> Result<()> {
    let supported =
        matches!(value.key, 0x30..=0x39 | 0x41..=0x5a | 0x70..=0x87 | 0x09 | 0x1b | 0x2e | 0x20);
    let reserved = value.win
        || (value.alt && matches!(value.key, 0x09 | 0x20 | 0x73))
        || (value.key == 0x1b && (value.ctrl || value.alt))
        || (value.key == 0x2e && value.ctrl && value.alt)
        || value.key == 0x7b; // F12 is reserved for the debugger by RegisterHotKey.
    if !supported || reserved {
        return Err(ConfigError::Invalid("hotkey"));
    }
    Ok(())
}

fn output_path(path: &Path, check_available: bool) -> Result<()> {
    if !path.is_absolute()
        || path
            .components()
            .any(|c| matches!(c, Component::ParentDir | Component::CurDir))
    {
        return Err(ConfigError::Invalid("output_directory"));
    }
    if check_available && (!path.is_dir() || !crate::model::is_canonical_path(path)) {
        return Err(ConfigError::Invalid("output_directory"));
    }
    Ok(())
}

pub fn validate_draft(value: &OnboardingDraft) -> Result<()> {
    validate_fields(value, true)
}

fn validate_fields(value: &OnboardingDraft, check_available: bool) -> Result<()> {
    if let Some(id) = &value.monitor_id {
        identity(id)?;
    }
    for audio in [&value.system_audio, &value.microphone]
        .into_iter()
        .flatten()
    {
        if let AudioSelection::Endpoint(id) = audio {
            identity(id)?;
        }
    }
    for (field, number) in [
        ("width", value.width), ("height", value.height), ("fps", value.fps),
        ("bitrate", value.bitrate), ("replay_seconds", value.replay_seconds),
    ] {
        let (min, max) = super::recording_range(field);
        if number.is_some_and(|v| v < min || v > max) { return Err(ConfigError::Invalid(field)); }
    }
    if let Some(limit) = value.replay_memory_limit_mb.filter(|limit| *limit != 0) {
        let (min, max) = super::recording_range("replay_memory_limit_mb");
        if limit < min || limit > max {
            return Err(ConfigError::Invalid("replay_memory_limit_mb"));
        }
    }
    if value.width.is_some_and(|v| v % 2 != 0) { return Err(ConfigError::Invalid("width")); }
    if value.height.is_some_and(|v| v % 2 != 0) { return Err(ConfigError::Invalid("height")); }
    if let Some(path) = &value.output_directory {
        output_path(path, check_available)?;
    }
    for key in [&value.save_replay_hotkey, &value.toggle_recording_hotkey]
        .into_iter()
        .flatten()
    {
        hotkey(key)?;
    }
    if value.save_replay_hotkey.is_some()
        && value.save_replay_hotkey == value.toggle_recording_hotkey
    {
        return Err(ConfigError::Invalid("duplicate_hotkeys"));
    }
    Ok(())
}

pub fn validate_active(value: &ActiveConfig, available: &DeviceInventory) -> Result<()> {
    validate_fields(&value.as_draft(), true)?;
    if !value.onboarding_completed {
        return Err(ConfigError::Invalid("onboarding_completed"));
    }
    if !available.monitor_ids.contains(&value.monitor_id) {
        return Err(ConfigError::Invalid("monitor_unavailable"));
    }
    for (audio, ids, field) in [
        (
            &value.system_audio,
            &available.system_audio_ids,
            "system_audio_unavailable",
        ),
        (
            &value.microphone,
            &available.microphone_ids,
            "microphone_unavailable",
        ),
    ] {
        if let AudioSelection::Endpoint(id) = audio {
            if !ids.contains(id) {
                return Err(ConfigError::Invalid(field));
            }
        }
    }
    Ok(())
}

pub(super) fn validate_recording_evidence(
    draft: &OnboardingDraft,
    test: &RecordingTestSummary,
) -> Result<()> {
    if !test.succeeded
        || test.video_packets == 0
        || test.fingerprint.len() != 64
        || !test.fingerprint.bytes().all(|b| b.is_ascii_hexdigit())
        || test.fingerprint != draft.fingerprint()?
    {
        return Err(ConfigError::Invalid("last_successful_test"));
    }
    let audio_enabled = [&draft.system_audio, &draft.microphone]
        .into_iter()
        .any(|value| matches!(value, Some(AudioSelection::Endpoint(_))));
    if audio_enabled && test.audio_packets == 0 {
        return Err(ConfigError::Invalid("test_audio_packets"));
    }
    let clip_parent = test.clip_path.parent().map(crate::model::strip_verbatim_prefix);
    let draft_output = draft.output_directory.as_deref().map(crate::model::strip_verbatim_prefix);
    if clip_parent != draft_output {
        return Err(ConfigError::Invalid("test_clip_path"));
    }
    Ok(())
}

/// Disk integrity is separate from live device/path availability. A disconnected
/// device must leave completed onboarding visible for the later repair route.
pub(super) fn validate_state(value: &StoredState) -> Result<()> {
    if value.schema_version != CONFIG_SCHEMA_VERSION {
        return Err(ConfigError::Schema(value.schema_version));
    }
    if value.onboarding.last_completed_step > 6
        || value.onboarding.completed != value.active.is_some()
    {
        return Err(ConfigError::Invalid("onboarding"));
    }
    validate_fields(&value.draft, false)?;
    if let Some(active) = &value.active {
        if !active.onboarding_completed {
            return Err(ConfigError::Invalid("onboarding_completed"));
        }
        validate_fields(&active.as_draft(), false)?;
    }
    if let Some(test) = &value.last_successful_test {
        validate_recording_evidence(&value.draft, test)?;
    }
    Ok(())
}
