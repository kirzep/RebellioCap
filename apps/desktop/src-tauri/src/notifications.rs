use crate::model::{EngineLifecycle, EngineSnapshot};
use serde::{Deserialize, Serialize};
use std::{path::PathBuf, sync::{Mutex, atomic::{AtomicBool, Ordering}}};

pub fn transitions(old: &EngineSnapshot, next: &EngineSnapshot) -> Vec<&'static str> {
    if next.lifecycle == EngineLifecycle::Failed && old.lifecycle != EngineLifecycle::Failed {
        return vec!["engine_stopped"];
    }
    let recording_error = next.metrics.continuous_failures > old.metrics.continuous_failures;
    let mut events = Vec::new();
    if next.metrics.save_requests > old.metrics.save_requests { events.push("replay_saving"); }
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
    #[serde(default = "default_layout", deserialize_with = "deserialize_layout")]
    pub layout: std::collections::BTreeMap<String, OverlayPosition>,
    #[serde(default)]
    pub custom_sound: Option<String>,
    #[serde(default)]
    pub custom_sound_name: Option<String>,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct OverlayPosition { pub x: f64, pub y: f64 }
fn default_layout() -> std::collections::BTreeMap<String, OverlayPosition> {
    ["messages", "recording", "replay"]
        .into_iter().map(|key| (key.to_owned(), match key {
            "recording" => OverlayPosition { x: 0.93, y: 0.98 },
            "replay" => OverlayPosition { x: 0.99, y: 0.98 },
            _ => OverlayPosition { x: 0.02, y: 0.03 },
        })).collect()
}
fn deserialize_layout<'de, D: serde::Deserializer<'de>>(deserializer: D) -> Result<std::collections::BTreeMap<String, OverlayPosition>, D::Error> {
    let mut layout = std::collections::BTreeMap::<String, OverlayPosition>::deserialize(deserializer)?;
    if !layout.contains_key("messages") {
        let legacy = ["replay_started", "replay_saved", "recording_started", "recording_stopped", "recording_error", "replay_stopped", "engine_stopped", "replay_error", "recording", "replay"];
        if layout.keys().all(|key| legacy.contains(&key.as_str())) {
            let messages = layout.get("replay_saved").or_else(|| layout.get("recording_started")).cloned().unwrap_or(OverlayPosition { x: 0.02, y: 0.03 });
            layout.retain(|key, _| key == "recording" || key == "replay");
            layout.insert("messages".into(), messages);
        }
    }
    Ok(layout)
}
fn valid_settings(settings: &NotificationSettings) -> bool {
    settings.volume <= 100 && ["glass", "soft", "bell", "custom"].contains(&settings.sound.as_str())
        && default_layout().keys().all(|key| settings.layout.contains_key(key))
        && settings.layout.len() == default_layout().len()
        && settings.layout.values().all(|p| p.x.is_finite() && p.y.is_finite() && (0.0..=1.0).contains(&p.x) && (0.0..=1.0).contains(&p.y))
        && settings.custom_sound.as_ref().is_none_or(|id| valid_sound_id(id))
        && (settings.sound != "custom" || settings.custom_sound.is_some())
        && settings.custom_sound_name.as_ref().is_none_or(|name| name.len() <= 512)
}
fn valid_sound_id(id: &str) -> bool { id.len() == 64 && id.bytes().all(|c| c.is_ascii_hexdigit()) }
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
            layout: default_layout(),
            custom_sound: None,
            custom_sound_name: None,
        }
    }
}
pub struct NotificationState {
    editor: Mutex<bool>,
    pending_editor_feedback: Mutex<Option<String>>,
    editor_feedback: Mutex<Option<String>>,
    restore_main_after_feedback: AtomicBool,
    settings: Mutex<NotificationSettings>,
    path: PathBuf,
}
fn read_settings(path: &std::path::Path) -> Option<NotificationSettings> {
    std::fs::read(path).ok()
        .and_then(|bytes| serde_json::from_slice::<NotificationSettings>(&bytes).ok())
        .filter(valid_settings)
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
            editor: Mutex::new(false),
            pending_editor_feedback: Mutex::new(None),
            editor_feedback: Mutex::new(None),
            restore_main_after_feedback: AtomicBool::new(false),
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
        if !valid_settings(&settings) {
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
    #[serde(rename = "editorFeedback")]
    editor_feedback: Option<String>,
    #[serde(rename = "replayActive")]
    replay_active: bool,
    #[serde(rename = "replaySaving")]
    replay_saving: bool,
    #[serde(rename = "recordingActive")]
    recording_active: bool,
    #[serde(rename = "editorActive")]
    editor_active: bool,
}
fn notification_batch(mut events: Vec<&'static str>, settings: NotificationSettings) -> NotificationBatch {
    if !settings.overlay_enabled && !settings.sound_enabled {
        events.clear();
    }
    NotificationBatch { events, settings, editor_feedback: None, replay_active: false, replay_saving: false, recording_active: false, editor_active: false }
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
    let editor_active = *state.editor.lock().map_err(|e| e.to_string())?;
    let editor_feedback = state.editor_feedback.lock().map_err(|e| e.to_string())?.clone();
    let settings = get_notification_settings(state)?;
    let (snapshot, _) = host.tray_status()?;
    let mut batch = notification_batch(events, settings);
    let healthy = !matches!(snapshot.lifecycle, EngineLifecycle::Failed | EngineLifecycle::Blocked | EngineLifecycle::Stopped);
    batch.editor_feedback = editor_feedback;
    batch.editor_active = editor_active;
    batch.replay_saving = healthy && snapshot.metrics.save_requests > snapshot.metrics.completed_saves.saturating_add(snapshot.metrics.failed_saves);
    batch.replay_active = healthy && snapshot.replay_active;
    batch.recording_active = healthy && snapshot.continuous_recording_active;
    Ok(batch)
}

const MAX_SOUND_BYTES: usize = 2 * 1024 * 1024;
fn validate_audio(bytes: &[u8]) -> bool {
    bytes.len() >= 12 && bytes.len() <= MAX_SOUND_BYTES && (
        (&bytes[..4] == b"RIFF" && &bytes[8..12] == b"WAVE") || &bytes[..4] == b"OggS" || &bytes[..3] == b"ID3" || (bytes[0] == 0xff && bytes[1] & 0xe0 == 0xe0))
}
#[tauri::command]
pub fn import_notification_sound(window: tauri::WebviewWindow, state: tauri::State<'_, NotificationState>, bytes: Vec<u8>) -> Result<String, String> {
    if window.label() != "main" { return Err("notifications.main_only".into()); }
    if !validate_audio(&bytes) { return Err("notifications.invalid_audio".into()); }
    use sha2::{Digest, Sha256};
    let id = format!("{:x}", Sha256::digest(&bytes));
    let dir = state.path.parent().ok_or("notifications.path")?.join("notification-sounds");
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let file = tempfile::NamedTempFile::new_in(&dir).map_err(|e| e.to_string())?;
    std::io::Write::write_all(&mut file.as_file(), &bytes).map_err(|e| e.to_string())?;
    file.as_file().sync_all().map_err(|e| e.to_string())?;
    file.persist(dir.join(&id)).map_err(|e| e.to_string())?;
    Ok(id)
}
#[tauri::command]
pub fn read_notification_sound(state: tauri::State<'_, NotificationState>, id: String) -> Result<String, String> {
    if !valid_sound_id(&id) { return Err("notifications.invalid_audio".into()); }
    use std::io::Read;
    let path = state.path.parent().ok_or("notifications.path")?.join("notification-sounds").join(id);
    let file = std::fs::File::open(path).map_err(|e| e.to_string())?;
    let mut bytes = Vec::new();
    file.take((MAX_SOUND_BYTES + 1) as u64).read_to_end(&mut bytes).map_err(|e| e.to_string())?;
    if !validate_audio(&bytes) { return Err("notifications.invalid_audio".into()); }
    use base64::Engine;
    Ok(base64::engine::general_purpose::STANDARD.encode(bytes))
}
#[derive(Serialize)]
pub struct OverlayDisplay { width: f64, height: f64 }
#[tauri::command]
pub fn get_overlay_display(app: tauri::AppHandle) -> Result<OverlayDisplay, String> {
    use tauri::Manager;
    let window = app.get_webview_window("overlay").ok_or("notifications.overlay_missing")?;
    let size = window.inner_size().map_err(|e| e.to_string())?;
    let scale = window.scale_factor().map_err(|e| e.to_string())?;
    Ok(OverlayDisplay { width: size.width as f64 / scale, height: size.height as f64 / scale })
}
#[tauri::command]
pub fn set_overlay_visible(window: tauri::WebviewWindow, state: tauri::State<'_, NotificationState>, visible: bool) -> Result<(), String> {
    if window.label() != "overlay" { return Err("notifications.overlay_only".into()); }
    let editing = *state.editor.lock().map_err(|e| e.to_string())?;
    crate::native::overlay_safety::set_visible(&window, visible && !editing)
}

fn request_overlay_editor_close(app: &tauri::AppHandle, feedback: Option<&str>) -> Result<(), String> {
    use tauri::Manager;
    let state = app.state::<NotificationState>();
    state.restore_main_after_feedback.store(false, Ordering::Release);
    if let Some(window) = app.get_webview_window("overlay-editor") {
        *state.pending_editor_feedback.lock().map_err(|e| e.to_string())? = feedback.map(str::to_owned);
        window.close().map_err(|e| e.to_string())?;
    } else {
        *state.editor.lock().map_err(|e| e.to_string())? = false;
        *state.pending_editor_feedback.lock().map_err(|e| e.to_string())? = None;
        notify_ready(app);
    }
    Ok(())
}

pub(crate) fn close_overlay_editor(app: &tauri::AppHandle) -> Result<(), String> {
    request_overlay_editor_close(app, Some("cancelled"))
}

#[tauri::command]
pub fn close_overlay_editor_from_main(app: tauri::AppHandle, window: tauri::WebviewWindow) -> Result<(), String> {
    if window.label() != "main" { return Err("notifications.main_only".into()); }
    close_overlay_editor(&app)
}

#[tauri::command]
// WebView2 creation must run outside the synchronous command dispatcher on Windows.
pub async fn open_overlay_editor(app: tauri::AppHandle, window: tauri::WebviewWindow, state: tauri::State<'_, NotificationState>) -> Result<(), String> {
    use tauri::Manager;
    if window.label() != "main" { return Err("notifications.main_only".into()); }
    {
        let mut active = state.editor.lock().map_err(|e| e.to_string())?;
        if *active {
            if let Some(editor) = app.get_webview_window("overlay-editor") {
                editor.set_focus().map_err(|e| e.to_string())?;
                return Ok(());
            }
            // Another invocation may still be building the window.
            return Ok(());
        }
        *active = true;
    }
    *state.pending_editor_feedback.lock().map_err(|e| e.to_string())? = None;
    *state.editor_feedback.lock().map_err(|e| e.to_string())? = None;
    if let Some(overlay) = app.get_webview_window("overlay") {
        if let Err(error) = crate::native::overlay_safety::set_visible(&overlay, false) {
            *state.editor.lock().map_err(|e| e.to_string())? = false;
            notify_ready(&app);
            return Err(error);
        }
    }
    let builder = tauri::WebviewWindowBuilder::new(
        &app,
        "overlay-editor",
        tauri::WebviewUrl::App("index.html".into()),
    )
    .title("Настройка оверлея")
    .decorations(false)
    .resizable(false)
    .skip_taskbar(true)
    .always_on_top(true)
    .fullscreen(true)
    // Windows shares one WebView2 environment; browser arguments must match main/overlay.
    .additional_browser_args("--autoplay-policy=no-user-gesture-required")
    .visible(false);
    let editor = match builder.build() {
        Ok(editor) => editor,
        Err(error) => {
            *state.editor.lock().map_err(|e| e.to_string())? = false;
            notify_ready(&app);
            return Err(error.to_string());
        }
    };
    if let Err(error) = editor.show().and_then(|_| editor.set_focus()) {
        *state.pending_editor_feedback.lock().map_err(|e| e.to_string())? = Some("suppress".into());
        let _ = editor.close();
        *state.editor.lock().map_err(|e| e.to_string())? = false;
        notify_ready(&app);
        return Err(error.to_string());
    }
    if let Err(error) = window.hide() {
        *state.pending_editor_feedback.lock().map_err(|e| e.to_string())? = Some("suppress".into());
        let _ = editor.close();
        *state.editor.lock().map_err(|e| e.to_string())? = false;
        let _ = window.show();
        let _ = window.set_focus();
        notify_ready(&app);
        return Err(error.to_string());
    }
    state.restore_main_after_feedback.store(true, Ordering::Release);
    notify_ready(&app);
    Ok(())
}

#[tauri::command]
pub fn finish_overlay_editor(window: tauri::WebviewWindow, state: tauri::State<'_, NotificationState>, status: String) -> Result<(), String> {
    if window.label() != "overlay-editor" { return Err("notifications.editor_only".into()); }
    if !matches!(status.as_str(), "saved" | "cancelled") { return Err("notifications.invalid_editor_status".into()); }
    *state.pending_editor_feedback.lock().map_err(|e| e.to_string())? = Some(status);
    window.close().map_err(|e| e.to_string())?;
    Ok(())
}

#[tauri::command]
pub fn restore_overlay_editor_host(app: tauri::AppHandle, window: tauri::WebviewWindow, state: tauri::State<'_, NotificationState>) -> Result<(), String> {
    use tauri::Manager;
    if window.label() != "overlay" { return Err("notifications.overlay_only".into()); }
    *state.editor_feedback.lock().map_err(|e| e.to_string())? = None;
    if state.restore_main_after_feedback.swap(false, Ordering::AcqRel) {
        let main = app.get_webview_window("main").ok_or("notifications.main_missing")?;
        main.show().map_err(|e| e.to_string())?;
        main.set_focus().map_err(|e| e.to_string())?;
    }
    Ok(())
}

pub(crate) fn overlay_editor_destroyed(app: &tauri::AppHandle) -> Result<(), String> {
    use tauri::Manager;
    let state = app.state::<NotificationState>();
    *state.editor.lock().map_err(|e| e.to_string())? = false;
    let feedback = state.pending_editor_feedback.lock().map_err(|e| e.to_string())?.take();
    let feedback = match feedback {
        Some(value) if matches!(value.as_str(), "saved" | "cancelled") => Some(value),
        Some(value) if value == "suppress" => None,
        None => Some("cancelled".into()),
        _ => None,
    };
    *state.editor_feedback.lock().map_err(|e| e.to_string())? = feedback;
    notify_ready(app);
    use tauri::Emitter;
    app.emit_to("main", "overlay-editor-closed", ()).map_err(|e| e.to_string())?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use crate::{
        model::{EngineLifecycle, EngineSnapshot},
        notifications::transitions,
    };

    #[test]
    fn legacy_event_layout_migrates_to_exactly_three_elements() {
        let root = tempfile::tempdir().unwrap();
        let value = serde_json::json!({"overlayEnabled": true, "soundEnabled": true, "sound": "bell", "volume": 60,
          "layout": {"recording": {"x": 0.8, "y": 0.9}, "replay": {"x": 0.9, "y": 0.9},
          "replay_saved": {"x": 0.6, "y": 0.7}, "recording_error": {"x": 0.1, "y": 0.1}}});
        std::fs::write(root.path().join("notifications.json"), serde_json::to_vec(&value).unwrap()).unwrap();
        let state = super::NotificationState::new(root.path().to_path_buf());
        let settings = state.settings.lock().unwrap().clone();
        assert_eq!(settings.layout.len(), 3);
        assert_eq!(settings.layout["messages"].x, 0.6);
        assert_eq!(settings.layout["recording"].x, 0.8);
        assert_eq!(settings.sound, "bell");
        state.save(settings).unwrap();
        let document: serde_json::Value = serde_json::from_slice(&std::fs::read(root.path().join("notifications.json")).unwrap()).unwrap();
        assert_eq!(document["layout"].as_object().unwrap().len(), 3);
        assert!(document["layout"].get("replay_saved").is_none());
    }

    #[test]
    fn audio_import_validation_rejects_unbounded_and_unrecognized_files() {
        assert!(!super::validate_audio(b"not audio"));
        assert!(!super::validate_audio(&vec![0; super::MAX_SOUND_BYTES + 1]));
        assert!(super::validate_audio(b"RIFF0000WAVEdata"));
        assert!(!super::valid_sound_id("../config.json"));
    }
    #[test]
    fn layout_and_custom_sound_survive_restart() {
        let root = tempfile::tempdir().unwrap();
        let state = super::NotificationState::new(root.path().to_path_buf());
        let mut settings = super::NotificationSettings::default();
        settings.layout.get_mut("recording").unwrap().x = 0.4;
        settings.custom_sound = Some("a".repeat(64));
        settings.custom_sound_name = Some("My chime.wav".into());
        settings.sound = "custom".into();
        state.save(settings).unwrap();
        let restored = super::NotificationState::new(root.path().to_path_buf());
        let settings = restored.settings.lock().unwrap();
        assert_eq!(settings.layout["recording"].x, 0.4);
        assert_eq!(settings.sound, "custom");
        assert_eq!(settings.custom_sound_name.as_deref(), Some("My chime.wav"));
    }

    #[test]
    fn legacy_layout_defaults_and_invalid_coordinates_are_rejected() {
        let settings: super::NotificationSettings = serde_json::from_value(serde_json::json!({
            "overlayEnabled":true,"soundEnabled":true,"sound":"glass","volume":35
        })).unwrap();
        let root = tempfile::tempdir().unwrap();
        let state = super::NotificationState::new(root.path().to_path_buf());
        assert!(settings.layout["recording"].y > 0.9);
        let mut invalid = settings;
        invalid.layout.get_mut("recording").unwrap().x = 1.5;
        assert!(state.save(invalid).is_err());
    }

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
