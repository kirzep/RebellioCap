use super::{protocol::*, Result};
use crate::model::{EngineLifecycle, EngineSnapshot, NativeError};
use std::{
    collections::HashMap,
    ffi::OsString,
    io::{BufReader, Read, Write},
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        mpsc::{self, Receiver, SyncSender},
        Arc, Mutex,
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

fn command_error(error: &NativeError) -> String {
    if error.code == "config.replay_memory_unavailable" {
        format!("{}: {}", error.code, error.message)
    } else {
        error.code.clone()
    }
}

struct BoundedCommandWriter {
    commands: Option<SyncSender<Vec<u8>>>,
    completed: Receiver<Result<()>>,
    worker: Option<JoinHandle<()>>,
}
impl BoundedCommandWriter {
    fn spawn(mut writer: impl Write + Send + 'static) -> Self {
        // One outstanding write: the actor waits for its completion or deadline
        // before submitting another. Neither queue grows with stalled stdin.
        let (commands, requests) = mpsc::sync_channel::<Vec<u8>>(1);
        let (finished, completed) = mpsc::sync_channel(1);
        let worker = thread::spawn(move || {
            while let Ok(bytes) = requests.recv() {
                let result = writer.write_all(&bytes)
                    .map_err(|e| format!("engine.stdin_write_failed: {e}"));
                let failed = result.is_err();
                if finished.send(result).is_err() || failed { break; }
            }
        });
        Self { commands: Some(commands), completed, worker: Some(worker) }
    }
    fn write(&mut self, bytes: Vec<u8>, timeout: Duration) -> Result<()> {
        self.commands.as_ref().ok_or("engine.stdin_closed")?
            .try_send(bytes).map_err(|_| "engine.stdin_closed")?;
        self.completed.recv_timeout(timeout).map_err(|e| match e {
            mpsc::RecvTimeoutError::Timeout => "engine.stdin_write_timeout",
            mpsc::RecvTimeoutError::Disconnected => "engine.stdin_closed",
        })?
    }
}
impl Drop for BoundedCommandWriter {
    fn drop(&mut self) {
        self.commands.take();
        // OwnedProcess terminates the complete job first. Closing all readers
        // releases a blocked pipe write; never cancel an individual IO thread.
        if let Some(worker) = self.worker.take() { let _ = worker.join(); }
    }
}

pub fn resolve_engine(
    resources: &Path,
    debug_override: Option<&Path>,
    allow_debug: bool,
) -> Result<PathBuf> {
    let path = match debug_override {
        Some(p) if cfg!(debug_assertions) && allow_debug => p.to_owned(),
        Some(_) => return Err("engine.release_override_prohibited".into()),
        None => resources.join("engine/RebellioCap.Engine.exe"),
    };
    if !path.is_absolute() || !path.is_file() {
        return Err("engine.executable_missing_or_relative".into());
    }
    let canonical = path.canonicalize().map_err(|e| e.to_string())?;
    if debug_override.is_some() && canonical != path {
        return Err("engine.override_not_canonical".into());
    }
    Ok(canonical)
}

struct OwnedProcess {
    child: Child,
    job: Job,
    frames: Receiver<Result<Option<Vec<u8>>>>,
    stdin_writer: Option<BoundedCommandWriter>,
}
impl OwnedProcess {
    fn spawn(executable: &Path, args: &[OsString]) -> Result<Self> {
        Self::spawn_routed(executable, args, None)
    }
    fn spawn_routed(executable: &Path, args: &[OsString], actor: Option<SyncSender<ActorInput>>) -> Result<Self> {
        if !executable.is_absolute()
            || !executable.is_file()
            || executable.canonicalize().map_err(|e| e.to_string())? != executable
        {
            return Err("engine.executable_not_canonical".into());
        }
        let job = Job::new()?;
        let mut command = Command::new(executable);
        command
            .args(args)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            command.creation_flags(0x08000000 | 0x00000004);
        }
        let mut child = command
            .spawn()
            .map_err(|e| format!("engine.spawn_failed: {e}"))?;
        if let Err(e) = job.assign_and_resume(&child) {
            let _ = child.kill();
            let _ = child.wait();
            return Err(e);
        }
        crate::logging::record("info", "engine", "spawn", serde_json::json!({"pid":child.id(), "mode":args.first().map(|a|a.to_string_lossy())}));
        if let Some(mut stderr) = child.stderr.take() {
            thread::spawn(move || {
                let mut bytes=[0u8;4096];
                loop {
                    match stderr.read(&mut bytes) {
                        Ok(0) | Err(_) => break,
                        Ok(n) => crate::logging::record("warn","engine","stderr",serde_json::json!({"message":String::from_utf8_lossy(&bytes[..n])})),
                    }
                }
            });
        }
        let stdout = child.stdout.take().ok_or("engine.stdout_missing")?;
        let stdin_writer = if actor.is_some() {
            Some(BoundedCommandWriter::spawn(child.stdin.take().ok_or("engine.stdin_closed")?))
        } else { None };
        let (tx, frames) = mpsc::sync_channel(32);
        thread::spawn(move || {
            let mut reader = BufReader::new(stdout);
            loop {
                let frame = read_frame(&mut reader);
                let finished = !matches!(frame, Ok(Some(_)));
                let failed = match &actor {
                    Some(events) => events.send(ActorInput::Frame(frame)).is_err(),
                    None => tx.send(frame).is_err(),
                };
                if failed || finished { break; }
            }
        });
        Ok(Self { child, job, frames, stdin_writer })
    }
    fn write(&mut self, bytes: Vec<u8>, timeout: Duration) -> Result<()> {
        self.stdin_writer.as_mut().ok_or("engine.stdin_closed")?.write(bytes, timeout)
    }
    fn terminate(&mut self) {
        self.job.terminate();
        let _ = self.child.kill();
        let _ = self.child.wait();
        self.stdin_writer.take();
    }
}
impl Drop for OwnedProcess {
    fn drop(&mut self) {
        self.terminate();
    }
}

#[cfg(windows)]
pub(crate) struct Job(windows_sys::Win32::Foundation::HANDLE);
#[cfg(windows)]
unsafe impl Send for Job {}
#[cfg(windows)]
impl Job {
    pub(crate) fn new() -> Result<Self> {
        use windows_sys::Win32::System::JobObjects::*;
        unsafe {
            let handle = CreateJobObjectW(std::ptr::null(), std::ptr::null());
            if handle.is_null() {
                return Err(std::io::Error::last_os_error().to_string());
            }
            let job = Self(handle);
            let mut limits: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = std::mem::zeroed();
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if SetInformationJobObject(
                handle,
                JobObjectExtendedLimitInformation,
                &limits as *const _ as *const _,
                std::mem::size_of_val(&limits) as u32,
            ) == 0
            {
                return Err(std::io::Error::last_os_error().to_string());
            }
            Ok(job)
        }
    }
    pub(crate) fn assign_and_resume(&self, child: &Child) -> Result<()> {
        use std::os::windows::io::AsRawHandle;
        use windows_sys::Win32::{
            Foundation::*,
            System::{
                Diagnostics::ToolHelp::*, JobObjects::AssignProcessToJobObject, Threading::*,
            },
        };
        unsafe {
            if AssignProcessToJobObject(self.0, child.as_raw_handle()) == 0 {
                return Err(std::io::Error::last_os_error().to_string());
            }
            // The process has never executed user code: resume only after job ownership exists.
            let list = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if list == INVALID_HANDLE_VALUE {
                return Err(std::io::Error::last_os_error().to_string());
            }
            let mut entry: THREADENTRY32 = std::mem::zeroed();
            entry.dwSize = std::mem::size_of_val(&entry) as u32;
            let mut has = Thread32First(list, &mut entry);
            let mut resumed = false;
            while has != 0 {
                if entry.th32OwnerProcessID == child.id() {
                    let thread = OpenThread(THREAD_SUSPEND_RESUME, 0, entry.th32ThreadID);
                    if !thread.is_null() {
                        resumed = ResumeThread(thread) != u32::MAX;
                        CloseHandle(thread);
                    }
                    break;
                }
                has = Thread32Next(list, &mut entry);
            }
            CloseHandle(list);
            if resumed {
                Ok(())
            } else {
                Err("engine.resume_failed".into())
            }
        }
    }
    pub(crate) fn terminate(&self) {
        unsafe {
            windows_sys::Win32::System::JobObjects::TerminateJobObject(self.0, 1);
        }
    }
}
#[cfg(windows)]
impl Drop for Job {
    fn drop(&mut self) {
        unsafe {
            windows_sys::Win32::Foundation::CloseHandle(self.0);
        }
    }
}

type Reply = SyncSender<Result<Event>>;
enum Control {
    Request(SessionCommand, Reply),
    Shutdown(SyncSender<Result<()>>),
}
enum ActorInput { Control(Control), Frame(Result<Option<Vec<u8>>>) }
/// A queue-readiness signal. It must not synchronously acquire the Host mutex.
pub type NotificationWake = Arc<dyn Fn() + Send + Sync>;
pub struct EngineSupervisor {
    control: SyncSender<ActorInput>,
    snapshot: Arc<Mutex<EngineSnapshot>>,
    running: Arc<AtomicBool>,
    worker: Mutex<Option<JoinHandle<()>>>,
    timeout: Duration,
    pid: u32,
    notifications: Arc<Mutex<Vec<&'static str>>>,
    wakeups: Arc<AtomicU64>,
}
impl EngineSupervisor {
    pub fn spawn(executable: &Path, args: &[OsString], timeout: Duration) -> Result<Self> {
        Self::spawn_with_notification_wake(executable, args, timeout, None)
    }
    pub fn spawn_with_notification_wake(executable: &Path, args: &[OsString], timeout: Duration, notification_wake: Option<NotificationWake>) -> Result<Self> {
        // Commands and stdout share one bounded queue: either wakes the actor
        // immediately, with no periodic control-channel polling or relay thread.
        let (control, rx) = mpsc::sync_channel(32);
        let process = OwnedProcess::spawn_routed(executable, args, Some(control.clone()))?;
        let first = match rx.recv_timeout(timeout).map_err(|_| "engine.ready_timeout")? {
            ActorInput::Frame(frame) => frame?.ok_or("engine.eof_before_ready")?,
            ActorInput::Control(_) => return Err("engine.ready_required".into()),
        };
        let event = decode_event(&first)?;
        if event.kind != EventType::Ready {
            return Err(event
                .error
                .map(|e| command_error(&e))
                .unwrap_or("engine.ready_required".into()));
        }
        crate::logging::record("info", "engine", "ready", serde_json::json!({"snapshot":event.snapshot}));
        let initial = event.snapshot.ok_or("engine.snapshot_missing")?;
        if initial.lifecycle != EngineLifecycle::Ready || !initial.replay_active {
            return Err("engine.invalid_ready".into());
        }
        let notifications = Arc::new(Mutex::new(crate::notifications::transitions(&EngineSnapshot::default(), &initial)));
        let snapshot = Arc::new(Mutex::new(initial));
        let running = Arc::new(AtomicBool::new(true));
        let pid = process.child.id();
        let state = snapshot.clone();
        let alive = running.clone();
        let signals = notifications.clone();
        let wakeups = Arc::new(AtomicU64::new(0));
        let observed_wakeups = wakeups.clone();
        if let Some(wake) = &notification_wake { wake(); }
        let worker = thread::spawn(move || actor(process, rx, state, alive, timeout, signals, observed_wakeups, notification_wake));
        Ok(Self {
            control,
            snapshot,
            running,
            worker: Mutex::new(Some(worker)),
            timeout,
            pid,
            notifications,
            wakeups,
        })
    }
    /// Actor loop entries, for idle-wakeup diagnostics (not process CPU usage).
    pub fn actor_wakeups(&self) -> u64 { self.wakeups.load(Ordering::Relaxed) }
    pub fn drain_notifications(&self) -> Vec<&'static str> {
        std::mem::take(&mut *self.notifications.lock().unwrap())
    }
    pub fn snapshot(&self) -> EngineSnapshot {
        self.snapshot.lock().unwrap().clone()
    }
    pub fn is_running(&self) -> bool {
        self.running.load(Ordering::Acquire)
    }
    pub fn process_id(&self) -> u32 {
        self.pid
    }
    pub fn request(&self, command: SessionCommand) -> Result<Event> {
        if command == SessionCommand::Stop {
            return Err("engine.use_stop".into());
        }
        crate::logging::record("info", "engine", "request", serde_json::json!({"command":format!("{command:?}")}));
        let (tx, rx) = mpsc::sync_channel(1);
        self.control
            .send(ActorInput::Control(Control::Request(command, tx)))
            .map_err(|_| "engine.not_running")?;
        rx.recv_timeout(self.timeout + Duration::from_secs(1))
            .map_err(|_| "engine.request_timeout")?
    }
    pub fn stop(&self) -> Result<()> {
        let (tx, rx) = mpsc::sync_channel(1);
        let result = if self.is_running() {
            self.control
                .send(ActorInput::Control(Control::Shutdown(tx)))
                .map_err(|_| "engine.not_running")?;
            rx.recv_timeout(self.timeout + Duration::from_secs(1))
                .map_err(|_| "engine.stop_timeout")?
        } else {
            Ok(())
        };
        if let Some(worker) = self.worker.lock().unwrap().take() {
            let _ = worker.join();
        }
        result
    }
}
impl Drop for EngineSupervisor {
    fn drop(&mut self) {
        let _ = self.stop();
    }
}

fn actor(
    mut process: OwnedProcess,
    control: Receiver<ActorInput>,
    snapshot: Arc<Mutex<EngineSnapshot>>,
    running: Arc<AtomicBool>,
    timeout: Duration,
    notifications: Arc<Mutex<Vec<&'static str>>>,
    wakeups: Arc<AtomicU64>,
    notification_wake: Option<NotificationWake>,
) {
    let mut correlation = Correlator::default();
    let mut pending: HashMap<String, (Instant, Reply)> = HashMap::new();
    let mut counter = 0u64;
    let mut last_log_snapshot = Instant::now();
    // The engine publishes a heartbeat snapshot once per second, including
    // genuine idle. Do not use capture counters as a liveness signal.
    let snapshot_timeout = timeout.max(Duration::from_secs(3));
    let mut snapshot_deadline = Instant::now() + snapshot_timeout;
    let mut shutdown: Option<(Instant, SyncSender<Result<()>>, bool)> = None;
    let outcome: Result<()> = (|| loop {
        wakeups.fetch_add(1, Ordering::Relaxed);
        let nearest_deadline = pending.values().map(|(deadline, _)| *deadline)
            .chain(shutdown.iter().map(|(deadline, _, _)| *deadline))
            .chain(shutdown.is_none().then_some(snapshot_deadline)).min().unwrap();
        let message = control.recv_timeout(nearest_deadline.saturating_duration_since(Instant::now()));
        match message {
            Ok(ActorInput::Control(command)) => {
                counter += 1;
                let id = format!("r{counter}");
                match command {
                    Control::Request(kind, reply) => {
                        if shutdown.is_some() || pending.len() >= 16 {
                            let _ = reply.send(Err("engine.busy".into()));
                        } else {
                            correlation.begin(&id, kind)?;
                            let deadline = Instant::now() + timeout;
                            let bytes = encode_request(&id, kind)?;
                            pending.insert(id, (deadline, reply));
                            process.write(bytes, deadline.saturating_duration_since(Instant::now()))?;
                        }
                    }
                    Control::Shutdown(reply) => {
                        if shutdown.is_some() {
                            let _ = reply.send(Err("engine.stopping".into()));
                        } else {
                            correlation.begin(&id, SessionCommand::Stop)?;
                            let deadline = Instant::now() + timeout;
                            let bytes = encode_request(&id, SessionCommand::Stop)?;
                            shutdown = Some((deadline, reply, false));
                            process.write(bytes, deadline.saturating_duration_since(Instant::now()))?;
                        }
                    }
                }
            }
            Ok(ActorInput::Frame(Ok(Some(bytes)))) => {
                let event = decode_event(&bytes)?;
                if event.kind != EventType::Snapshot || last_log_snapshot.elapsed() >= Duration::from_secs(5) {
                    crate::logging::record(if event.error.is_some() {"error"} else {"debug"}, "engine", "event", serde_json::json!({"type":format!("{:?}",event.kind),"requestId":event.request_id,"snapshot":event.snapshot,"error":event.error}));
                    if event.kind == EventType::Snapshot {last_log_snapshot=Instant::now();}
                }
                if event.kind == EventType::Ready {
                    return Err("protocol.duplicate_ready".into());
                }
                let complete = correlation.accept(&event)?;
                let mut notifications_added = false;
                if let Some(value) = &event.snapshot {
                    let mut state = snapshot.lock().unwrap();
                    if value.revision < state.revision {
                        return Err("protocol.stale_snapshot".into());
                    }
                    let mut signals = notifications.lock().unwrap();
                    let added = crate::notifications::transitions(&state, value);
                    notifications_added = !added.is_empty();
                    signals.extend(added);
                    if signals.len() > 32 { let excess = signals.len() - 32; signals.drain(..excess); }
                    *state = value.clone();
                    snapshot_deadline = Instant::now() + snapshot_timeout;
                }
                if notifications_added { if let Some(wake) = &notification_wake { wake(); } }
                if event.kind == EventType::FatalError {
                    return Err(command_error(&event.error.unwrap()));
                }
                if complete {
                    if let Some((_, reply)) =
                        event.request_id.as_ref().and_then(|id| pending.remove(id))
                    {
                        let result = event
                            .error
                            .as_ref()
                            .map_or_else(|| Ok(event.clone()), |e| Err(command_error(e)));
                        let _ = reply.send(result);
                    } else if let Some((_, _, acknowledged)) = &mut shutdown {
                        if let Some(e) = event.error {
                            return Err(command_error(&e));
                        }
                        *acknowledged = true;
                    }
                }
            }
            Ok(ActorInput::Frame(Ok(None))) | Err(mpsc::RecvTimeoutError::Disconnected) => {
                if shutdown.as_ref().is_some_and(|(_, _, ack)| *ack) {
                    // Wait for the process too; stdout closing alone does not establish shutdown.
                    loop {
                        if let Some(status) = process.child.try_wait().map_err(|e| e.to_string())? {
                            return if status.success() {
                                Ok(())
                            } else {
                                Err(format!("engine.exit_{status}"))
                            };
                        }
                        if Instant::now() >= shutdown.as_ref().unwrap().0 {
                            return Err("engine.stop_timeout".into());
                        }
                        thread::sleep(Duration::from_millis(10));
                    }
                }
                return Err("engine.unexpected_eof".into());
            }
            Ok(ActorInput::Frame(Err(e))) => return Err(e),
            Err(mpsc::RecvTimeoutError::Timeout) => (),
        }
        if pending
            .values()
            .any(|(deadline, _)| Instant::now() >= *deadline)
        {
            return Err("engine.request_timeout".into());
        }
        if shutdown
            .as_ref()
            .is_some_and(|(deadline, _, _)| Instant::now() >= *deadline)
        {
            return Err("engine.stop_timeout".into());
        }
        if shutdown.is_none() && Instant::now() >= snapshot_deadline {
            return Err("engine.snapshot_timeout".into());
        }
    })();
    crate::logging::record(if outcome.is_err() {"error"} else {"info"}, "engine", "stopped", serde_json::json!({"error":outcome.as_ref().err()}));
    process.terminate();
    let notifications_added = {
        let mut state = snapshot.lock().unwrap();
        let previous = state.clone();
        state.revision += 1;
        state.replay_active = false;
        state.continuous_recording_active = false;
        state.lifecycle = if outcome.is_ok() {
            EngineLifecycle::Stopped
        } else {
            EngineLifecycle::Failed
        };
        if let Err(e) = &outcome {
            state.last_error = Some(NativeError {
                code: e.clone(),
                message: "The native engine stopped unexpectedly. Retry or open diagnostics."
                    .into(),
                hresult: None,
            });
        }
        let added = crate::notifications::transitions(&previous, &state);
        let nonempty = !added.is_empty();
        notifications.lock().unwrap().extend(added);
        nonempty
    };
    running.store(false, Ordering::Release);
    if notifications_added { if let Some(wake) = &notification_wake { wake(); } }
    for (_, (_, reply)) in pending {
        let _ = reply.send(Err(outcome
            .clone()
            .err()
            .unwrap_or("engine.stopped".into())));
    }
    if let Some((_, reply, _)) = shutdown {
        let _ = reply.send(outcome);
    }
}

/// Single-output modes also run inside a kill-on-close job with bounded stdout/deadline.
pub fn run_once(
    executable: &Path,
    args: &[OsString],
    timeout: Duration,
) -> Result<(Vec<u8>, bool)> {
    let mut process = OwnedProcess::spawn(executable, args)?;
    process.child.stdin.take();
    let deadline = Instant::now() + timeout;
    let bytes = process
        .frames
        .recv_timeout(timeout)
        .map_err(|_| "engine.operation_timeout")??
        .ok_or("engine.empty_output")?;
    crate::logging::record("debug", "engine", "operation_result", serde_json::from_slice(&bytes).unwrap_or_else(|_| serde_json::json!({"output":String::from_utf8_lossy(&bytes)})));
    match process
        .frames
        .recv_timeout(deadline.saturating_duration_since(Instant::now()))
        .map_err(|_| "engine.operation_timeout")??
    {
        None => (),
        Some(_) => return Err("protocol.extra_output".into()),
    }
    loop {
        if let Some(status) = process.child.try_wait().map_err(|e| e.to_string())? {
            return Ok((bytes, status.success()));
        }
        if Instant::now() >= deadline {
            return Err("engine.operation_timeout".into());
        }
        thread::sleep(Duration::from_millis(10));
    }
}


#[cfg(test)]
mod memory_admission_error_tests {
    use super::*;
    #[test]
    fn preserves_ram_guidance_when_forwarding_memory_rejection() {
        let mut error = NativeError { code: "config.replay_memory_unavailable".into(), message: "Допустимый лимит: 2048 МиБ. Выберите Авто.".into(), hresult: None };
        assert!(command_error(&error).contains("2048 МиБ"));
        error.code = "engine.not_running".into();
        assert_eq!(command_error(&error), "engine.not_running");
    }
}

#[cfg(test)]
mod command_writer_tests {
    use super::*;

    struct GatedWriter {
        release: Option<Receiver<()>>,
        bytes: Arc<Mutex<Vec<u8>>>,
    }
    impl Write for GatedWriter {
        fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
            if let Some(release) = self.release.take() {
                release.recv_timeout(Duration::from_secs(2)).unwrap();
            }
            let count = bytes.len().min(2);
            self.bytes.lock().unwrap().extend_from_slice(&bytes[..count]);
            Ok(count)
        }
        fn flush(&mut self) -> std::io::Result<()> { Ok(()) }
    }

    #[test]
    fn blocked_stdin_write_returns_at_deadline_without_cancelling_its_thread() {
        let (release, gate) = mpsc::channel();
        let mut writer = BoundedCommandWriter::spawn(GatedWriter {
            release: Some(gate), bytes: Default::default(),
        });
        let unblock = thread::spawn(move || {
            thread::sleep(Duration::from_millis(400));
            release.send(()).unwrap();
        });
        let started = Instant::now();
        let result = writer.write(b"command\n".to_vec(), Duration::from_millis(20));
        let elapsed = started.elapsed();
        unblock.join().unwrap();
        drop(writer);
        assert_eq!(result.unwrap_err(), "engine.stdin_write_timeout");
        assert!(elapsed < Duration::from_millis(200), "write blocked for {elapsed:?}");
    }

    #[test]
    fn stdin_worker_preserves_serial_requests_and_completes_partial_writes() {
        let bytes = Arc::new(Mutex::new(Vec::new()));
        let mut writer = BoundedCommandWriter::spawn(GatedWriter {
            release: None, bytes: bytes.clone(),
        });
        writer.write(b"first\n".to_vec(), Duration::from_secs(1)).unwrap();
        writer.write(b"second\n".to_vec(), Duration::from_secs(1)).unwrap();
        assert_eq!(*bytes.lock().unwrap(), b"first\nsecond\n");
    }

    #[test]
    fn blocked_child_pipe_is_released_by_terminating_the_owned_job() {
        let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
        let executable = root.join("target").join(format!("stdin-fixture-{}.exe", std::process::id()));
        let compiler = std::env::var_os("RUSTC").map(PathBuf::from).unwrap_or_else(|| {
            PathBuf::from(std::env::var_os("CARGO_HOME").unwrap()).join("bin/rustc.exe")
        });
        assert!(Command::new(compiler).arg(root.join("tests/support/fixture.rs"))
            .arg("-o").arg(&executable).status().unwrap().success());
        let (events, _frames) = mpsc::sync_channel(32);
        let mut process = OwnedProcess::spawn_routed(
            &executable.canonicalize().unwrap(), &["silent_ready".into()], Some(events),
        ).unwrap();
        let started = Instant::now();
        let result = process.write(vec![b'x'; 1024 * 1024], Duration::from_millis(40));
        let write_elapsed = started.elapsed();
        process.terminate();
        assert_eq!(result.unwrap_err(), "engine.stdin_write_timeout");
        assert!(write_elapsed < Duration::from_millis(300), "write blocked for {write_elapsed:?}");
        assert!(process.child.try_wait().unwrap().is_some());
    }
}
