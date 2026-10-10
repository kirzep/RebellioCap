#![allow(dead_code)]
use rebelliocap_desktop::{config::*, model::*};
use std::{
    path::{Path, PathBuf},
    process::Command,
    sync::OnceLock,
};

pub fn fixture() -> PathBuf {
    static PATH: OnceLock<PathBuf> = OnceLock::new();
    PATH.get_or_init(|| {
        let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
        let exe = root
            .join("target")
            .join(format!("host-fixture-{}.exe", std::process::id()));
        let rustc = std::env::var_os("RUSTC")
            .map(PathBuf::from)
            .unwrap_or_else(|| {
                PathBuf::from(std::env::var_os("CARGO_HOME").unwrap()).join("bin/rustc.exe")
            });
        assert!(Command::new(rustc)
            .arg(root.join("tests/support/fixture.rs"))
            .arg("-o")
            .arg(&exe)
            .status()
            .unwrap()
            .success());
        exe.canonicalize().unwrap()
    })
    .clone()
}
pub fn draft(path: &Path) -> OnboardingDraft {
    OnboardingDraft {
        monitor_id: Some("monitor-1".into()),
        system_audio: Some(AudioSelection::Disabled),
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
        save_without_game_folders: None,
        save_replay_hotkey: Some(Hotkey {
            key: 0x77,
            ctrl: false,
            alt: false,
            shift: false,
            win: false,
        }),
        toggle_recording_hotkey: Some(Hotkey {
            key: 0x78,
            ctrl: false,
            alt: false,
            shift: false,
            win: false,
        }),
        preferences: Some(Preferences {
            overlay_enabled: None,
            start_with_windows: false,
        }),
        continuous_recording_enabled: Some(false),
    }
}
pub fn devices() -> DeviceInventory {
    DeviceInventory {
        monitor_ids: vec!["monitor-1".into()],
        ..Default::default()
    }
}
