//! Owner-scoped immutable indexes. Scan permits bound work before spawn_blocking.
use crate::{clip_catalog::SavedClip, engine::Result};
use serde::{Deserialize, Serialize};
use std::{collections::HashMap, sync::{Arc, Mutex, atomic::{AtomicU64, AtomicUsize, Ordering}}, time::{Duration, Instant}};

#[derive(Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PageRequest {
    pub owner: String,
    pub query: String,
    pub folder: Option<String>,
    pub folders: bool,
    pub page: usize,
    pub limit: usize,
    pub refresh: bool,
}
impl PageRequest {
    pub fn validate(&self) -> Result<()> {
        if self.owner.is_empty() || self.owner.len() > 128 || self.query.len() > 512 || self.folder.as_ref().is_some_and(|f| f.len() > 1024) || !(1..=60).contains(&self.limit) || self.page > 100_000 {
            return Err("clips.invalid_request".into());
        }
        Ok(())
    }
}
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ClipFolder { pub name: String, pub count: usize, pub latest: SavedClip }
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ClipPage {
    pub items: Vec<SavedClip>, pub folders: Vec<ClipFolder>, pub total_clips: usize,
    pub total_matches: usize, pub page: usize, pub page_count: usize,
}
pub(crate) fn page(clips: &[SavedClip], request: &PageRequest) -> Result<ClipPage> {
    request.validate()?;
    let query = request.query.trim().to_lowercase();
    let matches = |clip: &SavedClip| clip.name.to_lowercase().contains(&query) || clip.folder.to_lowercase().contains(&query);
    let (mut items, mut folders) = (Vec::new(), Vec::new());
    let total_matches;
    let selected_page;
    let page_count;
    if request.folders {
        let mut groups: HashMap<&str, (usize, usize, bool)> = HashMap::new();
        for (index, clip) in clips.iter().enumerate() {
            let group = groups.entry(&clip.folder).or_insert((0, index, false));
            group.0 += 1; group.2 |= matches(clip);
        }
        let mut groups: Vec<_> = groups.into_iter().filter(|(_, g)| g.2).collect();
        groups.sort_by_key(|(_, g)| g.1);
        total_matches = groups.len(); page_count = total_matches.div_ceil(request.limit).max(1);
        selected_page = request.page.min(page_count - 1);
        folders = groups.into_iter().skip(selected_page * request.limit).take(request.limit)
            .map(|(name, (count, index, _))| ClipFolder { name: name.into(), count, latest: clips[index].clone() }).collect();
    } else {
        let matched = || clips.iter().filter(|clip| matches(clip) && request.folder.as_ref().is_none_or(|folder| folder == &clip.folder));
        total_matches = matched().count(); page_count = total_matches.div_ceil(request.limit).max(1);
        selected_page = request.page.min(page_count - 1);
        items = matched().skip(selected_page * request.limit).take(request.limit).cloned().collect();
    }
    Ok(ClipPage { items, folders, total_clips: clips.len(), total_matches, page: selected_page, page_count })
}
#[derive(Default)]
struct Owner { epoch: AtomicU64, index: Mutex<Option<(std::path::PathBuf, Arc<Vec<SavedClip>>)>> }
#[derive(Default)]
pub struct CatalogState { owners: Mutex<HashMap<String, Arc<Owner>>>, active_scans: Arc<AtomicUsize> }
pub(crate) struct Ticket { owner: Arc<Owner>, expected: u64, deadline: Instant }
impl Ticket {
    pub fn check(&self) -> Result<()> {
        if self.owner.epoch.load(Ordering::Acquire) != self.expected { return Err("clips.cancelled".into()); }
        if Instant::now() >= self.deadline { return Err("clips.timeout".into()); }
        Ok(())
    }
    pub fn existing(&self, directory: &std::path::Path) -> Result<Option<Arc<Vec<SavedClip>>>> {
        self.check()?;
        Ok(self.owner.index.lock().map_err(|e| e.to_string())?.as_ref().filter(|(root, _)| root == directory).map(|(_, index)| index.clone()))
    }
    pub fn publish(&self, directory: std::path::PathBuf, index: Arc<Vec<SavedClip>>) -> Result<()> {
        let mut destination = self.owner.index.lock().map_err(|e| e.to_string())?;
        self.check()?; *destination = Some((directory, index)); Ok(())
    }
}
pub(crate) struct ScanPermit(Arc<AtomicUsize>);
impl Drop for ScanPermit { fn drop(&mut self) { self.0.fetch_sub(1, Ordering::AcqRel); } }
impl CatalogState {
    pub fn register(&self, name: &str) -> Result<()> {
        if name.is_empty() || name.len() > 128 { return Err("clips.invalid_owner".into()); }
        let mut owners = self.owners.lock().map_err(|e| e.to_string())?;
        if !owners.contains_key(name) && owners.len() >= 8 { return Err("clips.too_many_sessions".into()); }
        owners.entry(name.into()).or_default();
        Ok(())
    }
    pub(crate) fn begin(&self, name: &str) -> Result<Ticket> {
        // Keep registration locked through epoch assignment, so release cannot
        // invalidate the owner just before a late request assigns a new epoch.
        let owners = self.owners.lock().map_err(|e| e.to_string())?;
        let owner = owners.get(name).cloned().ok_or("clips.invalid_owner")?;
        let expected = owner.epoch.fetch_add(1, Ordering::AcqRel) + 1;
        drop(owners);
        Ok(Ticket { owner, expected, deadline: Instant::now() + Duration::from_secs(60) })
    }
    pub fn release(&self, name: &str) -> Result<()> {
        if let Some(owner) = self.owners.lock().map_err(|e| e.to_string())?.remove(name) {
            owner.epoch.fetch_add(1, Ordering::AcqRel);
            owner.index.lock().map_err(|e| e.to_string())?.take();
        }
        Ok(())
    }
    pub(crate) fn scan_permit(&self) -> Result<ScanPermit> {
        self.active_scans.fetch_update(Ordering::AcqRel, Ordering::Acquire, |n| (n < 2).then_some(n + 1))
            .map_err(|_| "clips.busy".to_string())?;
        Ok(ScanPermit(self.active_scans.clone()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn clip(n: usize) -> SavedClip {
        SavedClip { name: format!("clip-{n}.mp4"), relative_path: format!("Game-{}/clip-{n}.mp4", n % 10), folder: format!("Game-{}", n % 10), bytes: 1, modified_ms: n as u64, saved_ms: n as u64, recovery: false }
    }
    fn request(owner: &str) -> PageRequest {
        PageRequest { owner: owner.into(), query: String::new(), folder: None, folders: false, page: 0, limit: 60, refresh: false }
    }
    #[test]
    fn page_search_and_folders_use_the_whole_index_with_bounded_responses() {
        let mut clips: Vec<_> = (0..10_000).map(clip).collect();
        clips.reverse();
        let first = page(&clips, &request("one")).unwrap();
        assert_eq!(first.items.len(), 60); assert_eq!(first.total_clips, 10_000); assert_eq!(first.page_count, 167);
        assert_eq!(first.items[0].name, "clip-9999.mp4");
        let mut query = request("one"); query.query = "clip-0.mp4".into();
        let found = page(&clips, &query).unwrap(); assert_eq!(found.items.len(), 1); assert_eq!(found.items[0].name, "clip-0.mp4");
        query.query.clear(); query.folders = true;
        let folders = page(&clips, &query).unwrap(); assert!(folders.items.is_empty()); assert_eq!(folders.folders.len(), 10); assert!(folders.folders.iter().all(|f| f.count == 1000));
        query.folders = false; query.folder = Some("Game-1".into()); query.page = 999;
        let last = page(&clips, &query).unwrap(); assert_eq!(last.total_matches, 1000); assert_eq!(last.page, 16); assert_eq!(last.items.len(), 40);
    }
    #[test]
    fn released_or_superseded_owner_cannot_publish_and_scans_are_bounded() {
        let state = CatalogState::default();
        state.register("one").unwrap();
        let old = state.begin("one").unwrap(); let newer = state.begin("one").unwrap();
        assert_eq!(old.check().unwrap_err(), "clips.cancelled"); assert!(newer.check().is_ok());
        assert_eq!(old.publish(std::path::PathBuf::from("old"), Arc::new(vec![clip(1)])).unwrap_err(), "clips.cancelled");
        newer.publish(std::path::PathBuf::from("current"), Arc::new(vec![clip(2)])).unwrap();
        assert!(newer.existing(std::path::Path::new("different")).unwrap().is_none());
        assert_eq!(newer.existing(std::path::Path::new("current")).unwrap().unwrap()[0].name, "clip-2.mp4");
        state.release("one").unwrap(); assert_eq!(newer.check().unwrap_err(), "clips.cancelled");
        assert_eq!(newer.publish(std::path::PathBuf::from("current"), Arc::new(vec![])).unwrap_err(), "clips.cancelled");
        let first = state.scan_permit().unwrap(); let second = state.scan_permit().unwrap();
        assert_eq!(state.scan_permit().err().unwrap(), "clips.busy"); drop(first); assert!(state.scan_permit().is_ok()); drop(second);
    }
    #[test]
    fn delayed_request_cannot_recreate_a_released_owner() {
        let state = CatalogState::default();
        assert_eq!(state.begin("unregistered").err().unwrap(), "clips.invalid_owner");
        state.register("one").unwrap(); state.release("one").unwrap();
        assert_eq!(state.begin("one").err().unwrap(), "clips.invalid_owner");
    }
    #[test]
    fn requests_reject_unbounded_pages_and_owner_count() {
        let mut query = request("one"); query.limit = 61; assert!(page(&[], &query).is_err());
        query.limit = 60; query.query = "x".repeat(513); assert!(page(&[], &query).is_err());
        let state = CatalogState::default();
        for n in 0..8 { state.register(&format!("owner-{n}")).unwrap(); }
        assert_eq!(state.register("ninth").err().unwrap(), "clips.too_many_sessions");
    }
    #[test]
    fn native_scan_indexes_ten_thousand_files_and_searches_the_last_entry() {
        let directory = tempfile::tempdir().unwrap();
        let game = directory.path().join("Game"); std::fs::create_dir(&game).unwrap();
        for n in 0..10_000 { std::fs::write(game.join(format!("clip-{n}.mp4")), b"catalog fixture").unwrap(); }
        let clips = crate::clip_catalog::scan_clips(directory.path()).unwrap();
        assert_eq!(clips.len(), 10_000);
        let first = page(&clips, &request("one")).unwrap(); assert_eq!(first.items.len(), 60); assert_eq!(first.page_count, 167);
        let mut query = request("one"); query.query = "clip-9999.mp4".into();
        let found = page(&clips, &query).unwrap(); assert_eq!(found.items.len(), 1); assert_eq!(found.items[0].relative_path, "Game/clip-9999.mp4");
    }
}
