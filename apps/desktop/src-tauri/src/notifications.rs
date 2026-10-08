use crate::model::{EngineLifecycle, EngineSnapshot};
use serde::{Deserialize, Serialize};
use std::{path::PathBuf, sync::Mutex};

pub fn transitions(old: &EngineSnapshot, next: &EngineSnapshot) -> Vec<&'static str> {
    if next.lifecycle == EngineLifecycle::Failed && old.lifecycle != EngineLifecycle::Failed {
        return vec!["engine_stopped"];
    }
    let recording_error = next.metrics.continuous_failures > old.metrics.continuous_failures;
    let mut events = Vec::new();
    if recording_error {
        events.push("recording_error");
    }
    if old.replay_active != next.replay_active {
        events.push(if next.replay_active {
            "replay_started"
        } else {
            "replay_stopped"
        });
    }
    if old.continuous_recording_active != next.continuous_recording_active && !recording_error {
        events.push(if next.continuous_recording_active {
            "recording_started"
        } else {
            "recording_stopped"
        });
    }
    for _ in 0..next
        .metrics
        .completed_saves
        .saturating_sub(old.metrics.completed_saves)
        .min(32)
    {
        events.push("replay_saved");
    }
    if next.metrics.failed_saves > old.metrics.failed_saves
        || next.metrics.rejected_saves > old.metrics.rejected_saves
    {
        events.push("replay_error");
    }
    events
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct NotificationSettings {
    #[serde(default = "default_overlay_enabled")]
    pub overlay_enabled: bool,
    pub sound_enabled: bool,
    pub sound: String,
    pub volume: u8,
}
fn default_overlay_enabled() -> bool {
    true
}
impl Default for NotificationSettings {
    fn default() -> Self {
        Self {
            overlay_enabled: true,
            sound_enabled: true,
            sound: "glass".into(),
            volume: 35,
        }
    }
}
pub struct NotificationState {
    settings: Mutex<NotificationSettings>,
    path: PathBuf,
}
fn read_settings(path: &std::path::Path) -> Option<NotificationSettings> {
    std::fs::read(path).ok()
        .and_then(|bytes| serde_json::from_slice::<NotificationSettings>(&bytes).ok())
        .filter(|s| s.volume <= 100 && ["glass", "soft", "bell"].contains(&s.sound.as_str()))
}
pub(crate) fn has_canonical_settings(root: &std::path::Path) -> bool {
    read_settings(&root.join("notifications.json")).is_some()
}
impl NotificationState {
    pub fn new(root: PathBuf) -> Self {
        let path = root.join("notifications.json");
        let missing = matches!(std::fs::metadata(&path), Err(e) if e.kind() == std::io::ErrorKind::NotFound);
        let settings = read_settings(&path).unwrap_or_else(|| {
            let mut settings = NotificationSettings::default();
            if missing {
                let legacy = root.canonicalize().ok()
                    .and_then(|root| crate::config::ConfigStore::from_root_host(&root).ok())
                    .and_then(|store| store.load().ok())
                    .and_then(|config| config.active.as_ref().and_then(|active| active.preferences.overlay_enabled)
                        .or_else(|| config.draft.preferences.and_then(|preferences| preferences.overlay_enabled)));
                if let Some(enabled) = legacy { settings.overlay_enabled = enabled; }
            }
            settings
        });
        let state = Self {
            settings: Mutex::new(settings),
            path,
        };
        if missing {
            let settings = state.settings.lock().unwrap().clone();
            if let Err(error) = state.save(settings) {
                // Keep legacy config intact until canonical settings are durable.
                crate::logging::record("error", "notifications", "migration_failed", serde_json::json!({"error":error}));
            }
        }
        state
    }
    fn save(&self, settings: NotificationSettings) -> Result<(), String> {
        if settings.volume > 100 || !["glass", "soft", "bell"].contains(&settings.sound.as_str()) {
            return Err("notifications.invalid_settings".into());
        }
        let mut current = self.settings.lock().map_err(|e| e.to_string())?;
        std::fs::create_dir_all(self.path.parent().ok_or("notifications.path")?)
            .map_err(|e| e.to_string())?;
        let file = tempfile::NamedTempFile::new_in(self.path.parent().unwrap())
            .map_err(|e| e.to_string())?;
        serde_json::to_writer(file.as_file(), &settings).map_err(|e| e.to_string())?;
        file.as_file().sync_all().map_err(|e| e.to_string())?;
        file.persist(&self.path).map_err(|e| e.to_string())?;
        *current = settings;
        Ok(())
    }
}
#[tauri::command]
pub fn get_notification_settings(
    state: tauri::State<'_, NotificationState>,
) -> Result<NotificationSettings, String> {
    Ok(state.settings.lock().map_err(|e| e.to_string())?.clone())
}
#[tauri::command]
pub fn set_notification_settings(
    app: tauri::AppHandle,
    state: tauri::State<'_, NotificationState>,
    settings: NotificationSettings,
) -> Result<(), String> {
    state.save(settings)?;
    notify_ready(&app);
    Ok(())
}
pub(crate) fn notify_ready(app: &tauri::AppHandle) {
    use tauri::Emitter;
    if let Err(error) = app.emit_to("overlay", "notifications-ready", ()) {
        crate::logging::record("error", "notifications", "wake_failed", serde_json::json!({"error":error.to_string()}));
    }
}
#[derive(Serialize)]
pub struct NotificationBatch {
    events: Vec<&'static str>,
    settings: NotificationSettings,
}
fn notification_batch(mut events: Vec<&'static str>, settings: NotificationSettings) -> NotificationBatch {
    if !settings.overlay_enabled && !settings.sound_enabled {
        events.clear();
    }
    NotificationBatch { events, settings }
}
#[tauri::command]
pub async fn poll_notifications(
    window: tauri::WebviewWindow,
    host: tauri::State<'_, crate::commands::HostState>,
    state: tauri::State<'_, NotificationState>,
) -> Result<NotificationBatch, String> {
    if window.label() != "overlay" {
        return Err("notifications.overlay_only".into());
    }
    let events = host.drain_notifications().await?;
    let settings = get_notification_settings(state)?;
    Ok(notification_batch(events, settings))
}

#[cfg(test)]
mod tests {
    use crate::{
        model::{EngineLifecycle, EngineSnapshot},
        notifications::transitions,
    };

    #[test]
    fn events_reach_either_enabled_channel_and_are_dropped_only_when_both_are_off() {
        for overlay_enabled in [false, true] {
            for sound_enabled in [false, true] {
                let settings = super::NotificationSettings {
                    overlay_enabled,
                    sound_enabled,
                    ..Default::default()
                };
                let batch = super::notification_batch(vec!["replay_saved"], settings);
                assert_eq!(!batch.events.is_empty(), overlay_enabled || sound_enabled);
                assert_eq!(batch.settings.overlay_enabled, overlay_enabled);
                assert_eq!(batch.settings.sound_enabled, sound_enabled);
            }
        }
    }

    #[test]
    fn settings_survive_restart_and_invalid_changes_preserve_previous_value() {
        let root = tempfile::tempdir().unwrap();
        let state = super::NotificationState::new(root.path().to_path_buf());
        let mut settings = super::NotificationSettings::default();
        settings.volume = 80;
        settings.sound_enabled = false;
        state.save(settings.clone()).unwrap();
        let restored = super::NotificationState::new(root.path().to_path_buf());
        assert_eq!(restored.settings.lock().unwrap().volume, 80);
        assert!(!restored.settings.lock().unwrap().sound_enabled);
        settings.volume = 101;
        assert!(state.save(settings).is_err());
        assert_eq!(state.settings.lock().unwrap().volume, 80);
    }

    #[test]
    fn missing_notifications_migrate_legacy_overlay_and_preserve_canonical_choices() {
        for enabled in [false, true] {
            let root = tempfile::tempdir().unwrap();
            let mut config = serde_json::to_value(crate::config::StoredState::default()).unwrap();
            config["draft"]["preferences"] = serde_json::json!({"overlay_enabled":enabled,"start_with_windows":false});
            std::fs::write(root.path().join("config.json"), serde_json::to_vec(&config).unwrap()).unwrap();
            let state = super::NotificationState::new(root.path().to_path_buf());
            assert_eq!(state.settings.lock().unwrap().overlay_enabled, enabled);
            assert!(root.path().join("notifications.json").is_file());
            let mut settings = state.settings.lock().unwrap().clone();
            settings.overlay_enabled = !enabled;
            settings.sound = "bell".into();
            settings.volume = 80;
            state.save(settings).unwrap();
            let restarted = super::NotificationState::new(root.path().to_path_buf());
            let settings = restarted.settings.lock().unwrap();
            assert_eq!(settings.overlay_enabled, !enabled);
            assert_eq!(settings.sound, "bell");
            assert_eq!(settings.volume, 80);
        }
    }

    #[test]
    fn fresh_install_persists_one_default_and_corrupt_settings_do_not_erase_legacy_input() {
        let fresh=tempfile::tempdir().unwrap();
        let state=super::NotificationState::new(fresh.path().to_path_buf());
        assert!(state.settings.lock().unwrap().overlay_enabled);
        assert!(super::has_canonical_settings(fresh.path()));

        let root=tempfile::tempdir().unwrap();
        let mut config=serde_json::to_value(crate::config::StoredState::default()).unwrap();
        config["draft"]["preferences"]=serde_json::json!({"overlay_enabled":false,"start_with_windows":false});
        std::fs::write(root.path().join("config.json"),serde_json::to_vec(&config).unwrap()).unwrap();
        std::fs::write(root.path().join("notifications.json"),b"{broken").unwrap();
        let _state=super::NotificationState::new(root.path().to_path_buf());
        let store=crate::config::ConfigStore::from_root_host(&root.path().canonicalize().unwrap()).unwrap();
        assert_eq!(store.load().unwrap().draft.preferences.unwrap().overlay_enabled,Some(false));
        assert_eq!(std::fs::read(root.path().join("notifications.json")).unwrap(),b"{broken");
    }

    #[test]
    fn successful_stop_reports_both_modes_without_an_error() {
        let mut old = EngineSnapshot::default();
        old.replay_active = true;
        old.continuous_recording_active = true;
        assert_eq!(
            transitions(&old, &EngineSnapshot::default()),
            vec!["replay_stopped", "recording_stopped"]
        );
    }

    #[test]
    fn reports_confirmed_actions_and_not_repeated_snapshots() {
        let old = EngineSnapshot::default();
        let mut next = old.clone();
        next.replay_active = true;
        next.continuous_recording_active = true;
        assert_eq!(
            transitions(&old, &next),
            vec!["replay_started", "recording_started"]
        );
        assert!(transitions(&next, &next).is_empty());
        let old = next.clone();
        next.metrics.completed_saves += 1;
        assert_eq!(transitions(&old, &next), vec!["replay_saved"]);
    }

    #[test]
    fn failure_does_not_announce_successful_recording_stop() {
        let mut old = EngineSnapshot::default();
        old.replay_active = true;
        old.continuous_recording_active = true;
        let mut next = old.clone();
        next.replay_active = false;
        next.continuous_recording_active = false;
        next.metrics.continuous_failures += 1;
        assert_eq!(
            transitions(&old, &next),
            vec!["recording_error", "replay_stopped"]
        );
        next.lifecycle = EngineLifecycle::Failed;
        assert_eq!(transitions(&old, &next), vec!["engine_stopped"]);
    }
}
