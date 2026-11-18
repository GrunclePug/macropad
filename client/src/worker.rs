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
    /// A Save whose outcome was unclear did land: the pad holds this record.
    Landed(Vec<u8>),
    /// A Save reached COMMIT but its outcome stayed open: the pad may hold
    /// this record.
    Unresolved(Vec<u8>),
    /// A restart showed that every open Save didn't land.
    Settled,
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
                attempted: Vec::new(),
                connect_cfg: String::new(),
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
    /// Saves whose outcome is still open (crc, record): a reboot check that
    /// timed out or an E_VERIFY may still have landed. Every one is kept, so
    /// a later retry can't hide an earlier one that did land.
    attempted: Vec<(u32, Vec<u8>)>,
    /// `cfg` at Connect: a pad with no stored record that restarted is the
    /// same session only if this is unchanged.
    connect_cfg: String,
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
        // The pad picked is the one with this USB serial number, wherever it
        // is now: another pad may have taken the listed port name.
        let path = match &serial {
            Some(sn) => self
                .ports
                .find(sn)
                .ok_or_else(|| anyhow::anyhow!("the pad (USB serial {sn}) isn't plugged in"))?,
            None => path.to_string(),
        };
        let path = path.as_str();
        self.emit(Resp::Progress(format!("Opening {path}")));
        let mut c = Client::new(self.ports.open(path)?);
        self.emit(Resp::Progress("Asking the pad to enter config mode".into()));
        let hello = c.connect()?;
        if let Err(e) = Self::same_pad(&hello, serial.as_deref()) {
            if hello.mode == "config" {
                let _ = c.exit();
            }
            return Err(e);
        }
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
        // The record read must be the one HELLO described.
        if let Some(crc) = hello.crc
            && stored_crc(&record) != crc
        {
            if hello.mode == "config" {
                let _ = c.exit();
            }
            bail!("the pad's setup changed while it was being read; connect again");
        }
        // Only now is this the pad that Check and Save talk to.
        self.path = path.into();
        self.serial = serial;
        self.recovery = hello.mode == "recovery";
        self.boot = hello.boot;
        self.expected_crc = hello.crc;
        self.attempted.clear();
        self.connect_cfg = hello.cfg.clone();
        self.in_config = true;
        self.client = Some(c);
        Ok(Session {
            hello,
            pins,
            tools,
            record,
        })
    }

    /// The pad that answered must be the one asked for: a port name can
    /// change hands between finding it and opening it.
    fn same_pad(h: &Hello, serial: Option<&str>) -> Result<()> {
        if let Some(sn) = serial
            && h.sn.as_deref() != Some(sn)
        {
            bail!(
                "a different pad ({}) answered instead of {sn}",
                h.sn.as_deref().unwrap_or("unknown")
            );
        }
        Ok(())
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
        // A pad is identified by its USB serial number only: another pad may
        // have taken this port name.
        if let Some(sn) = self.serial.clone() {
            self.path = self
                .ports
                .find(&sn)
                .ok_or_else(|| anyhow::anyhow!("the pad (USB serial {sn}) isn't plugged in"))?;
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
        if let Err(e) = Self::same_pad(&h, self.serial.as_deref()) {
            if h.mode == "config" {
                let _ = self.client()?.exit();
            }
            self.client = None;
            self.in_config = false;
            self.path.clear();
            self.emit(Resp::Disconnected);
            return Err(e);
        }
        let known = h.crc.is_some_and(|c| {
            Some(c) == self.expected_crc || self.attempted.iter().any(|(a, _)| *a == c)
        });
        // A stored record this session didn't read or send: edited on the pad.
        // Any crc this session doesn't know: a fresh pad whose entry save
        // failed reports its staged edits' crc while still `cfg none`.
        let changed = h.crc.is_some() && !known;
        let restarted = h.boot != self.boot;
        // Nothing stored then or now (a fresh pad, or one in recovery), and
        // no change: a restart loses nothing.
        let nothing_stored =
            h.crc.is_none() && self.expected_crc.is_none() && h.cfg == self.connect_cfg;
        // A stored record this session knew, now reported without a crc
        // (its flash read failed or came back invalid): unverified, so no
        // write on top of it.
        let unverified = h.cfg == "ok" && h.crc.is_none() && self.expected_crc.is_some();
        let refuse = if unverified {
            Some("the pad can't confirm its stored setup right now")
        } else if changed {
            Some("the pad's saved setup changed since Connect (edited on the pad?)")
        } else if restarted && !(h.cfg == "ok" && known) && !nothing_stored {
            Some("the pad restarted and is no longer running the setup read at Connect")
        } else {
            None
        };
        if let Some(why) = refuse {
            // Not this session's pad any more: let it go back to work.
            if h.mode == "config" {
                let _ = self.client()?.exit();
            }
            self.client = None;
            self.in_config = false;
            self.path.clear();
            self.emit(Resp::Disconnected);
            bail!("{why}; connect again to load it");
        }
        if let Some(c) = h.crc
            && let Some(i) = self.attempted.iter().position(|(a, _)| *a == c)
        {
            // An earlier Save did land: tell the GUI which record, so its
            // snapshot follows (not whatever the current request sends).
            let (crc, rec) = self.attempted.swap_remove(i);
            self.attempted.clear();
            self.expected_crc = Some(crc);
            self.emit(Resp::Landed(rec));
        } else if restarted && (h.crc == self.expected_crc || (h.crc.is_none() && h.cfg != "ok")) {
            // Conclusively not landed: after a restart the pad reports its
            // flash, and that's the record read at Connect, or still none.
            // (Settled only after a restart, conservatively: a lost COMMIT
            // reply leaves the pad up and the outcome unknown until then.)
            if !self.attempted.is_empty() {
                self.attempted.clear();
                self.emit(Resp::Settled);
            }
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
        // Until the reboot check decides, the pad may hold this record.
        let crc = stored_crc(rec);
        self.attempted.push((crc, rec.to_vec()));
        let r = self.commit_and_verify(rec, &serial);
        // Still open (neither refused nor confirmed): tell the GUI the pad
        // may hold it.
        if r.is_err() && self.attempted.iter().any(|(c, _)| *c == crc) {
            self.emit(Resp::Unresolved(rec.to_vec()));
        }
        r
    }

    /// COMMIT, then the reboot check. The attempt stays in `attempted`
    /// unless it's refused outright or confirmed.
    fn commit_and_verify(&mut self, rec: &[u8], serial: &str) -> Result<()> {
        let serial = serial.to_string();
        self.emit(Resp::Progress("Saving".into()));
        match self.client()?.commit() {
            Ok(_) => {}
            // A refusal is definitive, except E_WRITE and E_VERIFY: an NVS
            // error can follow a write that happened, so the record may be
            // there.
            Err(e)
                if e.downcast_ref::<DeviceError>()
                    .is_some_and(|d| d.code != 105 && d.code != 106) =>
            {
                self.attempted.pop(); // this one didn't land; earlier ones may have
                return Err(e);
            }
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
                    if Self::same_pad(&h, Some(&serial)).is_err() {
                        if h.mode == "config" {
                            let _ = c.exit();
                        }
                        last_err = "a different pad answered on that port".into();
                        continue;
                    }
                    if h.boot == self.boot {
                        // Answered in config mode: let it go back to work;
                        // the next pass knocks again.
                        let _ = c.exit();
                        last_err = "the pad didn't restart, so the setup wasn't saved".into();
                        continue;
                    }
                    if h.mode != "config" || h.cfg != "ok" || h.crc != Some(want) {
                        if h.mode == "config" {
                            let _ = c.exit();
                        }
                        if h.cfg == "ok" && h.crc.is_some() && h.crc == self.expected_crc {
                            // Restarted on the record read at Connect: this
                            // Save conclusively didn't land.
                            let c = stored_crc(rec);
                            self.attempted.retain(|(a, _)| *a != c);
                            self.boot = h.boot;
                            self.path = path;
                            bail!(
                                "the pad restarted without the new setup; your edits are still here"
                            );
                        }
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
                    self.attempted.clear();
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

    /// The next reply, skipping progress and the Unresolved notes (tests
    /// that check those use `next_raw`).
    fn next(w: &Worker) -> Resp {
        loop {
            match next_raw(w) {
                Resp::Unresolved(_) => {}
                r => return r,
            }
        }
    }

    fn next_raw(w: &Worker) -> Resp {
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
        assert!(
            matches!(next(&w), Resp::Disconnected),
            "the app is told first"
        );
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
        assert!(
            matches!(next(&w), Resp::Disconnected),
            "the app is told first"
        );
        let Resp::Failed(m) = next(&w) else {
            panic!("not refused")
        };
        assert!(m.contains("changed since Connect"), "{m}");
        assert_eq!(pad.lock().commits, 0);
    }

    fn connect_to(pad: &FakePad) -> (Worker, Session) {
        let w = Worker::spawn(FakePorts(pad.clone()), || {});
        w.send(Req::Connect {
            path: FAKE_PATH.into(),
            serial: Some(FAKE_SERIAL.into()),
        });
        let Resp::Connected(s) = next(&w) else {
            panic!("not connected")
        };
        (w, *s)
    }

    fn edited(rec: &[u8]) -> Vec<u8> {
        let mut c = Config::decode(rec).expect("decode");
        c.release_ms = 13;
        c.encode().expect("encode")
    }

    #[test]
    fn a_fresh_pad_saves_its_first_record() {
        let rec = builtin("pad").expect("pad").encode().expect("encode");
        let pad = FakePad::new(rec);
        pad.lock().stored.clear();
        let (w, s) = connect_to(&pad);
        assert_eq!(s.hello.cfg, "none");
        let new = edited(&s.record);
        w.send(Req::Save(new.clone()));
        assert!(matches!(next(&w), Resp::Saved));
        assert_eq!(pad.lock().stored, new);
    }

    #[test]
    fn a_fresh_pad_that_restarted_still_saves() {
        let rec = builtin("pad").expect("pad").encode().expect("encode");
        let pad = FakePad::new(rec);
        pad.lock().stored.clear();
        let (w, s) = connect_to(&pad);
        pad.lock().restart();
        let new = edited(&s.record);
        w.send(Req::Save(new.clone()));
        assert!(matches!(next(&w), Resp::Saved));
        assert_eq!(pad.lock().stored, new);
    }

    #[test]
    fn a_recovery_pad_is_repaired() {
        let rec = builtin("pad").expect("pad").encode().expect("encode");
        let pad = FakePad::new(rec);
        {
            let mut p = pad.lock();
            p.recovery = true;
            p.stored = vec![0; 1355]; // invalid
        }
        let (w, s) = connect_to(&pad);
        assert_eq!(s.hello.mode, "recovery");
        let new = edited(&s.record);
        w.send(Req::Save(new.clone()));
        assert!(matches!(next(&w), Resp::Saved));
        let p = pad.lock();
        assert_eq!(p.stored, new);
        assert!(!p.recovery);
    }

    #[test]
    fn a_save_whose_check_timed_out_but_landed_is_accepted() {
        let (pad, w, s) = setup();
        pad.lock().no_reboot = true; // the reboot check fails...
        let new = edited(&s.record);
        w.send(Req::Save(new.clone()));
        assert!(matches!(next(&w), Resp::Failed(_)));
        {
            let mut p = pad.lock(); // ...but the record landed, and it restarts later
            assert_eq!(p.stored, new);
            p.no_reboot = false;
            p.restart();
        }
        w.send(Req::Check(new.clone()));
        let Resp::Landed(rec) = next(&w) else {
            panic!("the GUI isn't told the Save landed")
        };
        assert_eq!(rec, new, "with the record that landed");
        assert!(
            matches!(next(&w), Resp::Checked),
            "the attempted record counts as known"
        );
    }

    #[test]
    fn reopening_needs_the_same_serial_number() {
        let (pad, w, s) = setup();
        {
            let mut p = pad.lock();
            p.restart();
            p.away = 1000; // our pad is gone (find() fails); the path still opens
        }
        w.send(Req::Save(s.record));
        let Resp::Failed(m) = next(&w) else {
            panic!("not refused")
        };
        assert!(m.contains("isn't plugged in"), "{m}");
        assert_eq!(pad.lock().commits, 0);
    }

    #[test]
    fn a_fresh_pad_with_unsaved_pad_edits_is_not_overwritten() {
        let rec = builtin("pad").expect("pad").encode().expect("encode");
        let pad = FakePad::new(rec);
        pad.lock().stored.clear();
        let (w, s) = connect_to(&pad);
        pad.lock().report_crc = Some(0x1234_5678); // its entry save failed
        w.send(Req::Save(edited(&s.record)));
        assert!(matches!(next(&w), Resp::Disconnected));
        let Resp::Failed(m) = next(&w) else {
            panic!("not refused")
        };
        assert!(m.contains("changed since Connect"), "{m}");
        assert_eq!(pad.lock().commits, 0);
    }

    #[test]
    fn connect_finds_the_picked_pad_by_serial_number() {
        let rec = builtin("pad").expect("pad").encode().expect("encode");
        let pad = FakePad::new(rec);
        let w = Worker::spawn(FakePorts(pad.clone()), || {});
        // The picker listed pad A on this port name; pad A is gone and the
        // fake pad (another serial number) now answers there.
        w.send(Req::Connect {
            path: FAKE_PATH.into(),
            serial: Some("PAD-A".into()),
        });
        let Resp::Failed(m) = next(&w) else {
            panic!("connected to the wrong pad")
        };
        assert!(m.contains("isn't plugged in"), "{m}");
        assert_eq!(
            pad.lock().mode,
            Mode::Normal,
            "the other pad was never knocked"
        );
    }

    #[test]
    fn a_refused_commit_is_not_trusted_later() {
        let (pad, w, s) = setup();
        let new = edited(&s.record);
        pad.lock().fail_commit = true;
        w.send(Req::Save(new.clone()));
        assert!(matches!(next(&w), Resp::Refused(_)));
        {
            // The pad later holds exactly those bytes by other means.
            let mut p = pad.lock();
            p.fail_commit = false;
            p.stored = new.clone();
        }
        w.send(Req::Save(s.record));
        assert!(matches!(next(&w), Resp::Disconnected));
        let Resp::Failed(m) = next(&w) else {
            panic!("trusted a refused commit")
        };
        assert!(m.contains("changed since Connect"), "{m}");
    }

    #[test]
    fn a_different_pad_answering_is_refused() {
        let rec = builtin("pad").expect("pad").encode().expect("encode");
        let pad = FakePad::new(rec);
        // The port found for this serial number is now answered by another
        // pad (swapped between the lookup and the open).
        pad.lock().sn = "PAD-B".into();
        let w = Worker::spawn(FakePorts(pad.clone()), || {});
        w.send(Req::Connect {
            path: FAKE_PATH.into(),
            serial: Some(FAKE_SERIAL.into()),
        });
        let Resp::Failed(m) = next(&w) else {
            panic!("connected to the wrong pad")
        };
        assert!(m.contains("a different pad"), "{m}");
        assert_eq!(pad.lock().mode, Mode::Normal, "and it was let go");
    }

    #[test]
    fn an_unconfirmed_commit_is_reported_unresolved() {
        let (pad, w, s) = setup();
        let new = edited(&s.record);
        // E_VERIFY and a second read that doesn't confirm it: flash kept the
        // old record, but the client can't know that yet.
        pad.lock().fail_commit_code = Some(106);
        w.send(Req::Save(new.clone()));
        let Resp::Unresolved(rec) = next_raw(&w) else {
            panic!("the GUI isn't told it may have landed")
        };
        assert_eq!(rec, new);
        assert!(matches!(next(&w), Resp::Refused(_)));
        pad.lock().fail_commit_code = None;
        // HELLO reports flash (the old record, the Connect crc): no change.
        w.send(Req::Check(s.record.clone()));
        assert!(matches!(next(&w), Resp::Checked));
    }

    #[test]
    fn a_save_refused_before_commit_is_not_unresolved() {
        let (_pad, w, s) = setup();
        let mut bad = s.record.clone();
        bad[20] ^= 1; // CHECK refuses it
        w.send(Req::Save(bad));
        assert!(
            matches!(next_raw(&w), Resp::Refused(_)),
            "no Unresolved first"
        );
    }

    #[test]
    fn a_reconnect_after_e_verify_sees_one_baseline() {
        let (pad, w, s) = setup();
        let new = edited(&s.record);
        pad.lock().fail_commit_code = Some(106);
        w.send(Req::Save(new));
        assert!(matches!(next(&w), Resp::Refused(_)));
        pad.lock().fail_commit_code = None;
        drop(w);
        // Reconnect on the same boot: the record read and HELLO's crc agree.
        let (w, s2) = connect_to(&pad);
        assert_eq!(s2.hello.crc, Some(stored_crc(&s2.record)));
        pad.lock().restart();
        w.send(Req::Check(s2.record));
        assert!(matches!(next(&w), Resp::Checked));
    }

    #[test]
    fn a_failed_retry_keeps_an_earlier_candidate() {
        let (pad, w, s) = setup();
        pad.lock().no_reboot = true;
        let first = edited(&s.record); // its reboot check times out, but it landed
        w.send(Req::Save(first.clone()));
        assert!(matches!(next(&w), Resp::Failed(_)));
        {
            let mut p = pad.lock();
            p.no_reboot = false;
            p.fail_commit = true; // the retry's COMMIT is refused
        }
        let mut c = Config::decode(&s.record).expect("decode");
        c.release_ms = 7;
        w.send(Req::Save(c.encode().expect("encode")));
        // The first Save is recognised on the retry's ready() and reported.
        let Resp::Landed(rec) = next(&w) else {
            panic!("the earlier Save was forgotten")
        };
        assert_eq!(rec, first);
        assert!(matches!(next(&w), Resp::Refused(_)));
    }

    #[test]
    fn a_different_pad_answering_mid_session_is_refused() {
        let (pad, w, s) = setup();
        pad.lock().sn = "PAD-B".into(); // the port now answers as another pad
        w.send(Req::Save(edited(&s.record)));
        assert!(matches!(next(&w), Resp::Disconnected));
        let Resp::Failed(m) = next(&w) else {
            panic!("not refused")
        };
        assert!(m.contains("a different pad"), "{m}");
        assert_eq!(pad.lock().commits, 0);
    }

    #[test]
    fn the_reboot_check_never_accepts_a_different_pad() {
        let (pad, w, s) = setup();
        pad.lock().swap_sn_on_commit = Some("PAD-B".into()); // after the restart, another pad answers
        w.send(Req::Save(edited(&s.record)));
        let Resp::Failed(m) = next(&w) else {
            panic!("verified against another pad")
        };
        assert!(m.contains("couldn't confirm"), "{m}");
    }

    #[test]
    fn connect_refuses_a_record_that_doesnt_match_hellos_crc() {
        let rec = builtin("pad").expect("pad").encode().expect("encode");
        let pad = FakePad::new(rec.clone());
        pad.lock().report_crc = Some(0xDEAD_BEEF); // HELLO describes another record
        let w = Worker::spawn(FakePorts(pad.clone()), || {});
        w.send(Req::Connect {
            path: FAKE_PATH.into(),
            serial: Some(FAKE_SERIAL.into()),
        });
        let Resp::Failed(m) = next(&w) else {
            panic!("accepted two baselines")
        };
        assert!(m.contains("changed while it was being read"), "{m}");
        assert_eq!(pad.lock().mode, Mode::Normal, "the pad was let go");
        w.send(Req::Save(rec));
        let Resp::Failed(m) = next(&w) else {
            panic!("Save had a target")
        };
        assert!(m.contains("not connected"), "{m}");
    }

    #[test]
    fn an_e_write_is_kept_as_unresolved() {
        let (pad, w, s) = setup();
        let new = edited(&s.record);
        pad.lock().fail_commit_code = Some(105);
        w.send(Req::Save(new.clone()));
        let Resp::Unresolved(rec) = next_raw(&w) else {
            panic!("an E_WRITE was taken as definitive")
        };
        assert_eq!(rec, new);
        assert!(matches!(next(&w), Resp::Refused(_)));
    }

    #[test]
    fn a_restart_that_settles_open_saves_tells_the_gui() {
        let (pad, w, s) = setup();
        pad.lock().fail_commit_code = Some(106); // open, and it didn't land
        w.send(Req::Save(edited(&s.record)));
        assert!(matches!(next(&w), Resp::Refused(_)));
        {
            let mut p = pad.lock();
            p.fail_commit_code = None;
            p.restart(); // boots on the old record
        }
        w.send(Req::Check(s.record.clone()));
        assert!(matches!(next(&w), Resp::Settled));
        assert!(matches!(next(&w), Resp::Checked));
    }

    #[test]
    fn a_stored_setup_without_a_crc_is_not_written_over() {
        let (pad, w, s) = setup();
        pad.lock().hide_crc = true; // cfg ok, crc none: flash unreadable
        w.send(Req::Save(edited(&s.record)));
        assert!(matches!(next(&w), Resp::Disconnected));
        let Resp::Failed(m) = next(&w) else {
            panic!("wrote over an unverified setup")
        };
        assert!(m.contains("can't confirm"), "{m}");
        assert_eq!(pad.lock().commits, 0);
    }

    #[test]
    fn a_restart_on_the_old_record_settles_the_save() {
        let (pad, w, s) = setup();
        pad.lock().lose_commit = true; // OK, restart, but the record didn't stick
        w.send(Req::Save(edited(&s.record)));
        let Resp::Failed(m) = next_raw(&w) else {
            panic!("expected a plain failure, no Unresolved first")
        };
        assert!(m.contains("without the new setup"), "{m}");
    }
}
