use serde::{Deserialize,Serialize};
use std::{path::PathBuf,sync::{Arc,Mutex}};

const TOKENS:&[&str]=&["game","date","time","time_extended","counter","type","resolution","fps","year","month","day"];
#[derive(Clone,Debug,Serialize,Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Part {pub kind:String,pub value:String}
#[derive(Clone,Debug,Serialize,Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Preset {pub id:String,pub name:String,pub parts:Vec<Part>}
#[derive(Clone,Debug,Serialize,Deserialize)]
#[serde(rename_all="camelCase",deny_unknown_fields)]
pub struct Settings {pub active_id:String,pub presets:Vec<Preset>}
fn part(kind:&str,value:&str)->Part{Part{kind:kind.into(),value:value.into()}}
impl Default for Settings {
 fn default()->Self{Self{active_id:"shadowplay".into(),presets:vec![
 Preset{id:"shadowplay".into(),name:"NVIDIA ShadowPlay".into(),parts:vec![part("token","game"),part("text"," "),part("token","date"),part("text"," - "),part("token","time_extended"),part("text",".DVR")]},
 Preset{id:"rebcap".into(),name:"RebellioCap".into(),parts:vec![part("token","game"),part("text"," — "),part("token","date"),part("text"," "),part("token","time"),part("text"," — "),part("token","type"),part("text"," "),part("token","counter")]}
 ]}}
}
pub fn validate(settings:&Settings)->Result<(),String>{
 if settings.presets.is_empty()||settings.presets.len()>32||!settings.presets.iter().any(|p|p.id==settings.active_id){return Err("Выберите существующий пресет имени".into());}
 let mut ids=std::collections::HashSet::new();
 for p in &settings.presets{
  if p.id.is_empty()||p.id.len()>64||!ids.insert(&p.id)||p.name.trim().is_empty()||p.name.chars().count()>64||p.parts.is_empty()||p.parts.len()>64{return Err("Некорректный пресет имени".into());}
  let mut total=0;
  for part in &p.parts{total+=part.value.len();if part.value.len()>512||!(part.kind=="text"||(part.kind=="token"&&TOKENS.contains(&part.value.as_str()))){return Err("Используйте только готовые блоки имени".into());}}
  if total>2048||p.parts.iter().all(|p|p.kind=="text"&&p.value.trim().is_empty()){return Err("Имя записи не должно быть пустым или слишком длинным".into());}
 }
 Ok(())
}
#[derive(Clone)]
pub struct RecordingNamesState{settings:Arc<Mutex<Settings>>,path:PathBuf}
impl RecordingNamesState{
 pub fn new(root:PathBuf)->Self{let path=root.join("recording-names.json");let settings=std::fs::read(&path).ok().filter(|b|b.len()<=131072).and_then(|b|serde_json::from_slice::<Settings>(&b).ok()).filter(|s|validate(s).is_ok()).unwrap_or_default();Self{settings:Arc::new(Mutex::new(settings)),path}}
 fn save(&self,settings:Settings)->Result<(),String>{validate(&settings)?;let mut current=self.settings.lock().map_err(|e|e.to_string())?;let parent=self.path.parent().ok_or("naming.path")?;std::fs::create_dir_all(parent).map_err(|e|e.to_string())?;let file=tempfile::NamedTempFile::new_in(parent).map_err(|e|e.to_string())?;serde_json::to_writer(file.as_file(),&settings).map_err(|e|e.to_string())?;file.as_file().sync_all().map_err(|e|e.to_string())?;file.persist(&self.path).map_err(|e|e.to_string())?;*current=settings;Ok(())}
}
#[tauri::command]
pub fn get_recording_names(state:tauri::State<'_,RecordingNamesState>)->Result<Settings,String>{Ok(state.settings.lock().map_err(|e|e.to_string())?.clone())}
#[tauri::command]
pub async fn set_recording_names(state:tauri::State<'_,RecordingNamesState>,host:tauri::State<'_,crate::commands::HostState>,settings:Settings)->Result<(),String>{
 let names=state.inner().clone();
 crate::commands::dispatch(&host,move |host|{names.save(settings)?;host.reload_recording_names()}).await
}
#[cfg(test)]mod tests{
 use super::*;
 #[test]fn rejects_unknown_blocks_and_empty_names(){let mut s=Settings::default();s.presets[0].parts.push(part("token","executable_script"));assert!(validate(&s).is_err());s=Settings::default();s.presets[0].parts=vec![part("text"," ")];assert!(validate(&s).is_err());}
 #[test]fn saves_presets_and_restores_selection(){let dir=tempfile::tempdir().unwrap();let state=RecordingNamesState::new(dir.path().into());let mut s=Settings::default();s.active_id="rebcap".into();state.save(s).unwrap();let restored=RecordingNamesState::new(dir.path().into());assert_eq!(restored.settings.lock().unwrap().active_id,"rebcap");}
}
