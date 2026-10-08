use serde::{Deserialize, Serialize};
use std::{path::PathBuf, sync::{Mutex, atomic::{AtomicBool, Ordering}}};
use tauri::{Emitter, Manager};

static RUSSIAN: AtomicBool = AtomicBool::new(false);
pub fn text(ru: &'static str, en: &'static str) -> &'static str {
    if RUSSIAN.load(Ordering::Relaxed) { ru } else { en }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Preference { System, Ru, En }
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct LanguageSettings { pub preference: Preference, pub language: &'static str, #[serde(rename = "systemLanguage")] pub system_language: &'static str }
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Saved { preference: Preference }
pub fn language_for_id(id: u16) -> &'static str { if id & 0x3ff == 0x19 { "ru" } else { "en" } }
fn system_language() -> &'static str {
    #[cfg(windows)]
    { language_for_id(unsafe { windows::Win32::Globalization::GetUserDefaultUILanguage() }) }
    #[cfg(not(windows))]
    { "en" }
}
fn resolved(preference: Preference) -> LanguageSettings {
    let system_language = system_language();
    LanguageSettings { preference, system_language, language: match preference { Preference::System => system_language, Preference::Ru => "ru", Preference::En => "en" } }
}
pub struct LanguageState { transition: Mutex<()>, current: Mutex<LanguageSettings>, path: PathBuf }
impl LanguageState {
    pub fn new(root: PathBuf) -> Self {
        let path = root.join("language.json");
        let preference = std::fs::read(&path).ok().and_then(|bytes| serde_json::from_slice::<Saved>(&bytes).ok()).map(|saved| saved.preference).unwrap_or(Preference::System);
        let current = resolved(preference);
        RUSSIAN.store(current.language == "ru", Ordering::Relaxed);
        Self { transition: Mutex::new(()), current: Mutex::new(current), path }
    }
    fn save(&self, preference: Preference) -> Result<LanguageSettings, String> {
        let mut current = self.current.lock().map_err(|e| e.to_string())?;
        let parent = self.path.parent().ok_or("language.path")?;
        std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
        let file = tempfile::NamedTempFile::new_in(parent).map_err(|e| e.to_string())?;
        serde_json::to_writer(file.as_file(), &Saved { preference }).map_err(|e| e.to_string())?;
        file.as_file().sync_all().map_err(|e| e.to_string())?;
        file.persist(&self.path).map_err(|e| e.to_string())?;
        *current = resolved(preference);
        RUSSIAN.store(current.language == "ru", Ordering::Relaxed);
        Ok(current.clone())
    }
}
#[tauri::command]
pub fn get_language_settings(state: tauri::State<'_, LanguageState>) -> Result<LanguageSettings, String> {
    Ok(state.current.lock().map_err(|e| e.to_string())?.clone())
}
#[tauri::command]
pub fn set_language_settings(app: tauri::AppHandle, state: tauri::State<'_, LanguageState>, preference: Preference) -> Result<LanguageSettings, String> {
    let _transition = state.transition.lock().map_err(|e| e.to_string())?;
    let settings = state.save(preference)?;
    app.state::<crate::commands::HostState>().tray_updates().notify();
    if let Err(error) = app.emit("language-changed", &settings) {
        eprintln!("Could not broadcast language settings: {error}");
    }
    Ok(settings)
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn primary_language_ignores_sublanguage() {
        for id in [0x0419, 0x0819, 0x0019] { assert_eq!(language_for_id(id), "ru"); }
        for id in [0x0409, 0x0809, 0x0411, 0] { assert_eq!(language_for_id(id), "en"); }
    }
    #[test]
    fn override_roundtrip_and_invalid_preference() {
        let dir = tempfile::tempdir().unwrap();
        let state = LanguageState::new(dir.path().into());
        assert_eq!(state.current.lock().unwrap().preference, Preference::System);
        assert!(serde_json::from_str::<Saved>(r#"{"preference":"fr"}"#).is_err());
        for preference in [Preference::Ru, Preference::En, Preference::System] {
            let saved = state.save(preference).unwrap();
            assert_eq!(*LanguageState::new(dir.path().into()).current.lock().unwrap(), saved);
        }
    }
    #[test]
    fn malformed_settings_fall_back_without_rewriting_the_file() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("language.json");
        std::fs::write(&path, r#"{"preference":"fr"}"#).unwrap();
        let state = LanguageState::new(dir.path().into());
        assert_eq!(state.current.lock().unwrap().preference, Preference::System);
        assert_eq!(std::fs::read_to_string(path).unwrap(), r#"{"preference":"fr"}"#);
    }
    #[test]
    fn response_and_event_payload_share_the_resolved_snapshot() {
        let dir = tempfile::tempdir().unwrap();
        let state = LanguageState::new(dir.path().into());
        let payload = state.save(Preference::Ru).unwrap();
        assert_eq!(serde_json::to_value(&payload).unwrap(), serde_json::json!({"preference":"ru", "language":"ru", "systemLanguage": system_language()}));
        assert_eq!(*state.current.lock().unwrap(), payload);
    }
    #[test]
    fn persistence_failure_keeps_current_value() {
        let dir = tempfile::tempdir().unwrap();
        let state = LanguageState::new(dir.path().into());
        let before = state.save(Preference::En).unwrap();
        std::fs::remove_file(&state.path).unwrap();
        std::fs::create_dir(&state.path).unwrap();
        assert!(state.save(Preference::Ru).is_err());
        assert_eq!(*state.current.lock().unwrap(), before);
    }
}
