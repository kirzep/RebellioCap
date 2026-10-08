use rebelliocap_desktop::engine::protocol::{decode_event, read_frame, Correlator, SessionCommand};
use std::io::Cursor;

#[test]
fn naming_reload_uses_native_command_and_finishes_on_ack() {
    let bytes = rebelliocap_desktop::engine::protocol::encode_request("names", SessionCommand::ReloadRecordingNames).unwrap();
    let request: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
    assert_eq!(request["command"], "reload_recording_names");
    let mut correlator = Correlator::default();
    correlator.begin("names", SessionCommand::ReloadRecordingNames).unwrap();
    let ack = decode_event(br#"{"protocolVersion":1,"type":"command_result","requestId":"names"}"#).unwrap();
    assert!(correlator.accept(&ack).unwrap());
}

#[test]
fn strict_framing_rejects_malformed_oversized_truncated_and_duplicate_fields() {
    for bytes in [b"{}".to_vec(), b"garbage\n".to_vec(), vec![b'x'; 65537],
        b"{\"protocolVersion\":1,\"protocolVersion\":1,\"type\":\"error\",\"error\":{\"code\":\"x\",\"message\":\"x\"}}\n".to_vec(),
        b"{\"protocolVersion\":2,\"type\":\"clip_saved\",\"outputPath\":\"C:\\\\x.mp4\"}\n".to_vec(),
        b"{\"protocolVersion\":1,\"type\":\"clip_saved\",\"outputPath\":\"C:\\\\x.mp4\",\"extra\":true}\n".to_vec(),
        b"\xff\n".to_vec()] {
        assert!(read_frame(&mut Cursor::new(bytes)).and_then(|v| decode_event(&v.unwrap())).is_err());
    }
}

#[test]
fn save_ack_does_not_complete_save_and_hotkeys_do_not_consume_pending_requests() {
    let mut c = Correlator::default();
    c.begin("r1", SessionCommand::SaveReplay).unwrap();
    let ack =
        decode_event(br#"{"protocolVersion":1,"type":"command_result","requestId":"r1"}"#).unwrap();
    assert!(!c.accept(&ack).unwrap());
    assert!(c.accept(&ack).is_err());
    let hotkey =
        decode_event(br#"{"protocolVersion":1,"type":"clip_saved","outputPath":"C:\\clip.mp4"}"#)
            .unwrap();
    assert!(!c.accept(&hotkey).unwrap());
    let done = decode_event(br#"{"protocolVersion":1,"type":"clip_saved","requestId":"r1","outputPath":"C:\\clip.mp4"}"#).unwrap();
    assert!(c.accept(&done).unwrap());
    assert!(c.accept(&done).is_err());
}

#[test]
fn eof_and_crlf_and_request_ids_are_bounded() {
    assert!(read_frame(&mut Cursor::new(b"")).unwrap().is_none());
    assert_eq!(
        read_frame(&mut Cursor::new(b"{}\r\n")).unwrap().unwrap(),
        b"{}"
    );
    let mut c = Correlator::default();
    assert!(c.begin("bad id", SessionCommand::Stop).is_err());
    assert!(c.begin(&"x".repeat(65), SessionCommand::Stop).is_err());
}

#[test]
fn duplicate_optional_null_fields_are_rejected() {
    assert!(decode_event(br#"{"protocolVersion":1,"type":"clip_saved","requestId":null,"requestId":"r1","outputPath":"C:\\clip.mp4"}"#).is_err());
}
