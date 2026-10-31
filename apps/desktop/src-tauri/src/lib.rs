mod clip_catalog;
mod clip_index;
pub mod commands;
pub mod config;
pub mod engine;
pub mod model;
pub mod native;
mod tray;
mod language;
pub mod notifications;
pub mod logging;
pub mod editor;
mod editor_process;
mod editor_preview;
mod editor_remux_cache;
mod editor_import_progress;
mod instance_identity;
mod updater;
pub mod recording_names;

pub fn run() {
    use tauri::Manager;
    let mut context = tauri::generate_context!();
    let test_root = std::env::var_os("REBELLIOCAP_TEST_CONFIG_ROOT").map(std::path::PathBuf::from);
    context.config_mut().identifier = instance_identity::identifier(
        &context.config().identifier, test_root.as_deref(), cfg!(debug_assertions)
    ).expect("Invalid test instance configuration");
    tauri::Builder::default()
        .plugin(tauri_plugin_notification::init())
        .plugin(tauri_plugin_updater::Builder::new().build())
        .plugin(tauri_plugin_single_instance::init(|app, _, _| tray::show(app)))
        .plugin(tauri_plugin_autostart::init(tauri_plugin_autostart::MacosLauncher::LaunchAgent, Some(vec!["--autostart"])))
        .setup(|app| {
            // Set the runtime window icon explicitly as well as the EXE resource.
            // Incremental codegen can otherwise retain a previous default icon.
            let icon = tauri::image::Image::from_bytes(include_bytes!("../icons/icon.png"))?;
            for window in app.webview_windows().values() {
                window.set_icon(icon.clone())?;
            }
            let legacy_data = app.path().local_data_dir()?;
            let per_app_data = app.path().app_local_data_dir()?;
            let data_base = config::profile_data_base(&legacy_data, &per_app_data, &app.config().identifier);
            let store = config::ConfigStore::from_local_app_data(data_base)?;
            logging::initialize(store.root())?;
            app.manage(logging::DeveloperState::default());
            app.manage(language::LanguageState::new(store.root().to_path_buf()));
            app.manage(recording_names::RecordingNamesState::new(store.root().to_path_buf()));
            app.manage(notifications::NotificationState::new(store.root().to_path_buf()));
            if let Some(overlay) = app.get_webview_window("overlay") {
                if let Some(monitor) = overlay.primary_monitor()? {
                    overlay.set_position(*monitor.position())?;
                    overlay.set_size(*monitor.size())?;
                }
                if let Err(error) = native::overlay_safety::initialize(&overlay) {
                    // Fail closed: recording may continue, but an unprotected overlay stays hidden.
                    let _ = overlay.hide();
                    logging::record("error", "overlay", "protection_failed", serde_json::json!({"error":error}));
                    eprintln!("Overlay disabled: {error}");
                }
            }
            let notification_app = app.handle().clone();
            app.manage(commands::HostState::with_notification_wake(store, &app.path().resource_dir()?, Some(std::sync::Arc::new(move || {
                notifications::notify_ready(&notification_app);
            }))));
            app.manage(commands::PlaybackState::default());
            app.manage(clip_index::CatalogState::default());
            app.manage(editor::EditorState::default());
            app.manage(tray::QuitRequest::default());
            app.manage(updater::UpdateState::default());
            tray::setup(app)?;
            Ok(())
        })
        .on_window_event(|window, event| match event {
            tauri::WindowEvent::CloseRequested { .. } if window.label() == "overlay-editor" => {}
            tauri::WindowEvent::CloseRequested { api, .. } => {
                api.prevent_close();
                if window.label() == "main" {
                    if let Err(error) = notifications::close_overlay_editor(window.app_handle()) {
                        logging::record("error", "overlay", "close_failed", serde_json::json!({"error":error}));
                        return;
                    }
                }
                match window.hide() {
                    Ok(()) if window.label() == "main" => tray::notify_hidden(window.app_handle()),
                    Ok(()) => {}
                    Err(error) => logging::record("error", "window", "hide_failed", serde_json::json!({"error":error.to_string()})),
                }
            }
            tauri::WindowEvent::Destroyed if window.label() == "overlay-editor" => {
                if let Err(error) = notifications::overlay_editor_destroyed(window.app_handle()) {
                    logging::record("error", "overlay", "close_finalize_failed", serde_json::json!({"error":error}));
                }
            }
            _ => {}
        })
        .invoke_handler(tauri::generate_handler![
            language::get_language_settings,
            language::set_language_settings,
            updater::check_for_update,
            updater::install_update,
            tray::take_pending_app_quit,
            tray::quit_application,
            recording_names::get_recording_names,
            recording_names::set_recording_names,
            editor::editor_begin_session,
            editor::editor_import,
            editor::editor_cancel_import,
            editor::editor_open,
            editor::editor_restore,
            editor::editor_save,
            editor::editor_export,
            editor::editor_status,
            editor::editor_cancel,
            editor::editor_release,
            editor::editor_release_previews,
            notifications::get_notification_settings,
            notifications::set_notification_settings,
            notifications::poll_notifications,
            notifications::import_notification_sound,
            notifications::read_notification_sound,
            notifications::set_overlay_visible,
            notifications::get_overlay_display,
            notifications::open_overlay_editor,
            notifications::close_overlay_editor_from_main,
            notifications::finish_overlay_editor,
            notifications::restore_overlay_editor_host,
            logging::developer_click,
            logging::get_developer_mode,
            logging::export_application_logs,
            logging::log_frontend,
            commands::bootstrap_app,
            commands::get_autostart,
            commands::set_autostart,
            commands::get_onboarding_state,
            commands::run_system_check,
            commands::list_monitors,
            commands::list_game_windows,
            commands::list_audio_endpoints,
            commands::measure_audio_level,
            commands::read_audio_peak,
            commands::choose_output_directory,
            commands::save_onboarding_draft,
            commands::run_recording_test,
            commands::complete_onboarding,
            commands::get_engine_snapshot,
            commands::start_engine,
            commands::stop_engine,
            commands::save_replay,
            commands::toggle_recording,
            commands::start_replay,
            commands::stop_replay,
            commands::apply_runtime_settings,
            commands::open_diagnostics,
            commands::open_test_clip,
            commands::list_clips,
            commands::list_clip_page,
            commands::begin_clip_catalog,
            commands::release_clip_catalog,
            commands::open_clip,
            commands::prepare_clip_playback,
            commands::release_clip_playback,
            commands::clip_thumbnail,
            commands::folder_icon,
            commands::rename_clip,
            commands::delete_clip
        ])
        .build(context)
        .expect("RebellioCap desktop host failed")
        .run(|app, event| {
            if matches!(
                event,
                tauri::RunEvent::Exit | tauri::RunEvent::ExitRequested { .. }
            ) {
                logging::record("info", "app", "shutdown", serde_json::json!({}));
                app.state::<commands::HostState>().shutdown();
                app.state::<editor::EditorState>().shutdown();
            }
        });
}
