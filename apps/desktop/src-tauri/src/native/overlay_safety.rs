use windows_sys::Win32::{
    Foundation::{GetLastError, SetLastError, HWND, LPARAM, LRESULT, WPARAM},
    UI::{
        Shell::{DefSubclassProc, RemoveWindowSubclass, SetWindowSubclass},
        WindowsAndMessaging::*,
    },
};

const SUBCLASS_ID: usize = 0x53434f56;
const REQUIRED_STYLES: u32 =
    WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW;

unsafe extern "system" fn passive_window_proc(
    hwnd: HWND,
    message: u32,
    wparam: WPARAM,
    lparam: LPARAM,
    _id: usize,
    _data: usize,
) -> LRESULT {
    match message {
        // WS_EX_NOACTIVATE alone does not cover Windows' active-window tracking on hover.
        WM_MOUSEACTIVATE => MA_NOACTIVATE as LRESULT,
        WM_POINTERACTIVATE => PA_NOACTIVATE as LRESULT,
        WM_NCHITTEST => HTTRANSPARENT as LRESULT,
        WM_NCDESTROY => {
            RemoveWindowSubclass(hwnd, Some(passive_window_proc), SUBCLASS_ID);
            DefSubclassProc(hwnd, message, wparam, lparam)
        }
        _ => DefSubclassProc(hwnd, message, wparam, lparam),
    }
}

/// Install all protections on the owning UI thread before the first visible frame.
/// No SetFocus, SetActiveWindow, SetForegroundWindow, input hooks, or capture are used.
unsafe fn protect_and_show(hwnd: HWND) -> Result<(), String> {
    let styles = GetWindowLongPtrW(hwnd, GWL_EXSTYLE) as u32;
    SetLastError(0);
    let previous = SetWindowLongPtrW(
        hwnd,
        GWL_EXSTYLE,
        ((styles | REQUIRED_STYLES) & !WS_EX_APPWINDOW) as isize,
    );
    if previous == 0 && GetLastError() != 0 {
        return Err(std::io::Error::last_os_error().to_string());
    }
    if SetWindowSubclass(hwnd, Some(passive_window_proc), SUBCLASS_ID, 0) == 0 {
        return Err("overlay.activation_guard_failed".into());
    }
    let actual = GetWindowLongPtrW(hwnd, GWL_EXSTYLE) as u32;
    if actual & REQUIRED_STYLES != REQUIRED_STYLES || actual & WS_EX_APPWINDOW != 0 {
        return Err("overlay.unsafe_window_styles".into());
    }
    // Unlike a generic show(), this cannot request activation. The window stays visible;
    // notifications only change its pixels, never its foreground state or z-order.
    if SetWindowPos(
        hwnd,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED,
    ) == 0
    {
        return Err(std::io::Error::last_os_error().to_string());
    }
    Ok(())
}

pub fn initialize(window: &tauri::WebviewWindow) -> Result<(), String> {
    window.set_focusable(false).map_err(|e| e.to_string())?;
    window
        .set_ignore_cursor_events(true)
        .map_err(|e| e.to_string())?;
    let hwnd = window.hwnd().map_err(|e| e.to_string())?.0 as HWND;
    // setup runs on this window's UI thread. A failure leaves the initially hidden window hidden.
    unsafe { protect_and_show(hwnd) }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn native_overlay_cannot_activate_or_take_mouse_input() {
        unsafe {
            let class: Vec<u16> = "STATIC\0".encode_utf16().collect();
            let hwnd = CreateWindowExW(
                0,
                class.as_ptr(),
                class.as_ptr(),
                WS_POPUP,
                -10000,
                -10000,
                64,
                64,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                std::ptr::null(),
            );
            assert!(!hwnd.is_null());
            struct Window(HWND);
            impl Drop for Window {
                fn drop(&mut self) {
                    unsafe {
                        DestroyWindow(self.0);
                    }
                }
            }
            let _cleanup = Window(hwnd);
            let foreground = GetForegroundWindow();
            let mut gui_before: GUITHREADINFO = std::mem::zeroed();
            gui_before.cbSize = std::mem::size_of::<GUITHREADINFO>() as u32;
            let gui_available = GetGUIThreadInfo(0, &mut gui_before) != 0;
            protect_and_show(hwnd).unwrap();
            assert_ne!(SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA), 0);
            protect_and_show(hwnd).unwrap();
            let styles = GetWindowLongPtrW(hwnd, GWL_EXSTYLE) as u32;
            assert_eq!(styles & REQUIRED_STYLES, REQUIRED_STYLES);
            assert_eq!(styles & WS_EX_APPWINDOW, 0);
            assert_eq!(
                SendMessageW(hwnd, WM_MOUSEACTIVATE, 0, 0),
                MA_NOACTIVATE as isize
            );
            assert_eq!(
                SendMessageW(hwnd, WM_NCHITTEST, 0, 0),
                HTTRANSPARENT as isize
            );
            assert_eq!(
                SendMessageW(hwnd, WM_POINTERACTIVATE, 0, 0),
                PA_NOACTIVATE as isize
            );
            assert_eq!(GetForegroundWindow(), foreground);
            if gui_available {
                let mut gui_after: GUITHREADINFO = std::mem::zeroed();
                gui_after.cbSize = gui_before.cbSize;
                assert_ne!(GetGUIThreadInfo(0, &mut gui_after), 0);
                assert_eq!(gui_before.hwndFocus, gui_after.hwndFocus);
                assert_eq!(gui_before.hwndCapture, gui_after.hwndCapture);
            }
        }
    }

    #[test]
    fn overlay_is_hidden_until_native_protection_is_installed() {
        let config: serde_json::Value =
            serde_json::from_str(include_str!("../../tauri.conf.json")).unwrap();
        let overlay = config["app"]["windows"]
            .as_array()
            .unwrap()
            .iter()
            .find(|w| w["label"] == "overlay")
            .unwrap();
        assert_eq!(overlay["visible"], false);
        assert_eq!(overlay["focus"], false);
        assert_eq!(overlay["focusable"], false);
        assert_eq!(overlay["skipTaskbar"], true);
    }
}
