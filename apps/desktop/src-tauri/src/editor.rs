//! Local non-destructive editor. Encoding runs outside the recorder process.
use serde_json::{json, Value};
use std::{
    collections::{HashMap, HashSet},
    io::{BufRead, BufReader, Read, Write},
    os::windows::process::CommandExt,
    path::{Path, PathBuf},
    process::{Command, Stdio},
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        Arc, Mutex,
    },
};
use tauri::Manager;
use crate::editor_import_progress::Phase;
type Result<T> = std::result::Result<T, String>;
#[derive(Default)]
pub struct EditorState {
    temps: Mutex<Vec<tempfile::TempDir>>,
    status: Arc<Mutex<Value>>,
    cancel: Arc<AtomicBool>,
    running: AtomicBool,
    updating: AtomicBool,
    import_epoch: Arc<AtomicU64>,
    owners: Mutex<HashMap<String, PreviewOwner>>,
    remux_cache: Arc<Mutex<crate::editor_remux_cache::RemuxCache>>,
}
#[derive(Default)]
struct PreviewOwner { epoch: Arc<AtomicU64>, directories: HashSet<PathBuf> }
pub(crate) fn begin_owner(state: &EditorState, owner: &str) -> Result<()> {
    if owner.is_empty() || owner.len() > 128 { return Err("editor.invalid_owner".into()); }
    let mut owners = state.owners.lock().map_err(|e| e.to_string())?;
    if state.updating.load(Ordering::Acquire) { return Err("Идёт обновление приложения. Редактор временно недоступен.".into()); }
    if owners.contains_key(owner) { return Ok(()); }
    if owners.len() >= 32 { return Err("editor.too_many_sessions".into()); }
    owners.insert(owner.to_owned(), PreviewOwner::default());
    Ok(())
}
#[tauri::command]
pub fn editor_begin_session(window: tauri::WebviewWindow, state: tauri::State<'_, EditorState>, owner: String) -> Result<()> {
    main_only(&window)?;
    begin_owner(&state, &owner)
}
impl EditorState {
    pub(crate) fn reserve_update(&self) -> Result<EditorUpdateLease<'_>> {
        let owners = self.owners.lock().map_err(|e| e.to_string())?;
        if !owners.is_empty() || self.running.load(Ordering::Acquire) {
            return Err("Перед обновлением сохраните изменения и закройте редактор. Дождитесь завершения экспорта.".into());
        }
        self.updating.compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .map_err(|_| "Обновление уже выполняется.")?;
        Ok(EditorUpdateLease(self))
    }
    pub fn shutdown(&self) {
        self.cancel.store(true, Ordering::Release);
        self.import_epoch.fetch_add(1, Ordering::AcqRel);
    }
}
pub(crate) struct EditorUpdateLease<'a>(&'a EditorState);
impl Drop for EditorUpdateLease<'_> {
    fn drop(&mut self) { self.0.updating.store(false, Ordering::Release); }
}
#[derive(Clone)]
struct ImportSession { epoch: Arc<AtomicU64>, expected: u64, deadline: std::time::Instant, owner: Option<String>, shutdown_epoch: Arc<AtomicU64>, shutdown_expected: u64, remux_cache: Arc<Mutex<crate::editor_remux_cache::RemuxCache>>, progress: crate::editor_import_progress::Reporter }
impl ImportSession {
    fn with_progress(mut self, app: &tauri::AppHandle, operation: Option<String>) -> Result<Self> {
        if let Some(operation) = operation {
            let app = app.clone();
            self.progress = crate::editor_import_progress::Reporter::new(self.owner.clone().unwrap_or_default(), operation, Arc::new(move |event| {
                use tauri::Emitter;
                if let Err(error) = app.emit_to("main", "editor-import-progress", event) {
                    crate::logging::record("warn", "editor", "import_progress_failed", json!({"error":error.to_string()}));
                }
            }))?;
        }
        Ok(self)
    }
    fn for_asset(&self, completed: usize, total: usize, path: &Path) -> Self {
        Self { progress: self.progress.asset(completed, total, path), ..self.clone() }
    }
    fn report(&self, phase: Phase) {
        if self.check().is_ok() { self.progress.report(phase); }
    }
    #[cfg(test)]
    fn new(state: &EditorState) -> Self { Self { epoch: state.import_epoch.clone(), expected: state.import_epoch.load(Ordering::Acquire), deadline: std::time::Instant::now() + std::time::Duration::from_secs(300), owner: None, shutdown_epoch: state.import_epoch.clone(), shutdown_expected: state.import_epoch.load(Ordering::Acquire), remux_cache: state.remux_cache.clone(), progress: Default::default() } }
    fn new_owned(state: &EditorState, owner: &str) -> Result<Self> {
        let owners = state.owners.lock().map_err(|e| e.to_string())?;
        let record = owners.get(owner).ok_or("editor.operation_cancelled")?;
        Ok(Self { epoch: record.epoch.clone(), expected: record.epoch.load(Ordering::Acquire), deadline: std::time::Instant::now() + std::time::Duration::from_secs(300), owner: Some(owner.to_owned()), shutdown_epoch: state.import_epoch.clone(), shutdown_expected: state.import_epoch.load(Ordering::Acquire), remux_cache: state.remux_cache.clone(), progress: Default::default() })
    }
    fn begin_work(&self) -> Self { let mut session = self.clone(); session.deadline = std::time::Instant::now() + std::time::Duration::from_secs(300); session }
    fn remaining(&self, limit: std::time::Duration) -> Result<std::time::Duration> {
        self.check()?;
        let remaining = self.deadline.saturating_duration_since(std::time::Instant::now());
        if remaining.is_zero() { return Err("editor.operation_timeout".into()); }
        Ok(limit.min(remaining))
    }
    fn cancelled(&self) -> bool { self.epoch.load(Ordering::Acquire) != self.expected || self.shutdown_epoch.load(Ordering::Acquire) != self.shutdown_expected }
    fn check(&self) -> Result<()> {
        if self.cancelled() { Err("editor.operation_cancelled".into()) }
        else if std::time::Instant::now() >= self.deadline { Err("editor.operation_timeout".into()) }
        else { Ok(()) }
    }
}
// A whole import/open/restore owns its previews until its result is complete.
// Dropping a failed batch removes every directory prepared by that operation.
#[derive(Default)]
struct PendingPreviews(Vec<tempfile::TempDir>, Vec<PathBuf>);
impl PendingPreviews {
    #[cfg(test)]
    fn commit(self, state: &EditorState, session: &ImportSession) -> Result<()> {
        self.commit_scoped(state, session, |_| Ok(()), |_| {})
    }
    fn publish(self, app: &tauri::AppHandle, session: &ImportSession) -> Result<()> {
        let scope = app.asset_protocol_scope();
        self.commit_scoped(&app.state::<EditorState>(), session,
            |path| scope.allow_file(path).map_err(|e| e.to_string()),
            |path| {
                if let Err(error) = scope.forbid_file(path) {
                    crate::logging::record("error", "editor", "preview_scope_rollback_failed",
                        json!({"error":error.to_string()}));
                }
            })
    }
    fn commit_scoped(mut self, state: &EditorState, session: &ImportSession,
        mut allow: impl FnMut(&Path) -> Result<()>, mut forbid: impl FnMut(&Path)) -> Result<()> {
        // Cancellation and release use the same lock: a cancelled batch cannot
        // publish directories after either command has advanced the epoch.
        let mut temps = state.temps.lock().map_err(|e| e.to_string())?;
        session.check()?;
        // Only unique, operation-owned previews may be revoked. Forbidding
        // an original would permanently block later imports in Tauri's scope.
        if self.1.iter().any(|path| !self.0.iter().any(|dir| path.parent() == Some(dir.path()))) {
            return Err("editor.preview_scope_invalid".into());
        }
        let mut owners = state.owners.lock().map_err(|e| e.to_string())?;
        let record = match &session.owner {
            Some(owner) => Some(owners.get_mut(owner).ok_or("editor.operation_cancelled")?),
            None => None,
        };
        let mut granted = Vec::new();
        let result = (|| {
            for path in &self.1 {
                session.check()?;
                allow(path)?;
                granted.push(path);
            }
            session.check()
        })();
        if let Err(error) = result {
            for path in granted { forbid(path); }
            return Err(error);
        }
        if let Some(record) = record {
            record.directories.extend(self.0.iter().map(|dir| dir.path().to_owned()));
        }
        temps.append(&mut self.0);
        Ok(())
    }
}
fn main_only(window: &tauri::WebviewWindow) -> Result<()> {
    if window.label() == "main" {
        Ok(())
    } else {
        Err("editor.main_window_required".into())
    }
}
fn number(v: &Value, key: &str, default: f64) -> f64 {
    v[key].as_f64().filter(|n| n.is_finite()).unwrap_or(default)
}
fn text<'a>(v: &'a Value, key: &str) -> &'a str {
    v[key].as_str().unwrap_or("")
}
fn items(p: &Value) -> Result<&Vec<Value>> {
    p["items"]
        .as_array()
        .ok_or("Некорректная шкала монтажа".into())
}
fn duration(p: &Value) -> Result<f64> {
    Ok(items(p)?
        .iter()
        .map(|i| number(i, "start", 0.) + number(i, "duration", 0.))
        .fold(0., f64::max))
}
fn validate(p: &Value) -> Result<()> {
    if p["version"] != 1
        || !(16. ..=7680.).contains(&number(p, "width", 0.))
        || !(16. ..=7680.).contains(&number(p, "height", 0.))
        || !(1. ..=240.).contains(&number(p, "fps", 0.))
    {
        return Err("Неподдерживаемый формат проекта".into());
    }
    if items(p)?.len() > 500 || p["assets"].as_array().ok_or("Нет списка исходников")?.len() > 200
    {
        return Err("Проект превышает ограничения MVP (500 фрагментов, 200 исходников)".into());
    }
    for i in items(p)? {
        if number(i, "start", -1.) < 0.
            || number(i, "duration", 0.) < 0.01
            || number(i, "source", -1.) < 0.
        {
            return Err("Некорректная длительность фрагмента".into());
        }
    }
    Ok(())
}
fn dialog(save: bool, folder: bool, name: &str, extension: &str) -> Result<Option<PathBuf>> {
    use windows::{
        core::HSTRING,
        Win32::{System::Com::*, UI::Shell::*},
    };
    unsafe {
        CoInitializeEx(None, COINIT_APARTMENTTHREADED)
            .ok()
            .map_err(|e| e.to_string())?;
    }
    struct Apartment;
    impl Drop for Apartment {
        fn drop(&mut self) {
            unsafe { windows::Win32::System::Com::CoUninitialize() }
        }
    }
    let _apartment = Apartment;
    unsafe {
        let d: IFileDialog = if save {
            let d: IFileSaveDialog = CoCreateInstance(&FileSaveDialog, None, CLSCTX_INPROC_SERVER)
                .map_err(|e| e.to_string())?;
            windows::core::Interface::cast(&d).map_err(|e| e.to_string())?
        } else {
            let d: IFileOpenDialog = CoCreateInstance(&FileOpenDialog, None, CLSCTX_INPROC_SERVER)
                .map_err(|e| e.to_string())?;
            windows::core::Interface::cast(&d).map_err(|e| e.to_string())?
        };
        let mut options = FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST;
        if folder {
            options |= FOS_PICKFOLDERS;
        } else if save {
            options |= FOS_OVERWRITEPROMPT;
        } else {
            options |= FOS_FILEMUSTEXIST;
        }
        d.SetOptions(options).map_err(|e| e.to_string())?;
        d.SetTitle(&HSTRING::from(if folder {
            "Упаковать проект в папку"
        } else if save {
            "Сохранить"
        } else {
            "Открыть видео, аудио, изображение или проект"
        }))
        .map_err(|e| e.to_string())?;
        if save {
            d.SetFileName(&HSTRING::from(name))
                .map_err(|e| e.to_string())?;
            d.SetDefaultExtension(&HSTRING::from(extension))
                .map_err(|e| e.to_string())?;
        }
        if let Err(e) = d.Show(None) {
            if e.code().0 as u32 == 0x800704c7 {
                return Ok(None);
            }
            return Err(e.to_string());
        }
        let value = d
            .GetResult()
            .map_err(|e| e.to_string())?
            .GetDisplayName(SIGDN_FILESYSPATH)
            .map_err(|e| e.to_string())?;
        let path = value.to_string().map_err(|e| e.to_string());
        CoTaskMemFree(Some(value.0 as *const _));
        Ok(Some(PathBuf::from(path?)))
    }
}
fn command(path: &Path) -> Command {
    let mut c = Command::new(path);
    c.creation_flags(0x08000000);
    c
}
#[cfg(test)]
fn probe(engine: &Path, path: &Path) -> Result<Value> { probe_session(engine, path, None) }
fn probe_session(engine: &Path, path: &Path, session: Option<&ImportSession>) -> Result<Value> {
    if let Some(session) = session { session.report(Phase::Analyzing); }
    let output = crate::editor_process::run(command(&engine.with_file_name("ffprobe.exe"))
        .args([
            "-v",
            "error",
            "-show_streams",
            "-show_format",
            "-of",
            "json",
        ])
        .arg(path), match session { Some(session) => session.remaining(std::time::Duration::from_secs(30))?, None => std::time::Duration::from_secs(30) }, || session.is_some_and(ImportSession::cancelled))?;
    if !output.status.success() {
        return Err(String::from_utf8_lossy(&output.stderr).into_owned());
    }
    serde_json::from_slice(&output.stdout).map_err(|e| e.to_string())
}
fn hydrate(engine: &Path, mut asset: Value, session: &ImportSession, previews: &mut PendingPreviews) -> Result<Value> {
    session.check()?;
    if let Some(object) = asset.as_object_mut() { object.remove("previewDirectory"); }
    let path = PathBuf::from(text(&asset, "path"));
    if !path.is_file() {
        asset["missing"] = json!(true);
        return Ok(asset);
    }
    let kind = text(&asset, "kind");
    if kind == "video" {
        session.report(Phase::CheckingCache);
        // Files with a live writer or unsupported identity metadata still import,
        // but cannot populate or hit the immutable source cache.
        let identity = crate::editor_remux_cache::source_identity_checked(&path, || session.check()).ok();
        session.check()?;
        let key = identity.as_ref().map(|(key, _)| key.as_str());
        let cached = match key {
            Some(key) => session.remux_cache.lock().map_err(|e| e.to_string())?.get(key),
            None => None,
        };
        let (dir, media) = if let Some(cached) = cached {
            session.report(Phase::PreparingPreview);
            cached.alias(|| session.check())?
        } else {
        session.report(Phase::PreparingPreview);
        let dir = tempfile::Builder::new()
            .prefix("rebcap-editor-")
            .tempdir()
            .map_err(|e| e.to_string())?;
        let output = crate::editor_process::run(command(engine)
            .arg("--prepare-playback")
            .arg(&path)
            .arg(dir.path()), session.remaining(std::time::Duration::from_secs(300))?, || session.cancelled())?;
        if !output.status.success() {
            return Err(format!(
                "Не удалось подготовить превью: {}",
                String::from_utf8_lossy(&output.stderr)
            ));
        }
        session.check()?;
        let media: Value = serde_json::from_slice(&output.stdout).map_err(|e| e.to_string())?;
        let prepared = Arc::new(crate::editor_remux_cache::PreparedMedia::new(dir, media)?);
        let alias = prepared.alias(|| session.check())?;
        if let Some(key) = key {
            session.check()?;
            session.remux_cache.lock().map_err(|e| e.to_string())?.insert_prepared(key.to_owned(), prepared);
        }
        alias
        };
        let video = dir.path().join("video.mp4");
        previews.1.push(video.clone());
        asset["url"] = json!(video);
        let mut urls = Vec::new();
        for track in media["tracks"]
            .as_array()
            .ok_or("Некорректные аудиодорожки")?
        {
            let file = text(track, "path");
            if file.contains(['/', '\\']) || !file.starts_with("audio-") {
                return Err("Некорректный путь превью".into());
            }
            let path = dir.path().join(file);
            previews.1.push(path.clone());
            urls.push(json!({"path":path,"label":track["label"],"enabled":track["label"]!=json!("Системный звук + микрофон")}));
        }
        asset["audioUrls"] = json!(urls);
        session.check()?;
        asset["previewDirectory"] = json!(dir.path());
        previews.0.push(dir);
    } else {
        session.report(Phase::PreparingPreview);
        let (dir, preview) = crate::editor_preview::prepare_source_preview(&path, || session.check())?;
        previews.1.push(preview.clone());
        asset["url"] = json!(preview);
        asset["previewDirectory"] = json!(dir.path());
        previews.0.push(dir);
    }
    session.check()?;
    Ok(asset)
}
fn import(engine: &Path, path: &Path, session: &ImportSession, previews: &mut PendingPreviews) -> Result<Value> {
    let path =
        crate::model::strip_verbatim_prefix(&path.canonicalize().map_err(|e| e.to_string())?);
    let session = session.for_asset(0, 1, &path);
    let data = probe_session(engine, &path, Some(&session))?;
    let streams = data["streams"]
        .as_array()
        .ok_or("Файл не содержит дорожек")?;
    let video = streams.iter().find(|s| s["codec_type"] == "video");
    let audio: Vec<i64> = streams
        .iter()
        .filter(|s| s["codec_type"] == "audio")
        .filter_map(|s| s["index"].as_i64())
        .collect();
    let seconds = data["format"]["duration"]
        .as_str()
        .and_then(|v| v.parse::<f64>().ok())
        .unwrap_or(5.);
    let ext = path
        .extension()
        .unwrap_or_default()
        .to_string_lossy()
        .to_lowercase();
    let image = ["png", "jpg", "jpeg", "bmp", "webp"].contains(&ext.as_str());
    if video.is_none() && audio.is_empty() {
        return Err("Неподдерживаемый медиафайл".into());
    }
    let asset = json!({"id":format!("asset-{}",std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos()),"name":path.file_name().unwrap_or_default().to_string_lossy(),"path":path,"kind":if image{"image"}else if video.is_some(){"video"}else{"audio"},"duration":seconds,"width":video.map(|s|number(s,"width",0.)).unwrap_or(0.),"height":video.map(|s|number(s,"height",0.)).unwrap_or(0.),"audio":audio});
    hydrate(engine, asset, &session, previews)
}
#[tauri::command]
pub async fn editor_import(
    window: tauri::WebviewWindow,
    app: tauri::AppHandle,
    state: tauri::State<'_, crate::commands::HostState>,
    name: Option<String>,
    owner: String,
    operation_id: Option<String>,
) -> Result<Option<Value>> {
    main_only(&window)?;
    let session = ImportSession::new_owned(&app.state::<EditorState>(), &owner)?.with_progress(&app, operation_id)?;
    let (engine, source) = crate::commands::editor_paths(&state, name).await?;
    tauri::async_runtime::spawn_blocking(move || {
        if source.is_none() { session.report(Phase::Selecting); }
        let source = match source {
            Some(p) => Some(p),
            None => dialog(false, false, "", "")?,
        };
        let session = session.begin_work();
        session.check()?;
        match source {
            Some(p) => {
                let mut previews = PendingPreviews::default();
                let asset = import(&engine, &p, &session, &mut previews)?;
                session.report(Phase::Publishing);
                previews.publish(&app, &session)?;
                Ok(Some(asset))
            },
            None => Ok(None),
        }
    })
    .await
    .map_err(|e| e.to_string())?
}
fn clean_project(mut p: Value) -> Value {
    if let Some(assets) = p["assets"].as_array_mut() {
        for a in assets {
            if let Some(o) = a.as_object_mut() {
                o.remove("url");
                o.remove("audioUrls");
                o.remove("previewDirectory");
                o.remove("missing");
            }
        }
    }
    p
}
fn write_project(path: &Path, p: &Value) -> Result<()> {
    let mut tmp = tempfile::NamedTempFile::new_in(path.parent().ok_or("Некорректная папка")?)
        .map_err(|e| e.to_string())?;
    tmp.write_all(&serde_json::to_vec_pretty(p).map_err(|e| e.to_string())?)
        .map_err(|e| e.to_string())?;
    tmp.as_file().sync_all().map_err(|e| e.to_string())?;
    tmp.persist(path).map_err(|e| e.to_string())?;
    Ok(())
}
fn package_project(folder: &Path, project: Value, name: &str) -> Result<PathBuf> {
    let mut p = clean_project(project);
    let dir = folder.join(format!(
        "{}-{}",
        name,
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    std::fs::create_dir(&dir).map_err(|e| e.to_string())?;
    let result = (|| {
        let assets = dir.join("assets");
        std::fs::create_dir(&assets).map_err(|e| e.to_string())?;
        let mut copies: std::collections::HashMap<PathBuf, String> =
            std::collections::HashMap::new();
        for (index, a) in p["assets"].as_array_mut().unwrap().iter_mut().enumerate() {
            let source = PathBuf::from(text(a, "path"))
                .canonicalize()
                .map_err(|e| format!("Исходник недоступен: {} ({e})", text(a, "path")))?;
            let file = if let Some(file) = copies.get(&source) {
                file.clone()
            } else {
                let file = format!(
                    "{index}-{}",
                    source.file_name().unwrap_or_default().to_string_lossy()
                );
                std::fs::copy(&source, assets.join(&file)).map_err(|e| e.to_string())?;
                copies.insert(source, file.clone());
                file
            };
            a["path"] = json!(format!("assets/{file}"));
        }
        let path = dir.join(format!("{name}.rebcap"));
        write_project(&path, &p)?;
        Ok::<_, String>(path)
    })();
    if result.is_err() {
        let _ = std::fs::remove_dir_all(&dir);
    }
    result
}
#[tauri::command]
pub async fn editor_save(
    window: tauri::WebviewWindow,
    project: Value,
    pack: bool,
) -> Result<Option<String>> {
    main_only(&window)?;
    validate(&project)?;
    tauri::async_runtime::spawn_blocking(move || {
        let p = clean_project(project);
        let name = text(&p, "name").replace(['/', '\\', ':', '*', '?', '"', '<', '>', '|'], "_");
        let target = if pack {
            let Some(folder) = dialog(false, true, "", "")? else {
                return Ok(None);
            };
            return package_project(&folder, p, &name)
                .map(|p| Some(p.to_string_lossy().into_owned()));
        } else {
            let Some(path) = dialog(true, false, &format!("{name}.rebcap"), "rebcap")? else {
                return Ok(None);
            };
            if path
                .extension()
                .is_none_or(|e| !e.to_string_lossy().eq_ignore_ascii_case("rebcap"))
            {
                return Err("Сохраните проект с расширением .rebcap".into());
            }
            path
        };
        write_project(&target, &p)?;
        Ok(Some(target.to_string_lossy().into_owned()))
    })
    .await
    .map_err(|e| e.to_string())?
}
#[tauri::command]
pub async fn editor_open(
    window: tauri::WebviewWindow,
    app: tauri::AppHandle,
    state: tauri::State<'_, crate::commands::HostState>,
    owner: String,
    operation_id: Option<String>,
) -> Result<Option<Value>> {
    main_only(&window)?;
    let session = ImportSession::new_owned(&app.state::<EditorState>(), &owner)?.with_progress(&app, operation_id)?;
    let (engine, _) = crate::commands::editor_paths(&state, None).await?;
    tauri::async_runtime::spawn_blocking(move || {
        session.report(Phase::Selecting);
        let Some(path) = dialog(false, false, "", "")? else {
            return Ok(None);
        };
        let session = session.begin_work();
        session.check()?;
        session.report(Phase::ReadingProject);
        if std::fs::metadata(&path).map_err(|e| e.to_string())?.len() > 10_000_000 {
            return Err("Слишком большой файл проекта".into());
        }
        let bytes = std::fs::read(&path).map_err(|e| e.to_string())?;
        let mut p: Value =
            serde_json::from_slice(&bytes).map_err(|e| format!("Ожидается проект .rebcap: {e}"))?;
        validate(&p)?;
        let mut previews = PendingPreviews::default();
        let total = p["assets"].as_array().unwrap().len();
        for (index, a) in p["assets"].as_array_mut().unwrap().iter_mut().enumerate() {
            let source = PathBuf::from(text(a, "path"));
            if source.is_relative() {
                a["path"] = json!(path.parent().unwrap().join(source));
            }
            let asset_session = session.for_asset(index, total, &PathBuf::from(text(a, "path")));
            *a = hydrate(&engine, a.clone(), &asset_session, &mut previews)?;
        }
        session.report(Phase::Publishing);
        previews.publish(&app, &session)?;
        Ok(Some(p))
    })
    .await
    .map_err(|e| e.to_string())?
}
#[tauri::command]
pub fn editor_status(
    window: tauri::WebviewWindow,
    state: tauri::State<'_, EditorState>,
) -> Result<Value> {
    main_only(&window)?;
    Ok(state.status.lock().map_err(|e| e.to_string())?.clone())
}
#[tauri::command]
pub async fn editor_restore(
    window: tauri::WebviewWindow,
    app: tauri::AppHandle,
    state: tauri::State<'_, crate::commands::HostState>,
    project: Value,
    owner: String,
    operation_id: Option<String>,
) -> Result<Value> {
    main_only(&window)?;
    validate(&project)?;
    let session = ImportSession::new_owned(&app.state::<EditorState>(), &owner)?.with_progress(&app, operation_id)?;
    let (engine, _) = crate::commands::editor_paths(&state, None).await?;
    tauri::async_runtime::spawn_blocking(move || {
        let session = session.begin_work();
        session.check()?;
        let mut p = project;
        let mut previews = PendingPreviews::default();
        let total = p["assets"].as_array().unwrap().len();
        for (index, a) in p["assets"].as_array_mut().unwrap().iter_mut().enumerate() {
            let asset_session = session.for_asset(index, total, &PathBuf::from(text(a, "path")));
            *a = hydrate(&engine, a.clone(), &asset_session, &mut previews)?;
        }
        session.report(Phase::Publishing);
        previews.publish(&app, &session)?;
        Ok(p)
    })
    .await
    .map_err(|e| e.to_string())?
}
#[tauri::command]
pub fn editor_cancel(
    window: tauri::WebviewWindow,
    state: tauri::State<'_, EditorState>,
) -> Result<()> {
    main_only(&window)?;
    state.cancel.store(true, Ordering::Release);
    Ok(())
}
#[tauri::command]
pub fn editor_cancel_import(window: tauri::WebviewWindow, state: tauri::State<'_, EditorState>, owner: String) -> Result<()> {
    main_only(&window)?;
    let _temps = state.temps.lock().map_err(|e| e.to_string())?;
    if let Some(record) = state.owners.lock().map_err(|e| e.to_string())?.get(&owner) {
        record.epoch.fetch_add(1, Ordering::AcqRel);
    }
    Ok(())
}
// Release explicit retired ownership only. A stale frontend snapshot cannot
// delete a newly published batch, and unowned paths never reach filesystem I/O.
fn release_previews(state: &EditorState, directories: &[PathBuf], mut forbid: impl FnMut(&Path) -> Result<()>) -> Result<()> {
    // Export reads original assets and its own raster/output TempDir, never
    // preview aliases; a different mount's export cannot block retirement.
    let mut temps = state.temps.lock().map_err(|e| e.to_string())?;
    let mut index = 0;
    while index < temps.len() {
        if !directories.iter().any(|path| path == temps[index].path()) { index += 1; continue; }
        for entry in std::fs::read_dir(temps[index].path()).map_err(|e| e.to_string())? {
            forbid(&entry.map_err(|e| e.to_string())?.path())?;
        }
        std::fs::remove_dir_all(temps[index].path()).map_err(|e| e.to_string())?;
        temps.remove(index);
    }
    Ok(())
}
#[tauri::command]
pub fn editor_release_previews(window: tauri::WebviewWindow, app: tauri::AppHandle,
    state: tauri::State<'_, EditorState>, directories: Vec<PathBuf>, owner: String) -> Result<()> {
    main_only(&window)?;
    let owned: Vec<_> = {
        let owners = state.owners.lock().map_err(|e| e.to_string())?;
        directories.into_iter().filter(|path| owners.get(&owner).is_some_and(|record| record.directories.contains(path))).collect()
    };
    release_previews(&state, &owned, |path| app.asset_protocol_scope().forbid_file(path).map_err(|e| e.to_string()))?;
    if let Some(record) = state.owners.lock().map_err(|e| e.to_string())?.get_mut(&owner) {
        for path in owned { record.directories.remove(&path); }
    }
    Ok(())
}
fn release_owner(state: &EditorState, owner: &str, mut forbid: impl FnMut(&Path) -> Result<()>) -> Result<()> {
    let mut temps = state.temps.lock().map_err(|e| e.to_string())?;
    let mut owners = state.owners.lock().map_err(|e| e.to_string())?;
    let Some(record) = owners.get_mut(owner) else { return Ok(()); };
    record.epoch.fetch_add(1, Ordering::AcqRel);
    let mut index = 0;
    while index < temps.len() {
        let path = temps[index].path().to_owned();
        if !record.directories.contains(&path) { index += 1; continue; }
        for entry in std::fs::read_dir(&path).map_err(|e| e.to_string())? {
            forbid(&entry.map_err(|e| e.to_string())?.path())?;
        }
        std::fs::remove_dir_all(&path).map_err(|e| e.to_string())?;
        temps.remove(index);
        record.directories.remove(&path);
    }
    owners.remove(owner);
    Ok(())
}
#[tauri::command]
pub fn editor_release(window: tauri::WebviewWindow, app: tauri::AppHandle,
    state: tauri::State<'_, EditorState>, owner: String) -> Result<()> {
    main_only(&window)?;
    release_owner(&state, &owner, |path| app.asset_protocol_scope().forbid_file(path).map_err(|e| e.to_string()))
}

fn filter_graph(
    project: &Value,
    plan: &Value,
    rasters: &[(String, PathBuf)],
) -> Result<(Vec<String>, String)> {
    let total = duration(project)?;
    let w = number(plan, "width", 1920.) as i64;
    let h = number(plan, "height", 1080.) as i64;
    let fps = number(plan, "fps", 60.);
    let mut args = Vec::new();
    let mut graph = vec![format!(
        "color=c=black:s={w}x{h}:r={fps}:d={total},format=rgba[base0]"
    )];
    let mut input = 0;
    let mut layer = 0;
    let mut audio = Vec::new();
    let mut timeline = items(project)?.clone();
    timeline.sort_by(|a, b| number(a, "track", 0.).total_cmp(&number(b, "track", 0.)));
    for i in timeline {
        let kind = text(&i, "kind");
        let start = number(&i, "start", 0.);
        let duration = number(&i, "duration", 0.);
        let source = number(&i, "source", 0.);
        let path = if kind == "text" {
            rasters
                .iter()
                .find(|(id, _)| id == text(&i, "id"))
                .map(|(_, p)| p.clone())
                .ok_or("Не подготовлен текстовый слой")?
        } else {
            let a = project["assets"]
                .as_array()
                .unwrap()
                .iter()
                .find(|a| text(a, "id") == text(&i, "asset"))
                .ok_or("Исходник фрагмента не найден")?;
            PathBuf::from(text(a, "path"))
        };
        if !path.is_file() {
            return Err(format!("Исходник недоступен: {}", path.display()));
        }
        if kind == "image" || kind == "text" {
            args.extend(["-loop".into(), "1".into()]);
        } else {
            args.extend(["-ss".into(), source.to_string()]);
        }
        args.extend([
            "-t".into(),
            duration.to_string(),
            "-i".into(),
            path.to_string_lossy().into_owned(),
        ]);
        if kind == "audio" {
            let stream = number(&i, "stream", 0.) as i64;
            let gain = number(&i, "gain", 1.).clamp(0., 4.);
            graph.push(format!("[{input}:{stream}]atrim=duration={duration},asetpts=PTS-STARTPTS,aresample=48000,aformat=sample_fmts=fltp:channel_layouts=stereo,volume={gain},adelay={}|{}[a{input}]",(start*1000.).round() as i64,(start*1000.).round() as i64));
            audio.push(format!("[a{input}]"));
        } else {
            let lw = (w as f64 * number(&i, "width", 1.))
                .round()
                .clamp(2., 7680.) as i64;
            let lh = (h as f64 * number(&i, "height", 1.))
                .round()
                .clamp(2., 7680.) as i64;
            let x = (w as f64 * number(&i, "x", 0.)).round() as i64;
            let y = (h as f64 * number(&i, "y", 0.)).round() as i64;
            let sizing = if text(&i, "fit") == "cover" {
                format!("scale={lw}:{lh}:force_original_aspect_ratio=increase,crop={lw}:{lh}")
            } else {
                format!("scale={lw}:{lh}:force_original_aspect_ratio=decrease,pad={lw}:{lh}:(ow-iw)/2:(oh-ih)/2:color=black@0")
            };
            let brightness = number(&i, "brightness", 0.).clamp(-1., 1.);
            let contrast = number(&i, "contrast", 1.).clamp(0., 3.);
            let saturation = number(&i, "saturation", 1.).clamp(0., 3.);
            let opacity = number(&i, "opacity", 1.).clamp(0., 1.);
            let curve = format!("'clip((val*{}-128)*{contrast}+128,0,255)'", 1. + brightness);
            let inverse = 1. - saturation;
            let color=format!("lutrgb=r={curve}:g={curve}:b={curve},colorchannelmixer=rr={}:rg={}:rb={}:gr={}:gg={}:gb={}:br={}:bg={}:bb={}:aa={opacity}",0.213+0.787*saturation,0.715*inverse,0.072*inverse,0.213*inverse,0.715+0.285*saturation,0.072*inverse,0.213*inverse,0.715*inverse,0.072+0.928*saturation);
            graph.push(format!("[{input}:v]trim=duration={duration},setpts=PTS-STARTPTS+{start}/TB,format=rgba,{sizing},setsar=1,{color}[v{input}]"));
            graph.push(format!("[base{layer}][v{input}]overlay=x={x}:y={y}:enable='between(t,{start},{})':eof_action=pass:repeatlast=0[base{}]",start+duration,layer+1));
            layer += 1;
        }
        input += 1;
    }
    graph.push(format!("[base{layer}]fps={fps},format=nv12[outv]"));
    graph.push(format!(
        "anullsrc=r=48000:cl=stereo,atrim=duration={total}[silence]"
    ));
    let labels = audio.join("");
    graph.push(format!("[silence]{labels}amix=inputs={}:duration=first:normalize=0,alimiter=limit=0.95:level=0[outa]",audio.len()+1));
    Ok((args, graph.join(";")))
}
fn run_encode(
    engine: &Path,
    args: &[String],
    graph: &str,
    plan: &Value,
    path: &Path,
    encoder: &str,
    rate: u64,
    status: &Arc<Mutex<Value>>,
    cancel: &AtomicBool,
    attempt: usize,
) -> Result<()> {
    let job = crate::engine::supervisor::Job::new()?;
    let mut cmd = command(&engine.with_file_name("ffmpeg.exe"));
    cmd.creation_flags(0x08000000 | 0x00000004);
    cmd.args([
        "-hide_banner",
        "-nostdin",
        "-y",
        "-loglevel",
        "error",
        "-filter_complex_threads",
        "1",
    ])
    .args(args)
    .args([
        "-filter_complex",
        graph,
        "-map",
        "[outv]",
        "-map",
        "[outa]",
        "-c:v",
        if encoder == "h264_mf_sw" {
            "h264_mf"
        } else {
            encoder
        },
    ]);
    if encoder == "h264_nvenc" {
        cmd.args([
            "-preset",
            "p5",
            "-rc",
            "vbr",
            "-b:v",
            &rate.to_string(),
            "-maxrate",
            &(rate * 3 / 2).to_string(),
            "-bufsize",
            &(rate * 2).to_string(),
        ]);
    } else {
        cmd.args([
            "-hw_encoding",
            if encoder == "h264_mf" { "1" } else { "0" },
            "-b:v",
            &rate.to_string(),
        ]);
    }
    cmd.args([
        "-c:a",
        "aac",
        "-b:a",
        "128k",
        "-ar",
        "48000",
        "-ac",
        "2",
        "-t",
        &number(plan, "duration", 0.).to_string(),
        "-movflags",
        "+faststart",
        "-progress",
        "pipe:1",
    ])
    .arg(path)
    .stdout(Stdio::piped())
    .stderr(Stdio::piped());
    let mut child = cmd
        .spawn()
        .map_err(|e| format!("Не удалось запустить рендер: {e}"))?;
    if let Err(error) = job.assign_and_resume(&child) {
        let _ = child.kill();
        let _ = child.wait();
        return Err(error);
    }
    let mut stderr = child.stderr.take().unwrap();
    let error_thread = std::thread::spawn(move || {
        let mut b = Vec::new();
        let mut chunk = [0u8; 8192];
        loop {
            match stderr.read(&mut chunk) {
                Ok(0) | Err(_) => break,
                Ok(n) => {
                    b.extend_from_slice(&chunk[..n]);
                    if b.len() > 128_000 {
                        b.drain(..b.len() - 128_000);
                    }
                }
            }
        }
        String::from_utf8_lossy(&b).into_owned()
    });
    let stdout = child.stdout.take().unwrap();
    let state = status.clone();
    let duration = number(plan, "duration", 1.);
    let progress_thread = std::thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(|v| v.ok()) {
            if let Some(v) = line
                .strip_prefix("out_time_us=")
                .and_then(|s| s.parse::<f64>().ok())
            {
                if let Ok(mut s) = state.lock() {
                    *s = json!({"state":"running","progress":(v/1e6/duration).clamp(0.,0.99),"attempt":attempt});
                }
            }
        }
    });
    loop {
        if cancel.load(Ordering::Acquire) {
            let _ = child.kill();
            let _ = child.wait();
            let _ = progress_thread.join();
            let _ = error_thread.join();
            return Err("Экспорт отменён".into());
        }
        if let Some(result) = child.try_wait().map_err(|e| e.to_string())? {
            let _ = progress_thread.join();
            let error = error_thread.join().unwrap_or_default();
            return if result.success() { Ok(()) } else { Err(error) };
        }
        std::thread::sleep(std::time::Duration::from_millis(100));
    }
}
#[tauri::command]
pub async fn editor_export(
    window: tauri::WebviewWindow,
    app: tauri::AppHandle,
    state: tauri::State<'_, crate::commands::HostState>,
    project: Value,
    plan: Value,
    rasters: Vec<Value>,
) -> Result<bool> {
    main_only(&window)?;
    validate(&project)?;
    let total = duration(&project)?;
    if total < 0.05 {
        return Err("Шкала монтажа пуста".into());
    }
    for key in ["width", "height"] {
        let v = number(&plan, key, 0.);
        if !(16. ..=7680.).contains(&v) || v % 2. != 0. {
            return Err("Некорректное разрешение экспорта".into());
        }
    }
    if !(1. ..=240.).contains(&number(&plan, "fps", 0.))
        || !(64000. ..=200_000_000.).contains(&number(&plan, "bitrate", 0.))
    {
        return Err("Некорректные параметры экспорта".into());
    }
    let limit_mb = number(&plan, "discordLimitMb", 20.);
    if plan["discord"] == true && ![20., 50., 500.].contains(&limit_mb) {
        return Err("Некорректный лимит Discord".into());
    }
    let limit_bytes = limit_mb as u64 * 1_000_000;
    let (engine, _) = crate::commands::editor_paths(&state, None).await?;
    let app2 = app.clone();
    let path =
        tauri::async_runtime::spawn_blocking(move || dialog(true, false, "Монтаж.mp4", "mp4"))
            .await
            .map_err(|e| e.to_string())??;
    let Some(path) = path else { return Ok(false) };
    if path
        .extension()
        .is_none_or(|e| !e.to_string_lossy().eq_ignore_ascii_case("mp4"))
    {
        return Err("Сохраните готовое видео с расширением .mp4".into());
    }
    if let Ok(target) = path.canonicalize() {
        for asset in project["assets"].as_array().unwrap() {
            if Path::new(text(asset, "path")).canonicalize().ok().as_ref() == Some(&target) {
                return Err(
                    "Выберите другое имя: экспорт не должен перезаписывать исходник".into(),
                );
            }
        }
    }
    let s = app.state::<EditorState>();
    {
        let _owners = s.owners.lock().map_err(|e| e.to_string())?;
        if s.updating.load(Ordering::Acquire) { return Err("Идёт обновление приложения. Экспорт временно недоступен.".into()); }
        if s.running.swap(true, Ordering::AcqRel) {
            return Err("Экспорт уже запущен".into());
        }
    }
    s.cancel.store(false, Ordering::Release);
    *s.status.lock().map_err(|e| e.to_string())? = json!({"state":"running","progress":0});
    let status = s.status.clone();
    let cancel = s.cancel.clone();
    crate::logging::record(
        "info",
        "editor",
        "export_started",
        json!({"duration":total,"plan":plan}),
    );
    std::thread::spawn(move || {
        let result = (|| -> Result<()> {
            let temp = tempfile::tempdir().map_err(|e| e.to_string())?;
            let mut raster_paths = Vec::new();
            for (index, raster) in rasters.into_iter().enumerate() {
                let bytes: Vec<u8> =
                    serde_json::from_value(raster["bytes"].clone()).map_err(|e| e.to_string())?;
                if bytes.len() > 32_000_000 || !bytes.starts_with(b"\x89PNG\r\n\x1a\n") {
                    return Err("Некорректный текстовый слой".into());
                }
                let p = temp.path().join(format!("text-{index}.png"));
                std::fs::write(&p, bytes).map_err(|e| e.to_string())?;
                raster_paths.push((text(&raster, "id").to_owned(), p));
            }
            let mut plan = plan;
            plan["duration"] = json!(total);
            let (args, graph) = filter_graph(&project, &plan, &raster_paths)?;
            let mut rate = number(&plan, "bitrate", 5_000_000.) as u64;
            let discord = plan["discord"] == true;
            let output = temp.path().join("render.mp4");
            for attempt in 1..=3 {
                let mut last = String::new();
                let mut encoded = false;
                for encoder in ["h264_nvenc", "h264_mf", "h264_mf_sw"] {
                    match run_encode(
                        &engine, &args, &graph, &plan, &output, encoder, rate, &status, &cancel,
                        attempt,
                    ) {
                        Ok(()) => {
                            encoded = true;
                            break;
                        }
                        Err(e) => {
                            last = e;
                            if cancel.load(Ordering::Acquire) {
                                return Err(last);
                            }
                        }
                    }
                }
                if !encoded {
                    return Err(format!("Кодирование H.264 недоступно: {last}"));
                }
                let size = std::fs::metadata(&output).map_err(|e| e.to_string())?.len();
                if !discord || size < limit_bytes {
                    let mut target =
                        tempfile::NamedTempFile::new_in(path.parent().ok_or("Некорректная папка")?)
                            .map_err(|e| e.to_string())?;
                    let mut source = std::fs::File::open(&output).map_err(|e| e.to_string())?;
                    std::io::copy(&mut source, &mut target).map_err(|e| e.to_string())?;
                    target.as_file().sync_all().map_err(|e| e.to_string())?;
                    target.persist(&path).map_err(|e| e.to_string())?;
                    *status.lock().unwrap() =
                        json!({"state":"done","progress":1,"path":path,"bytes":size});
                    return Ok(());
                }
                rate = ((rate as f64 * limit_bytes as f64 * 0.925 / size as f64 * 0.92) as u64).max(64000);
            }
            Err(format!("Не удалось уложиться в {limit_mb} МБ. Сократите монтаж или снизьте разрешение."))
        })();
        if let Err(error) = result {
            if let Ok(mut s) = status.lock() {
                *s = json!({"state":if cancel.load(Ordering::Acquire){"cancelled"}else{"error"},"error":error});
            }
        }
        app2.state::<EditorState>()
            .running
            .store(false, Ordering::Release);
    });
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn owned_scope_publication_observes_lock_free_shutdown_and_rolls_back() {
        let state = EditorState::default(); begin_owner(&state, "owner").unwrap();
        let session = ImportSession::new_owned(&state, "owner").unwrap();
        let dir = tempfile::tempdir().unwrap(); let path = dir.path().to_owned();
        let preview = path.join("video.mp4");
        let mut batch = PendingPreviews::default(); batch.0.push(dir); batch.1.push(preview.clone());
        let mut revoked = Vec::new();
        let result = batch.commit_scoped(&state, &session, |_| { state.shutdown(); Ok(()) }, |file| revoked.push(file.to_owned()));
        assert_eq!(result.unwrap_err(), "editor.operation_cancelled");
        assert_eq!(revoked, vec![preview]); assert!(!path.exists());
        assert!(state.temps.lock().unwrap().is_empty());
    }

    #[test]
    fn owner_deletion_failure_can_be_retried_without_losing_its_record() {
        use std::os::windows::fs::OpenOptionsExt;
        let state = EditorState::default(); begin_owner(&state, "owner").unwrap();
        let session = ImportSession::new_owned(&state, "owner").unwrap();
        let dir = tempfile::tempdir().unwrap(); let path = dir.path().to_owned();
        let file = path.join("locked.mp4"); std::fs::write(&file, b"media").unwrap();
        let locked = std::fs::OpenOptions::new().read(true).share_mode(1).open(&file).unwrap();
        let mut batch = PendingPreviews::default(); batch.0.push(dir); batch.commit(&state, &session).unwrap();
        assert!(release_owner(&state, "owner", |_| Ok(())).is_err());
        assert!(session.cancelled());
        assert_eq!(state.temps.lock().unwrap().len(), 1);
        assert!(state.owners.lock().unwrap()["owner"].directories.contains(&path));
        drop(locked);
        release_owner(&state, "owner", |_| Ok(())).unwrap();
        assert!(!path.exists()); assert!(state.temps.lock().unwrap().is_empty());
        assert!(!state.owners.lock().unwrap().contains_key("owner"));
    }

    #[test]
    fn owner_release_during_another_export_still_cancels_and_cleans_its_imports() {
        let state = EditorState::default(); begin_owner(&state, "old").unwrap();
        let session = ImportSession::new_owned(&state, "old").unwrap();
        state.running.store(true, Ordering::Release);
        release_owner(&state, "old", |_| Ok(())).unwrap();
        assert!(session.cancelled());
        assert!(!state.owners.lock().unwrap().contains_key("old"));
        assert!(state.running.load(Ordering::Acquire));
    }

    #[test]
    fn deletion_failure_retains_preview_ownership_for_retry() {
        use std::os::windows::fs::OpenOptionsExt;
        let state = EditorState::default();
        let dir = tempfile::tempdir().unwrap(); let path = dir.path().to_owned();
        let file = path.join("locked.mp4"); std::fs::write(&file, b"media").unwrap();
        let locked = std::fs::OpenOptions::new().read(true).share_mode(1).open(&file).unwrap();
        state.temps.lock().unwrap().push(dir);
        assert!(release_previews(&state, &[path.clone()], |_| Ok(())).is_err());
        assert_eq!(state.temps.lock().unwrap().len(), 1);
        drop(locked);
        release_previews(&state, &[path.clone()], |_| Ok(())).unwrap();
        assert!(!path.exists()); assert!(state.temps.lock().unwrap().is_empty());
    }

    #[test]
    fn owner_release_cancels_only_its_imports_and_keeps_new_editor_previews() {
        let state = EditorState::default();
        begin_owner(&state, "old").unwrap();
        begin_owner(&state, "new").unwrap();
        let old = ImportSession::new_owned(&state, "old").unwrap();
        let new = ImportSession::new_owned(&state, "new").unwrap();
        let first = tempfile::tempdir().unwrap(); let first_path = first.path().to_owned();
        let second = tempfile::tempdir().unwrap(); let second_path = second.path().to_owned();
        let mut batch = PendingPreviews::default(); batch.0.push(first); batch.commit(&state, &old).unwrap();
        let mut batch = PendingPreviews::default(); batch.0.push(second); batch.commit(&state, &new).unwrap();
        release_owner(&state, "old", |_| Ok(())).unwrap();
        assert!(old.cancelled()); assert!(!new.cancelled());
        assert!(!first_path.exists()); assert!(second_path.exists());
        assert!(ImportSession::new_owned(&state, "old").is_err());
        assert!(ImportSession::new_owned(&state, "new").is_ok());
    }

    #[test]
    fn released_owner_cannot_publish_a_pending_batch() {
        let state = EditorState::default(); begin_owner(&state, "owner").unwrap();
        let session = ImportSession::new_owned(&state, "owner").unwrap();
        let dir = tempfile::tempdir().unwrap(); let path = dir.path().to_owned();
        let mut batch = PendingPreviews::default(); batch.0.push(dir);
        release_owner(&state, "owner", |_| Ok(())).unwrap();
        assert_eq!(batch.commit(&state, &session).unwrap_err(), "editor.operation_cancelled");
        assert!(!path.exists());
    }

    #[test]
    fn release_previews_removes_only_named_owned_directories() {
        let state = EditorState::default();
        let first = tempfile::tempdir().unwrap();
        let first_path = first.path().to_owned();
        let second = tempfile::tempdir().unwrap();
        let second_path = second.path().to_owned();
        let unrelated = tempfile::tempdir().unwrap();
        std::fs::write(first.path().join("preview.mp4"), b"media").unwrap();
        state.temps.lock().unwrap().extend([first, second]);
        let mut revoked = Vec::new();
        release_previews(&state, &[first_path.clone(), unrelated.path().to_owned()], |path| { revoked.push(path.to_owned()); Ok(()) }).unwrap();
        assert!(!first_path.exists());
        assert!(second_path.exists());
        assert!(unrelated.path().exists());
        assert_eq!(revoked, vec![first_path.join("preview.mp4")]);
        assert_eq!(state.temps.lock().unwrap().len(), 1);
    }

    #[test]
    fn release_previews_preserves_files_when_scope_revocation_fails() {
        let state = EditorState::default();
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().to_owned();
        std::fs::write(path.join("preview.mp4"), b"media").unwrap();
        state.temps.lock().unwrap().push(dir);
        assert!(release_previews(&state, &[path.clone()], |_| Err("scope failure".into())).is_err());
        assert!(path.exists());
        assert_eq!(state.temps.lock().unwrap().len(), 1);
    }

    #[test]
    fn repeated_video_hydration_uses_cache_without_sharing_revocable_paths() {
        let state = EditorState::default();
        let session = ImportSession::new(&state);
        let source_dir = tempfile::tempdir().unwrap();
        let source = source_dir.path().join("source.mp4");
        std::fs::write(&source, b"original").unwrap();
        let (key, guard) = crate::editor_remux_cache::source_identity(&source).unwrap();
        drop(guard);
        let cache_dir = tempfile::tempdir().unwrap();
        std::fs::write(cache_dir.path().join("video.mp4"), b"remuxed").unwrap();
        let prepared = Arc::new(crate::editor_remux_cache::PreparedMedia::new(cache_dir, json!({"tracks":[]})).unwrap());
        state.remux_cache.lock().unwrap().insert_prepared(key, prepared);
        let mut batch = PendingPreviews::default();
        let asset = json!({"kind":"video","path":source});
        let first = hydrate(Path::new("missing-engine"), asset.clone(), &session, &mut batch).unwrap();
        let second = hydrate(Path::new("missing-engine"), asset.clone(), &session, &mut batch).unwrap();
        assert_ne!(first["url"], second["url"]);
        assert_eq!(std::fs::read(text(&second, "url")).unwrap(), b"remuxed");
        drop(batch);
        let mut batch = PendingPreviews::default();
        hydrate(Path::new("missing-engine"), asset.clone(), &session, &mut batch).unwrap();
        drop(batch);
        std::fs::write(&source, b"modified").unwrap();
        assert!(hydrate(Path::new("missing-engine"), asset, &session, &mut PendingPreviews::default()).is_err(), "changed source must miss cache and invoke the missing engine");
    }
    #[test]
    fn import_progress_stops_after_owner_cancellation_or_deadline() {
        let state = EditorState::default(); begin_owner(&state, "owner").unwrap();
        let mut session = ImportSession::new_owned(&state, "owner").unwrap();
        let observed = Arc::new(Mutex::new(Vec::new())); let captured = observed.clone();
        session.progress = crate::editor_import_progress::Reporter::new("owner".into(), "operation".into(), Arc::new(move |event| captured.lock().unwrap().push(event))).unwrap();
        session.report(Phase::Analyzing);
        assert_eq!(observed.lock().unwrap().len(), 1);
        let mut expired = session.clone(); expired.deadline = std::time::Instant::now();
        expired.report(Phase::Publishing);
        release_owner(&state, "owner", |_| Ok(())).unwrap();
        session.report(Phase::PreparingPreview);
        assert_eq!(observed.lock().unwrap().len(), 1);
    }

    #[test]
    fn image_and_audio_hydration_publish_only_unique_previews() {
        for kind in ["image", "audio"] {
            let source_dir = tempfile::tempdir().unwrap();
            let original = source_dir.path().join(if kind == "image" { "пример.png" } else { "пример.wav" });
            std::fs::write(&original, b"original media bytes").unwrap();
            let state = EditorState::default();
            let session = ImportSession::new(&state);
            let mut batch = PendingPreviews::default();
            let asset = json!({"kind":kind,"path":original});
            let first = hydrate(Path::new("unused-engine"), asset.clone(), &session, &mut batch).unwrap();
            let second = hydrate(Path::new("unused-engine"), asset, &session, &mut batch).unwrap();
            assert_eq!(first["path"], json!(original));
            assert_eq!(second["path"], json!(original));
            assert_ne!(first["url"], second["url"]);
            let paths = batch.1.clone();
            assert_eq!(paths.len(), 2);
            assert!(paths.iter().all(|path| path != &original));
            let mut grants = Vec::new();
            batch.commit_scoped(&state, &session, |path| { grants.push(path.to_owned()); Ok(()) }, |_| panic!("successful publish revoked a preview")).unwrap();
            assert_eq!(grants, paths);
            let mut temps = state.temps.lock().unwrap();
            temps.remove(0).close().unwrap();
            assert!(!paths[0].exists());
            assert_eq!(std::fs::read(&paths[1]).unwrap(), b"original media bytes");
            temps.clear();
            assert!(paths.iter().all(|path| !path.exists()));
            assert_eq!(std::fs::read(original).unwrap(), b"original media bytes");
        }
    }

    #[test]
    fn cancelled_source_hydration_batch_grants_nothing_and_keeps_original() {
        let source_dir = tempfile::tempdir().unwrap();
        let original = source_dir.path().join("image.png");
        std::fs::write(&original, b"image").unwrap();
        let state = EditorState::default();
        let session = ImportSession::new(&state);
        let mut batch = PendingPreviews::default();
        hydrate(Path::new("unused-engine"), json!({"kind":"image","path":original}), &session, &mut batch).unwrap();
        let preview = batch.1[0].clone();
        state.import_epoch.fetch_add(1, Ordering::AcqRel);
        let result = batch.commit_scoped(&state, &session, |_| panic!("cancelled source grant"), |_| {});
        assert_eq!(result.unwrap_err(), "editor.operation_cancelled");
        assert!(!preview.exists());
        assert_eq!(std::fs::read(original).unwrap(), b"image");
    }
    #[test]
    fn failed_preview_batch_removes_all_unpublished_directories() {
        let first = tempfile::tempdir().unwrap();
        let second = tempfile::tempdir().unwrap();
        let paths = [first.path().to_owned(), second.path().to_owned()];
        let mut batch = PendingPreviews::default();
        batch.0.extend([first, second]);
        drop(batch);
        assert!(paths.iter().all(|path| !path.exists()));
    }
    #[test]
    fn cancelled_preview_batch_does_not_touch_published_history() {
        let state = EditorState::default();
        let previous = tempfile::tempdir().unwrap();
        let previous_path = previous.path().to_owned();
        state.temps.lock().unwrap().push(previous);
        let session = ImportSession::new(&state);
        let fresh = tempfile::tempdir().unwrap();
        let fresh_path = fresh.path().to_owned();
        let mut batch = PendingPreviews::default();
        batch.0.push(fresh);
        state.import_epoch.fetch_add(1, Ordering::AcqRel);
        assert_eq!(batch.commit(&state, &session).unwrap_err(), "editor.operation_cancelled");
        assert!(!fresh_path.exists());
        assert!(previous_path.exists());
        assert_eq!(state.temps.lock().unwrap().len(), 1);
    }
    #[test]
    fn successful_preview_batch_transfers_ownership() {
        let state = EditorState::default();
        let session = ImportSession::new(&state);
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().to_owned();
        let mut batch = PendingPreviews::default();
        batch.0.push(dir);
        batch.commit(&state, &session).unwrap();
        assert!(path.exists());
        assert_eq!(state.temps.lock().unwrap().len(), 1);
        drop(state);
        assert!(!path.exists());
    }
    #[test]
    fn failed_scope_publication_rolls_back_only_its_previews() {
        let state = EditorState::default();
        let prior = tempfile::tempdir().unwrap();
        let prior_path = prior.path().to_owned();
        state.temps.lock().unwrap().push(prior);
        let session = ImportSession::new(&state);
        let fresh = tempfile::tempdir().unwrap();
        let fresh_path = fresh.path().to_owned();
        let first = fresh_path.join("video.mp4");
        let second = fresh_path.join("audio-0.m4a");
        let mut batch = PendingPreviews::default();
        batch.0.push(fresh);
        batch.1.extend([first.clone(), second.clone()]);
        let mut allowed = Vec::new();
        let mut forbidden = Vec::new();
        let result = batch.commit_scoped(&state, &session, |path| {
            if path == second { return Err("scope failed".into()); }
            allowed.push(path.to_owned()); Ok(())
        }, |path| forbidden.push(path.to_owned()));
        assert_eq!(result.unwrap_err(), "scope failed");
        assert_eq!(allowed, vec![first.clone()]);
        assert_eq!(forbidden, vec![first]);
        assert!(!fresh_path.exists());
        assert!(prior_path.exists());
        assert_eq!(state.temps.lock().unwrap().len(), 1);
    }
    #[test]
    fn cancelled_scope_publication_grants_nothing() {
        let state = EditorState::default();
        let session = ImportSession::new(&state);
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().to_owned();
        let mut batch = PendingPreviews::default();
        batch.1.push(path.join("video.mp4"));
        batch.0.push(dir);
        state.import_epoch.fetch_add(1, Ordering::AcqRel);
        let result = batch.commit_scoped(&state, &session,
            |_| panic!("cancelled operation must not grant scope"),
            |_| panic!("cancelled operation must not revoke other grants"));
        assert_eq!(result.unwrap_err(), "editor.operation_cancelled");
        assert!(!path.exists());
        assert!(state.temps.lock().unwrap().is_empty());
    }
    #[test]
    fn scope_publication_cannot_revoke_originals() {
        let state = EditorState::default();
        let session = ImportSession::new(&state);
        let source = tempfile::NamedTempFile::new().unwrap();
        let mut batch = PendingPreviews::default();
        batch.0.push(tempfile::tempdir().unwrap());
        batch.1.push(source.path().to_owned());
        let result = batch.commit_scoped(&state, &session,
            |_| panic!("original must not enter preview scope transaction"),
            |_| panic!("original must not be forbidden"));
        assert_eq!(result.unwrap_err(), "editor.preview_scope_invalid");
        assert!(source.path().exists());
        assert!(state.temps.lock().unwrap().is_empty());
    }
    #[test]
    fn successful_scope_publication_keeps_grants_and_transfers_directories() {
        let state = EditorState::default();
        let session = ImportSession::new(&state);
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().to_owned();
        let files = vec![path.join("video.mp4"), path.join("audio-0.m4a")];
        let mut batch = PendingPreviews::default();
        batch.1 = files.clone();
        batch.0.push(dir);
        let mut allowed = Vec::new();
        batch.commit_scoped(&state, &session,
            |file| { allowed.push(file.to_owned()); Ok(()) },
            |_| panic!("successful publication must retain its grants")).unwrap();
        assert_eq!(allowed, files);
        assert!(path.exists());
        assert_eq!(state.temps.lock().unwrap().len(), 1);
        drop(state);
        assert!(!path.exists());
    }
    #[test]
    fn shutdown_during_scope_publication_rolls_back_the_new_grant() {
        let state = EditorState::default();
        let session = ImportSession::new(&state);
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().to_owned();
        let preview = path.join("video.mp4");
        let mut batch = PendingPreviews::default();
        batch.0.push(dir);
        batch.1.push(preview.clone());
        let mut revoked = Vec::new();
        let result = batch.commit_scoped(&state, &session,
            |_| { state.shutdown(); Ok(()) },
            |file| revoked.push(file.to_owned()));
        assert_eq!(result.unwrap_err(), "editor.operation_cancelled");
        assert_eq!(revoked, vec![preview]);
        assert!(!path.exists());
        assert!(state.temps.lock().unwrap().is_empty());
    }
    #[test]
    fn import_budget_expires_across_multiple_assets() {
        let state = EditorState::default();
        let mut session = ImportSession::new(&state);
        session.deadline = std::time::Instant::now() + std::time::Duration::from_millis(40);
        assert!(session.remaining(std::time::Duration::from_secs(300)).unwrap() <= std::time::Duration::from_millis(40));
        std::thread::sleep(std::time::Duration::from_millis(60));
        assert_eq!(session.remaining(std::time::Duration::from_secs(300)).unwrap_err(), "editor.operation_timeout");
        assert_eq!(session.check().unwrap_err(), "editor.operation_timeout");
    }
    #[test]
    fn dialog_wait_is_excluded_but_cancel_is_not_reset_when_work_starts() {
        let state = EditorState::default();
        let mut session = ImportSession::new(&state);
        session.deadline = std::time::Instant::now();
        let processing = session.begin_work();
        assert!(processing.remaining(std::time::Duration::from_secs(30)).is_ok());
        state.import_epoch.fetch_add(1, Ordering::AcqRel);
        assert_eq!(processing.check().unwrap_err(), "editor.operation_cancelled");
    }
    #[test]
    fn project_roundtrip_preserves_edits_without_preview_paths() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("Монтаж.rebcap");
        let p = json!({"version":1,"recoveryId":"roundtrip-project","name":"Момент","width":1920,"height":1080,"fps":60,"assets":[{"id":"v","path":"assets/clip.mp4","url":"temporary","audioUrls":[]}],"items":[{"id":"i","kind":"video","asset":"v","start":0,"source":3,"duration":2}]});
        validate(&p).unwrap();
        write_project(&path, &clean_project(p.clone())).unwrap();
        let loaded: Value = serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap();
        assert_eq!(loaded["items"], p["items"]);
        assert_eq!(loaded["recoveryId"], "roundtrip-project");
        assert!(loaded["assets"][0].get("url").is_none());
        assert_eq!(loaded["assets"][0]["path"], "assets/clip.mp4");
    }
    #[test]
    fn package_copies_sources_once_and_survives_original_removal() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("клип.mp4");
        std::fs::write(&source, b"fixture").unwrap();
        let p = json!({"assets":[{"path":source,"url":"temp"},{"path":source}],"items":[]});
        let path = package_project(dir.path(), p, "Момент").unwrap();
        let p: Value = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
        std::fs::remove_file(source).unwrap();
        assert_eq!(p["assets"][0]["path"], p["assets"][1]["path"]);
        let copy = path.parent().unwrap().join(text(&p["assets"][0], "path"));
        assert_eq!(std::fs::read(copy).unwrap(), b"fixture");
        assert!(p["assets"][0].get("url").is_none());
    }
    #[test]
    #[ignore = "Requires packaged FFmpeg"]
    fn cancellation_terminates_encoder() {
        let engine = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("resources/engine/RebellioCap.Engine.exe");
        let dir = tempfile::tempdir().unwrap();
        let cancel = AtomicBool::new(true);
        let status = Arc::new(Mutex::new(json!({})));
        let result = run_encode(
            &engine,
            &[],
            "color=s=640x360:d=100,format=nv12[outv];anullsrc,atrim=duration=100[outa]",
            &json!({"duration":100}),
            &dir.path().join("out.mp4"),
            "h264_mf_sw",
            500_000,
            &status,
            &cancel,
            1,
        );
        assert_eq!(result.unwrap_err(), "Экспорт отменён");
    }
    #[test]
    #[ignore = "Requires packaged FFmpeg and Windows H.264 encoder"]
    fn real_media_export_cuts_overlays_and_mixes_audio() {
        let engine = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("resources/engine/RebellioCap.Engine.exe");
        let ffmpeg = engine.with_file_name("ffmpeg.exe");
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("исходник.mp4");
        let image = dir.path().join("overlay.png");
        let fixture = command(&ffmpeg)
            .args([
                "-hide_banner",
                "-loglevel",
                "error",
                "-y",
                "-filter_complex",
                "testsrc2=s=320x180:r=30:d=3[v];sine=frequency=440:duration=3[a]",
                "-map",
                "[v]",
                "-map",
                "[a]",
                "-c:v",
                "mpeg4",
                "-c:a",
                "aac",
                "-shortest",
            ])
            .arg(&source)
            .output()
            .unwrap();
        assert!(
            fixture.status.success(),
            "{}",
            String::from_utf8_lossy(&fixture.stderr)
        );
        // Exercise the real miss -> native remux -> cache insert -> hit path.
        let original_bytes = std::fs::read(&source).unwrap();
        let editor_state = EditorState::default();
        let session = ImportSession::new(&editor_state);
        let mut previews = PendingPreviews::default();
        let preview_asset = json!({"kind":"video","path":source});
        let first = hydrate(&engine, preview_asset.clone(), &session, &mut previews).unwrap();
        let second = hydrate(Path::new("missing-engine-cache-hit"), preview_asset, &session, &mut previews).unwrap();
        assert_ne!(first["url"], second["url"]);
        assert_eq!(std::fs::read(text(&first, "url")).unwrap(), std::fs::read(text(&second, "url")).unwrap());
        assert_eq!(first["audioUrls"].as_array().unwrap().len(), 1);
        assert_eq!(probe(&engine, Path::new(text(&second, "url"))).unwrap()["streams"][0]["codec_type"], "video");
        assert_eq!(std::fs::read(&source).unwrap(), original_bytes);
        drop(previews);
        let fixture = command(&ffmpeg)
            .args([
                "-hide_banner",
                "-loglevel",
                "error",
                "-y",
                "-filter_complex",
                "color=c=blue@0.0:s=40x40:d=0.1,format=rgba,drawbox=x=10:y=10:w=20:h=20:color=blue@1:t=fill:replace=1",
                "-frames:v",
                "1",
            ])
            .arg(&image)
            .output()
            .unwrap();
        assert!(
            fixture.status.success(),
            "{}",
            String::from_utf8_lossy(&fixture.stderr)
        );
        let p = json!({"version":1,"width":320,"height":180,"fps":30,"assets":[{"id":"v","path":source},{"id":"img","path":image}],"items":[
        {"id":"v1","kind":"video","asset":"v","track":0,"source":0.3,"start":0,"duration":0.6},
        {"id":"v2","kind":"video","asset":"v","track":0,"source":1.5,"start":0.6,"duration":0.6},
        {"id":"a1","kind":"audio","asset":"v","track":1,"source":0.3,"start":0,"duration":0.6,"stream":1,"gain":0.5},
        {"id":"a2","kind":"audio","asset":"v","track":1,"source":1.5,"start":0.6,"duration":0.6,"stream":1,"gain":0.5},
        {"id":"overlay","kind":"image","asset":"img","track":2,"source":0,"start":0,"duration":1.2,"width":0.25,"height":0.4,"x":0.1,"y":0.1},
        {"id":"text","kind":"text","track":3,"source":0,"start":0.2,"duration":0.5,"width":0.1,"height":0.1,"x":0.8,"y":0.8}
        ]});
        let plan = json!({"width":320,"height":180,"fps":30,"duration":1.2});
        validate(&p).unwrap();
        let (args, graph) = filter_graph(&p, &plan, &[("text".into(), image)]).unwrap();
        let output = dir.path().join("render.mp4");
        let status = Arc::new(Mutex::new(json!({})));
        let cancel = AtomicBool::new(false);
        let result = run_encode(
            &engine,
            &args,
            &graph,
            &plan,
            &output,
            "h264_mf_sw",
            700_000,
            &status,
            &cancel,
            1,
        );
        assert!(
            result.is_ok(),
            "{}\n{graph}",
            result.unwrap_err_or_default()
        );
        let info = probe(&engine, &output).unwrap();
        let d = info["format"]["duration"]
            .as_str()
            .unwrap()
            .parse::<f64>()
            .unwrap();
        assert!((d - 1.2).abs() < 0.1);
        let streams = info["streams"].as_array().unwrap();
        assert_eq!(
            streams
                .iter()
                .filter(|s| s["codec_type"] == "audio")
                .count(),
            1
        );
        assert_eq!(streams[0]["width"], 320);
        assert!(std::fs::metadata(&output).unwrap().len() < 20_000_000);
        let frame = |path: &Path, seek: &str| {
            let result = command(&ffmpeg)
                .args(["-v", "error", "-ss", seek, "-i"])
                .arg(path)
                .args([
                    "-frames:v",
                    "1",
                    "-pix_fmt",
                    "rgb24",
                    "-f",
                    "rawvideo",
                    "pipe:1",
                ])
                .output()
                .unwrap();
            assert!(result.status.success());
            result.stdout
        };
        let original = frame(&source, "0.3");
        let rendered = frame(&output, "0");
        // Transparent PNG corners must reveal the video, rather than a black box.
        let pixel = (25 * 320 + 40) * 3;
        for c in 0..3 {
            assert!(
                (original[pixel + c] as i16 - rendered[pixel + c] as i16).abs() < 40,
                "PNG alpha was lost"
            );
        }
    }
    trait ErrorText {
        fn unwrap_err_or_default(self) -> String;
    }
    impl ErrorText for Result<()> {
        fn unwrap_err_or_default(self) -> String {
            self.err().unwrap_or_default()
        }
    }
}
