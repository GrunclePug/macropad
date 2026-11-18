//! The serial worker: one thread owns the port and runs one request at a
//! time, so the GUI never blocks on the pad.

use std::sync::mpsc::{Receiver, RecvTimeoutError, Sender, channel};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use anyhow::{Result, bail};

#[cfg(test)]
use crate::config::Config;
use crate::config::stored_crc;
use crate::protocol::{Client, DeviceError, Hello, ToolInfo, Transport};

pub type Port = Box<dyn Transport>;

/// How the worker finds and opens ports: the real serial ports, or a fake.
pub trait Ports: Send + 'static {
    fn open(&mut self, path: &str) -> Result<Port>;
    /// The port path of the pad with this USB serial number, if it's present.
    fn find(&mut self, serial: &str) -> Option<String>;
}

pub struct SerialPorts;

impl Ports for SerialPorts {
    fn open(&mut self, path: &str) -> Result<Port> {
        Ok(Box::new(crate::serial::Serial::open(path)?))
    }
    fn find(&mut self, serial: &str) -> Option<String> {
        crate::serial::ports()
            .into_iter()
            .find(|p| p.serial.as_deref() == Some(serial))
            .map(|p| p.path)
    }
}

pub enum Req {
    Connect {
        path: String,
        serial: Option<String>,
    },
    /// PUT and CHECK, without saving.
    Check(Vec<u8>),
    Save(Vec<u8>),
    Disconnect,
}

#[derive(Debug, Clone)]
pub struct Session {
    pub hello: Hello,
    pub pins: Vec<(u8, String)>,
    pub tools: Vec<ToolInfo>,
    pub record: Vec<u8>,
}

#[derive(Debug)]
pub enum Resp {
    Progress(String),
    Connected(Box<Session>),
    Checked,
    /// The pad rebooted with the new record and is back in normal mode.
    Saved,
    /// The pad refused the record; the editor keeps the edits.
    Refused(DeviceError),
    Failed(String),
    /// A keepalive went unanswered: the pad left config mode.
    Left,
    Disconnected,
}

pub struct Worker {
    pub tx: Sender<Req>,
    pub rx: Receiver<Resp>,
    thread: Option<JoinHandle<()>>,
}

const KEEPALIVE: Duration = Duration::from_secs(15);
const REBOOT_WAIT: Duration = Duration::from_secs(if cfg!(test) { 2 } else { 12 });

impl Worker {
    pub fn spawn(ports: impl Ports, wake: impl Fn() + Send + 'static) -> Worker {
        let (tx, req_rx) = channel();
        let (resp_tx, rx) = channel();
        let thread = std::thread::spawn(move || {
            State {
                ports,
                client: None,
                path: String::new(),
                serial: None,
                in_config: false,
                recovery: false,
                boot: 0,
                expected_crc: None,
                out: resp_tx,
                wake: Box::new(wake),
            }
            .run(&req_rx);
        });
        Worker {
            tx,
            rx,
            thread: Some(thread),
        }
    }

    pub fn send(&self, r: Req) {
        let _ = self.tx.send(r);
    }
}

impl Drop for Worker {
    /// Leave config mode (unless in recovery) and join the thread.
    fn drop(&mut self) {
        let _ = self.tx.send(Req::Disconnect);
        let (tx, _) = channel();
        drop(std::mem::replace(&mut self.tx, tx));
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

struct State<P: Ports> {
    ports: P,
    client: Option<Client<Port>>,
    path: String,
    serial: Option<String>,
    /// The pad is in config or recovery mode, kept there by keepalives.
    in_config: bool,
    recovery: bool,
    boot: u32,
    /// The record the pad should hold: read at Connect, or last saved. A
    /// pad that restarted on its own still counts as this session if it
    /// holds exactly this.
    expected_crc: Option<u32>,
    out: Sender<Resp>,
    wake: Box<dyn Fn() + Send>,
}

impl<P: Ports> State<P> {
    fn emit(&self, r: Resp) {
        let _ = self.out.send(r);
        (self.wake)();
    }

    fn run(&mut self, rx: &Receiver<Req>) {
        let mut last = Instant::now();
        loop {
            let wait = KEEPALIVE.saturating_sub(last.elapsed());
            match rx.recv_timeout(wait) {
                Ok(Req::Connect { path, serial }) => {
                    self.leave();
                    // No target until this Connect succeeds: a failed one
                    // must not leave Save pointed at a half-read pad.
                    self.path.clear();
                    self.serial = None;
                    let r = self.connect(&path, serial);
                    self.report(r, |s| Resp::Connected(Box::new(s)));
                }
                Ok(Req::Check(rec)) => {
                    let r = self.check(&rec);
                    self.report(r, |()| Resp::Checked);
                }
                Ok(Req::Save(rec)) => {
                    let r = self.save(&rec);
                    self.report(r, |()| Resp::Saved);
                }
                Ok(Req::Disconnect) => {
                    self.leave();
                    self.emit(Resp::Disconnected);
                }
                Err(RecvTimeoutError::Timeout) => self.keepalive(),
                Err(RecvTimeoutError::Disconnected) => {
                    self.leave();
                    return;
                }
            }
            last = Instant::now();
        }
    }

    fn report<T>(&mut self, r: Result<T>, ok: impl FnOnce(T) -> Resp) {
        let resp = match r {
            Ok(v) => ok(v),
            Err(e) => match e.downcast::<DeviceError>() {
                Ok(d) => Resp::Refused(d),
                Err(e) => Resp::Failed(format!("{e:#}")),
            },
        };
        self.emit(resp);
    }

    fn keepalive(&mut self) {
        if !self.in_config {
            return;
        }
        let Some(c) = self.client.as_mut() else {
            return;
        };
        if c.hello().is_err() {
            self.in_config = false;
            self.emit(Resp::Left);
        }
    }

    /// EXIT (unless in recovery, where it isn't allowed) and close the port.
    fn leave(&mut self) {
        if let Some(c) = self.client.as_mut()
            && self.in_config
            && !self.recovery
        {
            let _ = c.exit();
        }
        self.client = None;
        self.in_config = false;
    }

    fn connect(&mut self, path: &str, serial: Option<String>) -> Result<Session> {
        self.emit(Resp::Progress(format!("Opening {path}")));
        let mut c = Client::new(self.ports.open(path)?);
        self.emit(Resp::Progress("Asking the pad to enter config mode".into()));
        let hello = c.connect()?;
        if hello.mode != "config" && hello.mode != "recovery" {
            bail!("the pad answered in {} mode", hello.mode);
        }
        self.emit(Resp::Progress("Reading the setup".into()));
        let read = c.pins().and_then(|p| Ok((p, c.tools()?, c.get_record()?)));
        let (pins, tools, record) = match read {
            Ok(v) => v,
            Err(e) => {
                // Let the pad go back to work, and keep no target.
                if hello.mode == "config" {
                    let _ = c.exit();
                }
                return Err(e);
            }
        };
        // Only now is this the pad that Check and Save talk to.
        self.path = path.into();
        self.serial = serial;
        self.recovery = hello.mode == "recovery";
        self.boot = hello.boot;
        self.expected_crc = hello.crc;
        self.in_config = true;
        self.client = Some(c);
        Ok(Session {
            hello,
            pins,
            tools,
            record,
        })
    }

    fn client(&mut self) -> Result<&mut Client<Port>> {
        self.client
            .as_mut()
            .ok_or_else(|| anyhow::anyhow!("not connected"))
    }

    /// Open the session's port again: by USB serial number when there is
    /// one, because a restarted pad can come back under another name.
    fn reopen(&mut self) -> Result<()> {
        if self.path.is_empty() {
            bail!("not connected");
        }
        self.client = None;
        if let Some(sn) = self.serial.clone()
            && let Some(p) = self.ports.find(&sn)
        {
            self.path = p;
        }
        let port = self.ports.open(&self.path)?;
        self.client = Some(Client::new(port));
        Ok(())
    }

    /// Make sure the same pad is in config mode: after a pad-side cancel or
    /// a restart, reopen and knock. Every path checks the boot nonce.
    fn ready(&mut self) -> Result<()> {
        if self.client.is_none() {
            self.reopen()?;
        }
        let quick = if self.in_config {
            self.client()?.hello().ok()
        } else {
            None
        };
        let h = match quick {
            Some(h) => h,
            None => {
                self.emit(Resp::Progress("Asking the pad to enter config mode".into()));
                match self.client()?.connect() {
                    Ok(h) => h,
                    Err(_) => {
                        // The handle may belong to a pad that restarted.
                        self.reopen()?;
                        self.client()?.connect()?
                    }
                }
            }
        };
        // The stored record must still be the one this session read or saved:
        // edits made on the pad since then (saved by the pad itself) would
        // otherwise be overwritten silently. A restart (the pad's leave
        // fallback, a crash) is fine only on top of that.
        let same_record = h.crc.is_some() && h.crc == self.expected_crc;
        let changed = h.cfg == "ok" && !same_record;
        let restarted = h.boot != self.boot;
        if changed || (restarted && !(h.cfg == "ok" && same_record)) {
            // Not this session's pad any more: let it go back to work.
            if h.mode == "config" {
                let _ = self.client()?.exit();
            }
            self.client = None;
            self.in_config = false;
            self.path.clear();
            bail!(
                "the pad's saved setup changed since Connect (edited on the pad?); connect again to load it"
            );
        }
        self.boot = h.boot;
        self.recovery = h.mode == "recovery";
        self.in_config = true;
        Ok(())
    }

    fn check(&mut self, rec: &[u8]) -> Result<()> {
        self.ready()?;
        let c = self.client()?;
        c.put_record(rec)?;
        c.check()
    }

    fn save(&mut self, rec: &[u8]) -> Result<()> {
        if self.path.is_empty() {
            bail!("not connected");
        }
        let Some(serial) = self.serial.clone() else {
            bail!(
                "this port has no USB serial number, so the pad can't be found again after it restarts"
            );
        };
        self.ready()?;
        self.emit(Resp::Progress("Sending".into()));
        self.client()?.put_record(rec)?;
        self.client()?.check()?;
        self.emit(Resp::Progress("Saving".into()));
        match self.client()?.commit() {
            Ok(_) => {}
            // A refusal is definitive; anything else is decided by the
            // reboot check.
            Err(e) if e.is::<DeviceError>() => return Err(e),
            Err(e) => log::warn!("COMMIT: {e:#}"),
        }
        // Close the handle: Windows keeps stale ones across re-enumeration.
        self.client = None;
        self.in_config = false;
        self.emit(Resp::Progress("Waiting for the pad to restart".into()));
        let deadline = Instant::now() + REBOOT_WAIT;
        let want = stored_crc(rec);
        let mut last_err = String::from("the pad didn't come back");
        while Instant::now() < deadline {
            std::thread::sleep(Duration::from_millis(250));
            let Some(path) = self.ports.find(&serial) else {
                continue;
            };
            let Ok(port) = self.ports.open(&path) else {
                continue;
            };
            let mut c = Client::new(port);
            // A short deadline: a knock that landed while the pad was still
            // booting is simply repeated on the next pass.
            c.timeout = Duration::from_millis(500);
            match c.t.knock().and_then(|()| c.hello_retried()) {
                Ok(h) => {
                    if h.boot == self.boot {
                        last_err = "the pad didn't restart, so the setup wasn't saved".into();
                        continue;
                    }
                    if h.mode != "config" || h.cfg != "ok" || h.crc != Some(want) {
                        bail!(
                            "the pad restarted with a different setup (mode {}, cfg {}, crc {:08x?}); \
                             your edits are still here",
                            h.mode,
                            h.cfg,
                            h.crc
                        );
                    }
                    self.path = path;
                    self.boot = h.boot;
                    self.expected_crc = Some(want);
                    let _ = c.exit();
                    return Ok(());
                }
                Err(e) => last_err = format!("{e:#}"),
            }
        }
        bail!("couldn't confirm the save: {last_err}; your edits are still here")
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::fake::{FAKE_PATH, FAKE_SERIAL, FakePad, FakePorts, Mode};
    use crate::preset::builtin;

    fn next(w: &Worker) -> Resp {
        loop {
            match w.rx.recv_timeout(Duration::from_secs(20)).expect("a reply") {
                Resp::Progress(_) => {}
                r => return r,
            }
        }
    }

    fn setup() -> (FakePad, Worker, Session) {
        let rec = builtin("pad").expect("pad").encode().expect("encode");
        let pad = FakePad::new(rec.clone());
        let w = Worker::spawn(FakePorts(pad.clone()), || {});
        w.send(Req::Connect {
            path: FAKE_PATH.into(),
            serial: Some(FAKE_SERIAL.into()),
        });
        let Resp::Connected(s) = next(&w) else {
            panic!("not connected")
        };
        assert_eq!(s.record, rec);
        assert_eq!(pad.lock().mode, Mode::Config);
        (pad, w, *s)
    }

    #[test]
    fn connect_knocks_and_reads_the_record() {
        let (_, _, s) = setup();
        assert_eq!(s.pins.len(), 16);
        assert_eq!(s.tools.len(), 4);
        assert_eq!(s.hello.cfg, "ok");
    }

    #[test]
    fn save_reboots_verifies_and_exits() {
        let (pad, w, s) = setup();
        let mut c = Config::decode(&s.record).expect("decode");
        c.release_ms = 9;
        let rec = c.encode().expect("encode");
        w.send(Req::Save(rec.clone()));
        assert!(matches!(next(&w), Resp::Saved));
        let p = pad.lock();
        assert_eq!(p.stored, rec);
        assert_eq!(p.mode, Mode::Normal, "EXIT after the check");
        assert_eq!(p.commits, 1);
    }

    #[test]
    fn a_second_save_knocks_again() {
        let (pad, w, s) = setup();
        let mut c = Config::decode(&s.record).expect("decode");
        c.release_ms = 9;
        w.send(Req::Save(c.encode().expect("encode")));
        assert!(matches!(next(&w), Resp::Saved));
        c.release_ms = 10;
        let rec = c.encode().expect("encode");
        w.send(Req::Save(rec.clone()));
        assert!(matches!(next(&w), Resp::Saved));
        assert_eq!(pad.lock().stored, rec);
    }

    #[test]
    fn a_refusal_keeps_the_stored_record() {
        let (pad, w, s) = setup();
        let mut rec = s.record.clone();
        rec[20] ^= 1; // the CRC no longer matches
        w.send(Req::Save(rec));
        let Resp::Refused(e) = next(&w) else {
            panic!("not refused")
        };
        assert_eq!(e.code, 4);
        assert_eq!(pad.lock().commits, 0);
        assert_eq!(pad.lock().stored, s.record);
    }

    #[test]
    fn a_pad_that_doesnt_restart_isnt_a_success() {
        let (pad, w, s) = setup();
        pad.lock().no_reboot = true;
        w.send(Req::Save(s.record));
        let Resp::Failed(m) = next(&w) else {
            panic!("not failed")
        };
        assert!(m.contains("didn't restart"), "{m}");
    }

    #[test]
    fn closing_leaves_config_mode() {
        let (pad, w, _) = setup();
        drop(w);
        assert_eq!(pad.lock().mode, Mode::Normal);
    }

    #[test]
    fn check_reports_without_saving() {
        let (pad, w, s) = setup();
        w.send(Req::Check(s.record));
        assert!(matches!(next(&w), Resp::Checked));
        assert_eq!(pad.lock().commits, 0);
    }

    #[test]
    fn a_failed_connect_leaves_no_target() {
        let rec = builtin("pad").expect("pad").encode().expect("encode");
        let pad = FakePad::new(rec.clone());
        pad.lock().fail_pins = true;
        let w = Worker::spawn(FakePorts(pad.clone()), || {});
        w.send(Req::Connect {
            path: FAKE_PATH.into(),
            serial: Some(FAKE_SERIAL.into()),
        });
        assert!(!matches!(next(&w), Resp::Connected(_)));
        assert_eq!(pad.lock().mode, Mode::Normal, "the pad was let go");
        w.send(Req::Save(rec));
        let Resp::Failed(m) = next(&w) else {
            panic!("saved without a session")
        };
        assert!(m.contains("not connected"), "{m}");
        assert_eq!(pad.lock().commits, 0);
    }

    #[test]
    fn a_pad_that_restarted_with_the_same_record_still_saves() {
        let (pad, w, s) = setup();
        pad.lock().restart();
        let mut c = Config::decode(&s.record).expect("decode");
        c.release_ms = 11;
        let rec = c.encode().expect("encode");
        w.send(Req::Save(rec.clone()));
        assert!(matches!(next(&w), Resp::Saved));
        assert_eq!(pad.lock().stored, rec);
    }

    #[test]
    fn a_pad_that_restarted_with_another_record_is_refused() {
        let (pad, w, s) = setup();
        {
            let mut p = pad.lock();
            let mut c = Config::decode(&p.stored).expect("decode");
            c.release_ms = 12;
            p.stored = c.encode().expect("encode");
            p.restart();
        }
        w.send(Req::Save(s.record.clone()));
        let Resp::Failed(m) = next(&w) else {
            panic!("not refused")
        };
        assert!(m.contains("changed since Connect"), "{m}");
        assert_eq!(pad.lock().commits, 0);
        assert_eq!(pad.lock().mode, Mode::Normal, "the pad was let go");
    }

    #[test]
    fn edits_saved_on_the_pad_after_connect_are_not_overwritten() {
        let (pad, w, s) = setup();
        {
            // The pad saved an edit of its own; no restart.
            let mut p = pad.lock();
            let mut c = Config::decode(&p.stored).expect("decode");
            c.selected = 1;
            p.stored = c.encode().expect("encode");
        }
        w.send(Req::Save(s.record.clone()));
        let Resp::Failed(m) = next(&w) else {
            panic!("not refused")
        };
        assert!(m.contains("changed since Connect"), "{m}");
        assert_eq!(pad.lock().commits, 0);
    }
}
