pub use crate::clip_catalog::SavedClip;
use crate::clip_catalog::{recovery_extension, is_clip, is_orphan_recording, is_reparse, scan_clips};
use crate::{
    config::*,
    engine::{
        protocol::SessionCommand,
        supervisor::{resolve_engine, EngineSupervisor, NotificationWake},
        Result,
    },
    model::*,
    native,
};
use serde::Serialize;
use std::{
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex,
    },
    time::Duration,
};

pub fn complete_transaction(
    store: &ConfigStore,
    explicit: bool,
    devices: &DeviceInventory,
    mut start: impl FnMut(&ActiveConfig) -> Result<()>,
) -> Result<()> {
    let previous = store.load().map_err(|e| e.to_string())?;
    store
        .commit_completed(explicit, devices)
        .map_err(|e| e.to_string())?;
    let active = store
        .load()
        .map_err(|e| e.to_string())?
        .active
        .ok_or("config.active_missing")?;
    if let Err(start_error) = start(&active) {
        store
            .replace_host_state(&previous)
            .map_err(|e| format!("{start_error}; rollback failed: {e}"))?;
        return Err(start_error);
    }
    Ok(())
}

pub fn apply_transaction(
    store: &ConfigStore,
    candidate: ActiveConfig,
    devices: &DeviceInventory,
    mut stop: impl FnMut() -> Result<()>,
    mut start: impl FnMut(&ActiveConfig) -> Result<()>,
) -> Result<()> {
    validate_active(&candidate, devices).map_err(|e| e.to_string())?;
    native::recording_test::engine_config(&candidate, None)?;
    let previous = store.load().map_err(|e| e.to_string())?;
    let old = previous
        .active
        .as_ref()
        .ok_or("config.onboarding_required")?;
    if let Err(e) = stop() {
        let restart = start(old);
        return Err(format!("{e}; restore start: {restart:?}"));
    }
    let mut next = previous.clone();
    next.active = Some(candidate.clone());
    next.draft = candidate.as_draft();
    next.last_successful_test = None;
    let apply = store
        .replace_host_state(&next)
        .map_err(|e| e.to_string())
        .and_then(|()| start(&candidate));
    if let Err(e) = apply {
        let restored = store
            .replace_host_state(&previous)
            .map_err(|e| e.to_string());
        let restarted = start(old);
        return Err(format!(
            "{e}; rollback: {restored:?}; previous engine: {restarted:?}"
        ));
    }
    Ok(())
}

pub fn validate_owned_file(path: &Path, issued: &[PathBuf]) -> Result<PathBuf> {
    let clean_path = crate::model::strip_verbatim_prefix(path);
    if !issued
        .iter()
        .any(|p| crate::model::strip_verbatim_prefix(p) == clean_path)
        || !path.is_absolute()
        || !path.is_file()
        || !crate::model::is_canonical_path(path)
    {
        return Err("shell.path_not_host_owned".into());
    }
    let metadata = std::fs::symlink_metadata(path).map_err(|e| e.to_string())?;
    use std::os::windows::fs::MetadataExt;
    if metadata.file_attributes() & 0x400 != 0 {
        return Err("shell.reparse_point".into());
    }
    Ok(clean_path)
}

pub struct Host {
    updating: bool,
    store: ConfigStore,
    executable: Result<PathBuf>,
    engine: Option<EngineSupervisor>,
    diagnostics: Option<PathBuf>,
    test_clip: Option<PathBuf>,
    last_snapshot: EngineSnapshot,
    notifications: Vec<&'static str>,
    notification_wake: Option<NotificationWake>,
    tray_updates: Arc<crate::tray::TrayUpdates>,
    continuous_recording_enabled: bool,
}
pub struct HostState {
    inner: Arc<Mutex<Host>>,
    measurement: Mutex<Option<Arc<AtomicBool>>>,
    tray_updates: Arc<crate::tray::TrayUpdates>,
}
impl HostState {
    pub async fn drain_notifications(&self) -> Result<Vec<&'static str>> {
        dispatch(self, |host| {
            let mut events = std::mem::take(&mut host.notifications);
            if let Some(engine) = &host.engine { events.extend(engine.drain_notifications()); }
            Ok(events)
        }).await
    }
    pub fn tray_status(&self) -> Result<(EngineSnapshot, bool)> {
        let host = self.inner.lock().map_err(|e| e.to_string())?;
        Ok((host.snapshot(), host.continuous_recording_enabled))
    }
    pub fn new(store: ConfigStore, resources: &Path) -> Self {
        Self::with_notification_wake(store, resources, None)
    }
    pub(crate) fn tray_updates(&self) -> Arc<crate::tray::TrayUpdates> { self.tray_updates.clone() }
    pub fn with_notification_wake(store: ConfigStore, resources: &Path, notification_wake: Option<NotificationWake>) -> Self {
        let tray_updates = Arc::new(crate::tray::TrayUpdates::default());
        let engine_updates = tray_updates.clone();
        let notification_wake: Option<NotificationWake> = Some(Arc::new(move || {
            engine_updates.notify();
            if let Some(wake) = &notification_wake { wake(); }
        }));
        let override_path = std::env::var_os("REBELLIOCAP_ENGINE_PATH").map(PathBuf::from);
        // Host owns configuration mutations. The tray does not need filesystem
        // locks, backup validation and JSON parsing on every 750 ms tick.
        let continuous_recording_enabled = store.load().ok().and_then(|saved| saved.active)
            .is_some_and(|config| config.continuous_recording_enabled);
        Self {
            inner: Arc::new(Mutex::new(Host {
                updating: false,
                store,
                executable: resolve_engine(
                    resources,
                    override_path.as_deref(),
                    cfg!(debug_assertions),
                ),
                engine: None,
                diagnostics: None,
                test_clip: None,
                last_snapshot: EngineSnapshot::default(),
                notifications: Vec::new(),
                notification_wake,
                tray_updates: tray_updates.clone(),
                continuous_recording_enabled,
            })),
            measurement: Mutex::new(None),
            tray_updates,
        }
    }
    pub fn shutdown(&self) {
        if let Some(token) = self.measurement.lock().unwrap().take() {
            token.store(true, Ordering::Release);
        }
        if let Ok(mut host) = self.inner.lock() {
            let _ = host.stop();
        }
        self.tray_updates.close();
    }
    pub(crate) async fn prepare_update(&self) -> Result<()> {
        let inner = self.inner.clone();
        tauri::async_runtime::spawn_blocking(move || {
            let mut host = inner.lock().map_err(|_| "host.lock_poisoned")?;
            host.updating = true;
            if let Err(error) = host.stop() {
                host.updating = false;
                return Err(format!("Не удалось корректно завершить запись перед обновлением: {error}"));
            }
            Ok(())
        }).await.map_err(|e| e.to_string())?
    }
    pub(crate) fn cancel_update(&self) {
        if let Ok(mut host) = self.inner.lock() { host.updating = false; }
    }
}
impl Host {
    fn executable(&self) -> Result<&Path> {
        self.executable
            .as_ref()
            .map(|p| p.as_path())
            .map_err(Clone::clone)
    }
    fn inventory(&self) -> Result<DeviceInventory> {
        Ok(native::catalogs::load(self.executable()?)?.inventory())
    }
    fn start(&mut self, config: &ActiveConfig) -> Result<()> {
        if self
            .engine
            .as_ref()
            .is_some_and(EngineSupervisor::is_running)
        {
            return Err("engine.already_running".into());
        }
        validate_active(config, &self.inventory()?).map_err(|e| e.to_string())?;
        native::system_check::check_output(
            &config.output_directory,
            native::system_check::estimate_bytes(&config.as_draft()),
        )?;
        crate::logging::record("info", "engine", "configuration", serde_json::to_value(config).unwrap_or_default());
        let file = native::recording_test::write_config(self.store.root(), config, None)?;
        let engine = EngineSupervisor::spawn_with_notification_wake(
            self.executable()?,
            &[
                "session-json".into(),
                "--config".into(),
                file.path().as_os_str().into(),
            ],
            Duration::from_secs(15),
            self.notification_wake.clone(),
        )?;
        self.engine = Some(engine);
        self.continuous_recording_enabled = config.continuous_recording_enabled;
        self.tray_updates.notify();
        Ok(())
    }
    fn snapshot(&self) -> EngineSnapshot {
        self.engine
            .as_ref()
            .map_or_else(|| self.last_snapshot.clone(), EngineSupervisor::snapshot)
    }
    pub(crate) fn reload_recording_names(&self) -> Result<()> {
        if let Some(engine) = &self.engine {
            if engine.is_running() {
                engine.request(SessionCommand::ReloadRecordingNames)?;
            }
        }
        Ok(())
    }
    fn stop(&mut self) -> Result<()> {
        if let Some(engine) = self.engine.take() {
            let result = engine.stop();
            self.last_snapshot = engine.snapshot();
            self.notifications.extend(engine.drain_notifications());
            if !self.notifications.is_empty() { if let Some(wake) = &self.notification_wake { wake(); } }
            result
        } else {
            Ok(())
        }
    }
    fn blocked(&mut self, error: &str) {
        self.last_snapshot = EngineSnapshot {
            revision: self.last_snapshot.revision + 1,
            lifecycle: EngineLifecycle::Blocked,
            last_error: Some(NativeError {
                code: "host.configuration_or_start_failed".into(),
                message: error.into(),
                hresult: None,
            }),
            ..Default::default()
        };
        self.tray_updates.notify();
    }
    fn complete(&mut self, explicit: bool) -> Result<()> {
        if self
            .engine
            .as_ref()
            .is_some_and(EngineSupervisor::is_running)
        {
            return Err("engine.already_running".into());
        }
        let devices = self.inventory()?;
        let store = ConfigStore::from_root_host(self.store.root())?;
        complete_transaction(&store, explicit, &devices, |active| self.start(active))
    }
    fn apply(&mut self, candidate: ActiveConfig) -> Result<()> {
        let devices = self.inventory()?;
        let store = ConfigStore::from_root_host(self.store.root())?;
        // RefCell scopes the sequential stop/start closures without exposing host state to IPC.
        let host = std::cell::RefCell::new(self);
        apply_transaction(
            &store,
            candidate,
            &devices,
            || host.borrow_mut().stop(),
            |active| host.borrow_mut().start(active),
        )
    }
}

pub(crate) async fn dispatch<T: Send + 'static>(
    state: &HostState,
    action: impl FnOnce(&mut Host) -> Result<T> + Send + 'static,
) -> Result<T> {
    let inner = state.inner.clone();
    tauri::async_runtime::spawn_blocking(move || {
        let mut host = inner.lock().map_err(|_| "host.lock_poisoned")?;
        if host.updating { return Err("Идёт установка обновления. Дождитесь перезапуска приложения.".into()); }
        let started = std::time::Instant::now();
        let result = action(&mut host);
        if let Err(error) = &result { crate::logging::record("error", "host", "action_failed", serde_json::json!({"error":error,"elapsedMs":started.elapsed().as_millis()})); }
        result
    })
    .await
    .map_err(|e| e.to_string())?
}
#[derive(Serialize)]
pub struct Bootstrap {
    pub route: &'static str,
    pub state: StoredState,
    pub snapshot: EngineSnapshot,
}

#[tauri::command]
pub async fn bootstrap_app(state: tauri::State<'_, HostState>) -> Result<Bootstrap> {
    crate::logging::record("info", "host", "command", serde_json::json!({"name":"bootstrap_app"}));
    dispatch(&state, |host| {
        if let Ok(catalog) = host.executable().and_then(native::catalogs::load) {
            host.store.migrate_monitor_identity(&catalog.monitors).map_err(|e| e.to_string())?;
        }
        let saved = host.store.load().map_err(|e| e.to_string())?;
        if let Some(config) = &saved.active {
            if !host
                .engine
                .as_ref()
                .is_some_and(EngineSupervisor::is_running)
            {
                if let Err(e) = host.start(config) {
                    host.blocked(&e);
                }
            }
        }
        Ok(Bootstrap {
            route: if saved.onboarding.completed {
                "home"
            } else {
                "onboarding"
            },
            state: saved,
            snapshot: host.snapshot(),
        })
    })
    .await
}
#[tauri::command]
pub async fn get_onboarding_state(state: tauri::State<'_, HostState>) -> Result<StoredState> {
    dispatch(&state, |host| host.store.load().map_err(|e| e.to_string())).await
}
#[tauri::command]
pub async fn run_system_check(state: tauri::State<'_, HostState>) -> Result<SystemCheckResult> {
    crate::logging::record("info", "host", "command", serde_json::json!({"name":"run_system_check"}));
    dispatch(&state, |host| {
        let saved = host.store.load().map_err(|e| e.to_string())?;
        let mut result =
            native::system_check::run(host.executable(), &saved.draft, host.store.root());
        crate::logging::record("info", "system", "check_results", serde_json::to_value(&result).unwrap_or_default());
        use std::io::Write;
        let mut log = tempfile::Builder::new()
            .prefix("RebellioCap-diagnostics-")
            .suffix(".json")
            .tempfile_in(host.store.root())
            .map_err(|e| e.to_string())?;
        log.write_all(&serde_json::to_vec_pretty(&result).map_err(|e| e.to_string())?)
            .and_then(|()| log.as_file().sync_all())
            .map_err(|e| e.to_string())?;
        let (_, path) = log.keep().map_err(|e| e.to_string())?;
        let path = crate::model::strip_verbatim_prefix(
            &path.canonicalize().map_err(|e| e.to_string())?,
        );
        host.diagnostics = Some(path.clone());
        result.diagnostics_path = Some(path.to_string_lossy().into());
        Ok(result)
    })
    .await
}
#[tauri::command]
pub async fn list_monitors(state: tauri::State<'_, HostState>) -> Result<Vec<MonitorChoice>> {
    crate::logging::record("info", "host", "command", serde_json::json!({"name":"list_monitors"}));
    dispatch(&state, |host| {
        Ok(native::catalogs::load(host.executable()?)?.monitors)
    })
    .await
}
#[tauri::command]
pub async fn list_audio_endpoints(state: tauri::State<'_, HostState>) -> Result<AudioCatalog> {
    crate::logging::record("info", "host", "command", serde_json::json!({"name":"list_audio_endpoints"}));
    dispatch(&state, |host| {
        Ok(native::catalogs::load(host.executable()?)?.audio)
    })
    .await
}

/// None cancels the active sample; a new endpoint replaces/cancels the previous sample.
#[tauri::command]
pub async fn read_audio_peak(endpoint_id: String) -> Result<f32> {
    tauri::async_runtime::spawn_blocking(move || native::read_audio_peak(&endpoint_id))
        .await.map_err(|e| e.to_string())?
}

/// None cancels the active sample; a new endpoint replaces/cancels the previous sample.
#[tauri::command]
pub async fn measure_audio_level(
    state: tauri::State<'_, HostState>,
    endpoint_id: Option<String>,
) -> Result<AudioMeasurement> {
    let token = Arc::new(AtomicBool::new(false));
    if let Some(old) = state
        .measurement
        .lock()
        .map_err(|_| "host.lock_poisoned")?
        .replace(token.clone())
    {
        old.store(true, Ordering::Release);
    }
    let Some(id) = endpoint_id else {
        return Ok(AudioMeasurement {
            samples: vec![],
            cancelled: true,
        });
    };
    let endpoint = id.clone();
    dispatch(&state, move |host| {
        let available = host.inventory()?;
        if !available.system_audio_ids.contains(&endpoint)
            && !available.microphone_ids.contains(&endpoint)
        {
            return Err("audio.endpoint_not_in_catalog".into());
        }
        Ok(())
    })
    .await?;
    tauri::async_runtime::spawn_blocking(move || native::measure_endpoint(&id, &token))
        .await
        .map_err(|e| e.to_string())?
}
#[tauri::command]
pub async fn choose_output_directory() -> Result<Option<PathBuf>> {
    // A dedicated STA keeps folder-dialog apartment lifetime independent of the async pool.
    tauri::async_runtime::spawn_blocking(|| {
        std::thread::spawn(native::choose_directory)
            .join()
            .map_err(|_| "dialog.thread_failed")?
    })
    .await
    .map_err(|e| e.to_string())?
}
#[tauri::command]
pub async fn save_onboarding_draft(
    state: tauri::State<'_, HostState>,
    draft: OnboardingDraft,
    last_completed_step: u8,
) -> Result<()> {
    crate::logging::record("info", "host", "command", serde_json::json!({"name":"save_onboarding_draft"}));
    dispatch(&state, move |host| {
        host.store
            .save_draft(draft, last_completed_step)
            .map_err(|e| e.to_string())?;
        host.test_clip = None;
        Ok(())
    })
    .await
}
#[tauri::command]
pub async fn run_recording_test(
    state: tauri::State<'_, HostState>,
) -> Result<RecordingTestSummary> {
    crate::logging::record("info", "host", "command", serde_json::json!({"name":"run_recording_test"}));
    dispatch(&state, |host| {
        if host
            .engine
            .as_ref()
            .is_some_and(EngineSupervisor::is_running)
        {
            return Err("test.engine_already_running".into());
        }
        let draft = host.store.load().map_err(|e| e.to_string())?.draft;
        validate_active(
            &draft.to_active().map_err(|e| e.to_string())?,
            &host.inventory()?,
        )
        .map_err(|e| e.to_string())?;
        native::system_check::check_output(
            draft.output_directory.as_ref().ok_or("output.required")?,
            native::system_check::estimate_bytes(&draft),
        )?;
        let summary = native::recording_test::run(host.executable()?, host.store.root(), &draft)?;
        host.store
            .record_successful_test(summary.clone())
            .map_err(|e| e.to_string())?;
        host.test_clip = Some(summary.clip_path.clone());
        Ok(summary)
    })
    .await
}
#[tauri::command]
pub async fn complete_onboarding(
    state: tauri::State<'_, HostState>,
    explicit_completion: bool,
) -> Result<Bootstrap> {
    crate::logging::record("info", "host", "command", serde_json::json!({"name":"complete_onboarding"}));
    dispatch(&state, move |host| {
        host.complete(explicit_completion)?;
        Ok(Bootstrap {
            route: "home",
            state: host.store.load().map_err(|e| e.to_string())?,
            snapshot: host.snapshot(),
        })
    })
    .await
}
#[tauri::command]
pub async fn get_engine_snapshot(state: tauri::State<'_, HostState>) -> Result<EngineSnapshot> {
    dispatch(&state, |host| Ok(host.snapshot())).await
}
#[tauri::command]
pub async fn start_engine(state: tauri::State<'_, HostState>) -> Result<EngineSnapshot> {
    crate::logging::record("info", "host", "command", serde_json::json!({"name":"start_engine"}));
    dispatch(&state, |host| {
        let config = host
            .store
            .load()
            .map_err(|e| e.to_string())?
            .active
            .ok_or("config.onboarding_required")?;
        host.start(&config)?;
        Ok(host.snapshot())
    })
    .await
}
#[tauri::command]
pub async fn stop_engine(state: tauri::State<'_, HostState>) -> Result<EngineSnapshot> {
    crate::logging::record("info", "host", "command", serde_json::json!({"name":"stop_engine"}));
    dispatch(&state, |host| {
        host.stop()?;
        Ok(host.snapshot())
    })
    .await
}
#[tauri::command]
pub async fn save_replay(state: tauri::State<'_, HostState>) -> Result<EngineSnapshot> {
    crate::logging::record("info", "host", "command", serde_json::json!({"name":"save_replay"}));
    dispatch(&state, |host| {
        host.engine
            .as_ref()
            .ok_or("engine.not_running")?
            .request(SessionCommand::SaveReplay)?;
        Ok(host.snapshot())
    })
    .await
}
#[tauri::command]
pub async fn toggle_recording(state: tauri::State<'_, HostState>) -> Result<EngineSnapshot> {
    crate::logging::record("info", "host", "command", serde_json::json!({"name":"toggle_recording"}));
    dispatch(&state, |host| {
        if !host.engine.as_ref().is_some_and(EngineSupervisor::is_running) {
            let config = host.store.load().map_err(|e| e.to_string())?.active.ok_or("config.onboarding_required")?;
            host.start(&config)?;
            host.engine.as_ref().ok_or("engine.not_running")?.request(SessionCommand::StopReplay)?;
        }
        host.engine
            .as_ref()
            .ok_or("engine.not_running")?
            .request(SessionCommand::ToggleRecording)?;
        Ok(host.snapshot())
    })
    .await
}

#[tauri::command]
pub async fn start_replay(state: tauri::State<'_, HostState>) -> Result<EngineSnapshot> {
    crate::logging::record("info", "host", "command", serde_json::json!({"name":"start_replay"}));
    dispatch(&state, |host| {
        if !host.engine.as_ref().is_some_and(EngineSupervisor::is_running) {
            let config = host.store.load().map_err(|e| e.to_string())?.active.ok_or("config.onboarding_required")?;
            host.start(&config)?;
        } else {
            host.engine.as_ref().ok_or("engine.not_running")?.request(SessionCommand::StartReplay)?;
        }
        Ok(host.snapshot())
    }).await
}

#[tauri::command]
pub async fn stop_replay(state: tauri::State<'_, HostState>) -> Result<EngineSnapshot> {
    crate::logging::record("info", "host", "command", serde_json::json!({"name":"stop_replay"}));
    dispatch(&state, |host| {
        if let Some(engine) = host.engine.as_ref().filter(|engine| engine.is_running()) {
            engine.request(SessionCommand::StopReplay)?;
        }
        Ok(host.snapshot())
    }).await
}
#[tauri::command]
pub async fn apply_runtime_settings(
    app: tauri::AppHandle,
    state: tauri::State<'_, HostState>,
    mut candidate: ActiveConfig,
) -> Result<EngineSnapshot> {
    crate::logging::record("info", "host", "command", serde_json::json!({"name":"apply_runtime_settings"}));
    candidate.preferences.start_with_windows = get_autostart(app)?;
    dispatch(&state, move |host| {
        host.apply(candidate)?;
        Ok(host.snapshot())
    })
    .await
}
#[tauri::command]
pub async fn open_diagnostics(state: tauri::State<'_, HostState>) -> Result<()> {
    dispatch(&state, |host| {
        let path = host.diagnostics.as_ref().ok_or("diagnostics.not_created")?;
        native::open_file(&validate_owned_file(path, std::slice::from_ref(path))?)
    })
    .await
}
#[tauri::command]
pub async fn open_test_clip(state: tauri::State<'_, HostState>) -> Result<()> {
    dispatch(&state, |host| {
        let path = host
            .test_clip
            .as_ref()
            .ok_or("test.clip_not_created_this_session")?;
        native::open_file(&validate_owned_file(path, std::slice::from_ref(path))?)
    })
    .await
}

#[tauri::command]
pub fn get_autostart(app: tauri::AppHandle) -> Result<bool> {
    use tauri_plugin_autostart::ManagerExt;
    app.autolaunch().is_enabled().map_err(|e| e.to_string())
}

#[tauri::command]
pub fn set_autostart(app: tauri::AppHandle, state: tauri::State<'_, HostState>, enabled: bool) -> Result<()> {
    use tauri_plugin_autostart::ManagerExt;
    let host = state.inner.lock().map_err(|e| e.to_string())?;
    let mut config = host.store.load().map_err(|e| e.to_string())?;
    let manager = app.autolaunch();
    let previous = manager.is_enabled().map_err(|e| e.to_string())?;
    if enabled { manager.enable() } else { manager.disable() }.map_err(|e| e.to_string())?;
    if let Some(active) = config.active.as_mut() { active.preferences.start_with_windows = enabled; }
    if let Some(preferences) = config.draft.preferences.as_mut() { preferences.start_with_windows = enabled; }
    // The recording-test fingerprint includes preferences. Do not retain stale evidence.
    config.last_successful_test = None;
    if let Err(error) = host.store.replace_host_state(&config) {
        let rollback = if previous { manager.enable() } else { manager.disable() };
        return Err(format!("{error}; rollback: {rollback:?}"));
    }
    Ok(())
}


fn clip_directory(host: &Host) -> Result<PathBuf> {
    let active = host.store.load().map_err(|e| e.to_string())?.active
        .ok_or("config.onboarding_required")?;
    std::fs::canonicalize(&active.output_directory).map_err(|e| format!("clips.folder_unavailable: {e}"))
}

#[tauri::command]
pub async fn list_clips(state: tauri::State<'_, HostState>) -> Result<Vec<SavedClip>> {
    let directory = dispatch(&state, clip_directory_snapshot).await?;
    // A large library or sleeping external drive must not serialize recorder
    // controls behind a full directory walk.
    tauri::async_runtime::spawn_blocking(move || scan_clips(&directory))
        .await.map_err(|e| e.to_string())?
}

#[tauri::command]
pub fn begin_clip_catalog(window: tauri::WebviewWindow, catalog: tauri::State<'_, crate::clip_index::CatalogState>, owner: String) -> Result<()> {
    if window.label() != "main" { return Err("clips.main_window_required".into()); }
    catalog.register(&owner)
}

#[tauri::command]
pub async fn list_clip_page(window: tauri::WebviewWindow, state: tauri::State<'_, HostState>, catalog: tauri::State<'_, crate::clip_index::CatalogState>, request: crate::clip_index::PageRequest) -> Result<crate::clip_index::ClipPage> {
    if window.label() != "main" { return Err("clips.main_window_required".into()); }
    request.validate()?;
    let ticket = catalog.begin(&request.owner)?;
    let directory = dispatch(&state, clip_directory_snapshot).await?;
    let permit = catalog.scan_permit()?;
    tauri::async_runtime::spawn_blocking(move || {
        let _permit = permit;
        ticket.check()?;
        let existing = if request.refresh { None } else { ticket.existing(&directory)? };
        let index = match existing {
            Some(index) => index,
            None => {
                let clips = crate::clip_catalog::scan_clips_checked(&directory, || ticket.check(), 100_000, 16 * 1024 * 1024)?;
                let index = Arc::new(clips);
                ticket.publish(directory, index.clone())?;
                index
            }
        };
        let result = crate::clip_index::page(&index, &request)?;
        ticket.check()?;
        Ok(result)
    }).await.map_err(|e| e.to_string())?
}

#[tauri::command]
pub fn release_clip_catalog(window: tauri::WebviewWindow, catalog: tauri::State<'_, crate::clip_index::CatalogState>, owner: String) -> Result<()> {
    if window.label() != "main" { return Err("clips.main_window_required".into()); }
    catalog.release(&owner)
}

fn clip_directory_snapshot(host: &mut Host) -> Result<PathBuf> {
    // Reading the saved path must not touch a potentially disconnected drive
    // while the recorder state is locked. scan_clips validates it off-lock.
    host.store.load().map_err(|e| e.to_string())?.active
        .map(|active| active.output_directory)
        .ok_or_else(|| "config.onboarding_required".into())
}

fn resolve_clip(directory: &Path, name: &str) -> Result<PathBuf> {
    let path = resolve_library_file(directory, name)?;
    if !is_clip(&path) { return Err("clips.invalid_path".into()); }
    if recovery_extension(&path).is_some() && !is_orphan_recording(&path) { return Err("clips.recording_in_progress".into()); }
    Ok(path)
}

pub(crate) async fn editor_paths(state: &HostState, name: Option<String>) -> Result<(PathBuf, Option<PathBuf>)> {
    dispatch(state, move |host| Ok((host.executable.clone()?, match name { Some(name) => Some(resolve_clip(&clip_directory(host)?, &name)?), None => None }))).await
}

fn resolve_library_file(directory: &Path, name: &str) -> Result<PathBuf> {
    let parts: Vec<_> = name.split(['/', '\\']).collect();
    if !(1..=2).contains(&parts.len()) || parts.iter().any(|part| {
        part.is_empty() || *part == "." || *part == ".." || part.trim() != *part
            || part.ends_with('.') || part.chars().any(|c| c.is_control() || "<>:\"|?*".contains(c))
    }) { return Err("clips.invalid_name".into()); }
    let directory = std::fs::canonicalize(directory).map_err(|e| e.to_string())?;
    let mut path = directory.clone();
    for (index, part) in parts.iter().enumerate() {
        path.push(part);
        let metadata = std::fs::symlink_metadata(&path).map_err(|e| e.to_string())?;
        if is_reparse(&metadata) || (index + 1 < parts.len() && !metadata.is_dir()) {
            return Err("clips.invalid_path".into());
        }
        let canonical = std::fs::canonicalize(&path).map_err(|e| e.to_string())?;
        if canonical.parent() != path.parent() || !canonical.starts_with(&directory) {
            return Err("clips.invalid_path".into());
        }
        path = canonical;
    }
    validate_owned_file(&path, std::slice::from_ref(&path))
}

fn read_folder_icon(directory: &Path, folder: &str) -> Result<Vec<u8>> {
    use std::io::Read;
    if folder.contains(['/', '\\']) || folder == "Desktop" { return Err("clips.icon_unavailable".into()); }
    let path = resolve_library_file(directory, &format!("{folder}/.game-icon.ico"))?;
    let file = std::fs::File::open(path).map_err(|e| e.to_string())?;
    let mut bytes = Vec::new();
    file.take(2 * 1024 * 1024 + 23).read_to_end(&mut bytes).map_err(|e| e.to_string())?;
    if bytes.len() < 22 || bytes.len() > 2 * 1024 * 1024 + 22 || bytes[..6] != [0, 0, 1, 0, 1, 0] {
        return Err("clips.icon_invalid".into());
    }
    let size = u32::from_le_bytes(bytes[14..18].try_into().unwrap()) as usize;
    let offset = u32::from_le_bytes(bytes[18..22].try_into().unwrap()) as usize;
    if offset != 22 || size == 0 || size != bytes.len() - offset { return Err("clips.icon_invalid".into()); }
    Ok(bytes)
}

#[tauri::command]
pub async fn folder_icon(state: tauri::State<'_, HostState>, folder: String) -> Result<Vec<u8>> {
    let directory = dispatch(&state, |host| clip_directory(host)).await?;
    tauri::async_runtime::spawn_blocking(move || read_folder_icon(&directory, &folder))
        .await.map_err(|e| e.to_string())?
}

#[tauri::command]
pub async fn open_clip(state: tauri::State<'_, HostState>, name: String) -> Result<()> {
    dispatch(&state, move |host| {
        let directory = clip_directory(host)?;
        let path = resolve_clip(&directory, &name)?;
        native::open_file(&path)
    }).await
}

#[derive(Default)]
pub struct PlaybackState(Mutex<std::collections::HashMap<String, tempfile::TempDir>>);

#[tauri::command]
pub async fn prepare_clip_playback(app: tauri::AppHandle, state: tauri::State<'_, HostState>, name: String) -> Result<serde_json::Value> {
    use tauri::Manager;
    let (source, executable) = dispatch(&state, move |host| {
        Ok((resolve_clip(&clip_directory(host)?, &name)?, host.executable.clone()?))
    }).await?;
    let (directory, mut media) = tauri::async_runtime::spawn_blocking(move || -> Result<_> {
        use std::os::windows::process::CommandExt;
        let directory = tempfile::Builder::new().prefix("rebelliocap-playback-").tempdir().map_err(|e| e.to_string())?;
        let output = std::process::Command::new(executable)
            .arg("--prepare-playback").arg(source).arg(directory.path())
            .creation_flags(0x08000000).output().map_err(|e| e.to_string())?;
        if !output.status.success() { return Err(String::from_utf8_lossy(&output.stderr).into_owned()); }
        let media: serde_json::Value = serde_json::from_slice(&output.stdout).map_err(|e| e.to_string())?;
        Ok((directory, media))
    }).await.map_err(|e| e.to_string())??;
    let scope = app.asset_protocol_scope();
    let mut allowed = Vec::new();
    let result = (|| -> Result<()> {
        let video = directory.path().join("video.mp4");
        scope.allow_file(&video).map_err(|e| e.to_string())?;
        allowed.push(video.clone());
        media["video"] = serde_json::json!(video);
        let tracks = media["tracks"].as_array_mut().ok_or("clips.playback_invalid")?;
        for track in tracks {
            let file = track["path"].as_str().ok_or("clips.playback_invalid")?;
            if !file.starts_with("audio-") || file.contains(['/', '\\']) || !file.ends_with(".m4a") { return Err("clips.playback_invalid".into()); }
            let path = directory.path().join(file);
            scope.allow_file(&path).map_err(|e| e.to_string())?;
            allowed.push(path.clone());
            track["path"] = serde_json::json!(path);
            let mixed = track["label"].as_str() == Some("Системный звук + микрофон");
            track["enabled"] = serde_json::json!(!mixed);
        }
        Ok(())
    })();
    if let Err(error) = result {
        for path in allowed { let _ = scope.forbid_file(path); }
        return Err(error);
    }
    let id = directory.path().file_name().ok_or("clips.playback_invalid")?.to_string_lossy().into_owned();
    media["id"] = serde_json::json!(id);
    app.state::<PlaybackState>().0.lock().map_err(|e| e.to_string())?.insert(id, directory);
    Ok(media)
}

#[tauri::command]
pub fn release_clip_playback(app: tauri::AppHandle, state: tauri::State<'_, PlaybackState>, id: String) -> Result<()> {
    use tauri::Manager;
    if let Some(directory) = state.0.lock().map_err(|e| e.to_string())?.remove(&id) {
        if let Ok(entries) = std::fs::read_dir(directory.path()) {
            for entry in entries.flatten() { let _ = app.asset_protocol_scope().forbid_file(entry.path()); }
        }
        directory.close().map_err(|e| e.to_string())?;
    }
    Ok(())
}

#[tauri::command]
pub async fn clip_thumbnail(state: tauri::State<'_, HostState>, name: String) -> Result<Vec<u8>> {
    let path = dispatch(&state, move |host| resolve_clip(&clip_directory(host)?, &name)).await?;
    // Thumbnail providers can be slow: keep them outside the recording-state lock.
    tauri::async_runtime::spawn_blocking(move || native::thumbnails::extract(&path))
        .await.map_err(|e| e.to_string())?
}

fn renamed_clip_path(directory: &Path, original: &Path, new_name: &str) -> Result<PathBuf> {
    if new_name.trim() != new_name || new_name.is_empty() || new_name.ends_with('.')
        || new_name.chars().any(|c| c.is_control() || "<>:\"/\\|?*".contains(c)) {
        return Err("Недопустимое имя файла.".into());
    }
    let device = new_name.split('.').next().unwrap_or("").to_ascii_uppercase();
    if matches!(device.as_str(), "CON" | "PRN" | "AUX" | "NUL")
        || (device.len() == 4 && (device.starts_with("COM") || device.starts_with("LPT")) && matches!(device.as_bytes()[3], b'1'..=b'9')) {
        return Err("Это имя зарезервировано Windows.".into());
    }
    let extension = recovery_extension(original).or_else(|| original.extension().and_then(|s| s.to_str()).map(str::to_owned)).ok_or("clips.invalid_path")?;
    let clean_directory = crate::model::strip_verbatim_prefix(directory);
    let parent = original.parent().filter(|parent| crate::model::strip_verbatim_prefix(parent).starts_with(&clean_directory)).ok_or("clips.invalid_path")?;
    let target = parent.join(format!("{new_name}.{extension}"));
    if target != original && target.exists() { return Err("Клип с таким именем уже существует.".into()); }
    Ok(target)
}

#[tauri::command]
pub async fn rename_clip(state: tauri::State<'_, HostState>, name: String, new_name: String) -> Result<()> {
    dispatch(&state, move |host| {
        let directory = clip_directory(host)?;
        let source = resolve_clip(&directory, &name)?;
        let target = renamed_clip_path(&directory, &source, &new_name)?;
        if target == source { return Ok(()); }
        std::fs::rename(source, target).map_err(|e| format!("Не удалось переименовать клип: {e}"))
    }).await
}

#[tauri::command]
pub async fn delete_clip(state: tauri::State<'_, HostState>, name: String) -> Result<()> {
    let path = dispatch(&state, move |host| resolve_clip(&clip_directory(host)?, &name)).await?;
    tauri::async_runtime::spawn_blocking(move || native::recycle_file(&path)).await.map_err(|e| e.to_string())?
}

#[cfg(test)]
mod clips_tests {
    use super::*;

    #[test]
    fn library_snapshot_does_not_access_an_unavailable_output_directory() {
        let temp = tempfile::tempdir().unwrap();
        let store = ConfigStore::from_root_host(&temp.path().canonicalize().unwrap()).unwrap();
        let directory = store.root().join("disconnected-drive");
        let active = ActiveConfig {
            onboarding_completed: true,
            monitor_id: "monitor-1".into(),
            system_audio: AudioSelection::Disabled,
            microphone: AudioSelection::Disabled,
            width: 1920,
            height: 1080,
            fps: 60,
            bitrate: 30_000_000,
            replay_seconds: 30,
            replay_memory_limit_mb: 0,
            replay_mode: ReplayMode::Ram,
            container: Container::Mp4,
            output_directory: directory.clone(),
            save_replay_hotkey: Hotkey { key: 0x77, ctrl: false, alt: false, shift: false, win: false },
            toggle_recording_hotkey: Hotkey { key: 0x78, ctrl: false, alt: false, shift: false, win: false },
            preferences: Preferences { overlay_enabled: None, start_with_windows: false },
            continuous_recording_enabled: false,
        };
        store.replace_host_state(&StoredState {
            draft: active.as_draft(),
            active: Some(active),
            onboarding: OnboardingProgress { completed: true, last_completed_step: 6 },
            ..Default::default()
        }).unwrap();
        let state = HostState::new(store, temp.path());
        let snapshot = {
            let mut host = state.inner.lock().unwrap();
            clip_directory_snapshot(&mut host).unwrap()
        };
        assert_eq!(snapshot, directory);
        assert!(scan_clips(&snapshot).err().unwrap().starts_with("clips.folder_unavailable:"));
    }

    #[test]
    fn tray_status_uses_memory_when_config_storage_is_unavailable() {
        let temp = tempfile::tempdir().unwrap();
        let store = ConfigStore::from_root_host(&temp.path().canonicalize().unwrap()).unwrap();
        let state = HostState::new(store, temp.path());
        let lock = temp.path().join("config.lock");
        if lock.is_file() { std::fs::remove_file(&lock).unwrap(); }
        std::fs::create_dir(&lock).unwrap();
        let (snapshot, enabled) = state.tray_status().unwrap();
        assert!(!snapshot.replay_active);
        assert!(!enabled);
    }

    #[test]
    fn folder_icons_are_bounded_and_cannot_escape_the_library() {
        let temp = tempfile::tempdir().unwrap();
        let directory = std::fs::canonicalize(temp.path()).unwrap();
        std::fs::create_dir(directory.join("Game")).unwrap();
        let icon = directory.join("Game/.game-icon.ico");
        let mut bytes = vec![0u8; 23];
        bytes[..6].copy_from_slice(&[0, 0, 1, 0, 1, 0]);
        bytes[14..18].copy_from_slice(&1u32.to_le_bytes());
        bytes[18..22].copy_from_slice(&22u32.to_le_bytes());
        std::fs::write(&icon, &bytes).unwrap();
        assert_eq!(read_folder_icon(&directory, "Game").unwrap(), bytes);
        for name in ["../Game", "Game/..", "", "C:\\Game", "Desktop", ".", ".."] {
            assert!(read_folder_icon(&directory, name).is_err(), "{name}");
        }
        bytes[18] = 0;
        std::fs::write(&icon, &bytes).unwrap();
        assert!(read_folder_icon(&directory, "Game").is_err());
        std::fs::write(&icon, vec![0u8; 2 * 1024 * 1024 + 23]).unwrap();
        assert!(read_folder_icon(&directory, "Game").is_err());
    }

    #[test]
    fn rename_keeps_extension_and_rejects_invalid_or_existing_names() {
        let temp = tempfile::tempdir().unwrap();
        let directory = std::fs::canonicalize(temp.path()).unwrap();
        let source = directory.join("clip.mp4");
        std::fs::write(&source, b"video").unwrap();
        std::fs::write(directory.join("taken.mp4"), b"video").unwrap();
        for name in ["../outside", "CON", "COM1", "bad:", "", "trailing.", "taken"] {
            assert!(renamed_clip_path(&directory, &source, name).is_err());
        }
        assert_eq!(renamed_clip_path(&directory, &source, "Новое имя").unwrap(), directory.join("Новое имя.mp4"));
    }

    #[test]
    fn catalog_only_contains_video_files_from_the_configured_directory() {
        let temp = tempfile::tempdir().unwrap();
        let directory = std::fs::canonicalize(temp.path()).unwrap();
        std::fs::write(directory.join("clip.mp4"), b"video").unwrap();
        std::fs::write(directory.join("capture.MKV"), b"recording").unwrap();
        std::fs::write(directory.join("notes.txt"), b"notes").unwrap();
        std::fs::create_dir(directory.join("folder.mp4")).unwrap();
        let clips = scan_clips(&directory).unwrap();
        assert_eq!(clips.len(), 2);
        assert!(clips.iter().any(|clip| clip.name == "clip.mp4" && clip.bytes == 5));
        assert!(clips.iter().any(|clip| clip.name == "capture.MKV"));
    }

    #[test]
    fn catalog_finds_orphan_recordings_without_exposing_live_or_unrelated_partials() {
        use std::os::windows::fs::OpenOptionsExt;
        let temp = tempfile::tempdir().unwrap();
        let directory = std::fs::canonicalize(temp.path()).unwrap();
        let orphan = directory.join("recovered.mp4.partial");
        let live = directory.join("live.mkv.partial");
        std::fs::write(&orphan, b"media").unwrap();
        std::fs::write(&live, b"media").unwrap();
        std::fs::write(directory.join("empty.mp4.partial"), b"").unwrap();
        std::fs::write(directory.join("unrelated.partial"), b"data").unwrap();
        let writer = std::fs::OpenOptions::new().read(true).write(true).share_mode(1).open(&live).unwrap();
        let clips = scan_clips(&directory).unwrap();
        assert_eq!(clips.len(), 1);
        assert_eq!(clips[0].name, "recovered.mp4.partial");
        assert_eq!(serde_json::to_value(&clips[0]).unwrap()["recovery"], true);
        assert!(resolve_clip(&directory, "recovered.mp4.partial").is_ok());
        assert!(resolve_clip(&directory, "live.mkv.partial").is_err());
        let renamed = renamed_clip_path(&directory, &orphan, "saved").unwrap();
        assert_eq!(renamed.file_name().unwrap(), "saved.mp4.partial");
        drop(writer);
        assert_eq!(scan_clips(&directory).unwrap().len(), 2);
        assert_eq!(std::fs::read(orphan).unwrap(), b"media");
    }

    #[test]
    fn category_catalog_preserves_identity_and_legacy_clips() {
        let temp = tempfile::tempdir().unwrap();
        let directory = std::fs::canonicalize(temp.path()).unwrap();
        for folder in ["Desktop", "Game"] {
            std::fs::create_dir(directory.join(folder)).unwrap();
            std::fs::write(directory.join(folder).join("clip.mp4"), b"video").unwrap();
        }
        std::fs::write(directory.join("legacy.mp4"), b"video").unwrap();
        std::fs::create_dir(directory.join("Game/nested")).unwrap();
        std::fs::write(directory.join("Game/nested/hidden.mp4"), b"video").unwrap();
        let clips = scan_clips(&directory).unwrap();
        assert_eq!(clips.len(), 3);
        assert!(clips.iter().any(|clip| clip.relative_path == "legacy.mp4" && clip.folder == "Desktop"));
        let game = resolve_clip(&directory, "Game/clip.mp4").unwrap();
        assert_ne!(game, resolve_clip(&directory, "Desktop/clip.mp4").unwrap());
        let renamed = renamed_clip_path(&directory, &game, "new").unwrap();
        assert_eq!(renamed, crate::model::strip_verbatim_prefix(&directory.join("Game/new.mp4")));
        std::fs::rename(game, renamed).unwrap();
        assert!(resolve_clip(&directory, "Game/new.mp4").is_ok());
        let json = serde_json::to_value(&clips[0]).unwrap();
        assert!(json.get("relativePath").is_some());
        assert!(json.get("savedMs").is_some());
        assert!(clips.windows(2).all(|pair| pair[0].saved_ms >= pair[1].saved_ms));
        for name in ["Game/../legacy.mp4", "Game/nested/hidden.mp4", "Game//clip.mp4", "/legacy.mp4", "Game/clip.mp4:stream", "./legacy.mp4"] {
            assert!(resolve_clip(&directory, name).is_err(), "{name}");
        }
    }

    #[test]
    fn category_catalog_rejects_junctions() {
        let temp = tempfile::tempdir().unwrap();
        let outside = tempfile::tempdir().unwrap();
        let directory = std::fs::canonicalize(temp.path()).unwrap();
        std::fs::write(outside.path().join("clip.mp4"), b"video").unwrap();
        let link = directory.join("Linked");
        let result = std::process::Command::new("cmd").args(["/C", "mklink", "/J"])
            .arg(&link).arg(outside.path()).output().unwrap();
        assert!(result.status.success(), "{}", String::from_utf8_lossy(&result.stderr));
        assert!(scan_clips(&directory).unwrap().is_empty());
        assert!(resolve_clip(&directory, "Linked/clip.mp4").is_err());
        assert!(read_folder_icon(&directory, "Linked").is_err());
        std::fs::remove_dir(link).unwrap();
    }

    #[test]
    fn open_rejects_paths_and_nonvideo_files() {
        let temp = tempfile::tempdir().unwrap();
        let directory = std::fs::canonicalize(temp.path()).unwrap();
        std::fs::write(directory.join("clip.mp4"), b"video").unwrap();
        std::fs::write(directory.join("app.exe"), b"program").unwrap();
        for name in ["../clip.mp4", "C:\\clip.mp4", "app.exe", "", "sub/clip.mp4"] {
            assert!(resolve_clip(&directory, name).is_err(), "{name}");
        }
        assert!(resolve_clip(&directory, "clip.mp4").is_ok());
    }
}
