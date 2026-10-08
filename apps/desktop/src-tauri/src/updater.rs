use serde::Serialize;
use base64::Engine;
use std::{sync::{atomic::{AtomicBool, Ordering}, Mutex}, time::{Duration, Instant}};
use tauri::{ipc::Channel, Manager};
use tauri_plugin_updater::{Update, UpdaterExt};

type Result<T> = std::result::Result<T, String>;

#[derive(Default)]
struct OperationGate(AtomicBool);
struct OperationLease<'a>(&'a OperationGate);
impl OperationGate {
    fn acquire(&self) -> Result<OperationLease<'_>> {
        self.0.compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .map_err(|_| "Обновление уже проверяется или устанавливается. Попробуйте позже.")?;
        Ok(OperationLease(self))
    }
}
impl Drop for OperationLease<'_> {
    fn drop(&mut self) { self.0.0.store(false, Ordering::Release); }
}

#[derive(Default)]
pub struct UpdateState {
    gate: OperationGate,
    checked: Mutex<Option<Option<Update>>>,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AvailableUpdate {
    version: String,
    release_notes: Option<String>,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateProgress {
    phase: &'static str,
    downloaded_bytes: u64,
    total_bytes: Option<u64>,
}

fn validate_version(checked: Option<&str>, requested: &str) -> Result<()> {
    if checked == Some(requested) { Ok(()) }
    else { Err("Версия обновления изменилась. Перезапустите приложение для повторной проверки.".into()) }
}

fn validate_signature_version(signature: &str, version: &str) -> Result<()> {
    let invalid = || "Подписанный пакет не соответствует предложенной версии. Обновление отменено.".to_string();
    let decoded = base64::engine::general_purpose::STANDARD.decode(signature.trim()).map_err(|_| invalid())?;
    let decoded = std::str::from_utf8(&decoded).map_err(|_| invalid())?;
    let comment = decoded.lines().nth(2).and_then(|line| line.strip_prefix("trusted comment: ")).ok_or_else(invalid)?;
    let mut filenames = comment.split('\t').filter_map(|field| field.strip_prefix("file:"));
    let expected = format!("RebellioCap_{version}_x64-setup.exe");
    if filenames.next() == Some(expected.as_str()) && filenames.next().is_none() { Ok(()) }
    else { Err(invalid()) }
}

fn main_only(window: &tauri::WebviewWindow) -> Result<()> {
    if window.label() == "main" { Ok(()) } else { Err("Main window required".into()) }
}

#[tauri::command]
pub async fn check_for_update(window: tauri::WebviewWindow, app: tauri::AppHandle,
    state: tauri::State<'_, UpdateState>) -> Result<Option<AvailableUpdate>> {
    main_only(&window)?;
    if cfg!(debug_assertions) { return Ok(None); }
    let _lease = state.gate.acquire()?;
    if let Some(cached) = state.checked.lock().map_err(|_| "updater.lock_poisoned")?.as_ref() {
        return Ok(cached.as_ref().map(|u| AvailableUpdate { version: u.version.clone(), release_notes: u.body.clone() }));
    }
    let exit_app = app.clone();
    let checked = app.updater_builder()
        .timeout(Duration::from_secs(15))
        .version_comparator(|current, release| release.version > current && release.version.pre.is_empty())
        // Windows updater exits via process::exit, bypassing Tauri's Exit events.
        .on_before_exit(move || {
            // The host was already stopped by prepare_update. Do not close its tray
            // worker here: ShellExecute can fail after this hook, leaving the app alive.
            exit_app.state::<crate::editor::EditorState>().shutdown();
        })
        .build().map_err(|e| e.to_string())?
        .check().await.map_err(|e| {
            crate::logging::record("info", "updater", "check_failed", serde_json::json!({"error":e.to_string()}));
            "Не удалось проверить обновления. Программа продолжит работать без обновления.".to_string()
        })?;
    let offer = checked.as_ref().map(|u| AvailableUpdate { version: u.version.clone(), release_notes: u.body.clone() });
    *state.checked.lock().map_err(|_| "updater.lock_poisoned")? = Some(checked);
    Ok(offer)
}

#[tauri::command]
pub async fn install_update(window: tauri::WebviewWindow, app: tauri::AppHandle,
    state: tauri::State<'_, UpdateState>, version: String, on_progress: Channel<UpdateProgress>) -> Result<()> {
    main_only(&window)?;
    if cfg!(debug_assertions) { return Err("Обновление недоступно в режиме разработки.".into()); }
    let _lease = state.gate.acquire()?;
    let mut update = state.checked.lock().map_err(|_| "updater.lock_poisoned")?
        .as_ref().and_then(|u| u.clone());
    validate_version(update.as_ref().map(|u| u.version.as_str()), &version)?;
    let mut update = update.take().ok_or("Обновление не найдено.")?;
    let editor = app.state::<crate::editor::EditorState>();
    let _editor_lease = editor.reserve_update()?;
    update.timeout = Some(Duration::from_secs(600));
    let mut downloaded = 0u64;
    let mut total = None;
    let mut last_progress = Instant::now();
    let _ = on_progress.send(UpdateProgress { phase: "downloading", downloaded_bytes: 0, total_bytes: None });
    let bytes = update.download(|length, content_length| {
        downloaded += length as u64;
        total = content_length;
        if last_progress.elapsed() >= Duration::from_millis(100) || total == Some(downloaded) {
            let _ = on_progress.send(UpdateProgress { phase: "downloading", downloaded_bytes: downloaded, total_bytes: total });
            last_progress = Instant::now();
        }
    }, || {}).await.map_err(|e| {
        crate::logging::record("error", "updater", "download_failed", serde_json::json!({"error":e.to_string()}));
        "Не удалось скачать или проверить подпись обновления. Запись не прервана. Попробуйте ещё раз.".to_string()
    })?;
    // This CLI signs the filename in the trusted comment. The plugin has verified
    // that comment too; binding it to the offered version prevents replaying an older installer.
    validate_signature_version(&update.signature, &update.version)?;
    // download() verifies the signature before returning; never stop recording on an untrusted package.
    let _ = on_progress.send(UpdateProgress { phase: "preparing", downloaded_bytes: downloaded, total_bytes: total });
    let host = app.state::<crate::commands::HostState>();
    host.prepare_update().await?;
    let _ = on_progress.send(UpdateProgress { phase: "installing", downloaded_bytes: downloaded, total_bytes: total });
    let result = tauri::async_runtime::spawn_blocking(move || update.install(bytes)).await
        .map_err(|e| e.to_string()).and_then(|r| r.map_err(|e| e.to_string()));
    // Successful Windows installation exits the process; this path handles launch failures only.
    host.cancel_update();
    result.map_err(|e| {
        crate::logging::record("error", "updater", "install_failed", serde_json::json!({"error":e}));
        "Не удалось запустить установку обновления. Запись остановлена; её можно запустить снова.".into()
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn windows_updates_use_silent_per_user_installation() {
        let config: serde_json::Value = serde_json::from_str(include_str!("../tauri.conf.json")).unwrap();
        assert_eq!(config["plugins"]["updater"]["windows"]["installMode"], "quiet");
        assert_eq!(config["bundle"]["windows"]["nsis"]["installMode"], "currentUser");
    }

    #[test]
    fn announced_version_must_match_the_authenticated_installer_filename() {
        let signature = base64::engine::general_purpose::STANDARD.encode(
            "untrusted comment: test\nsignature\ntrusted comment: timestamp:1\tfile:RebellioCap_0.1.8_x64-setup.exe\nglobal\n");
        assert!(validate_signature_version(&signature, "0.1.8").is_ok());
        assert!(validate_signature_version(&signature, "0.1.9").is_err());
        assert!(validate_signature_version("invalid base64", "0.1.8").is_err());
        let missing_filename = base64::engine::general_purpose::STANDARD.encode(
            "untrusted comment: test\nsignature\ntrusted comment: timestamp:1\nglobal\n");
        assert!(validate_signature_version(&missing_filename, "0.1.8").is_err());
    }

    #[test]
    #[ignore = "Requires a locally built signed NSIS release installer"]
    fn packaged_installer_matches_the_embedded_key_and_rejects_tampering() {
        let config: serde_json::Value = serde_json::from_str(include_str!("../tauri.conf.json")).unwrap();
        let version = config["version"].as_str().unwrap();
        let pubkey = base64::engine::general_purpose::STANDARD.decode(config["plugins"]["updater"]["pubkey"].as_str().unwrap()).unwrap();
        let key = minisign_verify::PublicKey::decode(std::str::from_utf8(&pubkey).unwrap()).unwrap();
        let installer = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join(format!("target/release/bundle/nsis/RebellioCap_{version}_x64-setup.exe"));
        let mut bytes = std::fs::read(&installer).unwrap();
        let signature = std::fs::read_to_string(installer.with_extension("exe.sig")).unwrap();
        let decoded = base64::engine::general_purpose::STANDARD.decode(signature.trim()).unwrap();
        let parsed = minisign_verify::Signature::decode(std::str::from_utf8(&decoded).unwrap()).unwrap();
        key.verify(&bytes, &parsed, false).unwrap();
        validate_signature_version(&signature, version).unwrap();
        assert!(validate_signature_version(&signature, "999.999.999").is_err());
        bytes[0] ^= 1;
        assert!(key.verify(&bytes, &parsed, false).is_err());
        let changed = String::from_utf8(decoded).unwrap().replace(&format!("RebellioCap_{version}"), "RebellioCap_999.999.999");
        let changed = minisign_verify::Signature::decode(&changed).unwrap();
        assert!(key.verify(&std::fs::read(&installer).unwrap(), &changed, false).is_err());
    }

    #[test]
    fn operation_lease_rejects_duplicates_and_releases_after_failure() {
        let gate = OperationGate::default();
        let lease = gate.acquire().unwrap();
        assert!(gate.acquire().is_err());
        drop(lease);
        assert!(gate.acquire().is_ok());
    }

    #[test]
    fn install_requires_the_exact_previously_checked_version() {
        assert!(validate_version(Some("0.1.9"), "0.1.9").is_ok());
        assert!(validate_version(None, "0.1.9").is_err());
        assert!(validate_version(Some("0.1.9"), "0.2.0").is_err());
    }

    #[test]
    fn update_guard_blocks_new_editor_sessions_and_releases_on_failure() {
        let editor = crate::editor::EditorState::default();
        let lease = editor.reserve_update().unwrap();
        assert!(editor.reserve_update().is_err());
        assert!(crate::editor::begin_owner(&editor, "editor").is_err());
        drop(lease);
        crate::editor::begin_owner(&editor, "editor").unwrap();
        assert!(editor.reserve_update().is_err());
    }
}
