mod support;
use rebelliocap_desktop::{
    commands::{apply_transaction, complete_transaction, validate_owned_file},
    config::*,
    model::*,
};

#[test]
fn completion_requires_explicit_action_and_matching_test_and_rolls_back_failed_start() {
    let tmp = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(tmp.path().into()).unwrap();
    let draft = support::draft(tmp.path());
    store.save_draft(draft.clone(), 5).unwrap();
    assert!(complete_transaction(&store, false, &support::devices(), |_| Ok(())).is_err());
    assert!(complete_transaction(&store, true, &support::devices(), |_| Ok(())).is_err());
    store
        .record_successful_test(RecordingTestSummary {
            fingerprint: draft.fingerprint().unwrap(),
            succeeded: true,
            clip_path: draft.output_directory.as_ref().unwrap().join("test.mp4"),
            video_packets: 20,
            audio_packets: 0,
        })
        .unwrap();
    assert!(
        complete_transaction(&store, true, &support::devices(), |_| Err(
            "startup failed".into()
        ))
        .is_err()
    );
    assert!(store.load().unwrap().active.is_none());
    assert!(!store.load().unwrap().onboarding.completed);
    complete_transaction(&store, true, &support::devices(), |_| Ok(())).unwrap();
    assert!(store.load().unwrap().onboarding.completed);
}

#[test]
fn failed_settings_start_restores_and_restarts_last_good() {
    let tmp = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(tmp.path().into()).unwrap();
    let old = support::draft(tmp.path()).to_active().unwrap();
    store
        .replace_host_state(&StoredState {
            active: Some(old.clone()),
            draft: old.as_draft(),
            onboarding: OnboardingProgress {
                completed: true,
                last_completed_step: 6,
            },
            ..Default::default()
        })
        .unwrap();
    let mut candidate = old.clone();
    candidate.fps = 120;
    let mut starts = Vec::new();
    let result = apply_transaction(
        &store,
        candidate,
        &support::devices(),
        || Ok(()),
        |c| {
            starts.push(c.fps);
            if c.fps == 120 {
                Err("candidate failed".into())
            } else {
                Ok(())
            }
        },
    );
    assert!(result.is_err());
    assert_eq!(starts, [120, 60]);
    assert_eq!(store.load().unwrap().active.unwrap(), old);
}

#[test]
fn open_path_requires_exact_host_allowlist_and_canonical_existing_file() {
    let tmp = tempfile::tempdir().unwrap();
    let path = tmp.path().join("RebellioCap-test.mp4");
    std::fs::write(&path, b"clip").unwrap();
    let canonical = path.canonicalize().unwrap();
    assert!(validate_owned_file(&canonical, std::slice::from_ref(&canonical)).is_ok());
    assert!(validate_owned_file(&canonical, &[]).is_err());
    assert!(validate_owned_file(&tmp.path().join("other.mp4"), &[canonical]).is_err());
}

#[test]
fn native_config_pins_explicit_audio_and_builds_hotkeys_without_renderer_arguments() {
    let tmp = tempfile::tempdir().unwrap();
    let c = support::draft(tmp.path()).to_active().unwrap();
    let value = rebelliocap_desktop::native::recording_test::engine_config(&c, None).unwrap();
    assert_eq!(value["systemAudioEnabled"], false);
    assert_eq!(value["microphoneEnabled"], false);
    assert_eq!(value["saveReplayHotkey"], "F8");
    assert_eq!(value["monitorId"], "monitor-1");
    assert_eq!(value["replayMemoryLimitMb"], 0);
    assert_eq!(value.as_object().unwrap().len(), 18);
}

#[test]
fn recording_evidence_requires_native_mux_success_matching_output_size_and_enabled_audio() {
    use rebelliocap_desktop::native::recording_test::validate_test_result;
    let tmp = tempfile::tempdir().unwrap();
    let mut draft = support::draft(tmp.path());
    let output = tmp
        .path()
        .canonicalize()
        .unwrap()
        .join("RebellioCap-test.mp4");
    std::fs::write(&output, b"valid clip bytes").unwrap();
    let mut result = serde_json::json!({"protocolVersion":1,"type":"test_result","passed":true,"initializationStages":["configuration","monitor","nvenc","audio","recorder","capture","mux","file_validation"],"durationSeconds":5,"videoPackets":42,"audioPackets":0,"missedFrames":0,"droppedFrames":0,"muxCompleted":true,"fileSize":16,"outputPath":output,"metrics":{"videoTicks":1,"missedVideoDeadlines":0,"videoPackets":42,"audioPackets":0,"saveRequests":1,"completedSaves":1,"failedSaves":0,"rejectedSaves":0,"pipelineErrors":0,"continuousPackets":0,"continuousFailures":0,"continuousRecordingActive":false,"lastHotkeySaveLatencyTicks":0,"replayBytes":0}});
    assert!(
        validate_test_result(&serde_json::to_vec(&result).unwrap(), true, &draft, &output).is_ok()
    );
    assert!(validate_test_result(
        &serde_json::to_vec(&result).unwrap(),
        false,
        &draft,
        &output
    )
    .is_err());
    draft.microphone = Some(AudioSelection::Endpoint("mic".into()));
    assert!(
        validate_test_result(&serde_json::to_vec(&result).unwrap(), true, &draft, &output).is_err()
    );
    draft.microphone = Some(AudioSelection::Disabled);
    result["muxCompleted"] = false.into();
    assert!(
        validate_test_result(&serde_json::to_vec(&result).unwrap(), true, &draft, &output).is_err()
    );
}

#[test]
fn recording_failure_preserves_native_error_code() {
    let tmp = tempfile::tempdir().unwrap();
    let error = rebelliocap_desktop::native::recording_test::validate_test_result(br#"{"protocolVersion":1,"type":"fatal_error","error":{"code":"mux.no_video_packets","message":"No video packets"}}"#, false, &support::draft(tmp.path()), &tmp.path().join("test.mp4")).unwrap_err();
    assert!(error.contains("mux.no_video_packets"), "{error}");
}

#[test]
fn unsupported_windows_and_native_architecture_are_rejected() {
    use rebelliocap_desktop::native::system_check::supported_platform;
    assert!(!supported_platform(6, 9));
    assert!(!supported_platform(10, 12));
    assert!(!supported_platform(10, 0));
    assert!(supported_platform(10, 9));
}

#[test]
fn escape_hotkey_matches_native_parser_spelling() {
    let tmp = tempfile::tempdir().unwrap();
    let mut config = support::draft(tmp.path()).to_active().unwrap();
    config.save_replay_hotkey.key = 0x1b;
    assert_eq!(
        rebelliocap_desktop::native::recording_test::engine_config(&config, None).unwrap()
            ["saveReplayHotkey"],
        "Escape"
    );
}

#[test]
fn doctor_does_not_fail_optional_missing_microphone() {
    use rebelliocap_desktop::native::system_check::validate_doctor_result;
    let tmp = tempfile::tempdir().unwrap();
    let mut draft = support::draft(tmp.path());
    let mut result = serde_json::json!({"protocolVersion":1,"type":"doctor","passed":false,"details":{"windows_x64":true,"os_version":"10.0.26200.0","gpu":"NVIDIA","driver_version":"1","monitor_count":1,"nvenc":true,"nvenc_max_major":13,"nvenc_max_minor":0,"h264":true,"nv12":true,"video_error":"","audio_endpoints_and_aac":false,"system_audio":true,"system_audio_error":"","microphone":false,"microphone_error":"audio.endpoint_unavailable","microphone_message":"No microphone","microphone_hresult":0,"aac":true,"aac_error":"","libavformat":1,"libavcodec":1,"libavutil":1,"passed":false}});
    result["details"]["monitor_id"] = serde_json::json!(draft.monitor_id);
    assert!(validate_doctor_result(&serde_json::to_vec(&result).unwrap(), false, &draft).is_ok());
    result["details"]["recording_color_space"] = "sdr_bt709".into();
    result["details"]["hdr_fidelity"] = false.into();
    result["details"]["hdr_tone_mapping"] = false.into();
    assert!(validate_doctor_result(&serde_json::to_vec(&result).unwrap(), false, &draft).is_ok());
    draft.monitor_id = Some("42:1".into());
    assert!(validate_doctor_result(&serde_json::to_vec(&result).unwrap(), false, &draft).is_err(), "unidentified primary GPU must not validate a selected monitor");
    result["details"]["monitor_id"] = "42:1".into();
    assert!(validate_doctor_result(&serde_json::to_vec(&result).unwrap(), false, &draft).is_ok());
    result["details"].as_object_mut().unwrap().remove("monitor_id");
    assert!(validate_doctor_result(&serde_json::to_vec(&result).unwrap(), false, &draft).is_err());
    result["details"]["monitor_id"] = "42:1".into();
    draft.microphone = Some(AudioSelection::Endpoint("mic".into()));
    assert!(validate_doctor_result(&serde_json::to_vec(&result).unwrap(), false, &draft).is_err());
    draft.microphone = Some(AudioSelection::Disabled);
    result["details"]["nvenc"] = false.into();
    assert!(validate_doctor_result(&serde_json::to_vec(&result).unwrap(), false, &draft).is_err());
}
