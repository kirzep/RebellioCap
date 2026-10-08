mod support;
use rebelliocap_desktop::{
    engine::{protocol::SessionCommand, supervisor::EngineSupervisor},
    model::*,
    native,
};
use std::{path::PathBuf, sync::atomic::AtomicBool, time::Duration};

#[test]
#[ignore = "requires real interactive Windows engine; set REBELLIOCAP_ENGINE_PATH"]
fn real_catalog_doctor_session_control_and_audio_meter() {
    let executable =
        PathBuf::from(std::env::var_os("REBELLIOCAP_ENGINE_PATH").expect("engine path"))
            .canonicalize()
            .unwrap();
    let tmp = tempfile::Builder::new()
        .prefix("RebellioCap-host-controls-")
        .tempdir_in(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("target"))
        .unwrap();
    let catalog = native::catalogs::load(&executable).unwrap();
    let mut draft = support::draft(tmp.path());
    draft.monitor_id = Some(catalog.monitors[0].id.clone());
    draft.width = Some(1280);
    draft.height = Some(720);
    draft.fps = Some(30);
    draft.bitrate = Some(8_000_000);
    let check =
        native::system_check::run(Ok(&executable), &draft, &tmp.path().canonicalize().unwrap());
    assert!(check.passed, "{:?}", check.stages);
    let endpoint = catalog.audio.system_audio.first().expect("system endpoint");
    let peaks = native::measure_endpoint(&endpoint.id, &AtomicBool::new(false)).unwrap();
    assert!(peaks.samples.len() >= 40);
    assert!(peaks
        .samples
        .iter()
        .all(|v| v.level.is_finite() && (0.0..=1.0).contains(&v.level)));
    assert!(peaks
        .samples
        .windows(2)
        .all(|v| v[0].timestamp_ms <= v[1].timestamp_ms));
    assert!(
        native::measure_endpoint(&endpoint.id, &AtomicBool::new(true))
            .unwrap()
            .cancelled
    );
    let file = native::recording_test::write_config(tmp.path(), &draft.to_active().unwrap(), None)
        .unwrap();
    let s = EngineSupervisor::spawn(
        &executable,
        &[
            "session-json".into(),
            "--config".into(),
            file.path().as_os_str().into(),
        ],
        Duration::from_secs(15),
    )
    .unwrap();
    assert!(s.snapshot().replay_active);
    s.request(SessionCommand::GetSnapshot).unwrap();
    s.stop().unwrap();
    assert_eq!(s.snapshot().lifecycle, EngineLifecycle::Stopped);
    assert!(!s.is_running());
    println!("native catalogs, doctor, session ready/snapshot/stop, {} WASAPI peaks and cancellation passed", peaks.samples.len());
}

#[test]
#[ignore = "requires real interactive Windows/NVIDIA engine; set REBELLIOCAP_ENGINE_PATH"]
fn real_catalog_doctor_recording_session_and_audio_peak() {
    let executable =
        PathBuf::from(std::env::var_os("REBELLIOCAP_ENGINE_PATH").expect("engine path"))
            .canonicalize()
            .unwrap();
    let tmp = tempfile::Builder::new()
        .prefix("RebellioCap-host-integration-")
        .tempdir_in(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("target"))
        .unwrap();
    let catalog = native::catalogs::load(&executable).unwrap();
    assert!(!catalog.monitors.is_empty());
    let mut draft = support::draft(tmp.path());
    draft.monitor_id = Some(catalog.monitors[0].id.clone());
    draft.width = Some(1280);
    draft.height = Some(720);
    draft.fps = Some(30);
    draft.bitrate = Some(8_000_000);
    let check =
        native::system_check::run(Ok(&executable), &draft, &tmp.path().canonicalize().unwrap());
    assert!(check.passed, "{:?}", check.stages);
    let summary = native::recording_test::run(&executable, tmp.path(), &draft).unwrap();
    assert!(summary.video_packets > 0);
    assert_eq!(summary.audio_packets, 0);
    let file = native::recording_test::write_config(tmp.path(), &draft.to_active().unwrap(), None)
        .unwrap();
    let s = EngineSupervisor::spawn(
        &executable,
        &[
            "session-json".into(),
            "--config".into(),
            file.path().as_os_str().into(),
        ],
        Duration::from_secs(15),
    )
    .unwrap();
    assert!(s.snapshot().replay_active);
    std::thread::sleep(Duration::from_secs(2));
    s.request(SessionCommand::ToggleRecording).unwrap();
    assert!(s.snapshot().continuous_recording_active);
    std::thread::sleep(Duration::from_millis(500));
    s.request(SessionCommand::ToggleRecording).unwrap();
    let save = s.request(SessionCommand::SaveReplay).unwrap();
    assert!(PathBuf::from(save.output_path.unwrap()).is_file());
    s.stop().unwrap();
    assert_eq!(s.snapshot().lifecycle, EngineLifecycle::Stopped);
    if let Some(endpoint) = catalog.audio.system_audio.first() {
        let measurement = native::measure_endpoint(&endpoint.id, &AtomicBool::new(false)).unwrap();
        assert!(measurement.samples.len() >= 40);
        assert!(measurement
            .samples
            .iter()
            .all(|v| (0.0..=1.0).contains(&v.level)));
        assert!(
            native::measure_endpoint(&endpoint.id, &AtomicBool::new(true))
                .unwrap()
                .cancelled
        );
    }
    println!(
        "native integration clip: {} ({} video packets)",
        summary.clip_path.display(),
        summary.video_packets
    );
    let _ = tmp.keep();
}
