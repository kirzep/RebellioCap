//! Filesystem catalog and recovery eligibility; no Host mutex or UI work.
use std::path::Path;
use serde::Serialize;
use crate::engine::Result;

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct SavedClip {
    pub(crate) name: String,
    pub(crate) relative_path: String,
    pub(crate) folder: String,
    pub(crate) bytes: u64,
    pub(crate) modified_ms: u64,
    pub(crate) saved_ms: u64,
    pub(crate) recovery: bool,
}

pub(crate) fn recovery_extension(path: &Path) -> Option<String> {
    if !path.extension()?.to_str()?.eq_ignore_ascii_case("partial") { return None; }
    let stem = Path::new(path.file_stem()?);
    let extension = stem.extension()?.to_str()?;
    matches!(extension.to_ascii_lowercase().as_str(), "mp4" | "mkv").then(|| format!("{extension}.partial"))
}
pub(crate) fn is_clip(path: &Path) -> bool {
    path.extension().and_then(|ext| ext.to_str())
        .is_some_and(|ext| matches!(ext.to_ascii_lowercase().as_str(), "mp4" | "mkv")) || recovery_extension(path).is_some()
}
pub(crate) fn is_orphan_recording(path: &Path) -> bool {
    use std::os::windows::fs::OpenOptionsExt;
    // Excluding FILE_SHARE_WRITE rejects the recorder's live GENERIC_WRITE
    // handle without touching the file. Empty leftovers are hidden.
    std::fs::OpenOptions::new().read(true).share_mode(1).open(path)
        .and_then(|file| file.metadata()).is_ok_and(|metadata| metadata.len() > 0)
}

pub(crate) fn is_reparse(metadata: &std::fs::Metadata) -> bool {
    use std::os::windows::fs::MetadataExt;
    metadata.file_type().is_symlink() || metadata.file_attributes() & 0x400 != 0
}

fn timestamp_ms(date: std::io::Result<std::time::SystemTime>) -> u64 {
    date.ok().and_then(|date| date.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|date| date.as_millis().min(u64::MAX as u128) as u64).unwrap_or(0)
}

pub(crate) fn scan_clips(directory: &Path) -> Result<Vec<SavedClip>> {
    scan_clips_checked(directory, || Ok(()), 100_000, 16 * 1024 * 1024)
}

pub(crate) fn scan_clips_checked(
    directory: &Path,
    check: impl Fn() -> Result<()>,
    maximum_items: usize,
    maximum_bytes: usize,
) -> Result<Vec<SavedClip>> {
    check()?;
    let directory = std::fs::canonicalize(directory)
        .map_err(|e| format!("clips.folder_unavailable: {e}"))?;
    let mut clips = Vec::new();
    let mut folders = vec![(directory.clone(), None)];
    let mut index_bytes = 2 * std::mem::size_of::<(std::path::PathBuf, Option<String>)>() + directory.capacity();
    if index_bytes > maximum_bytes { return Err("clips.index_limit".into()); }
    for entry in std::fs::read_dir(&directory).map_err(|e| e.to_string())? {
        check()?;
        let entry = entry.map_err(|e| e.to_string())?;
        let path = entry.path();
        let metadata = match std::fs::symlink_metadata(&path) { Ok(value) => value, Err(_) => continue };
        if !metadata.is_dir() || is_reparse(&metadata) { continue; }
        let canonical = match std::fs::canonicalize(&path) { Ok(value) => value, Err(_) => continue };
        if canonical.parent() == Some(directory.as_path()) {
            if folders.len() >= 10_000 { return Err("clips.index_limit".into()); }
            let name = entry.file_name().to_string_lossy().into_owned();
            index_bytes = index_bytes.saturating_add(2 * std::mem::size_of::<(std::path::PathBuf, Option<String>)>())
                .saturating_add(canonical.capacity()).saturating_add(name.capacity());
            if index_bytes > maximum_bytes { return Err("clips.index_limit".into()); }
            folders.push((canonical, Some(name)));
        }
    }
    for (parent, folder) in folders {
        check()?;
        let entries = match std::fs::read_dir(&parent) { Ok(value) => value, Err(_) => continue };
        for entry in entries.flatten() {
            check()?;
            let path = entry.path();
            if !is_clip(&path) { continue; }
            let recovery = recovery_extension(&path).is_some();
            let metadata = match std::fs::symlink_metadata(&path) { Ok(value) => value, Err(_) => continue };
            if !metadata.is_file() || is_reparse(&metadata) { continue; }
            let canonical = match std::fs::canonicalize(&path) { Ok(value) => value, Err(_) => continue };
            if canonical.parent() != Some(parent.as_path()) { continue; }
            if recovery && !is_orphan_recording(&canonical) { continue; }
            let name = entry.file_name().to_string_lossy().into_owned();
            let modified_ms = timestamp_ms(metadata.modified());
            let saved_ms = metadata.created().or_else(|_| metadata.modified());
            let saved_ms = timestamp_ms(saved_ms);
            let folder_name = folder.clone().unwrap_or_else(|| "Desktop".into());
            let relative_path = folder.as_ref().map_or_else(|| name.clone(), |folder| format!("{folder}/{name}"));
            index_bytes = index_bytes.saturating_add(2 * std::mem::size_of::<SavedClip>())
                .saturating_add(name.capacity()).saturating_add(folder_name.capacity()).saturating_add(relative_path.capacity());
            if clips.len() >= maximum_items || index_bytes > maximum_bytes { return Err("clips.index_limit".into()); }
            clips.push(SavedClip {
                relative_path,
                name,
                folder: folder_name,
                bytes: metadata.len(),
                modified_ms,
                saved_ms,
                recovery,
            });
        }
    }
    check()?;
    clips.sort_by(|a, b| b.saved_ms.cmp(&a.saved_ms).then(a.relative_path.cmp(&b.relative_path)));
    check()?;
    Ok(clips)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};

    #[test]
    fn scan_cancels_between_entries_without_returning_partial_results() {
        let directory = tempfile::tempdir().unwrap();
        for n in 0..20 { std::fs::write(directory.path().join(format!("clip-{n}.mp4")), b"clip").unwrap(); }
        let checks = AtomicUsize::new(0);
        let error = scan_clips_checked(directory.path(), || {
            if checks.fetch_add(1, Ordering::Relaxed) >= 5 { Err("clips.cancelled".into()) } else { Ok(()) }
        }, 100, 1024 * 1024).unwrap_err();
        assert_eq!(error, "clips.cancelled");
        assert_eq!(std::fs::read_dir(directory.path()).unwrap().count(), 20);
    }

    #[test]
    fn scan_rejects_item_and_memory_limits_instead_of_truncating() {
        let directory = tempfile::tempdir().unwrap();
        for n in 0..3 { std::fs::write(directory.path().join(format!("clip-{n}.mp4")), b"clip").unwrap(); }
        assert_eq!(scan_clips_checked(directory.path(), || Ok(()), 2, 1024 * 1024).unwrap_err(), "clips.index_limit");
        assert_eq!(scan_clips_checked(directory.path(), || Ok(()), 100, 1).unwrap_err(), "clips.index_limit");
    }

    #[test]
    fn folder_only_catalog_also_obeys_memory_budget() {
        let directory = tempfile::tempdir().unwrap();
        std::fs::create_dir(directory.path().join("a-long-folder-name")).unwrap();
        assert_eq!(scan_clips_checked(directory.path(), || Ok(()), 100, 1).unwrap_err(), "clips.index_limit");
    }
}

