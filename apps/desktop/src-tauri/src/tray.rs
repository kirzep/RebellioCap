use tauri::{Manager, Emitter, menu::{Menu, MenuItem}, tray::{TrayIconBuilder, TrayIconEvent, MouseButton, MouseButtonState}};
use std::{sync::{Condvar, Mutex}, time::{Duration, Instant}};

#[derive(Default)]
pub(crate) struct TrayUpdates {
    state: Mutex<(u64, bool)>,
    changed: Condvar,
}
impl TrayUpdates {
    pub(crate) fn notify(&self) {
        let mut state = self.state.lock().unwrap_or_else(|e| e.into_inner());
        state.0 = state.0.wrapping_add(1);
        self.changed.notify_all();
    }
    pub(crate) fn close(&self) {
        self.state.lock().unwrap_or_else(|e| e.into_inner()).1 = true;
        self.changed.notify_all();
    }
    pub(crate) fn revision(&self) -> u64 {
        self.state.lock().unwrap_or_else(|e| e.into_inner()).0
    }
    pub(crate) fn wait_after(&self, revision: u64, retry: Option<Duration>) -> Option<u64> {
        let deadline = retry.map(|duration| Instant::now() + duration);
        let mut state = self.state.lock().unwrap_or_else(|e| e.into_inner());
        while state.0 == revision && !state.1 {
            state = match deadline {
                Some(deadline) => {
                    let remaining = deadline.saturating_duration_since(Instant::now());
                    if remaining.is_zero() { break; }
                    self.changed.wait_timeout(state, remaining).unwrap_or_else(|e| e.into_inner()).0
                }
                None => self.changed.wait(state).unwrap_or_else(|e| e.into_inner()),
            };
        }
        if state.1 { None } else { Some(state.0) }
    }
}

#[derive(Default)]
pub struct QuitRequest(std::sync::Mutex<bool>);
impl QuitRequest {
    fn queue(&self) { *self.0.lock().unwrap_or_else(|e| e.into_inner()) = true; }
    fn take(&self) -> bool { std::mem::take(&mut *self.0.lock().unwrap_or_else(|e| e.into_inner())) }
}

#[tauri::command]
pub fn take_pending_app_quit(window: tauri::WebviewWindow, state: tauri::State<'_, QuitRequest>) -> Result<bool, String> {
    if window.label() != "main" { return Err("Main window required".into()); }
    Ok(state.take())
}

#[tauri::command]
pub async fn quit_application(window: tauri::WebviewWindow, app: tauri::AppHandle, state: tauri::State<'_, crate::commands::HostState>) -> Result<(), String> {
    if window.label() != "main" { return Err("Main window required".into()); }
    let code = match crate::commands::stop_engine(state).await {
        Ok(_) => 0,
        Err(error) => {
            crate::logging::record("error", "app", "quit_stop_failed", serde_json::json!({"error":error}));
            1
        }
    };
    app.exit(code);
    Ok(())
}

pub fn show(app: &tauri::AppHandle) {
    if let Some(window) = app.get_webview_window("main") {
        let _ = window.unminimize();
        let _ = window.show();
        let _ = window.set_focus();
    }
}

pub fn setup(app: &mut tauri::App) -> tauri::Result<()> {
    let open = MenuItem::with_id(app, "open", crate::language::text("Открыть", "Open"), true, None::<&str>)?;
    let replay = MenuItem::with_id(app, "replay", crate::language::text("Сохранить повтор", "Save replay"), false, None::<&str>)?;
    let recording = MenuItem::with_id(app, "recording", crate::language::text("Начать запись", "Start recording"), false, None::<&str>)?;
    let settings = MenuItem::with_id(app, "settings", crate::language::text("Настройки", "Settings"), true, None::<&str>)?;
    let quit = MenuItem::with_id(app, "quit", crate::language::text("Выйти", "Quit"), true, None::<&str>)?;
    let menu = Menu::with_items(app, &[&open, &replay, &recording, &settings, &quit])?;
    TrayIconBuilder::with_id("main-tray")
        .icon(tauri::image::Image::from_bytes(include_bytes!("../icons/tray.png"))?)
        .tooltip("RebellioCap")
        .menu(&menu)
        .show_menu_on_left_click(false)
        .on_tray_icon_event(|tray, event| {
            if matches!(event, TrayIconEvent::Click { button: MouseButton::Left, button_state: MouseButtonState::Up, .. }) {
                show(tray.app_handle());
            }
        })
        .on_menu_event(|app, event| {
            let id = event.id.as_ref();
            match id {
                "open" => show(app),
                "settings" => { show(app); let _ = app.emit("tray-settings", ()); }
                "quit" => { app.state::<QuitRequest>().queue(); show(app); let _ = app.emit("tray-quit", ()); }
                "replay" | "recording" => {
                    let app = app.clone();
                    let id = id.to_owned();
                    tauri::async_runtime::spawn(async move {
                        let state = app.state::<crate::commands::HostState>();
                        let result = match id.as_str() {
                            "replay" => crate::commands::save_replay(state).await,
                            _ => crate::commands::toggle_recording(state).await,
                        };
                        match result {
                            Ok(_) => (),
                            // Background errors must never take foreground focus from a game.
                            Err(error) => { let _ = app.emit("tray-error", error); }
                        }
                    });
                }
                _ => (),
            }
        })
        .build(app)?;
    let handle = app.handle().clone();
    let updates = app.state::<crate::commands::HostState>().tray_updates();
    std::thread::spawn(move || {
        let mut previous = None;
        loop {
            // Capture before reading Host: a transition during the update must
            // cause another pass rather than disappearing into the wait.
            let revision = updates.revision();
            let mut retry = None;
            let state = handle.state::<crate::commands::HostState>();
            if let Ok((snapshot, recording_enabled)) = state.tray_status() {
                let current = (snapshot.replay_active, snapshot.continuous_recording_active, recording_enabled, crate::language::text("ru", "en"));
                if previous != Some(current) {
                    let updated = open.set_text(crate::language::text("Открыть", "Open"))
                        .and_then(|_| replay.set_text(crate::language::text("Сохранить повтор", "Save replay")))
                        .and_then(|_| settings.set_text(crate::language::text("Настройки", "Settings")))
                        .and_then(|_| quit.set_text(crate::language::text("Выйти", "Quit")))
                        .and_then(|_| replay.set_enabled(current.0))
                        .and_then(|_| recording.set_enabled(current.1 || current.2))
                        .and_then(|_| recording.set_text(if current.1 { crate::language::text("Остановить запись", "Stop recording") } else { crate::language::text("Начать запись", "Start recording") }));
                    if updated.is_ok() { previous = Some(current); }
                    else { retry = Some(Duration::from_secs(1)); }
                }
            } else { retry = Some(Duration::from_secs(1)); }
            if updates.wait_after(revision, retry).is_none() { break; }
        }
    });
    if !std::env::args().any(|arg| arg == "--autostart") { show(app.handle()); }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::QuitRequest;
    #[test]
    fn quit_survives_missing_listener_and_drains_once() {
        let request = QuitRequest::default();
        assert!(!request.take());
        request.queue(); request.queue();
        assert!(request.take());
        assert!(!request.take());
        request.queue(); assert!(request.take());
    }
    #[test]
    fn tray_wait_is_idle_until_change_and_does_not_lose_an_early_signal() {
        let signal = std::sync::Arc::new(super::TrayUpdates::default());
        signal.notify();
        assert_eq!(signal.wait_after(0, None), Some(1));
        let worker_signal = signal.clone();
        let (send, receive) = std::sync::mpsc::channel();
        let worker = std::thread::spawn(move || { send.send(worker_signal.wait_after(1, None)).unwrap(); });
        assert!(receive.recv_timeout(std::time::Duration::from_millis(900)).is_err(), "idle tray woke without a state change");
        signal.notify();
        assert_eq!(receive.recv_timeout(std::time::Duration::from_secs(1)).unwrap(), Some(2));
        worker.join().unwrap();
        signal.close();
        assert_eq!(signal.wait_after(2, None), None);
    }
    #[test]
    fn tray_wait_has_a_deadline_only_for_a_failed_update_and_close_wakes_waiters() {
        let signal = std::sync::Arc::new(super::TrayUpdates::default());
        assert_eq!(signal.wait_after(0, Some(std::time::Duration::from_millis(10))), Some(0));
        let worker_signal=signal.clone();
        let (send,receive)=std::sync::mpsc::channel();
        let worker=std::thread::spawn(move || {send.send(worker_signal.wait_after(0,None)).unwrap();});
        signal.close();
        assert_eq!(receive.recv_timeout(std::time::Duration::from_secs(1)).unwrap(),None);
        worker.join().unwrap();
    }
}
