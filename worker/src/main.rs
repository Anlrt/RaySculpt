use serde::Deserialize;
use serde_json::{Value, json};
use std::error::Error;
use std::fs;
use std::io::{self, BufRead, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError};
use std::time::{Duration, Instant};
mod project;

#[derive(Deserialize)]
struct Request {
    version: u32,
    job_id: String,
    revision: u64,
    task: String,
    settings: PathBuf,
    profile: PathBuf,
    output: PathBuf,
    #[serde(default = "default_memory_budget")]
    memory_limit_bytes: usize,
}

fn default_memory_budget() -> usize {
    1024 * 1024 * 1024
}

impl Request {
    fn event(&self, event: &str, detail: Value) -> io::Result<()> {
        let mut stdout = io::stdout().lock();
        writeln!(
            stdout,
            "{}",
            json!({
                "version": 1, "job_id": self.job_id, "revision": self.revision,
                "event": event, "detail": detail
            })
        )?;
        stdout.flush()
    }
}

#[cfg(windows)]
struct Job(windows_sys::Win32::Foundation::HANDLE);

#[cfg(windows)]
impl Job {
    fn new(memory_limit: usize) -> io::Result<Self> {
        use windows_sys::Win32::System::JobObjects::*;
        let handle = unsafe { CreateJobObjectW(std::ptr::null(), std::ptr::null()) };
        if handle.is_null() {
            return Err(io::Error::last_os_error());
        }
        let job = Self(handle);
        let mut limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION::default();
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_JOB_MEMORY;
        limits.JobMemoryLimit = memory_limit;
        let ok = unsafe {
            SetInformationJobObject(
                handle,
                JobObjectExtendedLimitInformation,
                (&limits as *const JOBOBJECT_EXTENDED_LIMIT_INFORMATION).cast(),
                size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
            )
        };
        if ok == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(job)
    }

    fn contain_worker(self) -> io::Result<()> {
        use windows_sys::Win32::System::JobObjects::AssignProcessToJobObject;
        use windows_sys::Win32::System::Threading::GetCurrentProcess;
        let ok = unsafe { AssignProcessToJobObject(self.0, GetCurrentProcess()) };
        if ok == 0 {
            return Err(io::Error::last_os_error());
        }
        // Keep the handle open until this single-job worker exits.
        std::mem::forget(self);
        Ok(())
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

fn run_trace(
    engine: &Path,
    request: &Request,
    signals: Receiver<()>,
) -> Result<(), Box<dyn Error>> {
    if request.version != 1 || request.task != "trace" {
        return Err("unsupported worker protocol or task".into());
    }
    if request.output.exists() {
        return Err("output already exists".into());
    }
    let parent = request
        .output
        .parent()
        .filter(|path| !path.as_os_str().is_empty())
        .unwrap_or_else(|| Path::new("."));
    let stage = tempfile::tempdir_in(parent)?;
    let result = stage.path().join("result");
    let cancelled = stage.path().join("cancel");
    let log = stage.path().join("engine.log");
    #[cfg(windows)]
    Job::new(request.memory_limit_bytes)?.contain_worker()?;
    let mut child = Command::new(engine)
        .arg("trace-scene")
        .arg(&request.settings)
        .arg(&request.profile)
        .arg(&result)
        .args(["0", "1"])
        .arg(&cancelled)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(fs::File::create(&log)?)
        .spawn()?;

    request.event("progress", json!({"stage": "tracing", "pid": child.id()}))?;
    let started = Instant::now();
    loop {
        match signals.recv_timeout(Duration::from_millis(20)) {
            Ok(()) | Err(RecvTimeoutError::Disconnected) => {
                fs::write(&cancelled, b"cancel")?;
                let deadline = Instant::now() + Duration::from_secs(1);
                while child.try_wait()?.is_none() {
                    if Instant::now() >= deadline {
                        let _ = child.kill();
                        child.wait()?;
                        break;
                    }
                    std::thread::sleep(Duration::from_millis(20));
                }
                request.event("cancelled", json!({}))?;
                return Ok(());
            }
            Err(RecvTimeoutError::Timeout) => {}
        }
        if let Some(status) = child.try_wait()? {
            if !status.success() {
                let log = fs::read_to_string(&log)?;
                let kind = if log.contains("memory budget") || log.contains("insufficient memory") {
                    io::ErrorKind::OutOfMemory
                } else if log.contains("insufficient disk space") {
                    io::ErrorKind::StorageFull
                } else if log.contains("exceeded its time budget") {
                    io::ErrorKind::TimedOut
                } else {
                    io::ErrorKind::Other
                };
                return Err(io::Error::new(kind, log.trim().to_owned()).into());
            }
            request.event("progress", json!({"stage": "publishing"}))?;
            fs::rename(&result, &request.output)?;
            request.event(
                "result",
                json!({"report": request.output.join("report.json")}),
            )?;
            return Ok(());
        }
        if started.elapsed() >= Duration::from_secs(240) {
            child.kill()?;
            child.wait()?;
            return Err(
                io::Error::new(io::ErrorKind::TimedOut, "trace exceeded its time budget").into(),
            );
        }
    }
}

fn transport_command() -> Result<(), Box<dyn Error>> {
    let args: Vec<_> = std::env::args_os().skip(2).collect();
    if args.len() != 7 {
        return Err("forward-transport needs directory, map cells, grid, aperture, width, height and threads".into());
    }
    let directory = Path::new(&args[0]);
    let parse = |index: usize| args[index].to_str().ok_or("invalid transport argument");
    let n: usize = parse(1)?.parse()?;
    let grid: usize = parse(2)?.parse()?;
    let threads: usize = parse(6)?.parse()?;
    if !(1..=1024).contains(&n) || !(1..=12).contains(&threads) {
        return Err("invalid transport budget".into());
    }
    let read = |name: &str| -> io::Result<Vec<f64>> {
        let path = directory.join(name);
        if fs::metadata(&path)?.len() != ((n + 1).pow(2) * 8) as u64 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "invalid map file length",
            ));
        }
        Ok(fs::read(path)?
            .chunks_exact(8)
            .map(|chunk| f64::from_le_bytes(chunk.try_into().expect("eight bytes")))
            .collect())
    };
    let (x, y) = caustic_transport::forward_transport(
        &read("mx.f64")?,
        &read("my.f64")?,
        n,
        grid,
        parse(3)?.parse()?,
        [parse(4)?.parse()?, parse(5)?.parse()?],
        threads,
    )?;
    for (name, values) in [("tx.f64", x), ("ty.f64", y)] {
        let bytes: Vec<_> = values.into_iter().flat_map(f64::to_le_bytes).collect();
        fs::write(directory.join(name), bytes)?;
    }
    Ok(())
}

fn main() -> Result<(), Box<dyn Error>> {
    let argument = std::env::args_os().nth(1).ok_or("missing engine path")?;
    if argument == "forward-transport" {
        return transport_command();
    }
    if argument == "project-read" || argument == "project-save-as" {
        let mut args = std::env::args_os().skip(2);
        let source = PathBuf::from(args.next().ok_or("missing CCAD source")?);
        return if argument == "project-read" {
            project::inspect(&source)
        } else {
            let output = PathBuf::from(args.next().ok_or("missing CCAD destination")?);
            let edit = serde_json::from_reader(io::stdin().lock().take(8 * 1024 * 1024))?;
            project::save_as(&source, &output, edit)
        };
    }
    let engine = PathBuf::from(argument);
    let mut stdin = io::stdin().lock();
    let mut header = String::new();
    stdin.read_line(&mut header)?;
    let request: Request = serde_json::from_str(&header)?;
    drop(stdin);
    let (send, receive) = mpsc::channel();
    let job_id = request.job_id.clone();
    let revision = request.revision;
    std::thread::spawn(move || {
        for line in io::stdin().lock().lines().map_while(Result::ok) {
            let Ok(message) = serde_json::from_str::<Value>(&line) else {
                continue;
            };
            if message["version"] == 1
                && message["event"] == "cancel"
                && message["job_id"] == job_id
                && message["revision"] == revision
            {
                let _ = send.send(());
                break;
            }
        }
    });
    if let Err(error) = run_trace(&engine, &request, receive) {
        let io_error = error.downcast_ref::<io::Error>();
        let kind = match io_error.map(io::Error::kind) {
            Some(io::ErrorKind::TimedOut) => "timeout",
            Some(io::ErrorKind::OutOfMemory) => "memory",
            Some(io::ErrorKind::StorageFull) => "disk_space",
            _ if io_error
                .and_then(io::Error::raw_os_error)
                .is_some_and(|code| matches!(code, 8 | 14 | 1455 | 1816)) =>
            {
                "memory"
            }
            _ => "engine",
        };
        request.event("error", json!({"code": kind, "message": error.to_string()}))?;
        std::process::exit(1);
    }
    Ok(())
}
