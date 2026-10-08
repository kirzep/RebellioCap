use serde_json::{json, Value};
use std::{fs::{self, OpenOptions, File}, io::{Write, Read}, path::{Path, PathBuf}, sync::{Mutex, OnceLock, atomic::{AtomicBool, AtomicU8, Ordering}}, time::{SystemTime, UNIX_EPOCH, Duration, Instant}};

const MAX_FILE: u64 = 2 * 1024 * 1024;
const KEEP_FILES: usize = 4;
const MAX_RECORD: usize = 16 * 1024;
static LOGGER: OnceLock<Mutex<RollingLog>> = OnceLock::new();
fn timestamp() -> u128 { SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_millis() }
fn sanitize(value: Value) -> Value {
    match value {
        Value::String(mut text) => {
            for name in ["USERPROFILE", "TEMP", "LOCALAPPDATA"] {
                if let Ok(path) = std::env::var(name) { if !path.is_empty() { text = text.replace(&path, &format!("<{name}>")); } }
            }
            Value::String(text.chars().take(4096).collect())
        }
        Value::Array(items) => Value::Array(items.into_iter().take(128).map(sanitize).collect()),
        Value::Object(items) => Value::Object(items.into_iter().map(|(key, value)| {
            let lower = key.to_lowercase();
            let value = if lower.contains("path") || lower.contains("directory") || lower.contains("endpoint") { json!("<redacted>") } else { sanitize(value) };
            (key,value)
        }).collect()),
        other => other,
    }
}
struct RollingLog { root: PathBuf, limit: u64, keep: usize }
impl RollingLog {
    fn new(root: PathBuf, limit: u64, keep: usize) -> std::io::Result<Self> {
        fs::create_dir_all(&root)?;
        let result = Self { root, limit, keep };
        for n in 0..keep {
            let path = result.path(n);
            if fs::metadata(&path).and_then(|m| m.modified()).ok().is_some_and(|date| SystemTime::now().duration_since(date).unwrap_or_default() > Duration::from_secs(7*86400)) { let _ = fs::remove_file(path); }
        }
        Ok(result)
    }
    fn path(&self, n: usize) -> PathBuf { self.root.join(format!("application-{n}.jsonl")) }
    fn write(&mut self, level: &str, component: &str, event: &str, details: Value) -> std::io::Result<()> {
        let mut bytes = serde_json::to_vec(&json!({"utcUnixMs":timestamp(),"pid":std::process::id(),"level":level,"component":component,"event":event,"details":sanitize(details)}))?;
        if bytes.len() > MAX_RECORD || bytes.len() as u64 + 1 > self.limit {
            bytes = serde_json::to_vec(&json!({"utcUnixMs":timestamp(),"level":level,"component":component,"event":event,"details":"<record truncated>"}))?;
        }
        bytes.push(b'\n');
        let current = self.path(0);
        if fs::metadata(&current).map_or(0, |m| m.len()) + bytes.len() as u64 > self.limit {
            let _ = fs::remove_file(self.path(self.keep - 1));
            for n in (1..self.keep).rev() { if self.path(n-1).exists() { fs::rename(self.path(n-1),self.path(n))?; } }
        }
        OpenOptions::new().create(true).append(true).open(current)?.write_all(&bytes)
    }
    fn export(&mut self, destination: &Path) -> Result<(), String> {
        use zip::{ZipWriter, write::SimpleFileOptions, CompressionMethod};
        let parent = destination.parent().ok_or("logs.export_path")?;
        let temporary = tempfile::NamedTempFile::new_in(parent).map_err(|e| e.to_string())?;
        {
            let mut archive = ZipWriter::new(temporary.as_file());
            let options = SimpleFileOptions::default().compression_method(CompressionMethod::Deflated);
            archive.start_file("report.json",options).map_err(|e|e.to_string())?;
            archive.write_all(&serde_json::to_vec_pretty(&json!({"version":env!("CARGO_PKG_VERSION"),"platform":"Windows x64","windowsVersion":format!("{:?}",windows_version::OsVersion::current()),"exportedUtcUnixMs":timestamp(),"maxLogBytes":self.limit*self.keep as u64,"retentionDays":7})).map_err(|e|e.to_string())?).map_err(|e|e.to_string())?;
            for n in 0..self.keep {
                let path = self.path(n);
                if !path.is_file() {continue;}
                archive.start_file(format!("logs/application-{n}.jsonl"),options).map_err(|e|e.to_string())?;
                std::io::copy(&mut File::open(path).map_err(|e|e.to_string())?.take(self.limit), &mut archive).map_err(|e|e.to_string())?;
            }
            archive.finish().map_err(|e|e.to_string())?;
        }
        temporary.persist_noclobber(destination).map_err(|e|e.to_string())?;
        Ok(())
    }
}
pub fn initialize(root: &Path) -> Result<(), String> {
    let log = RollingLog::new(root.join("logs"),MAX_FILE,KEEP_FILES).map_err(|e|e.to_string())?;
    let _ = LOGGER.set(Mutex::new(log));
    record("info","app","startup",json!({"version":env!("CARGO_PKG_VERSION"),"debugBuild":cfg!(debug_assertions),"windows":format!("{:?}",windows_version::OsVersion::current())}));
    let previous = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |panic| { record("error","app","panic",json!({"message":panic.to_string()})); previous(panic); }));
    Ok(())
}
pub fn record(level: &str, component: &str, event: &str, details: Value) {
    if let Some(logger) = LOGGER.get() { if let Ok(mut log) = logger.lock() { let _ = log.write(level,component,event,details); } }
}
#[derive(Default)]
pub struct DeveloperState { enabled: AtomicBool, clicks: AtomicU8, frontend_rate: Mutex<Option<(Instant,u32)>> }
impl DeveloperState {
    fn click(&self) -> bool {
        if self.enabled.load(Ordering::Acquire) {return true;}
        if self.clicks.fetch_add(1,Ordering::AcqRel) >= 9 {
            self.enabled.store(true,Ordering::Release);
            record("info","app","developer_mode_enabled",json!({}));
        }
        self.enabled.load(Ordering::Acquire)
    }
}
#[tauri::command]
pub fn developer_click(window: tauri::WebviewWindow, state: tauri::State<'_,DeveloperState>) -> Result<bool,String> {
    if window.label() != "main" { return Err("developer.main_window_only".into()); }
    Ok(state.click())
}
#[tauri::command]
pub fn get_developer_mode(state: tauri::State<'_,DeveloperState>) -> bool {state.enabled.load(Ordering::Acquire)}
#[tauri::command]
pub fn log_frontend(window: tauri::WebviewWindow, state: tauri::State<'_,DeveloperState>, level: String, message: String) {
    let Ok(mut rate) = state.frontend_rate.lock() else {return};
    let entry = rate.get_or_insert((Instant::now(),0));
    if entry.0.elapsed() > Duration::from_secs(60) { *entry=(Instant::now(),0); }
    if entry.1 >= 120 {return;}
    entry.1 += 1;
    record(if level == "error" {"error"} else {"warn"},"renderer","console",json!({"window":window.label(),"message":message.chars().take(8192).collect::<String>()}));
}
#[tauri::command]
pub async fn export_application_logs(window: tauri::WebviewWindow, state: tauri::State<'_,DeveloperState>) -> Result<Option<String>,String> {
    if window.label() != "main" || !state.enabled.load(Ordering::Acquire) {return Err("logs.developer_mode_required".into());}
    tauri::async_runtime::spawn_blocking(|| {
        // Native dialog chooses the destination; no arbitrary renderer path is accepted.
        let folder = std::thread::spawn(crate::native::choose_directory).join().map_err(|_|"logs.dialog_thread_failed".to_string())??;
        let Some(folder)=folder else {return Ok(None)};
        record("info","app","logs_export",json!({}));
        let path=folder.join(format!("RebellioCap-logs-{}.zip",timestamp()));
        LOGGER.get().ok_or("logs.unavailable")?.lock().map_err(|e|e.to_string())?.export(&path)?;
        Ok(Some(path.to_string_lossy().into_owned()))
    }).await.map_err(|e|e.to_string())?
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn developer_mode_requires_ten_clicks() {
        let state=DeveloperState::default();
        for _ in 0..9 {assert!(!state.click());}
        assert!(state.click());
        assert!(state.click());
    }
    #[test]
    fn expired_logs_are_removed_and_personal_paths_are_redacted() {
        let root=tempfile::tempdir().unwrap();
        let folder=root.path().join("logs");
        std::fs::create_dir_all(&folder).unwrap();
        let file=std::fs::File::create(folder.join("application-2.jsonl")).unwrap();
        file.set_times(std::fs::FileTimes::new().set_modified(SystemTime::now()-Duration::from_secs(8*86400))).unwrap();
        RollingLog::new(folder.clone(),MAX_FILE,KEEP_FILES).unwrap();
        assert!(!folder.join("application-2.jsonl").exists());
        assert_eq!(sanitize(json!({"outputDirectory":"C:/Users/Someone/Videos"}))["outputDirectory"],"<redacted>");
    }
    #[test]
    fn log_rotation_is_bounded_and_export_contains_only_diagnostics() {
        let root = tempfile::tempdir().unwrap();
        let mut log = RollingLog::new(root.path().join("logs"), 256, 3).unwrap();
        for n in 0..40 { log.write("info", "test", "event", serde_json::json!({"n":n})).unwrap(); }
        let files: Vec<_> = std::fs::read_dir(root.path().join("logs")).unwrap().collect();
        assert!(files.len() <= 3);
        for entry in files { assert!(entry.unwrap().metadata().unwrap().len() <= 256); }
        let archive = root.path().join("export.zip");
        log.export(&archive).unwrap();
        let mut zip = zip::ZipArchive::new(std::fs::File::open(archive).unwrap()).unwrap();
        assert!(zip.by_name("report.json").is_ok());
        assert!(zip.len() <= 4);
    }
}
