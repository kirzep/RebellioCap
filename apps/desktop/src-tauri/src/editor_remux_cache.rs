//! Process-local, bounded remux cache. Cache paths never enter WebView scope.
use std::{collections::VecDeque, fs::{File, OpenOptions}, io::Read, os::windows::{fs::OpenOptionsExt, io::AsRawHandle}, path::{Path, PathBuf}, sync::Arc};
use serde_json::Value;
type Result<T> = std::result::Result<T, String>;

#[cfg(test)]
pub(crate) fn source_identity(path: &Path) -> Result<(String, File)> { source_identity_checked(path, || Ok(())) }
pub(crate) fn source_identity_checked(path: &Path, check: impl Fn() -> Result<()>) -> Result<(String, File)> {
    use windows_sys::Win32::Storage::FileSystem::*;
    // Hold the read handle through preparation: no concurrent writes/deletion
    // can turn a cache key into a different source while the remux runs.
    check()?;
    let mut file = OpenOptions::new().read(true).share_mode(1).open(path).map_err(|e| e.to_string())?;
    let mut identity: BY_HANDLE_FILE_INFORMATION = unsafe { std::mem::zeroed() };
    let mut basic: FILE_BASIC_INFO = unsafe { std::mem::zeroed() };
    unsafe {
        if GetFileInformationByHandle(file.as_raw_handle(), &mut identity) == 0 ||
            GetFileInformationByHandleEx(file.as_raw_handle(), FileBasicInfo, &mut basic as *mut _ as *mut _, std::mem::size_of::<FILE_BASIC_INFO>() as u32) == 0 {
            return Err(std::io::Error::last_os_error().to_string());
        }
    }
    let canonical = path.canonicalize().map_err(|e| e.to_string())?;
    // NTFS can report identical timestamps for immediate same-size overwrites.
    // A checked streaming digest prevents such changes from reusing stale media.
    use sha2::{Digest, Sha256};
    let mut digest = Sha256::new();
    let mut bytes = [0u8; 64 * 1024];
    loop {
        check()?;
        let count = file.read(&mut bytes).map_err(|e| e.to_string())?;
        check()?;
        if count == 0 { break; }
        digest.update(&bytes[..count]);
    }
    Ok((format!("{}|{}:{}:{}|{}:{}|{}:{}:{}|{:x}", canonical.to_string_lossy(), identity.dwVolumeSerialNumber,
        identity.nFileIndexHigh, identity.nFileIndexLow, identity.nFileSizeHigh, identity.nFileSizeLow,
        basic.CreationTime, basic.LastWriteTime, basic.ChangeTime, digest.finalize()), file))
}

pub(crate) struct PreparedMedia { pub(crate) dir: tempfile::TempDir, metadata: Value, files: Vec<PathBuf>, bytes: u64 }
impl PreparedMedia {
    pub(crate) fn new(dir: tempfile::TempDir, metadata: Value) -> Result<Self> {
        let mut files = vec![PathBuf::from("video.mp4")];
        for track in metadata["tracks"].as_array().ok_or("editor.invalid_preview_tracks")? {
            let name = track["path"].as_str().ok_or("editor.invalid_preview_path")?;
            if name.contains(['/', '\\', ':']) || !name.starts_with("audio-") || Path::new(name).components().count() != 1 {
                return Err("editor.invalid_preview_path".into());
            }
            if files.iter().any(|file| file == Path::new(name)) { return Err("editor.duplicate_preview_path".into()); }
            files.push(PathBuf::from(name));
        }
        let mut bytes = 0u64;
        for name in &files {
            let info = std::fs::symlink_metadata(dir.path().join(name)).map_err(|e| e.to_string())?;
            use std::os::windows::fs::MetadataExt;
            if !info.is_file() || info.file_attributes() & 0x400 != 0 { return Err("editor.invalid_preview_file".into()); }
            bytes = bytes.checked_add(info.len()).ok_or("editor.preview_too_large")?;
        }
        Ok(Self { dir, metadata, files, bytes })
    }
    pub(crate) fn alias(&self, check: impl Fn() -> Result<()>) -> Result<(tempfile::TempDir, Value)> {
        check()?;
        let dir = tempfile::Builder::new().prefix("rebcap-editor-").tempdir().map_err(|e| e.to_string())?;
        for name in &self.files {
            check()?;
            let source = self.dir.path().join(name);
            let target = dir.path().join(name);
            if std::fs::hard_link(&source, &target).is_err() {
                crate::editor_preview::copy_checked(File::open(source).map_err(|e| e.to_string())?,
                    OpenOptions::new().write(true).create_new(true).open(target).map_err(|e| e.to_string())?, &check)?;
            }
        }
        check()?;
        Ok((dir, self.metadata.clone()))
    }
}

pub(crate) struct RemuxCache { entries: VecDeque<(String, Arc<PreparedMedia>, bool)>, bytes: u64, max_entries: usize, max_bytes: u64 }
impl Default for RemuxCache { fn default() -> Self { Self::with_limits(8, 512 * 1024 * 1024) } }
impl RemuxCache {
    fn with_limits(max_entries: usize, max_bytes: u64) -> Self { Self { entries: VecDeque::new(), bytes: 0, max_entries, max_bytes } }
    pub(crate) fn get(&mut self, key: &str) -> Option<Arc<PreparedMedia>> {
        let index = self.entries.iter().position(|(name, _, retired)| name == key && !retired)?;
        let entry = self.entries.remove(index)?;
        let media = entry.1.clone();
        self.entries.push_back(entry);
        Some(media)
    }
    #[cfg(test)]
    fn insert(&mut self, key: String, dir: tempfile::TempDir, metadata: Value) -> Result<bool> {
        Ok(self.insert_prepared(key, Arc::new(PreparedMedia::new(dir, metadata)?)))
    }
    pub(crate) fn insert_prepared(&mut self, key: String, media: Arc<PreparedMedia>) -> bool {
        if media.bytes > self.max_bytes || self.max_entries == 0 { return false; }
        // Failed Windows deletions retain both ownership and budget. Retry on
        // subsequent insertions; retired incomplete entries never produce hits.
        for index in (0..self.entries.len()).rev() {
            if self.entries[index].2 { self.retire(index); }
        }
        if let Some(index) = self.entries.iter().position(|(name, _, _)| name == &key) {
            if !self.retire(index) { return false; }
        }
        let mut index = 0;
        while (self.entries.len() >= self.max_entries || self.bytes > self.max_bytes - media.bytes) && index < self.entries.len() {
            if !self.retire(index) { index += 1; }
        }
        if self.entries.len() >= self.max_entries || self.bytes > self.max_bytes - media.bytes { return false; }
        self.bytes += media.bytes;
        self.entries.push_back((key, media, false));
        true
    }
    fn retire(&mut self, index: usize) -> bool {
        if Arc::strong_count(&self.entries[index].1) != 1 { return false; }
        self.entries[index].2 = true;
        let path = self.entries[index].1.dir.path();
        if path.exists() && std::fs::remove_dir_all(path).is_err() { return false; }
        self.bytes -= self.entries.remove(index).unwrap().1.bytes;
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn media(bytes: &[u8]) -> (tempfile::TempDir, serde_json::Value) {
        let dir = tempfile::tempdir().unwrap();
        std::fs::write(dir.path().join("video.mp4"), bytes).unwrap();
        std::fs::write(dir.path().join("audio-0.m4a"), bytes).unwrap();
        (dir, serde_json::json!({"tracks":[{"path":"audio-0.m4a","label":"audio"}]}))
    }
    #[test]
    fn locked_cache_file_prevents_untracked_eviction_and_cleanup_retries() {
        let mut cache = RemuxCache::with_limits(1, 100);
        let (dir, metadata) = media(b"locked");
        let original = dir.path().to_owned();
        cache.insert("first".into(), dir, metadata).unwrap();
        let entry = cache.get("first").unwrap();
        let (alias, _) = entry.alias(|| Ok(())).unwrap();
        drop(entry);
        let locked = OpenOptions::new().read(true).share_mode(1).open(original.join("video.mp4")).unwrap();
        let (dir, metadata) = media(b"new");
        assert!(!cache.insert("second".into(), dir, metadata).unwrap());
        assert!(original.exists());
        assert_eq!(cache.entries.len(), 1);
        assert_eq!(cache.bytes, 12);
        assert!(cache.get("first").is_none());
        drop(locked);
        let (dir, metadata) = media(b"new");
        assert!(cache.insert("second".into(), dir, metadata).unwrap());
        assert!(!original.exists());
        assert_eq!(cache.bytes, 6);
        assert_eq!(std::fs::read(alias.path().join("video.mp4")).unwrap(), b"locked");
    }
    #[test]
    fn cancellation_after_first_alias_file_preserves_cached_files() {
        let (dir, metadata) = media(b"media");
        let media = PreparedMedia::new(dir, metadata).unwrap();
        let checks = std::cell::Cell::new(0);
        let result = media.alias(|| {
            checks.set(checks.get() + 1);
            if checks.get() == 3 { Err("editor.operation_cancelled".into()) } else { Ok(()) }
        });
        assert_eq!(result.unwrap_err(), "editor.operation_cancelled");
        assert!(media.dir.path().join("video.mp4").exists());
        assert!(media.dir.path().join("audio-0.m4a").exists());
    }
    #[test]
    fn hits_create_independent_aliases_and_eviction_keeps_published_media() {
        let mut cache = RemuxCache::with_limits(1, 100);
        let (dir, metadata) = media(b"first");
        let original = dir.path().to_owned();
        cache.insert("first".into(), dir, metadata).unwrap();
        let entry = cache.get("first").unwrap();
        let (one, first) = entry.alias(|| Ok(())).unwrap();
        let (two, second) = entry.alias(|| Ok(())).unwrap();
        assert_ne!(one.path(), two.path());
        assert_eq!(first, second);
        drop(entry);
        let (dir, metadata) = media(b"second");
        cache.insert("second".into(), dir, metadata).unwrap();
        assert!(cache.get("first").is_none());
        assert!(!original.exists());
        assert_eq!(std::fs::read(one.path().join("video.mp4")).unwrap(), b"first");
        drop(one);
        assert_eq!(std::fs::read(two.path().join("audio-0.m4a")).unwrap(), b"first");
        assert!(cache.bytes <= 100);
    }
    #[test]
    fn size_limit_and_lru_bound_retained_files() {
        let mut cache = RemuxCache::with_limits(2, 20);
        for key in ["a", "b"] {
            let (dir, metadata) = media(b"12345");
            cache.insert(key.into(), dir, metadata).unwrap();
        }
        assert!(cache.get("a").is_some());
        let (dir, metadata) = media(b"12345");
        cache.insert("c".into(), dir, metadata).unwrap();
        assert!(cache.get("b").is_none());
        assert!(cache.get("a").is_some());
        let (dir, metadata) = media(&[0; 21]);
        let oversized = dir.path().to_owned();
        assert!(!cache.insert("large".into(), dir, metadata).unwrap());
        assert!(!oversized.exists());
        assert_eq!(cache.bytes, 20);
    }
    #[test]
    fn cancelled_alias_leaves_cache_intact() {
        let mut cache = RemuxCache::default();
        let (dir, metadata) = media(b"media");
        cache.insert("key".into(), dir, metadata).unwrap();
        let entry = cache.get("key").unwrap();
        assert_eq!(entry.alias(|| Err("editor.operation_cancelled".into())).unwrap_err(), "editor.operation_cancelled");
        assert!(entry.dir.path().join("video.mp4").exists());
    }
    #[test]
    fn source_identity_pins_file_and_invalidates_same_length_replacement() {
        let dir = tempfile::tempdir().unwrap();
        let source = dir.path().join("source.mp4");
        std::fs::write(&source, b"first").unwrap();
        let (first, guard) = source_identity(&source).unwrap();
        assert!(std::fs::write(&source, b"other").is_err());
        assert!(std::fs::remove_file(&source).is_err());
        assert_eq!(source_identity(&source).unwrap().0, first);
        drop(guard);
        std::fs::write(&source, b"other").unwrap();
        let (changed, guard) = source_identity(&source).unwrap();
        assert_ne!(changed, first);
        drop(guard);
        std::fs::remove_file(&source).unwrap();
        std::fs::write(&source, b"other").unwrap();
        assert_ne!(source_identity(&source).unwrap().0, changed);
    }
    #[test]
    fn malformed_track_paths_are_never_cached() {
        let mut cache = RemuxCache::default();
        let (dir, _) = media(b"media");
        assert!(cache.insert("key".into(), dir, serde_json::json!({"tracks":[{"path":"../original.mp4"}]})).is_err());
        assert!(cache.get("key").is_none());
    }
}
