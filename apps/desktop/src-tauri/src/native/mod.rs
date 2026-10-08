pub mod catalogs;
pub mod recording_test;
pub mod overlay_safety;
pub mod system_check;
pub mod thumbnails;

pub fn recycle_file(path: &std::path::Path) -> crate::engine::Result<()> {
    use windows_sys::Win32::UI::Shell::*;
    use std::os::windows::ffi::OsStrExt;
    let path = crate::model::strip_verbatim_prefix(path);
    let mut from: Vec<u16> = path.as_os_str().encode_wide().collect();
    from.extend([0, 0]);
    unsafe {
        let mut operation: SHFILEOPSTRUCTW = std::mem::zeroed();
        operation.wFunc = FO_DELETE;
        operation.pFrom = from.as_ptr();
        operation.fFlags = (FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT) as u16;
        let result = SHFileOperationW(&mut operation);
        if result != 0 || operation.fAnyOperationsAborted != 0 {
            return Err(format!("Не удалось переместить клип в корзину Windows ({result})."));
        }
    }
    Ok(())
}

/// Read the current Windows endpoint meter without opening a recording stream.
pub fn read_audio_peak(id: &str) -> crate::engine::Result<f32> {
    use windows::{core::HSTRING, Win32::{Media::Audio::{Endpoints::IAudioMeterInformation, IMMDeviceEnumerator, MMDeviceEnumerator}, System::Com::*}};
    let _apartment = Apartment::new(false)?;
    unsafe {
        let enumerator: IMMDeviceEnumerator = CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_INPROC_SERVER).map_err(|e| e.to_string())?;
        let device = enumerator.GetDevice(&HSTRING::from(id)).map_err(|e| e.to_string())?;
        let meter: IAudioMeterInformation = device.Activate(CLSCTX_INPROC_SERVER, None).map_err(|e| e.to_string())?;
        let peak = meter.GetPeakValue().map_err(|e| e.to_string())?;
        if !peak.is_finite() || !(0.0..=1.0).contains(&peak) { return Err("audio.invalid_peak".into()); }
        Ok(peak)
    }
}

use crate::{
    engine::Result,
    model::{AudioLevel, AudioMeasurement},
};
use std::{
    path::{Path, PathBuf},
    sync::atomic::{AtomicBool, Ordering},
    time::{Duration, Instant},
};

struct Apartment;
impl Apartment {
    fn new(sta: bool) -> Result<Self> {
        use windows::Win32::System::Com::*;
        unsafe {
            CoInitializeEx(
                None,
                if sta {
                    COINIT_APARTMENTTHREADED
                } else {
                    COINIT_MULTITHREADED
                },
            )
            .ok()
            .map_err(|e| e.to_string())?;
        }
        Ok(Self)
    }
}
impl Drop for Apartment {
    fn drop(&mut self) {
        unsafe {
            windows::Win32::System::Com::CoUninitialize();
        }
    }
}

/// Caller must first resolve this exact endpoint ID from a fresh native catalog.
pub fn measure_endpoint(id: &str, cancellation: &AtomicBool) -> Result<AudioMeasurement> {
    use windows::{
        core::HSTRING,
        Win32::{
            Media::Audio::{
                Endpoints::IAudioMeterInformation, IMMDeviceEnumerator, MMDeviceEnumerator,
            },
            System::Com::*,
        },
    };
    let _apartment = Apartment::new(false)?;
    let mut samples = Vec::with_capacity(60);
    unsafe {
        let enumerator: IMMDeviceEnumerator =
            CoCreateInstance(&MMDeviceEnumerator, None, CLSCTX_INPROC_SERVER)
                .map_err(|e| e.to_string())?;
        let device = enumerator
            .GetDevice(&HSTRING::from(id))
            .map_err(|e| e.to_string())?;
        let meter: IAudioMeterInformation = device
            .Activate(CLSCTX_INPROC_SERVER, None)
            .map_err(|e| e.to_string())?;
        let start = Instant::now();
        while start.elapsed() < Duration::from_secs(3) && !cancellation.load(Ordering::Acquire) {
            let level = meter.GetPeakValue().map_err(|e| e.to_string())?;
            if !level.is_finite() || !(0.0..=1.0).contains(&level) {
                return Err("audio.invalid_peak".into());
            }
            samples.push(AudioLevel {
                timestamp_ms: start.elapsed().as_millis() as u64,
                level,
            });
            std::thread::sleep(Duration::from_millis(50));
        }
    }
    Ok(AudioMeasurement {
        samples,
        cancelled: cancellation.load(Ordering::Acquire),
    })
}

pub fn choose_directory() -> Result<Option<PathBuf>> {
    use windows::Win32::{System::Com::*, UI::Shell::*};
    let _apartment = Apartment::new(true)?;
    unsafe {
        let dialog: IFileOpenDialog = CoCreateInstance(&FileOpenDialog, None, CLSCTX_INPROC_SERVER)
            .map_err(|e| e.to_string())?;
        dialog
            .SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR)
            .map_err(|e| e.to_string())?;
        if let Err(e) = dialog.Show(None) {
            if e.code().0 as u32 == 0x800704c7 {
                return Ok(None);
            }
            return Err(e.to_string());
        }
        let item = dialog.GetResult().map_err(|e| e.to_string())?;
        let text = item
            .GetDisplayName(SIGDN_FILESYSPATH)
            .map_err(|e| e.to_string())?;
        let value = text.to_string().map_err(|e| e.to_string());
        CoTaskMemFree(Some(text.0 as *const _));
        let raw_path = PathBuf::from(value?);
        let path = crate::model::strip_verbatim_prefix(
            &raw_path.canonicalize().map_err(|e| e.to_string())?,
        );
        system_check::check_output(&path, 0)?;
        Ok(Some(path))
    }
}

/// Fixed action, no executable, verb, parameters or renderer path are accepted.
pub(crate) fn open_file(path: &Path) -> Result<()> {
    use std::os::windows::ffi::OsStrExt;
    use windows_sys::Win32::UI::{Shell::ShellExecuteW, WindowsAndMessaging::SW_SHOWNORMAL};
    let text: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
    let verb: Vec<u16> = "open\0".encode_utf16().collect();
    let result = unsafe {
        ShellExecuteW(
            std::ptr::null_mut(),
            verb.as_ptr(),
            text.as_ptr(),
            std::ptr::null(),
            std::ptr::null(),
            SW_SHOWNORMAL,
        )
    };
    if result as isize <= 32 {
        Err("shell.open_failed".into())
    } else {
        Ok(())
    }
}
