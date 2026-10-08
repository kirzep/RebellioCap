use rebelliocap_desktop::config::*;
use rebelliocap_desktop::model::*;
use std::{fs, path::Path};

fn draft(path: &Path) -> OnboardingDraft {
    OnboardingDraft {
        monitor_id: Some("monitor-stable-1".into()),
        system_audio: Some(AudioSelection::Endpoint("render-stable-1".into())),
        microphone: Some(AudioSelection::Disabled),
        width: Some(1920),
        height: Some(1080),
        fps: Some(60),
        bitrate: Some(30_000_000),
        replay_seconds: Some(30),
        replay_memory_limit_mb: None,
        replay_mode: Some(ReplayMode::Ram),
        container: Some(Container::Mp4),
        output_directory: Some(path.canonicalize().unwrap()),
        save_replay_hotkey: Some(Hotkey {
            key: 0x77,
            ctrl: false,
            alt: false,
            shift: false,
            win: false,
        }),
        toggle_recording_hotkey: Some(Hotkey {
            key: 0x52,
            ctrl: true,
            alt: false,
            shift: true,
            win: false,
        }),
        preferences: Some(Preferences {
            overlay_enabled: None,
            start_with_windows: false,
        }),
        continuous_recording_enabled: Some(false),
    }
}

fn devices() -> DeviceInventory {
    DeviceInventory {
        monitor_ids: vec!["monitor-stable-1".into()],
        system_audio_ids: vec!["render-stable-1".into()],
        microphone_ids: vec![],
    }
}

fn tested(store: &ConfigStore, value: &OnboardingDraft) {
    store.save_draft(value.clone(), 5).unwrap();
    store
        .record_successful_test(RecordingTestSummary {
            fingerprint: value.fingerprint().unwrap(),
            succeeded: true,
            clip_path: value.output_directory.as_ref().unwrap().join("test.mp4"),
            video_packets: 120,
            audio_packets: 90,
        })
        .unwrap();
}

#[test]
fn config_missing_state_is_incomplete() {
    let temp = tempfile::tempdir().unwrap();
    let loaded = ConfigStore::at(temp.path().into()).unwrap().load().unwrap();
    assert_eq!(loaded.schema_version, 1);
    assert!(!loaded.onboarding.completed);
    assert!(loaded.active.is_none());
}

#[test]
fn legacy_overlay_migration_preserves_verified_onboarding_evidence_and_restart() {
    let temp=tempfile::tempdir().unwrap();
    let store=ConfigStore::at(temp.path().into()).unwrap();
    let mut value=draft(temp.path());
    value.preferences.as_mut().unwrap().overlay_enabled=Some(false);
    tested(&store,&value);
    store.commit_completed(true,&devices()).unwrap();
    let old=store.load().unwrap();
    assert!(old.active.as_ref().unwrap().preferences.overlay_enabled.is_some());
    let old_fingerprint=old.last_successful_test.as_ref().unwrap().fingerprint.clone();
    let _notifications=rebelliocap_desktop::notifications::NotificationState::new(temp.path().to_path_buf());
    let migrated=store.load().unwrap();
    assert!(migrated.onboarding.completed);
    assert!(migrated.active.as_ref().unwrap().preferences.overlay_enabled.is_none());
    assert!(migrated.draft.preferences.as_ref().unwrap().overlay_enabled.is_none());
    let evidence=migrated.last_successful_test.as_ref().unwrap();
    assert_eq!(evidence.fingerprint,migrated.draft.fingerprint().unwrap());
    assert_ne!(evidence.fingerprint,old_fingerprint);
    assert_eq!(evidence.video_packets,120);
    assert_eq!(evidence.audio_packets,90);
    store.commit_completed(true,&devices()).unwrap();
    let document:serde_json::Value=serde_json::from_slice(&fs::read(temp.path().join("config.json")).unwrap()).unwrap();
    assert!(document["draft"]["preferences"].get("overlay_enabled").is_none());
    assert!(document["active"]["preferences"].get("overlay_enabled").is_none());
    let before=fs::read(temp.path().join("config.json")).unwrap();
    let _restarted=rebelliocap_desktop::notifications::NotificationState::new(temp.path().to_path_buf());
    assert_eq!(store.load().unwrap(),migrated);
    assert_eq!(fs::read(temp.path().join("config.json")).unwrap(),before);
    let settings:serde_json::Value=serde_json::from_slice(&fs::read(temp.path().join("notifications.json")).unwrap()).unwrap();
    assert_eq!(settings["overlayEnabled"],false);
}

#[test]
fn notification_migration_cannot_promote_invalid_recording_evidence() {
    let temp=tempfile::tempdir().unwrap();
    let store=ConfigStore::at(temp.path().into()).unwrap();
    let mut value=draft(temp.path());
    value.preferences.as_mut().unwrap().overlay_enabled=Some(false);
    let mut state=StoredState::default();
    state.draft=value;
    state.last_successful_test=Some(RecordingTestSummary{
        fingerprint:"0".repeat(64),succeeded:true,clip_path:temp.path().canonicalize().unwrap().join("test.mp4"),video_packets:120,audio_packets:90
    });
    let bytes=serde_json::to_vec(&state).unwrap();
    fs::write(temp.path().join("config.json"),&bytes).unwrap();
    let _notifications=rebelliocap_desktop::notifications::NotificationState::new(temp.path().to_path_buf());
    assert!(store.load().is_err());
    assert_eq!(fs::read(temp.path().join("config.json")).unwrap(),bytes);
}

#[test]
#[cfg(windows)]
fn locked_config_migration_keeps_valid_state_and_retries_after_unlock() {
    use std::os::windows::fs::OpenOptionsExt;
    let temp=tempfile::tempdir().unwrap();
    let store=ConfigStore::at(temp.path().into()).unwrap();
    let mut value=draft(temp.path());
    value.preferences.as_mut().unwrap().overlay_enabled=Some(false);
    store.save_draft(value,1).unwrap();
    let _notifications=rebelliocap_desktop::notifications::NotificationState::new(temp.path().to_path_buf());
    let path=temp.path().join("config.json");
    let before=fs::read(&path).unwrap();
    let held=fs::OpenOptions::new().read(true).share_mode(1).open(&path).unwrap();
    let loaded=store.load().unwrap();
    assert_eq!(loaded.draft.preferences.unwrap().overlay_enabled,Some(false));
    assert_eq!(fs::read(&path).unwrap(),before);
    drop(held);
    assert!(store.load().unwrap().draft.preferences.unwrap().overlay_enabled.is_none());
}

#[test]
fn config_incomplete_draft_resumes_without_becoming_active() {
    let temp = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    let value = OnboardingDraft {
        monitor_id: Some("monitor-stable-1".into()),
        ..Default::default()
    };
    store.save_draft(value.clone(), 1).unwrap();
    let loaded = store.load().unwrap();
    assert_eq!(loaded.draft, value);
    assert_eq!(loaded.onboarding.last_completed_step, 1);
    assert!(loaded.active.is_none());
    assert!(store.commit_completed(true, &devices()).is_err());
}

#[test]
fn config_rejects_invalid_schema_without_overwriting_it() {
    let temp = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    let mut value = serde_json::to_value(StoredState::default()).unwrap();
    value["schema_version"] = 999.into();
    let bytes = serde_json::to_vec(&value).unwrap();
    fs::write(temp.path().join("config.json"), &bytes).unwrap();
    assert!(store.load().is_err());
    assert!(store.save_draft(Default::default(), 0).is_err());
    assert_eq!(fs::read(temp.path().join("config.json")).unwrap(), bytes);
}

#[test]
fn config_completion_requires_success_matching_fingerprint_and_explicit_action() {
    let temp = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    let mut value = draft(temp.path());
    store.save_draft(value.clone(), 5).unwrap();
    assert!(store.commit_completed(true, &devices()).is_err());
    tested(&store, &value);
    assert!(store.commit_completed(false, &devices()).is_err());
    assert!(store.load().unwrap().active.is_none());
    value.bitrate = Some(40_000_000);
    store.save_draft(value.clone(), 5).unwrap();
    assert!(store.commit_completed(true, &devices()).is_err());
    tested(&store, &value);
    store.commit_completed(true, &devices()).unwrap();
    let loaded = store.load().unwrap();
    assert!(loaded.onboarding.completed);
    assert_eq!(loaded.onboarding.last_completed_step, 6);
    assert!(loaded.active.unwrap().onboarding_completed);
}

#[test]
fn config_failed_or_stale_test_cannot_authorize_completion() {
    let temp = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    let value = draft(temp.path());
    store.save_draft(value.clone(), 5).unwrap();
    for (fingerprint, succeeded, packets) in [
        (value.fingerprint().unwrap(), false, 120),
        ("stale".into(), true, 120),
        (value.fingerprint().unwrap(), true, 0),
    ] {
        assert!(store
            .record_successful_test(RecordingTestSummary {
                fingerprint,
                succeeded,
                clip_path: temp.path().join("test.mp4"),
                video_packets: packets,
                audio_packets: 90
            })
            .is_err());
    }
    assert!(store.commit_completed(true, &devices()).is_err());
}

#[test]
fn config_invalid_fields_do_not_replace_valid_draft() {
    let temp = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    let value = draft(temp.path());
    store.save_draft(value.clone(), 5).unwrap();
    let mut invalid = value.clone();
    invalid.fps = Some(0);
    assert!(store.save_draft(invalid, 5).is_err());
    assert!(store.save_draft(value.clone(), 7).is_err());
    assert_eq!(store.load().unwrap().draft, value);
}

#[test]
fn config_invalid_output_paths_are_rejected() {
    let temp = tempfile::tempdir().unwrap();
    let mut value = draft(temp.path());
    fs::write(temp.path().join("file"), "not a directory").unwrap();
    for path in [
        "relative".into(),
        temp.path().join("missing"),
        temp.path().join("file"),
    ] {
        value.output_directory = Some(path);
        assert!(validate_draft(&value).is_err());
    }
}

#[test]
fn config_accepts_clean_and_verbatim_output_paths() {
    let temp = tempfile::tempdir().unwrap();
    let mut value = draft(temp.path());
    let clean = strip_verbatim_prefix(&temp.path().canonicalize().unwrap());
    value.output_directory = Some(clean);
    assert!(validate_draft(&value).is_ok());

    let verbatim = temp.path().canonicalize().unwrap();
    value.output_directory = Some(verbatim);
    assert!(validate_draft(&value).is_ok());
}

#[test]
fn config_duplicate_and_reserved_hotkeys_are_rejected() {
    let temp = tempfile::tempdir().unwrap();
    let mut value = draft(temp.path());
    value.toggle_recording_hotkey = value.save_replay_hotkey.clone();
    assert!(validate_draft(&value).is_err());
    for hotkey in [
        Hotkey {
            key: 0x73,
            alt: true,
            ctrl: false,
            shift: false,
            win: false,
        },
        Hotkey {
            key: 0x09,
            alt: true,
            ctrl: false,
            shift: false,
            win: false,
        },
        Hotkey {
            key: 0x2e,
            alt: true,
            ctrl: true,
            shift: false,
            win: false,
        },
        Hotkey {
            key: 0x4c,
            alt: false,
            ctrl: false,
            shift: false,
            win: true,
        },
        Hotkey {
            key: 0,
            alt: false,
            ctrl: false,
            shift: false,
            win: false,
        },
    ] {
        value = draft(temp.path());
        value.save_replay_hotkey = Some(hotkey);
        assert!(validate_draft(&value).is_err());
    }
}

#[test]
fn config_vanished_identity_blocks_completion_without_device_substitution() {
    let temp = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    let value = draft(temp.path());
    tested(&store, &value);
    let mut missing = devices();
    missing.monitor_ids.clear();
    assert!(store.commit_completed(true, &missing).is_err());
    missing = devices();
    missing.system_audio_ids.clear();
    assert!(store.commit_completed(true, &missing).is_err());
    store.commit_completed(true, &devices()).unwrap();
    let loaded = store.load().unwrap();
    assert!(validate_active(loaded.active.as_ref().unwrap(), &missing).is_err());
    assert!(loaded.onboarding.completed);
    assert_eq!(loaded.active.unwrap().monitor_id, "monitor-stable-1");
}

#[test]
fn config_interrupted_replace_recovers_last_known_good() {
    let temp = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    tested(&store, &draft(temp.path()));
    store.commit_completed(true, &devices()).unwrap();
    fs::write(temp.path().join("config.json"), b"{").unwrap();
    assert!(store.load().unwrap().active.unwrap().onboarding_completed);
    assert!(
        serde_json::from_slice::<StoredState>(&fs::read(temp.path().join("config.json")).unwrap())
            .unwrap()
            .onboarding
            .completed
    );
}

#[test]
fn config_missing_primary_recovers_backup() {
    let temp = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    tested(&store, &draft(temp.path()));
    store.commit_completed(true, &devices()).unwrap();
    fs::remove_file(temp.path().join("config.json")).unwrap();
    assert!(store.recover().unwrap().onboarding.completed);
}

#[test]
fn config_cleans_abandoned_temporary_files_without_promoting_them() {
    let temp = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    fs::write(temp.path().join("config.json.tmp"), b"{").unwrap();
    assert!(store.load().unwrap().active.is_none());
    assert!(!temp.path().join("config.json.tmp").exists());
    store.save_draft(Default::default(), 0).unwrap();
    assert!(!temp.path().join("config.json.tmp").exists());
}

#[test]
fn config_explicit_recovery_restores_previous_valid_state() {
    let temp = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    let first = draft(temp.path());
    tested(&store, &first);
    store.commit_completed(true, &devices()).unwrap();
    let mut second = first.clone();
    second.replay_seconds = Some(60);
    store.save_draft(second, 5).unwrap();
    let recovered = store.recover().unwrap();
    assert_eq!(recovered.draft.replay_seconds, Some(30));
    assert!(recovered.onboarding.completed);
}

#[test]
fn config_root_policy_canonicalizes_debug_override_and_rejects_release_override() {
    let temp = tempfile::tempdir().unwrap();
    let root = resolve_config_root(temp.path(), Some(temp.path()), true).unwrap();
    assert_eq!(root, temp.path().canonicalize().unwrap());
    assert!(resolve_config_root(temp.path(), Some(temp.path()), false).is_err());
    assert_eq!(
        resolve_config_root(temp.path(), None, false).unwrap(),
        temp.path().join("RebellioCap").canonicalize().unwrap()
    );
}

#[test]
fn config_invalid_backups_do_not_hide_corruption() {
    let temp = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    fs::write(temp.path().join("config.json"), b"{").unwrap();
    fs::write(temp.path().join("config.last-good.json"), b"{").unwrap();
    assert!(store.load().is_err());
    assert!(store.recover().is_err());
}

#[test]
fn config_test_requires_audio_packets_for_enabled_sources_and_local_clip_path() {
    let temp = tempfile::tempdir().unwrap();
    let outside = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    let value = draft(temp.path());
    store.save_draft(value.clone(), 5).unwrap();
    for (audio_packets, clip_path) in [
        (0, value.output_directory.as_ref().unwrap().join("test.mp4")),
        (90, outside.path().canonicalize().unwrap().join("test.mp4")),
    ] {
        assert!(store
            .record_successful_test(RecordingTestSummary {
                fingerprint: value.fingerprint().unwrap(),
                succeeded: true,
                clip_path,
                video_packets: 120,
                audio_packets,
            })
            .is_err());
    }
}

#[test]
fn config_completion_without_all_onboarding_steps_is_rejected() {
    let temp = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    let value = draft(temp.path());
    tested(&store, &value);
    store.save_draft(value, 2).unwrap();
    assert!(store.commit_completed(true, &devices()).is_err());
}

#[test]
fn config_vanished_microphone_and_output_do_not_erase_completed_state() {
    let temp = tempfile::tempdir().unwrap();
    let output = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    let mut value = draft(output.path());
    value.microphone = Some(AudioSelection::Endpoint("capture-stable-1".into()));
    let mut catalog = devices();
    catalog.microphone_ids.push("capture-stable-1".into());
    tested(&store, &value);
    store.commit_completed(true, &catalog).unwrap();
    catalog.microphone_ids.clear();
    assert!(validate_active(store.load().unwrap().active.as_ref().unwrap(), &catalog).is_err());
    output.close().unwrap();
    let state = store.load().unwrap();
    assert!(state.onboarding.completed);
    assert!(validate_active(state.active.as_ref().unwrap(), &devices()).is_err());
}

#[cfg(windows)]
#[test]
fn config_failed_windows_replace_keeps_primary_and_removes_temps() {
    use std::os::windows::fs::OpenOptionsExt;
    let temp = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    store.save_draft(Default::default(), 0).unwrap();
    let before = fs::read(temp.path().join("config.json")).unwrap();
    let held = fs::OpenOptions::new()
        .read(true)
        .share_mode(1)
        .open(temp.path().join("config.json"))
        .unwrap();
    let candidate = OnboardingDraft {
        width: Some(1920),
        ..Default::default()
    };
    assert!(store.save_draft(candidate, 1).is_err());
    assert_eq!(fs::read(temp.path().join("config.json")).unwrap(), before);
    assert!(!temp.path().join("config.json.tmp").exists());
    assert!(!temp.path().join("config.last-good.json.tmp").exists());
    drop(held);
    assert_eq!(store.load().unwrap().draft.width, None);
}

#[cfg(windows)]
#[test]
fn config_transient_read_error_never_restores_backup() {
    use std::os::windows::fs::OpenOptionsExt;
    let temp = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    store.save_draft(Default::default(), 0).unwrap();
    store
        .save_draft(
            OnboardingDraft {
                width: Some(1920),
                ..Default::default()
            },
            1,
        )
        .unwrap();
    let before = fs::read(temp.path().join("config.json")).unwrap();
    // Permit rename/delete but deny subsequent reads: recovery could overwrite
    // this healthy primary while the handle is held unless I/O is classified.
    let held = fs::OpenOptions::new()
        .read(true)
        .share_mode(2 | 4)
        .open(temp.path().join("config.json"))
        .unwrap();
    let result = store.load();
    drop(held);
    assert_eq!(fs::read(temp.path().join("config.json")).unwrap(), before);
    assert!(matches!(result, Err(ConfigError::Io(e)) if e.raw_os_error() == Some(32)));
}

fn assert_future_schema_preserved(operation: &str) {
    let temp = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    store.save_draft(Default::default(), 0).unwrap();
    let backup = fs::read(temp.path().join("config.last-good.json")).unwrap();
    let bytes =
        br#"{"schema_version":2,"draft":"new representation","future_required_field":true}"#;
    fs::write(temp.path().join("config.json"), bytes).unwrap();
    let result = match operation {
        "load" => store.load().map(|_| ()),
        "save" => store.save_draft(Default::default(), 0),
        "recover" => store.recover().map(|_| ()),
        _ => unreachable!(),
    };
    assert!(matches!(result, Err(ConfigError::Schema(2))));
    assert_eq!(fs::read(temp.path().join("config.json")).unwrap(), bytes);
    assert_eq!(
        fs::read(temp.path().join("config.last-good.json")).unwrap(),
        backup
    );
}

#[test]
fn config_load_preserves_future_schema_with_changed_fields() {
    assert_future_schema_preserved("load");
}

#[test]
fn config_save_preserves_future_schema_with_changed_fields() {
    assert_future_schema_preserved("save");
}

#[test]
fn config_recover_preserves_future_schema_with_changed_fields() {
    assert_future_schema_preserved("recover");
}

fn assert_tampered_evidence_rejected(field: &str, replacement: serde_json::Value) {
    let temp = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    tested(&store, &draft(temp.path()));
    store.commit_completed(true, &devices()).unwrap();
    let good = store.load().unwrap();
    let mut tampered = serde_json::to_value(&good).unwrap();
    tampered["last_successful_test"][field] = replacement;
    fs::write(
        temp.path().join("config.json"),
        serde_json::to_vec(&tampered).unwrap(),
    )
    .unwrap();
    let recovered = store.load().unwrap();
    assert_eq!(recovered.last_successful_test, good.last_successful_test);
    assert_eq!(recovered.active, good.active);
    assert!(recovered.onboarding.completed);
}

#[test]
fn config_loaded_evidence_rejects_zero_enabled_audio_packets() {
    assert_tampered_evidence_rejected("audio_packets", 0.into());
}

#[test]
fn config_loaded_evidence_rejects_clip_outside_output_directory() {
    let outside = tempfile::tempdir().unwrap();
    assert_tampered_evidence_rejected(
        "clip_path",
        serde_json::to_value(outside.path().canonicalize().unwrap().join("test.mp4")).unwrap(),
    );
}

#[test]
fn config_loaded_evidence_rejects_mismatched_fingerprint() {
    assert_tampered_evidence_rejected("fingerprint", "0".repeat(64).into());
}

#[cfg(windows)]
#[test]
fn config_failed_first_completion_cannot_be_promoted_by_recovery() {
    use std::os::windows::fs::OpenOptionsExt;
    let temp = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    tested(&store, &draft(temp.path()));
    let before = fs::read(temp.path().join("config.json")).unwrap();
    let held = fs::OpenOptions::new()
        .read(true)
        .share_mode(1)
        .open(temp.path().join("config.json"))
        .unwrap();
    assert!(store.commit_completed(true, &devices()).is_err());
    drop(held);
    assert_eq!(fs::read(temp.path().join("config.json")).unwrap(), before);
    let backup: StoredState =
        serde_json::from_slice(&fs::read(temp.path().join("config.last-good.json")).unwrap())
            .unwrap();
    assert!(
        !backup.onboarding.completed,
        "failed completion must not publish a completed backup"
    );
    assert!(backup.active.is_none());
    fs::write(temp.path().join("config.json"), b"{").unwrap();
    let recovered = store.recover().unwrap();
    assert!(!recovered.onboarding.completed);
    assert!(recovered.active.is_none());
}

#[test]
fn draft_resolution_agrees_with_desktop_minimum() {
    let draft = OnboardingDraft { width: Some(62), ..Default::default() };
    assert!(validate_draft(&draft).is_err());
}

#[test]
fn recording_contract_boundaries_and_messages_are_enforced() {
    let contract = recording_contract();
    assert_eq!(contract["version"], 1);
    for (field, range) in contract["ranges"].as_object().unwrap() {
        let min = range["min"].as_u64().unwrap(); let max = range["max"].as_u64().unwrap();
        for (value, accepted) in [(min - 1, false), (min, true), (max, true), (max + 1, false)] {
            let draft: OnboardingDraft = serde_json::from_value(serde_json::json!({field: value})).unwrap();
            let result = validate_draft(&draft);
            assert_eq!(result.is_ok(), accepted, "{field}={value}");
            if let Err(error) = result { assert_eq!(error.to_string(), contract["messages"][field].as_str().unwrap()); }
        }
    }
    for field in ["save_replay_hotkey", "toggle_recording_hotkey"] {
        let draft: OnboardingDraft = serde_json::from_value(serde_json::json!({field: contract["defaults"][field]})).unwrap();
        assert!(validate_draft(&draft).is_ok());
    }
}

#[test]
fn replay_memory_manual_json_roundtrips_to_engine() {
    let temp = tempfile::tempdir().unwrap();
    let mut value = serde_json::to_value(draft(temp.path())).unwrap();
    value["replay_memory_limit_mb"] = 512.into();
    let selected: OnboardingDraft = serde_json::from_value(value).unwrap();
    validate_draft(&selected).unwrap();
    let active = selected.to_active().unwrap();
    let engine = rebelliocap_desktop::native::recording_test::engine_config(&active, None).unwrap();
    assert_eq!(engine["replayMemoryLimitMb"], 512);
    let test_engine = rebelliocap_desktop::native::recording_test::engine_config(&active, Some(&temp.path().join("test.mp4"))).unwrap();
    assert_eq!(test_engine["replayMemoryLimitMb"], 512);
    assert_eq!(serde_json::to_value(active.as_draft()).unwrap()["replay_memory_limit_mb"], 512);
}

#[test]
fn replay_memory_auto_preserves_legacy_serialization_and_fingerprint() {
    use sha2::{Digest, Sha256};
    let temp = tempfile::tempdir().unwrap();
    let legacy = draft(temp.path());
    let bytes = serde_json::to_vec(&(CONFIG_SCHEMA_VERSION, &legacy)).unwrap();
    assert!(!String::from_utf8(bytes.clone()).unwrap().contains("replay_memory_limit_mb"));
    let expected = format!("{:x}", Sha256::digest(&bytes));
    let old_json = serde_json::to_value(&legacy).unwrap();
    let loaded: OnboardingDraft = serde_json::from_value(old_json).unwrap();
    assert_eq!(loaded.replay_memory_limit_mb, None);
    assert_eq!(loaded.fingerprint().unwrap(), expected);
    let mut auto = loaded.clone();
    auto.replay_memory_limit_mb = Some(0);
    assert_eq!(auto.fingerprint().unwrap(), expected);
    let active = auto.to_active().unwrap();
    assert_eq!(active.replay_memory_limit_mb, 0);
    assert_eq!(active.as_draft().fingerprint().unwrap(), expected);
    let json = serde_json::to_value(&active).unwrap();
    assert!(json.get("replay_memory_limit_mb").is_none());
    let old_active: ActiveConfig = serde_json::from_value(json).unwrap();
    assert_eq!(old_active.replay_memory_limit_mb, 0);
    auto.replay_memory_limit_mb = Some(512);
    assert_ne!(auto.fingerprint().unwrap(), expected);
}

#[test]
fn replay_memory_validation_accepts_auto_and_bounds_and_rejects_invalid_manual() {
    let temp = tempfile::tempdir().unwrap();
    for limit in [0, 64, 512, 8192] {
        let mut value = draft(temp.path());
        value.replay_memory_limit_mb = Some(limit);
        validate_draft(&value).unwrap();
    }
    for limit in [1, 63, 8193, u32::MAX] {
        let mut value = draft(temp.path());
        value.replay_memory_limit_mb = Some(limit);
        assert!(matches!(validate_draft(&value), Err(ConfigError::Invalid("replay_memory_limit_mb"))));
    }
    for invalid in [serde_json::json!(-1), serde_json::json!(64.5), serde_json::json!("512")] {
        let mut json = serde_json::to_value(draft(temp.path())).unwrap();
        json["replay_memory_limit_mb"] = invalid;
        assert!(serde_json::from_value::<OnboardingDraft>(json).is_err());
    }
}

#[test]
fn replay_memory_manual_persists_across_completed_settings_reload() {
    let temp = tempfile::tempdir().unwrap();
    let output = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    let mut selected = draft(output.path());
    selected.replay_memory_limit_mb = Some(1024);
    tested(&store, &selected);
    store.commit_completed(true, &devices()).unwrap();
    let loaded = store.load().unwrap();
    assert!(loaded.onboarding.completed);
    assert_eq!(loaded.active.unwrap().replay_memory_limit_mb, 1024);
    assert_eq!(loaded.draft.replay_memory_limit_mb, Some(1024));
}

#[test]
fn replay_memory_old_completed_config_retains_test_evidence() {
    let temp = tempfile::tempdir().unwrap();
    let output = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    let selected = draft(output.path());
    tested(&store, &selected);
    store.commit_completed(true, &devices()).unwrap();
    let before = store.load().unwrap();
    let on_disk = fs::read_to_string(temp.path().join("config.json")).unwrap();
    assert!(!on_disk.contains("replay_memory_limit_mb"));
    let after = ConfigStore::at(temp.path().into()).unwrap().load().unwrap();
    assert!(after.onboarding.completed);
    assert_eq!(after.last_successful_test, before.last_successful_test);
    assert_eq!(after.active.unwrap().replay_memory_limit_mb, 0);
}

#[test]
fn replay_memory_explicit_auto_survives_store_readback() {
    let temp = tempfile::tempdir().unwrap();
    let output = tempfile::tempdir().unwrap();
    let store = ConfigStore::at(temp.path().into()).unwrap();
    let mut selected = draft(output.path());
    selected.replay_memory_limit_mb = Some(0);
    store.save_draft(selected.clone(), 5).unwrap();
    let loaded = store.load().unwrap();
    assert_eq!(loaded.draft.replay_memory_limit_mb, Some(0));
    assert_eq!(loaded.draft.fingerprint().unwrap(), selected.fingerprint().unwrap());
}
