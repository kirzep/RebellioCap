use super::{
    validation::{validate_recording_evidence, validate_state},
    *,
};
use crate::model::{DeviceInventory, RecordingTestSummary};
use serde::Deserialize;
use std::{
    fs::{self, File, OpenOptions},
    io::{Read, Write},
    path::{Path, PathBuf},
};

const PRIMARY: &str = "config.json";
const BACKUP: &str = "config.last-good.json";
const TEMP: &str = "config.json.tmp";
const BACKUP_TEMP: &str = "config.last-good.json.tmp";
const MAX_BYTES: u64 = 1024 * 1024;

fn has_legacy_overlay(state: &StoredState) -> bool {
    state.draft.preferences.as_ref().is_some_and(|p| p.overlay_enabled.is_some()) ||
        state.active.as_ref().is_some_and(|active| active.preferences.overlay_enabled.is_some())
}

fn retire_legacy_overlay(state: &mut StoredState) -> Result<()> {
    // Validate the old evidence before rebinding its fingerprint to a metadata-only migration.
    validate_state(state)?;
    if let Some(preferences) = &mut state.draft.preferences { preferences.overlay_enabled = None; }
    if let Some(active) = &mut state.active { active.preferences.overlay_enabled = None; }
    let fingerprint = state.draft.fingerprint()?;
    if let Some(test) = &mut state.last_successful_test { test.fingerprint = fingerprint; }
    validate_state(state)
}

pub struct ConfigStore {
    root: PathBuf,
}

pub fn resolve_config_root(
    local_app_data: &Path,
    test_override: Option<&Path>,
    allow_debug: bool,
) -> Result<PathBuf> {
    let root = match test_override {
        Some(path) if cfg!(debug_assertions) && allow_debug => path.to_path_buf(),
        Some(_) => return Err(ConfigError::ReleaseOverride),
        None => local_app_data.join("RebellioCap"),
    };
    if !root.is_absolute() {
        return Err(ConfigError::Invalid("config_root"));
    }
    fs::create_dir_all(&root)?;
    Ok(root.canonicalize()?)
}

impl ConfigStore {
    /// Host transaction journal restoration / full settings commit. Never registered as IPC.
    pub fn replace_host_state(&self, state: &StoredState) -> Result<()> {
        let _lock = self.lock()?;
        self.load_locked()?;
        self.persist(state)
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    pub(crate) fn from_root_host(root: &Path) -> std::result::Result<Self, String> {
        if !root.is_absolute() || root.canonicalize().map_err(|e| e.to_string())? != root {
            return Err("config.root_not_canonical".into());
        }
        Ok(Self { root: root.into() })
    }
    /// The argument comes from Tauri's local_data_dir resolver, never the WebView.
    pub fn from_local_app_data(local_app_data: &Path) -> Result<Self> {
        let test_override = std::env::var_os("REBELLIOCAP_TEST_CONFIG_ROOT").map(PathBuf::from);
        Ok(Self {
            root: resolve_config_root(
                local_app_data,
                test_override.as_deref(),
                cfg!(debug_assertions),
            )?,
        })
    }

    #[cfg(debug_assertions)]
    pub fn at(root: PathBuf) -> Result<Self> {
        Ok(Self {
            root: resolve_config_root(&root, Some(&root), true)?,
        })
    }

    // File locks serialize the entire read/modify/write across store instances
    // and processes. Closing the file releases the lock even after a crash.
    fn lock(&self) -> Result<File> {
        if self.root.canonicalize()? != self.root {
            return Err(ConfigError::Invalid("config_root"));
        }
        let path = self.checked("config.lock")?;
        let file = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .open(path)?;
        file.lock()?;
        Ok(file)
    }

    fn checked(&self, name: &str) -> Result<PathBuf> {
        let path = self.root.join(name);
        match fs::symlink_metadata(&path) {
            Ok(meta) => {
                #[cfg(windows)]
                {
                    use std::os::windows::fs::MetadataExt;
                    if meta.file_attributes() & 0x400 != 0 {
                        return Err(ConfigError::Invalid("config_reparse_point"));
                    }
                }
                if !meta.is_file() || meta.file_type().is_symlink() {
                    return Err(ConfigError::Invalid("config_file"));
                }
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => (),
            Err(e) => return Err(e.into()),
        }
        Ok(path)
    }

    fn cleanup(&self) -> Result<()> {
        for name in [TEMP, BACKUP_TEMP] {
            match fs::remove_file(self.checked(name)?) {
                Ok(()) => (),
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => (),
                Err(e) => return Err(e.into()),
            }
        }
        Ok(())
    }

    fn read(&self, name: &str) -> Result<StoredState> {
        read_state(&self.checked(name)?)
    }

    pub fn load(&self) -> Result<StoredState> {
        let _lock = self.lock()?;
        let mut state = self.load_locked()?;
        if has_legacy_overlay(&state) && crate::notifications::has_canonical_settings(&self.root) {
            let legacy = state.clone();
            retire_legacy_overlay(&mut state)?;
            if let Err(error) = self.persist(&state) {
                crate::logging::record("error", "notifications", "config_migration_failed", serde_json::json!({"error":error.to_string()}));
                return Ok(legacy);
            }
        }
        Ok(state)
    }

    /// Upgrade only a live, exact legacy address. Output indices alone cannot
    /// identify a physical display after reboot or topology changes.
    pub fn migrate_monitor_identity(&self, monitors: &[crate::model::MonitorChoice]) -> Result<StoredState> {
        let _lock = self.lock()?;
        let mut state = self.load_locked()?;
        let replacement = |id: &str| -> Option<String> {
            let (adapter, output) = id.split_once(':')?;
            adapter.parse::<u64>().ok()?;
            output.parse::<u32>().ok()?;
            let matches: Vec<_> = monitors.iter().filter(|monitor|
                monitor.legacy_id.as_deref() == Some(id) && monitor.id.starts_with("monitor-path:")).collect();
            if matches.len() != 1 || monitors.iter().filter(|m| m.id == matches[0].id).count() != 1 { return None; }
            Some(matches[0].id.clone())
        };
        let mut changed = false;
        if let Some(id) = state.draft.monitor_id.as_ref().and_then(|id| replacement(id)) {
            state.draft.monitor_id = Some(id);
            changed = true;
            let fingerprint = state.draft.fingerprint()?;
            if let Some(test) = &mut state.last_successful_test { test.fingerprint = fingerprint; }
        }
        if let Some(active) = &mut state.active {
            if let Some(id) = replacement(&active.monitor_id) { active.monitor_id = id; changed = true; }
        }
        if changed { validate_state(&state)?; self.persist(&state)?; }
        Ok(state)
    }

    fn load_locked(&self) -> Result<StoredState> {
        self.cleanup()?;
        // Path-policy failures and transient I/O errors are not evidence of bad
        // content, so only a missing or successfully-read invalid primary may
        // be replaced from the backup.
        let primary_path = self.checked(PRIMARY)?;
        match read_state(&primary_path) {
            Ok(value) => Ok(value),
            // Future schemas belong to a newer host; never roll them back.
            Err(e @ ConfigError::Schema(_)) => Err(e),
            Err(ConfigError::Io(io)) if io.kind() != std::io::ErrorKind::NotFound => {
                Err(ConfigError::Io(io))
            }
            Err(
                primary_error @ (ConfigError::Io(_)
                | ConfigError::Json(_)
                | ConfigError::Invalid(_)),
            ) => match self.read(BACKUP) {
                Ok(backup) => {
                    self.install(&backup, PRIMARY, TEMP, &NativeIo)?;
                    Ok(backup)
                }
                Err(ConfigError::Io(e))
                    if e.kind() == std::io::ErrorKind::NotFound
                        && matches!(&primary_error, ConfigError::Io(p) if p.kind() == std::io::ErrorKind::NotFound) =>
                {
                    Ok(StoredState::default())
                }
                Err(_) => Err(primary_error),
            },
            Err(e) => Err(e),
        }
    }

    pub fn save_draft(&self, draft: OnboardingDraft, last_completed_step: u8) -> Result<()> {
        validate_draft(&draft)?;
        if last_completed_step > 5 {
            return Err(ConfigError::Invalid("last_completed_step"));
        }
        let _lock = self.lock()?;
        let mut state = self.load_locked()?;
        if state.draft.fingerprint()? != draft.fingerprint()? {
            state.last_successful_test = None;
        }
        state.draft = draft;
        if !state.onboarding.completed {
            state.onboarding.last_completed_step = last_completed_step;
        }
        self.persist(&state)
    }

    /// Host-only transaction. Task 6 must never accept this summary from invoke
    /// arguments: it must come from the owned engine's validated recording test.
    pub fn record_successful_test(&self, summary: RecordingTestSummary) -> Result<()> {
        let _lock = self.lock()?;
        let mut state = self.load_locked()?;
        validate_draft(&state.draft)?;
        state.draft.to_active()?;
        validate_recording_evidence(&state.draft, &summary)?;
        state.last_successful_test = Some(summary);
        self.persist(&state)
    }

    pub fn commit_completed(
        &self,
        explicit_completion: bool,
        devices: &DeviceInventory,
    ) -> Result<()> {
        if !explicit_completion {
            return Err(ConfigError::CompletionRequired);
        }
        let _lock = self.lock()?;
        let mut state = self.load_locked()?;
        if !state.onboarding.completed && state.onboarding.last_completed_step != 5 {
            return Err(ConfigError::CompletionRequired);
        }
        let active = state.draft.to_active()?;
        validate_active(&active, devices)?;
        let test = state
            .last_successful_test
            .as_ref()
            .ok_or(ConfigError::CompletionRequired)?;
        validate_recording_evidence(&state.draft, test)?;
        state.active = Some(active);
        state.onboarding = OnboardingProgress {
            completed: true,
            last_completed_step: 6,
        };
        self.persist(&state)
    }

    pub fn recover(&self) -> Result<StoredState> {
        let _lock = self.lock()?;
        self.cleanup()?;
        let primary_path = self.checked(PRIMARY)?;
        match read_schema_version(&primary_path) {
            Ok(version) if version != CONFIG_SCHEMA_VERSION => {
                return Err(ConfigError::Schema(version));
            }
            Ok(_) | Err(ConfigError::Json(_) | ConfigError::Invalid(_)) => (),
            Err(ConfigError::Io(error)) if error.kind() == std::io::ErrorKind::NotFound => (),
            Err(error) => return Err(error),
        }
        let state = self.read(BACKUP)?;
        self.install(&state, PRIMARY, TEMP, &NativeIo)?;
        Ok(state)
    }

    fn persist(&self, state: &StoredState) -> Result<()> {
        self.persist_with_io(state, &NativeIo)
    }

    fn persist_with_io(&self, state: &StoredState, io: &impl PersistenceIo) -> Result<()> {
        validate_state(state)?;
        let mut normalized = state.clone();
        if has_legacy_overlay(&normalized) && crate::notifications::has_canonical_settings(&self.root) {
            retire_legacy_overlay(&mut normalized)?;
        }
        let state = &normalized;
        let prior = match self.read(PRIMARY) {
            Ok(value) => Some(value),
            Err(ConfigError::Io(e)) if e.kind() == std::io::ErrorKind::NotFound => None,
            Err(e) => return Err(e),
        };
        let first_completion =
            state.active.is_some() && prior.as_ref().is_none_or(|value| value.active.is_none());
        let backup = prior.as_ref().unwrap_or(state);
        let result = (|| {
            let temp = self.stage(state, TEMP, io)?;
            if !first_completion {
                self.install(backup, BACKUP, BACKUP_TEMP, io)?;
            }
            io.replace(&temp, &self.checked(PRIMARY)?)?;
            flush_directory(&self.root)?;
            if first_completion {
                // The primary transaction is already committed. Refreshing
                // redundancy is best-effort: a failure here must not report the
                // successfully committed primary as failed.
                if let Ok(committed) = self.read(PRIMARY) {
                    let _ = self.install(&committed, BACKUP, BACKUP_TEMP, io);
                }
            }
            Ok(())
        })();
        if result.is_err() {
            let _ = self.cleanup();
        }
        result
    }

    fn install(
        &self,
        state: &StoredState,
        destination: &str,
        temporary: &str,
        io: &impl PersistenceIo,
    ) -> Result<()> {
        let target = self.checked(destination)?;
        let result = (|| {
            let temp = self.stage(state, temporary, io)?;
            io.replace(&temp, &target)?;
            flush_directory(&self.root)?;
            Ok(())
        })();
        if result.is_err() {
            let _ = fs::remove_file(self.checked(temporary)?);
        }
        result
    }

    fn stage(
        &self,
        state: &StoredState,
        temporary: &str,
        io: &impl PersistenceIo,
    ) -> Result<PathBuf> {
        let temp = self.checked(temporary)?;
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&temp)?;
        file.write_all(&serde_json::to_vec_pretty(state)?)?;
        io.flush(&file)?;
        drop(file);
        let verified = read_state(&temp)?;
        if &verified != state {
            return Err(ConfigError::Invalid("readback"));
        }
        Ok(temp)
    }
}

fn read_state(path: &Path) -> Result<StoredState> {
    let bytes = read_bytes(path)?;
    let schema_version = schema_version_from_bytes(&bytes)?;
    if schema_version != CONFIG_SCHEMA_VERSION {
        return Err(ConfigError::Schema(schema_version));
    }
    let state = serde_json::from_slice(&bytes)?;
    validate_state(&state)?;
    Ok(state)
}

fn read_schema_version(path: &Path) -> Result<u32> {
    schema_version_from_bytes(&read_bytes(path)?)
}

fn read_bytes(path: &Path) -> Result<Vec<u8>> {
    let mut bytes = Vec::new();
    File::open(path)?
        .take(MAX_BYTES + 1)
        .read_to_end(&mut bytes)?;
    if bytes.len() as u64 > MAX_BYTES {
        return Err(ConfigError::Invalid("config_size"));
    }
    Ok(bytes)
}

fn schema_version_from_bytes(bytes: &[u8]) -> Result<u32> {
    #[derive(Deserialize)]
    struct SchemaProbe {
        schema_version: u32,
    }
    Ok(serde_json::from_slice::<SchemaProbe>(bytes)?.schema_version)
}

trait PersistenceIo {
    fn flush(&self, file: &File) -> std::io::Result<()>;
    fn replace(&self, from: &Path, to: &Path) -> std::io::Result<()>;
}

struct NativeIo;
impl PersistenceIo for NativeIo {
    fn flush(&self, file: &File) -> std::io::Result<()> {
        file.sync_all()
    }

    fn replace(&self, from: &Path, to: &Path) -> std::io::Result<()> {
        #[cfg(windows)]
        {
            use std::os::windows::ffi::OsStrExt;
            use windows_sys::Win32::Storage::FileSystem::{
                MoveFileExW, MOVEFILE_REPLACE_EXISTING, MOVEFILE_WRITE_THROUGH,
            };
            let from: Vec<u16> = from.as_os_str().encode_wide().chain(Some(0)).collect();
            let to: Vec<u16> = to.as_os_str().encode_wide().chain(Some(0)).collect();
            // Both names are fixed siblings under the canonical store root.
            if unsafe {
                MoveFileExW(
                    from.as_ptr(),
                    to.as_ptr(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
                )
            } == 0
            {
                return Err(std::io::Error::last_os_error());
            }
            Ok(())
        }
        #[cfg(not(windows))]
        {
            fs::rename(from, to)
        }
    }
}

fn flush_directory(root: &Path) -> std::io::Result<()> {
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        use windows_sys::Win32::Storage::FileSystem::FILE_FLAG_BACKUP_SEMANTICS;
        let result = OpenOptions::new()
            .read(true)
            .custom_flags(FILE_FLAG_BACKUP_SEMANTICS)
            .open(root)
            .and_then(|f| f.sync_all());
        // Windows/filesystems commonly do not permit FlushFileBuffers on a
        // directory handle. File sync and write-through replacement still apply.
        match result {
            Err(e) if matches!(e.raw_os_error(), Some(1 | 5 | 6 | 50)) => Ok(()),
            other => other,
        }
    }
    #[cfg(not(windows))]
    {
        File::open(root)?.sync_all()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;

    struct FailPrimaryFlush {
        root: PathBuf,
        saw_primary: Cell<bool>,
    }
    impl PersistenceIo for FailPrimaryFlush {
        fn flush(&self, _: &File) -> std::io::Result<()> {
            self.saw_primary.set(self.root.join(TEMP).is_file());
            Err(std::io::Error::other("injected flush failure"))
        }
        fn replace(&self, _: &Path, _: &Path) -> std::io::Result<()> {
            panic!("must not replace after failed flush")
        }
    }

    #[test]
    fn config_flushes_candidate_before_touching_backup() {
        let temp = tempfile::tempdir().unwrap();
        let store = ConfigStore::from_root_host(&temp.path().canonicalize().unwrap()).unwrap();
        store.save_draft(Default::default(), 0).unwrap();
        let before = fs::read(temp.path().join(PRIMARY)).unwrap();
        let backup_before = fs::read(temp.path().join(BACKUP)).unwrap();
        let mut candidate = StoredState::default();
        candidate.draft.width = Some(1920);
        let io = FailPrimaryFlush {
            root: temp.path().into(),
            saw_primary: Cell::new(false),
        };
        assert!(store.persist_with_io(&candidate, &io).is_err());
        assert!(
            io.saw_primary.get(),
            "candidate must be staged and flushed before backup rotation"
        );
        assert_eq!(fs::read(temp.path().join(PRIMARY)).unwrap(), before);
        assert_eq!(fs::read(temp.path().join(BACKUP)).unwrap(), backup_before);
        assert!(!temp.path().join(TEMP).exists());
        assert!(!temp.path().join(BACKUP_TEMP).exists());
    }

    struct CorruptPrimaryReadback {
        root: PathBuf,
    }
    impl PersistenceIo for CorruptPrimaryReadback {
        fn flush(&self, file: &File) -> std::io::Result<()> {
            file.sync_all()?;
            if self.root.join(TEMP).exists() {
                fs::write(self.root.join(TEMP), b"{")?;
            }
            Ok(())
        }
        fn replace(&self, _: &Path, _: &Path) -> std::io::Result<()> {
            panic!("must read back candidate before rotating backup")
        }
    }

    #[test]
    fn config_readback_failure_preserves_primary_and_backup() {
        let temp = tempfile::tempdir().unwrap();
        let store = ConfigStore::from_root_host(&temp.path().canonicalize().unwrap()).unwrap();
        store.save_draft(Default::default(), 0).unwrap();
        let before = fs::read(temp.path().join(PRIMARY)).unwrap();
        let backup_before = fs::read(temp.path().join(BACKUP)).unwrap();
        let mut candidate = StoredState::default();
        candidate.draft.width = Some(1920);
        assert!(store
            .persist_with_io(
                &candidate,
                &CorruptPrimaryReadback {
                    root: temp.path().into()
                }
            )
            .is_err());
        assert_eq!(fs::read(temp.path().join(PRIMARY)).unwrap(), before);
        assert_eq!(fs::read(temp.path().join(BACKUP)).unwrap(), backup_before);
        assert!(!temp.path().join(TEMP).exists());
    }
}
