use crate::{
    engine::{supervisor::run_once, Result},
    model::*,
};
use serde::Deserialize;
use std::{path::Path, time::Duration};

#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct Endpoint {
    id: String,
    name: String,
    default_console: bool,
}
#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct Catalog {
    protocol_version: u32,
    #[serde(rename = "type")]
    kind: String,
    monitors: Vec<MonitorChoice>,
    system_audio: Vec<Endpoint>,
    microphones: Vec<Endpoint>,
}
pub struct Catalogs {
    pub monitors: Vec<MonitorChoice>,
    pub audio: AudioCatalog,
}
impl Catalogs {
    pub fn inventory(&self) -> DeviceInventory {
        DeviceInventory {
            monitor_ids: self.monitors.iter().map(|v| v.id.clone()).collect(),
            system_audio_ids: self
                .audio
                .system_audio
                .iter()
                .map(|v| v.id.clone())
                .collect(),
            microphone_ids: self
                .audio
                .microphones
                .iter()
                .map(|v| v.id.clone())
                .collect(),
        }
    }
}
pub fn load(executable: &Path) -> Result<Catalogs> {
    let (bytes, success) = run_once(
        executable,
        &["catalog-json".into()],
        Duration::from_secs(15),
    )?;
    let result: Catalog = serde_json::from_slice(&bytes).map_err(|e| e.to_string())?;
    if !success || result.protocol_version != 1 || result.kind != "catalog" {
        return Err("catalog.incompatible_or_failed".into());
    }
    let mut ids = std::collections::HashSet::new();
    for id in result
        .monitors
        .iter()
        .map(|v| &v.id)
        .chain(result.system_audio.iter().map(|v| &v.id))
        .chain(result.microphones.iter().map(|v| &v.id))
    {
        if id.is_empty() || id.len() > 4096 || id.chars().any(char::is_control) || !ids.insert(id) {
            return Err("catalog.invalid_identity".into());
        }
    }
    fn choices(items: Vec<Endpoint>) -> Vec<AudioChoice> {
        items
            .into_iter()
            .map(|v| AudioChoice {
                id: v.id,
                name: v.name,
                is_default: v.default_console,
                available: true,
            })
            .collect()
    }
    Ok(Catalogs {
        monitors: result.monitors,
        audio: AudioCatalog {
            system_audio: choices(result.system_audio),
            microphones: choices(result.microphones),
        },
    })
}
