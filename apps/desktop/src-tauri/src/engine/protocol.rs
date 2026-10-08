use super::Result;
use crate::model::{EngineSnapshot, NativeError};
use serde::{Deserialize, Serialize};
use std::{collections::HashMap, io::BufRead};

pub const MAX_FRAME: usize = 65536;

/// Read at most one bounded NDJSON frame, without allocating an unbounded line.
pub fn read_frame(reader: &mut impl BufRead) -> Result<Option<Vec<u8>>> {
    let mut frame = Vec::new();
    loop {
        let bytes = reader.fill_buf().map_err(|e| e.to_string())?;
        if bytes.is_empty() {
            return if frame.is_empty() {
                Ok(None)
            } else {
                Err("protocol.truncated_message".into())
            };
        }
        let newline = bytes.iter().position(|b| *b == b'\n');
        let count = newline.map_or(bytes.len(), |p| p + 1);
        if frame.len() + count > MAX_FRAME + 1 {
            return Err("protocol.message_too_large".into());
        }
        frame.extend_from_slice(&bytes[..count]);
        reader.consume(count);
        if newline.is_some() {
            frame.pop();
            if frame.last() == Some(&b'\r') {
                frame.pop();
            }
            if frame.len() > MAX_FRAME
                || frame.is_empty()
                || frame.contains(&0)
                || frame.contains(&b'\r')
            {
                return Err("protocol.invalid_frame".into());
            }
            std::str::from_utf8(&frame).map_err(|_| "protocol.invalid_utf8")?;
            return Ok(Some(frame));
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum SessionCommand {
    SaveReplay,
    ToggleRecording,
    StartReplay,
    StopReplay,
    Stop,
    GetSnapshot,
    ReloadRecordingNames,
}
#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum EventType {
    Ready,
    Snapshot,
    CommandResult,
    ClipSaved,
    Error,
    FatalError,
}
#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Event {
    pub protocol_version: u32,
    #[serde(rename = "type")]
    pub kind: EventType,
    pub request_id: Option<String>,
    pub snapshot: Option<EngineSnapshot>,
    pub error: Option<NativeError>,
    pub output_path: Option<String>,
}
fn valid_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 64
        && id
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-')
}
pub fn decode_event(bytes: &[u8]) -> Result<Event> {
    if bytes.len() > MAX_FRAME
        || bytes.contains(&0)
        || bytes.contains(&b'\n')
        || bytes.contains(&b'\r')
    {
        return Err("protocol.invalid_frame".into());
    }
    // Deriving Deserialize on each nested structure rejects duplicate fields as well as unknown keys.
    let e: Event =
        serde_json::from_slice(bytes).map_err(|e| format!("protocol.invalid_event: {e}"))?;
    if e.protocol_version != 1 || e.request_id.as_deref().is_some_and(|id| !valid_id(id)) {
        return Err("protocol.incompatible_version_or_id".into());
    }
    match e.kind {
        EventType::Ready | EventType::Snapshot if e.snapshot.is_none() => {
            return Err("protocol.missing_snapshot".into())
        }
        EventType::CommandResult if e.request_id.is_none() => {
            return Err("protocol.missing_request_id".into())
        }
        EventType::ClipSaved if e.output_path.is_none() || e.error.is_some() => {
            return Err("protocol.invalid_clip".into())
        }
        EventType::Error | EventType::FatalError if e.error.is_none() => {
            return Err("protocol.missing_error".into())
        }
        _ => (),
    }
    Ok(e)
}

#[derive(Default)]
pub struct Correlator {
    requests: HashMap<String, (SessionCommand, bool, bool)>,
}
impl Correlator {
    pub fn begin(&mut self, id: &str, command: SessionCommand) -> Result<()> {
        if !valid_id(id) || self.requests.len() >= 65536 || self.requests.contains_key(id) {
            return Err("protocol.invalid_or_duplicate_request_id".into());
        }
        self.requests.insert(id.into(), (command, false, false));
        Ok(())
    }
    pub fn accept(&mut self, event: &Event) -> Result<bool> {
        let Some(id) = &event.request_id else {
            return Ok(false);
        };
        let state = self
            .requests
            .get_mut(id)
            .ok_or("protocol.unknown_response_id")?;
        if state.2 {
            return Err("protocol.duplicate_response_id".into());
        }
        match event.kind {
            EventType::Snapshot => Ok(false),
            EventType::CommandResult => {
                if state.1 {
                    return Err("protocol.duplicate_response_id".into());
                }
                state.1 = true;
                state.2 = state.0 != SessionCommand::SaveReplay || event.error.is_some();
                Ok(state.2)
            }
            EventType::ClipSaved | EventType::Error => {
                if event.kind == EventType::ClipSaved
                    && (state.0 != SessionCommand::SaveReplay || !state.1)
                {
                    return Err("protocol.unexpected_completion".into());
                }
                state.2 = true;
                Ok(true)
            }
            _ => Err("protocol.unexpected_response".into()),
        }
    }
}

pub fn encode_request(id: &str, command: SessionCommand) -> Result<Vec<u8>> {
    if !valid_id(id) {
        return Err("protocol.invalid_request_id".into());
    }
    #[derive(Serialize)]
    #[serde(rename_all = "camelCase")]
    struct Request<'a> {
        protocol_version: u32,
        request_id: &'a str,
        #[serde(rename = "type")]
        kind: &'static str,
        command: SessionCommand,
    }
    let mut bytes = serde_json::to_vec(&Request {
        protocol_version: 1,
        request_id: id,
        kind: "command",
        command,
    })
    .map_err(|e| e.to_string())?;
    bytes.push(b'\n');
    Ok(bytes)
}
