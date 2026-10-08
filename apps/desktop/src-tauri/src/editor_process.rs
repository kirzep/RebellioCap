use crate::engine::supervisor::Job;
use std::{io::Read, os::windows::process::CommandExt, process::{Child, Command, Output, Stdio}, sync::mpsc, thread, time::{Duration, Instant}};

const STDOUT_LIMIT: usize = 4 * 1024 * 1024;
const STDERR_LIMIT: usize = 64 * 1024;
struct OwnedProcess { child: Child, job: Job }
impl Drop for OwnedProcess {
    fn drop(&mut self) { self.job.terminate(); let _ = self.child.kill(); let _ = self.child.wait(); }
}
fn read_bounded(mut reader: impl Read, limit: usize) -> Result<Vec<u8>, String> {
    let mut bytes = Vec::new();
    let mut block = [0; 8192];
    loop {
        let count = reader.read(&mut block).map_err(|e| e.to_string())?;
        if count == 0 { return Ok(bytes); }
        if count > limit.saturating_sub(bytes.len()) { return Err("editor.output_limit".into()); }
        bytes.extend_from_slice(&block[..count]);
    }
}

pub(crate) fn run(command: &mut Command, timeout: Duration, cancelled: impl Fn() -> bool) -> Result<Output, String> {
    if cancelled() { return Err("editor.operation_cancelled".into()); }
    let deadline = Instant::now() + timeout;
    let job = Job::new()?;
    let child = command.creation_flags(0x08000000 | 0x00000004)
        .stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped())
        .spawn().map_err(|e| e.to_string())?;
    let mut process = OwnedProcess { child, job };
    let stdout = process.child.stdout.take().ok_or("editor.missing_stdout")?;
    let stderr = process.child.stderr.take().ok_or("editor.missing_stderr")?;
    let (send, receive) = mpsc::channel();
    let stdout_send = send.clone();
    let stdout_worker = thread::spawn(move || { let _ = stdout_send.send((true, read_bounded(stdout, STDOUT_LIMIT))); });
    let stderr_worker = thread::spawn(move || { let _ = send.send((false, read_bounded(stderr, STDERR_LIMIT))); });
    let result = (|| {
        process.job.assign_and_resume(&process.child)?;
        let mut stdout = None; let mut stderr = None;
        loop {
            if cancelled() { return Err("editor.operation_cancelled".into()); }
            if Instant::now() >= deadline { return Err("editor.operation_timeout".into()); }
            while let Ok((is_stdout, bytes)) = receive.try_recv() {
                let bytes = bytes?;
                if is_stdout { stdout = Some(bytes); } else { stderr = Some(bytes); }
            }
            if let Some(status) = process.child.try_wait().map_err(|e| e.to_string())? {
                if stdout.is_some() && stderr.is_some() { return Ok(Output { status, stdout: stdout.take().unwrap(), stderr: stderr.take().unwrap() }); }
            }
            thread::sleep(Duration::from_millis(10));
        }
    })();
    // Close inherited pipes and terminate descendants before joining readers.
    drop(process);
    let _ = stdout_worker.join(); let _ = stderr_worker.join();
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    fn script(code: &str) -> Command {
        let mut command = Command::new("powershell.exe");
        command.args(["-NoProfile", "-NonInteractive", "-Command", code]);
        command
    }
    #[test]
    fn pre_cancelled_operation_does_not_start() {
        let error = run(&mut script("Write-Output ok"), Duration::from_secs(5), || true).unwrap_err();
        assert_eq!(error, "editor.operation_cancelled");
    }
    #[test]
    fn timeout_stops_a_hung_process() {
        let started = std::time::Instant::now();
        let error = run(&mut script("Start-Sleep -Seconds 2"), Duration::from_millis(100), || false).unwrap_err();
        assert_eq!(error, "editor.operation_timeout");
        assert!(started.elapsed() < Duration::from_secs(1));
    }
    #[test]
    fn stderr_is_bounded() {
        let error = run(&mut script("[Console]::Error.Write(('x' * 70000))"), Duration::from_secs(5), || false).unwrap_err();
        assert_eq!(error, "editor.output_limit");
    }
    #[test]
    fn captures_both_pipes_without_deadlock_and_preserves_exit_status() {
        let output = run(&mut script("[Console]::Out.Write(('o' * 60000)); [Console]::Error.Write(('e' * 60000)); exit 7"), Duration::from_secs(10), || false).unwrap();
        assert_eq!(output.stdout, vec![b'o'; 60000]);
        assert_eq!(output.stderr, vec![b'e'; 60000]);
        assert_eq!(output.status.code(), Some(7));
    }
    #[test]
    fn stdout_is_bounded() {
        let error = run(&mut script("[Console]::Out.Write(('x' * 4200000))"), Duration::from_secs(10), || false).unwrap_err();
        assert_eq!(error, "editor.output_limit");
    }
    fn alive(pid: u32) -> bool {
        use windows_sys::Win32::{Foundation::CloseHandle, System::Threading::*};
        unsafe {
            let handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
            if handle.is_null() { return false; }
            let mut code = 0;
            let active = GetExitCodeProcess(handle, &mut code) != 0 && code == 259;
            CloseHandle(handle); active
        }
    }
    #[test]
    fn cancellation_terminates_parent_and_descendant() {
        let dir = tempfile::tempdir().unwrap();
        let marker = dir.path().join("pids.txt");
        let path = marker.to_string_lossy().replace('\'', "''");
        let code = format!("$p=Start-Process powershell.exe -WindowStyle Hidden -ArgumentList '-NoProfile','-Command','Start-Sleep -Seconds 30' -PassThru; [IO.File]::WriteAllText('{path}.pending', \"$PID,$($p.Id)\"); Move-Item -LiteralPath '{path}.pending' -Destination '{path}'; Start-Sleep -Seconds 30");
        let error = run(&mut script(&code), Duration::from_secs(10), || marker.exists()).unwrap_err();
        assert_eq!(error, "editor.operation_cancelled");
        let pids: Vec<u32> = std::fs::read_to_string(marker).unwrap().split(',').map(|p| p.parse().unwrap()).collect();
        let deadline = Instant::now() + Duration::from_secs(2);
        while pids.iter().any(|pid| alive(*pid)) && Instant::now() < deadline { thread::sleep(Duration::from_millis(10)); }
        assert!(pids.iter().all(|pid| !alive(*pid)));
    }
}
