mod support;
use rebelliocap_desktop::{
    engine::{
        protocol::SessionCommand,
        supervisor::{resolve_engine, EngineSupervisor},
    },
    model::EngineLifecycle,
};
use std::{
    path::Path,
    time::{Duration, Instant},
};

#[test]
fn resolver_rejects_missing_relative_and_release_override() {
    assert!(resolve_engine(Path::new("C:\\missing"), None, true).is_err());
    assert!(resolve_engine(
        Path::new("C:\\missing"),
        Some(Path::new("relative.exe")),
        true
    )
    .is_err());
    assert!(resolve_engine(Path::new("C:\\missing"), Some(&support::fixture()), false).is_err());
}
#[test]
fn startup_rejects_incompatible_malformed_oversized_and_timeout() {
    for mode in ["incompatible", "malformed", "oversized", "startup_timeout"] {
        assert!(
            EngineSupervisor::spawn(
                &support::fixture(),
                &[mode.into()],
                Duration::from_millis(400)
            )
            .is_err(),
            "{mode}"
        );
    }
}
#[test]
fn crash_and_eof_publish_failed_snapshot() {
    for mode in ["crash", "eof"] {
        let s =
            EngineSupervisor::spawn(&support::fixture(), &[mode.into()], Duration::from_secs(2));
        if let Ok(s) = s {
            let end = Instant::now() + Duration::from_secs(3);
            while s.snapshot().lifecycle != EngineLifecycle::Failed && Instant::now() < end {
                std::thread::sleep(Duration::from_millis(20));
            }
            assert_eq!(s.snapshot().lifecycle, EngineLifecycle::Failed);
            assert!(!s.snapshot().replay_active);
            assert!(!s.is_running());
        }
    }
}
#[test]
fn request_timeout_and_duplicate_response_fail_session() {
    for mode in ["timeout", "duplicate", "flood_timeout"] {
        let s = EngineSupervisor::spawn(
            &support::fixture(),
            &[mode.into()],
            Duration::from_millis(400),
        )
        .unwrap();
        let _ = s.request(SessionCommand::GetSnapshot);
        std::thread::sleep(Duration::from_millis(150));
        assert_eq!(s.snapshot().lifecycle, EngineLifecycle::Failed);
        assert!(!s.is_running());
    }
}

#[test]
fn concurrent_requests_and_stop_release_all_callers() {
    let supervisor = std::sync::Arc::new(EngineSupervisor::spawn(
        &support::fixture(), &["normal".into()], Duration::from_secs(2),
    ).unwrap());
    let barrier = std::sync::Arc::new(std::sync::Barrier::new(65));
    let (completed, results) = std::sync::mpsc::channel();
    for _ in 0..64 {
        let supervisor = supervisor.clone();
        let barrier = barrier.clone();
        let completed = completed.clone();
        std::thread::spawn(move || {
            barrier.wait();
            let result = supervisor.request(SessionCommand::GetSnapshot);
            completed.send(result).unwrap();
        });
    }
    barrier.wait();
    supervisor.stop().unwrap();
    for _ in 0..64 {
        let result = results.recv_timeout(Duration::from_secs(4)).expect("request caller remained blocked after stop");
        if let Err(error) = result {
            assert!(["engine.busy", "engine.not_running", "engine.stopped"].contains(&error.as_str()), "{error}");
        }
    }
    assert!(!supervisor.is_running());
    assert_eq!(supervisor.snapshot().lifecycle, EngineLifecycle::Stopped);
}
#[test]
fn graceful_stop_and_drop_leave_no_owned_process() {
    let s = EngineSupervisor::spawn(
        &support::fixture(),
        &["normal".into()],
        Duration::from_secs(2),
    )
    .unwrap();
    s.stop().unwrap();
    assert!(!s.is_running());
    assert_eq!(s.snapshot().lifecycle, EngineLifecycle::Stopped);
    let s = EngineSupervisor::spawn(
        &support::fixture(),
        &["timeout".into()],
        Duration::from_millis(300),
    )
    .unwrap();
    let pid = s.process_id();
    drop(s);
    unsafe {
        use windows_sys::Win32::{
            Foundation::CloseHandle,
            System::Threading::{
                GetExitCodeProcess, OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION,
            },
        };
        let p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
        if !p.is_null() {
            let mut code = 259;
            GetExitCodeProcess(p, &mut code);
            CloseHandle(p);
            assert_ne!(code, 259);
        }
    }
}

#[test]
fn idle_supervisor_blocks_for_events_without_periodic_wakeups() {
    let supervisor = EngineSupervisor::spawn(&support::fixture(), &["normal".into()], Duration::from_secs(2)).unwrap();
    supervisor.request(SessionCommand::GetSnapshot).unwrap();
    let initial = supervisor.actor_wakeups();
    std::thread::sleep(Duration::from_millis(180));
    let idle_wakeups = supervisor.actor_wakeups() - initial;
    assert!(idle_wakeups <= 1, "idle supervisor woke {idle_wakeups} times without events or pending deadlines");
    supervisor.request(SessionCommand::GetSnapshot).unwrap();
    supervisor.stop().unwrap();
}

#[test]
fn notification_readiness_is_signalled_after_queueing_and_not_for_unchanged_snapshots() {
    let (sender, receiver) = std::sync::mpsc::channel();
    let supervisor = EngineSupervisor::spawn_with_notification_wake(
        &support::fixture(), &["normal".into()], Duration::from_secs(2),
        Some(std::sync::Arc::new(move || { sender.send(()).unwrap(); })),
    ).unwrap();
    receiver.recv_timeout(Duration::from_secs(1)).unwrap();
    assert_eq!(supervisor.drain_notifications(), vec!["replay_started"]);
    supervisor.request(SessionCommand::GetSnapshot).unwrap();
    assert!(receiver.recv_timeout(Duration::from_millis(180)).is_err());
    supervisor.stop().unwrap();
    receiver.recv_timeout(Duration::from_secs(1)).unwrap();
    assert_eq!(supervisor.drain_notifications(), vec!["replay_stopped"]);
    assert!(receiver.try_recv().is_err());

    let (sender, receiver) = std::sync::mpsc::channel();
    let failed = EngineSupervisor::spawn_with_notification_wake(
        &support::fixture(), &["crash".into()], Duration::from_secs(2),
        Some(std::sync::Arc::new(move || { let _ = sender.send(()); })),
    ).unwrap();
    receiver.recv_timeout(Duration::from_secs(1)).unwrap();
    receiver.recv_timeout(Duration::from_secs(1)).unwrap();
    assert_eq!(failed.drain_notifications(), vec!["replay_started", "engine_stopped"]);
    assert_eq!(failed.snapshot().lifecycle, EngineLifecycle::Failed);
}
