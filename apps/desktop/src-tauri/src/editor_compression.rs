//! File-size policy, independent of the process runner so every export path shares
//! the same strict byte ceiling and cancellation checks.
use serde_json::Value;
use std::sync::atomic::{AtomicBool, Ordering};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Profile {
    Quality,
    HighQuality,
    Fast,
    MfHardware,
    MfSoftware,
}

impl Profile {
    pub fn name(self) -> &'static str {
        match self {
            Self::Quality | Self::HighQuality | Self::Fast => "h264_nvenc",
            Self::MfHardware | Self::MfSoftware => "h264_mf",
        }
    }

    pub fn video_args(self, rate: u64) -> Vec<String> {
        let mut args = vec!["-c:v".to_owned(), self.name().to_owned()];
        let mut push = |values: &[&str]| args.extend(values.iter().map(|v| v.to_string()));
        match self {
            Self::Quality | Self::HighQuality | Self::Fast => {
                push(&[
                    "-preset",
                    if self == Self::Fast { "p5" } else { "p7" },
                    "-rc",
                    "vbr",
                ]);
                if self != Self::Fast {
                    // NVENC analyzes each frame twice, not the whole file twice.
                    // Use spatial AQ only; combining spatial and temporal AQ is
                    // not recommended by NVIDIA for this quality preset.
                    push(&[
                        "-tune",
                        "hq",
                        "-multipass",
                        "fullres",
                        "-rc-lookahead",
                        "32",
                        "-spatial-aq",
                        "1",
                        "-aq-strength",
                        "8",
                        "-bf",
                        "3",
                        "-b_ref_mode",
                        "middle",
                    ]);
                }
                if self == Self::Quality {
                    push(&[
                        "-cq",
                        "20",
                        "-b:v",
                        "0",
                        "-maxrate",
                        "200000000",
                        "-bufsize",
                        "400000000",
                    ]);
                } else {
                    let (peak, buffer) = if self == Self::Fast {
                        (rate * 3 / 2, rate * 2)
                    } else {
                        (rate * 2, rate * 8)
                    };
                    push(&[
                        "-b:v",
                        &rate.to_string(),
                        "-maxrate",
                        &peak.to_string(),
                        "-bufsize",
                        &buffer.to_string(),
                    ]);
                }
            }
            Self::MfHardware | Self::MfSoftware => {
                push(&[
                    "-hw_encoding",
                    if self == Self::MfHardware { "1" } else { "0" },
                    "-b:v",
                    &rate.to_string(),
                ]);
            }
        }
        args
    }
}

pub(super) struct Encoded {
    pub plan: Value,
    pub bytes: u64,
    pub profile: Profile,
}

const AUDIO_RATE: f64 = 128_000.;
const HEADER_BYTES: f64 = 65_536.;
const MIN_RATE: u64 = 64_000;

fn number(plan: &Value, key: &str) -> f64 {
    plan[key].as_f64().unwrap_or(f64::NAN)
}
fn mode(plan: &Value) -> &str {
    plan["compression"].as_str().unwrap_or("smart")
}
fn limit(plan: &Value) -> Option<u64> {
    (plan["discord"] == true).then(|| (plan["discordLimitMb"].as_u64().unwrap_or(20)) * 1_000_000)
}
fn budget(plan: &Value, duration: f64) -> u64 {
    let rate = number(plan, "bitrate");
    let capped = limit(plan)
        .map(|bytes| ((bytes as f64 * 0.95 - HEADER_BYTES) * 8. / duration - AUDIO_RATE).floor())
        .unwrap_or(rate);
    rate.min(capped).max(0.) as u64
}
fn check_cancel(cancel: &AtomicBool) -> Result<(), String> {
    if cancel.load(Ordering::Acquire) {
        Err("Экспорт отменён".into())
    } else {
        Ok(())
    }
}

pub(super) fn validate_plan(plan: &Value, duration: f64) -> Result<(), String> {
    for key in ["width", "height", "requestedWidth", "requestedHeight"] {
        if key.starts_with("requested") && plan.get(key).is_none() {
            continue;
        }
        let value = number(plan, key);
        if !value.is_finite() || !(16. ..=7680.).contains(&value) || value % 2. != 0. {
            return Err("Некорректное разрешение экспорта".into());
        }
    }
    for key in ["fps", "requestedFps"] {
        if key == "requestedFps" && plan.get(key).is_none() {
            continue;
        }
        if !(1. ..=240.).contains(&number(plan, key)) {
            return Err("Некорректные параметры экспорта".into());
        }
    }
    if !duration.is_finite()
        || duration < 0.05
        || !(64_000. ..=200_000_000.).contains(&number(plan, "bitrate"))
    {
        return Err("Некорректные параметры экспорта".into());
    }
    if plan.get("compression").is_some()
        && !matches!(
            plan["compression"].as_str(),
            Some("smart" | "size" | "fast")
        )
    {
        return Err("Проверьте параметры сжатия.".into());
    }
    if plan["discord"] == true
        && plan.get("discordLimitMb").is_some()
        && !matches!(plan["discordLimitMb"].as_u64(), Some(20 | 50 | 500))
    {
        return Err("Некорректный лимит Discord".into());
    }
    if budget(plan, duration) < MIN_RATE {
        return Err(format!(
            "Монтаж слишком длинный для {} МБ. Сократите его.",
            limit(plan).unwrap_or(0) / 1_000_000
        ));
    }
    Ok(())
}

pub(super) fn encode_to_limit(
    plan: &Value,
    duration: f64,
    cancel: &AtomicBool,
    mut encode: impl FnMut(&Value, Profile, u64, usize) -> Result<u64, String>,
) -> Result<Encoded, String> {
    check_cancel(cancel)?;
    validate_plan(plan, duration)?;
    let ceiling = limit(plan);
    let fits = |bytes: u64| bytes > 0 && ceiling.is_none_or(|limit| bytes < limit);
    let mut attempt = 0;
    if mode(plan) == "smart" && ceiling.is_some() {
        let mut original = plan.clone();
        for (key, requested) in [
            ("width", "requestedWidth"),
            ("height", "requestedHeight"),
            ("fps", "requestedFps"),
        ] {
            if let Some(value) = plan.get(requested) {
                original[key] = value.clone();
            }
        }
        attempt += 1;
        let trial = encode(&original, Profile::Quality, 0, attempt);
        check_cancel(cancel)?;
        if let Ok(bytes) = trial {
            if fits(bytes) {
                return Ok(Encoded {
                    plan: original,
                    bytes,
                    profile: Profile::Quality,
                });
            }
        }
    }
    let profiles: &[Profile] = if mode(plan) == "fast" {
        &[Profile::Fast, Profile::MfHardware, Profile::MfSoftware]
    } else {
        &[
            Profile::HighQuality,
            Profile::Fast,
            Profile::MfHardware,
            Profile::MfSoftware,
        ]
    };
    let mut first_profile = 0;
    let mut rate = budget(plan, duration);
    for _ in 0..5 {
        attempt += 1;
        let mut last = String::new();
        let mut output = None;
        for (index, &profile) in profiles.iter().enumerate().skip(first_profile) {
            check_cancel(cancel)?;
            let result = encode(plan, profile, rate, attempt);
            check_cancel(cancel)?;
            match result {
                Ok(bytes) if bytes > 0 => {
                    first_profile = index;
                    output = Some((bytes, profile));
                    break;
                }
                Ok(_) => last = "Кодировщик создал пустой файл".into(),
                Err(error) => last = error,
            }
        }
        let (bytes, profile) =
            output.ok_or_else(|| format!("Кодирование H.264 недоступно: {last}"))?;
        if fits(bytes) {
            return Ok(Encoded {
                plan: plan.clone(),
                bytes,
                profile,
            });
        }
        let ceiling = ceiling.unwrap();
        // Keep the audio/container allowance separate. Scaling the total file
        // size as though it were all video cuts the bitrate unnecessarily.
        let fixed = duration * AUDIO_RATE / 8. + HEADER_BYTES;
        let ratio = ((ceiling as f64 * 0.98 - fixed) / (bytes as f64 - fixed).max(1.) * 0.99)
            .clamp(0.1, 0.95);
        let next = (rate as f64 * ratio) as u64;
        if rate == MIN_RATE {
            break;
        }
        rate = next.max(MIN_RATE);
    }
    Err(format!(
        "Не удалось уложиться в {} МБ. Сократите монтаж или снизьте разрешение.",
        ceiling.unwrap() / 1_000_000
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    fn plan(mode: &str) -> Value {
        json!({"width":960,"height":540,"fps":30,"requestedWidth":1920,
            "requestedHeight":1080,"requestedFps":60,"compression":mode,
            "bitrate":1_100_000,"discord":true,"discordLimitMb":20})
    }
    #[test]
    fn smart_keeps_full_geometry_when_quality_trial_fits() {
        let result = encode_to_limit(
            &plan("smart"),
            120.,
            &AtomicBool::new(false),
            |p, profile, _, attempt| {
                assert_eq!(profile, Profile::Quality);
                assert_eq!(
                    (p["width"].as_u64(), p["fps"].as_u64()),
                    (Some(1920), Some(60))
                );
                assert_eq!(attempt, 1);
                Ok(8_000_000)
            },
        )
        .unwrap();
        assert_eq!(result.bytes, 8_000_000);
        assert_eq!(result.plan["height"], 1080);
    }
    #[test]
    fn oversized_trial_uses_budget_geometry_and_retries_from_measured_size() {
        let mut calls = Vec::new();
        let result = encode_to_limit(
            &plan("smart"),
            120.,
            &AtomicBool::new(false),
            |p, profile, rate, _| {
                calls.push((profile, p["width"].as_u64().unwrap(), rate));
                Ok(match calls.len() {
                    1 => 40_000_000,
                    2 => 21_000_000,
                    _ => 19_500_000,
                })
            },
        )
        .unwrap();
        assert_eq!(result.bytes, 19_500_000);
        assert_eq!(calls.len(), 3);
        assert_eq!(calls[0].1, 1920);
        assert_eq!(calls[1].1, 960);
        assert!(calls[2].2 < calls[1].2);
        assert!(
            calls[2].2 > 900_000,
            "retry must avoid the old excessive bitrate cut"
        );
    }
    #[test]
    fn unsupported_hq_falls_back_and_does_not_retry_failed_profiles() {
        let mut calls = Vec::new();
        let mut successes = 0;
        let result = encode_to_limit(
            &plan("size"),
            120.,
            &AtomicBool::new(false),
            |_, profile, _, _| {
                calls.push(profile);
                if profile != Profile::MfSoftware {
                    return Err("unsupported".into());
                }
                successes += 1;
                Ok(if successes == 1 {
                    22_000_000
                } else {
                    18_000_000
                })
            },
        )
        .unwrap();
        assert_eq!(result.profile, Profile::MfSoftware);
        assert_eq!(
            calls,
            vec![
                Profile::HighQuality,
                Profile::Fast,
                Profile::MfHardware,
                Profile::MfSoftware,
                Profile::MfSoftware
            ]
        );
    }
    #[test]
    fn equality_with_upload_limit_is_rejected_and_attempts_are_bounded() {
        let mut count = 0;
        let result = encode_to_limit(
            &plan("fast"),
            120.,
            &AtomicBool::new(false),
            |_, _, _, _| {
                count += 1;
                Ok(20_000_000)
            },
        );
        assert!(result.is_err());
        assert_eq!(count, 5);
    }
    #[test]
    fn cancellation_prevents_trial_retry_and_publication() {
        let cancel = AtomicBool::new(true);
        let result = encode_to_limit(&plan("smart"), 120., &cancel, |_, _, _, _| {
            panic!("must not start")
        });
        assert_eq!(result.err().unwrap(), "Экспорт отменён");
        cancel.store(false, Ordering::Release);
        let result = encode_to_limit(&plan("smart"), 120., &cancel, |_, _, _, _| {
            cancel.store(true, Ordering::Release);
            Ok(1_000_000)
        });
        assert_eq!(result.err().unwrap(), "Экспорт отменён");
    }
    #[test]
    fn validates_all_tiers_and_impossible_budgets_before_encoding() {
        for limit in [20, 50, 500] {
            let mut p = plan("size");
            p["discordLimitMb"] = json!(limit);
            p["bitrate"] = json!(200_000_000);
            let result = encode_to_limit(&p, 120., &AtomicBool::new(false), |_, _, rate, _| {
                assert!(rate as f64 * 120. / 8. + 1_920_000. + 65_536. < limit as f64 * 1e6);
                Ok(limit * 1_000_000 - 1)
            });
            assert!(result.is_ok());
        }
        for key in ["width", "requestedWidth", "fps", "bitrate", "compression"] {
            let mut p = plan("smart");
            p[key] = json!("bad");
            assert!(validate_plan(&p, 120.).is_err(), "{key}");
        }
        assert!(validate_plan(&plan("smart"), 10_000.).is_err());
    }
    #[test]
    fn non_discord_exports_keep_the_manual_bitrate_and_skip_quality_trial() {
        let mut p = plan("smart");
        p["discord"] = json!(false);
        p["bitrate"] = json!(12_000_000);
        let result = encode_to_limit(&p, 120., &AtomicBool::new(false), |_, profile, rate, _| {
            assert_eq!(profile, Profile::HighQuality);
            assert_eq!(rate, 12_000_000);
            Ok(80_000_000)
        })
        .unwrap();
        assert_eq!(result.bytes, 80_000_000);
    }
    #[test]
    fn failed_trial_and_zero_length_output_cannot_be_accepted() {
        let mut profiles = Vec::new();
        let result = encode_to_limit(
            &plan("smart"),
            120.,
            &AtomicBool::new(false),
            |_, profile, _, _| {
                profiles.push(profile);
                match profile {
                    Profile::Quality => Err("No GPU".into()),
                    Profile::HighQuality => Ok(0),
                    _ => Ok(19_000_000),
                }
            },
        )
        .unwrap();
        assert_eq!(result.profile, Profile::Fast);
        assert_eq!(
            profiles,
            vec![Profile::Quality, Profile::HighQuality, Profile::Fast]
        );
    }
    #[test]
    #[ignore = "Requires packaged FFmpeg and NVIDIA NVENC"]
    fn real_nvenc_oversized_trial_fits_limit_after_budget_fallback() {
        use super::super::{probe, run_encode};
        use std::{
            path::PathBuf,
            sync::{Arc, Mutex},
        };
        let engine = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("resources/engine/RebellioCap.Engine.exe");
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("fit.mp4");
        let status = Arc::new(Mutex::new(json!({})));
        let cancel = AtomicBool::new(false);
        let p = json!({"width":480,"height":270,"fps":30,"requestedWidth":640,"requestedHeight":360,
            "requestedFps":60,"duration":90,"bitrate":1_550_000,"discord":true,"discordLimitMb":20,"compression":"smart"});
        let mut sizes = Vec::new();
        let encoded=encode_to_limit(&p,90.,&cancel,|candidate,profile,rate,attempt| {
            let w=candidate["width"].as_u64().unwrap();let h=candidate["height"].as_u64().unwrap();
            let fps=candidate["fps"].as_u64().unwrap();
            let graph=format!("testsrc2=s={w}x{h}:r={fps}:d=90,noise=alls=20:allf=t:all_seed=42,format=nv12[outv];sine=frequency=440:sample_rate=48000:duration=90[outa]");
            run_encode(&engine,&[],&graph,candidate,&path,profile,rate,&status,&cancel,attempt)?;
            let bytes=std::fs::metadata(&path).unwrap().len();sizes.push((profile,bytes));Ok(bytes)
        }).unwrap();
        assert!(
            sizes[0].1 >= 20_000_000,
            "fixture must exercise the oversized trial: {sizes:?}"
        );
        assert_ne!(encoded.profile, Profile::Quality);
        assert!(encoded.bytes < 20_000_000);
        let info = probe(&engine, &path).unwrap();
        assert_eq!(info["streams"][0]["width"], 480);
        assert_eq!(info["streams"][0]["r_frame_rate"], "30/1");
        let seconds = info["format"]["duration"]
            .as_str()
            .unwrap()
            .parse::<f64>()
            .unwrap();
        assert!(
            (seconds - 90.).abs() < 0.1,
            "export must keep the whole timeline"
        );
        eprintln!(
            "Real byte-limited fallback: {sizes:?}; final {} bytes",
            encoded.bytes
        );
    }
    #[test]
    #[ignore = "Requires packaged FFmpeg and NVIDIA NVENC"]
    fn real_nvenc_mid_run_cancellation_terminates_encoder() {
        use super::super::run_encode;
        use std::{
            path::PathBuf,
            sync::{Arc, Mutex},
            time::{Duration, Instant},
        };
        let engine = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("resources/engine/RebellioCap.Engine.exe");
        let dir = tempfile::tempdir().unwrap();
        let status = Arc::new(Mutex::new(json!({})));
        let cancel = Arc::new(AtomicBool::new(false));
        let signal = cancel.clone();
        let progress = status.clone();
        let started = Instant::now();
        let trigger = std::thread::spawn(move || {
            while started.elapsed() < Duration::from_secs(5) {
                if progress.lock().unwrap()["state"] == "running" {
                    signal.store(true, Ordering::Release);
                    return true;
                }
                std::thread::sleep(Duration::from_millis(10));
            }
            signal.store(true, Ordering::Release);
            false
        });
        let result = run_encode(
            &engine,
            &[],
            "testsrc2=s=1920x1080:r=60:d=600,format=nv12[outv];anullsrc,atrim=duration=600[outa]",
            &json!({"duration":600}),
            &dir.path().join("cancel.mp4"),
            Profile::Quality,
            0,
            &status,
            &cancel,
            1,
        );
        let was_running = trigger.join().unwrap();
        assert!(
            was_running,
            "encoder must publish progress before cancellation"
        );
        assert_eq!(result.unwrap_err(), "Экспорт отменён");
        assert!(started.elapsed() < Duration::from_secs(6));
    }
    #[test]
    #[ignore = "Requires packaged FFmpeg and NVIDIA NVENC"]
    fn real_nvenc_smart_and_budget_exports_are_decodable() {
        use super::super::{command, probe, run_encode};
        use std::{
            path::PathBuf,
            sync::{Arc, Mutex},
        };
        let engine = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("resources/engine/RebellioCap.Engine.exe");
        let dir = tempfile::tempdir().unwrap();
        let status = Arc::new(Mutex::new(json!({})));
        let cancel = AtomicBool::new(false);
        let p = json!({"width":1440,"height":810,"fps":60,"requestedWidth":1920,"requestedHeight":1080,
            "requestedFps":60,"duration":30,"bitrate":4_900_000,"discord":true,"discordLimitMb":20,"compression":"smart"});
        let path = dir.path().join("quality.mp4");
        let encoded=encode_to_limit(&p,30.,&cancel,|candidate,profile,rate,attempt| {
            let w=candidate["width"].as_u64().unwrap();let h=candidate["height"].as_u64().unwrap();
            let graph=format!("color=c=0x1c2344:s={w}x{h}:r=60:d=30,drawgrid=w=120:h=120:t=1:c=white@0.5,format=nv12[outv];sine=frequency=440:sample_rate=48000:duration=30[outa]");
            run_encode(&engine,&[],&graph,candidate,&path,profile,rate,&status,&cancel,attempt)?;
            Ok(std::fs::metadata(&path).unwrap().len())
        }).unwrap();
        assert_eq!(encoded.profile, Profile::Quality);
        assert!(encoded.bytes < 20_000_000);
        let info = probe(&engine, &path).unwrap();
        assert_eq!(info["streams"][0]["width"], 1920);
        assert_eq!(info["streams"][0]["height"], 1080);
        assert_eq!(info["streams"][0]["r_frame_rate"], "60/1");
        assert_eq!(info["streams"][1]["codec_name"], "aac");
        eprintln!(
            "Smart static fixture: {} bytes, 1920x1080 60 FPS, 30 seconds",
            encoded.bytes
        );
        for profile in [Profile::HighQuality, Profile::Fast] {
            let path = dir.path().join(format!("{profile:?}.mp4"));
            run_encode(&engine,&[],"testsrc2=s=640x360:r=60:d=8,format=nv12[outv];sine=frequency=440:sample_rate=48000:duration=8[outa]",
                &json!({"duration":8}),&path,profile,500_000,&status,&cancel,1).unwrap();
            let info = probe(&engine, &path).unwrap();
            assert_eq!(info["streams"][0]["codec_name"], "h264");
            assert_eq!(info["streams"][0]["r_frame_rate"], "60/1");
            assert!(std::fs::metadata(&path).unwrap().len() < 1_000_000);
            let decoded = command(&engine.with_file_name("ffmpeg.exe"))
                .args(["-v", "error", "-i"])
                .arg(&path)
                .args(["-f", "null", "-"])
                .output()
                .unwrap();
            assert!(
                decoded.status.success(),
                "{}",
                String::from_utf8_lossy(&decoded.stderr)
            );
            let measured = command(&engine.with_file_name("ffmpeg.exe"))
                .args(["-hide_banner", "-i"])
                .arg(&path)
                .args([
                    "-filter_complex",
                    "testsrc2=s=640x360:r=60:d=8,format=yuv420p[ref];[0:v][ref]ssim",
                    "-an",
                    "-f",
                    "null",
                    "-",
                ])
                .output()
                .unwrap();
            assert!(
                measured.status.success(),
                "{}",
                String::from_utf8_lossy(&measured.stderr)
            );
            let log = String::from_utf8_lossy(&measured.stderr);
            let metric = log.lines().find(|line| line.contains("SSIM Y:")).unwrap();
            eprintln!(
                "{profile:?}: {} bytes; {metric}",
                std::fs::metadata(path).unwrap().len()
            );
        }
    }
}
