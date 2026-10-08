use serde::Serialize;
use std::{path::Path, sync::Arc};

#[derive(Clone, Copy, Serialize)]
#[serde(rename_all = "snake_case")]
pub(crate) enum Phase { Selecting, ReadingProject, Analyzing, CheckingCache, PreparingPreview, Publishing }
#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct Progress {
    owner: String,
    operation_id: String,
    phase: Phase,
    completed: usize,
    total: usize,
    #[serde(skip_serializing_if = "Option::is_none")]
    filename: Option<String>,
}
#[derive(Clone, Default)]
pub(crate) struct Reporter {
    target: Option<(String, String, Arc<dyn Fn(Progress) + Send + Sync>)>,
    completed: usize,
    total: usize,
    filename: Option<String>,
}
impl Reporter {
    pub(crate) fn new(owner: String, operation: String, sink: Arc<dyn Fn(Progress) + Send + Sync>) -> Result<Self, String> {
        if operation.is_empty() || operation.len() > 128 { return Err("editor.invalid_operation".into()); }
        Ok(Self { target: Some((owner, operation, sink)), ..Self::default() })
    }
    pub(crate) fn asset(&self, completed: usize, total: usize, path: &Path) -> Self {
        Self { completed: completed.min(total), total, filename: path.file_name().map(|name| name.to_string_lossy().chars().take(512).collect()), ..self.clone() }
    }
    pub(crate) fn report(&self, phase: Phase) {
        if let Some((owner, operation, sink)) = &self.target {
            sink(Progress { owner: owner.clone(), operation_id: operation.clone(), phase, completed: self.completed, total: self.total, filename: self.filename.clone() });
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn progress_has_operation_identity_and_basename_with_real_completed_count() {
        let events = Arc::new(std::sync::Mutex::new(Vec::new()));
        let captured = events.clone();
        let reporter = Reporter::new("owner".into(), "operation".into(), Arc::new(move |event| captured.lock().unwrap().push(event))).unwrap();
        reporter.report(Phase::Selecting);
        reporter.asset(1, 3, Path::new("C:/private/path/clip.mp4")).report(Phase::PreparingPreview);
        let events = events.lock().unwrap();
        let event = serde_json::to_value(&events[1]).unwrap();
        assert_eq!(event["owner"], "owner");
        assert_eq!(event["operationId"], "operation");
        assert_eq!(event["completed"], 1);
        assert_eq!(event["total"], 3);
        assert_eq!(event["filename"], "clip.mp4");
        assert_eq!(event["phase"], "preparing_preview");
        assert!(event.get("percent").is_none());
        assert!(serde_json::to_value(&events[0]).unwrap().get("filename").is_none());
        assert!(Reporter::new("owner".into(), "".into(), Arc::new(|_| {})).is_err());
    }
}
