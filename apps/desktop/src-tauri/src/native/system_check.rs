use crate::{
    config::OnboardingDraft,
    engine::{supervisor::run_once, Result},
    model::*,
};
use serde::Deserialize;
use std::{io::Write, path::Path, time::Duration};

pub fn supported_platform(major: u32, native_architecture: u16) -> bool {
    major >= 10 && native_architecture == 9 // PROCESSOR_ARCHITECTURE_AMD64
}

pub fn check_output(path: &Path, required_bytes: u64) -> Result<()> {
    if !path.is_absolute()
        || !path.is_dir()
        || !crate::model::is_canonical_path(path)
    {
        return Err("output.not_canonical_directory".into());
    }
    let mut probe = tempfile::Builder::new()
        .prefix(".RebellioCap-write-check-")
        .tempfile_in(path)
        .map_err(|e| e.to_string())?;
    probe
        .write_all(b"RebellioCap")
        .and_then(|()| probe.as_file().sync_all())
        .map_err(|e| e.to_string())?;
    use std::os::windows::ffi::OsStrExt;
    let wide: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
    let mut available = 0;
    let ok = unsafe {
        windows_sys::Win32::Storage::FileSystem::GetDiskFreeSpaceExW(
            wide.as_ptr(),
            &mut available,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
        )
    };
    if ok == 0 || available < required_bytes {
        return Err("output.insufficient_space".into());
    }
    Ok(())
}
pub fn estimate_bytes(draft: &OnboardingDraft) -> u64 {
    u64::from(draft.bitrate.unwrap_or(30_000_000)) / 8
        * u64::from(draft.replay_seconds.unwrap_or(30))
        * 2
        + 256 * 1024 * 1024
}
pub fn run(
    executable: Result<&Path>,
    draft: &OnboardingDraft,
    fallback: &Path,
) -> SystemCheckResult {
    let mut stages = Vec::new();
    let mut stage = |id: &str, result: Result<()>| {
        stages.push(SystemCheckStage {
            id: id.into(),
            passed: result.is_ok(),
            message: result.err().unwrap_or_else(|| "Available".into()),
        })
    };
    stage(
        "windows_x64",
        if cfg!(all(windows, target_arch = "x86_64"))
            && unsafe {
                let mut info: windows_sys::Win32::System::SystemInformation::SYSTEM_INFO =
                    std::mem::zeroed();
                windows_sys::Win32::System::SystemInformation::GetNativeSystemInfo(&mut info);
                supported_platform(
                    windows_version::OsVersion::current().major,
                    info.Anonymous.Anonymous.wProcessorArchitecture,
                )
            }
        {
            Ok(())
        } else {
            Err("Windows x64 is required".into())
        },
    );
    let memory = unsafe {
        use windows_sys::Win32::System::SystemInformation::*;
        let mut status: MEMORYSTATUSEX = std::mem::zeroed();
        status.dwLength = std::mem::size_of_val(&status) as u32;
        if GlobalMemoryStatusEx(&mut status) != 0 && status.ullAvailPhys >= estimate_bytes(draft) {
            Ok(())
        } else {
            Err("memory.insufficient_available".into())
        }
    };
    stage("available_memory", memory);
    stage(
        "output_directory_and_space",
        check_output(
            draft.output_directory.as_deref().unwrap_or(fallback),
            estimate_bytes(draft),
        ),
    );
    match executable {
        Err(e) => stage("engine_presence", Err(e)),
        Ok(path) => {
            stage("engine_presence", Ok(()));
            stage("native_hardware_and_runtime", doctor(path, draft));
            let catalog = super::catalogs::load(path).and_then(|c| {
                if c.monitors.is_empty() {
                    return Err("monitor.none_available".into());
                }
                if let Some(id) = &draft.monitor_id {
                    if !c.inventory().monitor_ids.contains(id) {
                        return Err("monitor.selected_unavailable".into());
                    }
                }
                for (selection, endpoints) in [
                    (&draft.system_audio, &c.audio.system_audio),
                    (&draft.microphone, &c.audio.microphones),
                ] {
                    if let Some(AudioSelection::Endpoint(id)) = selection {
                        if !endpoints.iter().any(|e| &e.id == id) {
                            return Err("audio.selected_unavailable".into());
                        }
                    }
                }
                Ok(())
            });
            stage("monitor_and_audio_catalogs", catalog);
        }
    }
    SystemCheckResult {
        passed: stages.iter().all(|v| v.passed),
        stages,
        diagnostics_path: None,
    }
}
#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct Doctor {
    protocol_version: u32,
    #[serde(rename = "type")]
    kind: String,
    passed: bool,
    details: DoctorDetails,
}
// Keep the complete wire schema typed even for diagnostic fields not projected to the renderer.
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
#[allow(dead_code)]
struct DoctorDetails {
    monitor_id: String,
    #[serde(default)]
    recording_color_space: Option<String>,
    #[serde(default)]
    hdr_fidelity: bool,
    #[serde(default)]
    hdr_tone_mapping: bool,
    windows_x64: bool, os_version: String, gpu: String, driver_version: String, monitor_count: u32,
    nvenc: bool, nvenc_max_major: u32, nvenc_max_minor: u32, h264: bool, nv12: bool, video_error: String,
    audio_endpoints_and_aac: bool, system_audio: bool, system_audio_error: String,
    microphone: bool, microphone_error: String, microphone_message: String, microphone_hresult: i64,
    aac: bool, aac_error: String, libavformat: u32, libavcodec: u32, libavutil: u32, passed: bool,
}
pub fn validate_doctor_result(bytes: &[u8], success: bool, draft: &OnboardingDraft) -> Result<()> {
    let result: Doctor = serde_json::from_slice(bytes).map_err(|e| e.to_string())?;
    let d = &result.details;
    if draft.monitor_id.as_ref().is_some_and(|id| id != &d.monitor_id) {
        return Err("monitor.selected_diagnostic_mismatch".into());
    }
    let native_video = d.monitor_count > 0 && d.nvenc && d.h264 && d.nv12;
    let native_audio = d.system_audio && d.microphone && d.aac;
    // Legacy doctor tests every audio device. Recompute requirements using the current draft,
    // but first verify that its envelope, exit status and detailed summary agree.
    if result.protocol_version != 1 || result.kind != "doctor" || result.passed != success ||
        result.passed != d.passed || d.passed != (native_video && native_audio) || d.audio_endpoints_and_aac != native_audio ||
        !d.windows_x64 || !native_video || d.libavformat == 0 || d.libavcodec == 0 || d.libavutil == 0 {
        return Err("native.hardware_or_runtime_unavailable".into());
    }
    let system = matches!(&draft.system_audio, Some(AudioSelection::Endpoint(_)));
    let microphone = matches!(&draft.microphone, Some(AudioSelection::Endpoint(_)));
    if (system && !d.system_audio) || (microphone && !d.microphone) || ((system || microphone) && !d.aac) { return Err("native.selected_audio_unavailable".into()); }
    Ok(())
}
fn doctor_arguments(draft: &OnboardingDraft) -> Vec<std::ffi::OsString> {
    let mut args = vec!["doctor-json".into()];
    if let Some(id) = &draft.monitor_id { args.extend(["--monitor".into(), id.into()]); }
    args
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn doctor_passes_the_current_draft_monitor() {
        let mut draft = OnboardingDraft::default();
        assert_eq!(doctor_arguments(&draft), vec![std::ffi::OsString::from("doctor-json")]);
        draft.monitor_id = Some("42:1".into());
        assert_eq!(doctor_arguments(&draft), ["doctor-json", "--monitor", "42:1"].map(std::ffi::OsString::from).to_vec());
    }
}

fn doctor(path: &Path, draft: &OnboardingDraft) -> Result<()> {
    let (bytes, success) = run_once(path, &doctor_arguments(draft), Duration::from_secs(30))?;
    validate_doctor_result(&bytes, success, draft)
}
