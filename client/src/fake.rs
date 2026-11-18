//! A pretend pad that speaks protocol v1: for the worker's tests and the
//! screenshot mode. It keeps a mode, a staging record, a stored record and a
//! boot nonce, and "reboots" on COMMIT.

use std::collections::VecDeque;
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Duration;

use anyhow::{Result, bail};

use crate::config::{Config, SIZE, from_hex, stored_crc, to_hex};
use crate::protocol::{BUILTIN_TOOLS, Transport};
use crate::worker::{Port, Ports};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    Normal,
    Config,
}

#[derive(Debug)]
pub struct Pad {
    pub mode: Mode,
    /// Empty: a fresh pad (`cfg none`).
    pub stored: Vec<u8>,
    /// The stored record is invalid: recovery mode, staging the default.
    pub recovery: bool,
    /// What a fresh or recovering pad stages: its compiled default.
    pub default: Vec<u8>,
    pub staging: Vec<u8>,
    pub boot: u32,
    /// Finds that fail after a reboot (the pad is re-enumerating).
    pub away: u32,
    /// Don't restart on COMMIT (a pad that lost power, say).
    pub no_reboot: bool,
    /// Report this crc in HELLO whatever is stored (a failed entry save:
    /// the staged edits' crc).
    pub report_crc: Option<u32>,
    /// Refuse COMMIT definitively.
    pub fail_commit: bool,
    /// Reply OK to COMMIT and restart, but keep the old record.
    pub lose_commit: bool,
    /// Report `crc none` while `cfg ok` (a flash read that failed).
    pub hide_crc: bool,
    /// Refuse COMMIT with this code instead (106: E_VERIFY).
    pub fail_commit_code: Option<u8>,
    /// The USB serial number HELLO reports.
    pub sn: String,
    /// After a COMMIT's restart, answer with this serial number instead.
    pub swap_sn_on_commit: Option<String>,
    /// Answer PINS with an error (a Connect that fails after HELLO).
    pub fail_pins: bool,
    pub commits: u32,
    out: VecDeque<String>,
}

#[derive(Clone)]
pub struct FakePad(pub Arc<Mutex<Pad>>);

impl FakePad {
    pub fn new(rec: Vec<u8>) -> FakePad {
        FakePad(Arc::new(Mutex::new(Pad {
            mode: Mode::Normal,
            staging: rec.clone(),
            default: rec.clone(),
            stored: rec,
            recovery: false,
            boot: 0x1000,
            away: 0,
            no_reboot: false,
            fail_pins: false,
            fail_commit: false,
            lose_commit: false,
            hide_crc: false,
            fail_commit_code: None,
            sn: FAKE_SERIAL.into(),
            swap_sn_on_commit: None,
            report_crc: None,
            commits: 0,
            out: VecDeque::new(),
        })))
    }

    pub fn lock(&self) -> MutexGuard<'_, Pad> {
        self.0
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
    }
}

impl Pad {
    /// Restart on its own (a leave fallback or a crash): new nonce, normal
    /// mode, staging back to the stored record. Old handles stop working.
    pub fn restart(&mut self) {
        self.mode = Mode::Normal;
        self.boot = self.boot.wrapping_mul(2_654_435_761).wrapping_add(7);
        self.report_crc = None; // a boot reads the crc from flash
        // ...and decides recovery from what flash holds.
        self.recovery = !self.stored.is_empty() && Config::decode(&self.stored).is_err();
        self.staging = if self.stored.is_empty() || self.recovery {
            self.default.clone()
        } else {
            self.stored.clone()
        };
        self.out.clear();
    }

    /// Config or recovery mode: the protocol is live.
    fn live(&self) -> bool {
        self.mode == Mode::Config || self.recovery
    }

    fn reply(&mut self, s: impl Into<String>) {
        self.out.push_back(s.into());
    }

    fn err(&mut self, code: u8, off: usize, text: &str) {
        self.reply(format!("ERR\t{code}\t{off}\t{text}"));
    }

    fn hello(&mut self) {
        let (mode, cfg, mut crc) = if self.recovery {
            ("recovery", "invalid 4".to_string(), "none".to_string())
        } else if self.stored.is_empty() {
            ("config", "none".to_string(), "none".to_string())
        } else {
            (
                "config",
                "ok".to_string(),
                format!("{:08x}", stored_crc(&self.stored)),
            )
        };
        if let Some(c) = self.report_crc {
            crc = format!("{c:08x}");
        }
        if self.hide_crc {
            crc = "none".into();
        }
        self.reply(format!(
            "OK\tmacropad\t0.3.0\tproto 1\tmode {mode}\tcfg {cfg}\tcrc {crc}\tboot {:08x}\tsn {}",
            self.boot, self.sn
        ));
    }

    fn command(&mut self, line: &str) {
        if !self.live() {
            return; // normal mode reads nothing
        }
        let f: Vec<&str> = line.split('\t').collect();
        match f[0] {
            "HELLO" => self.hello(),
            "PINS" if self.fail_pins => self.err(101, 0, "E_MODE: test"),
            "PINS" => {
                for (i, n) in crate::keys::PIN_NAMES.iter().enumerate() {
                    self.reply(format!("PIN\t{i}\t{n}"));
                }
                self.reply("OK");
            }
            "TOOLS" => {
                for l in BUILTIN_TOOLS.lines() {
                    self.reply(l);
                }
            }
            "GET" => {
                let off: usize = f.get(1).and_then(|s| s.parse().ok()).unwrap_or(usize::MAX);
                let n: usize = f.get(2).and_then(|s| s.parse().ok()).unwrap_or(0);
                if off >= SIZE || n == 0 || n > 64 || off + n > SIZE {
                    return self.err(103, 0, "E_RANGE: out of range");
                }
                let data = to_hex(&self.staging[off..off + n]);
                self.reply(format!("DATA\t{data}"));
                self.reply("OK");
            }
            "PUT" => {
                let off: usize = f.get(1).and_then(|s| s.parse().ok()).unwrap_or(usize::MAX);
                let Some(bytes) = f.get(2).and_then(|h| from_hex(h).ok()) else {
                    return self.err(104, 0, "E_HEX: bad hex");
                };
                if off > SIZE || bytes.len() > 64 || off + bytes.len() > SIZE {
                    return self.err(103, 0, "E_RANGE: out of range");
                }
                self.staging[off..off + bytes.len()].copy_from_slice(&bytes);
                self.reply("OK");
            }
            "CHECK" | "COMMIT" => {
                if let Err(e) = Config::decode(&self.staging) {
                    return self.err(4, 12, &format!("E_CRC: {e}"));
                }
                if f[0] == "CHECK" {
                    return self.reply("OK");
                }
                if self.fail_commit {
                    // A definitive refusal (E_WRITE and E_VERIFY are
                    // ambiguous: see fail_commit_code).
                    return self.err(101, 0, "E_MODE: refused");
                }
                if let Some(code) = self.fail_commit_code {
                    // E_VERIFY whose second read doesn't confirm the record:
                    // flash keeps the old one (the firmware turns a
                    // confirmed second read into an ordinary save).
                    return self.err(code, 0, "E_VERIFY: read-back differs");
                }
                if !self.lose_commit {
                    self.stored = self.staging.clone();
                    self.recovery = false;
                }
                self.commits += 1;
                let crc = stored_crc(&self.stored);
                self.reply(format!("OK\t{crc:08x}"));
                if let Some(sn) = self.swap_sn_on_commit.take() {
                    self.sn = sn;
                }
                if !self.no_reboot {
                    self.mode = Mode::Normal;
                    self.boot = self.boot.wrapping_mul(2_654_435_761).wrapping_add(1);
                    self.away = 2;
                }
            }
            "EXIT" if self.recovery => self.err(101, 0, "E_MODE: not in recovery"),
            "EXIT" => {
                self.reply("OK");
                self.mode = Mode::Normal;
            }
            _ => self.err(100, 0, "E_LINE: unknown command"),
        }
    }
}

/// One open handle on the fake pad.
pub struct FakePort {
    pad: FakePad,
    boot: u32,
}

impl Transport for FakePort {
    fn send(&mut self, line: &str) -> Result<()> {
        let mut p = self.pad.lock();
        if p.boot != self.boot {
            bail!("device disconnected");
        }
        p.command(line);
        Ok(())
    }

    fn recv(&mut self, _: Duration) -> Result<Option<String>> {
        let mut p = self.pad.lock();
        // A reply sent before a reboot still arrives on the old handle.
        Ok(p.out.pop_front())
    }

    fn knock(&mut self) -> Result<()> {
        let mut p = self.pad.lock();
        if p.boot != self.boot {
            bail!("device disconnected");
        }
        if p.mode == Mode::Normal {
            // Entering config mode stages the stored record (or the default),
            // as the firmware does.
            p.staging = if p.stored.is_empty() || p.recovery {
                p.default.clone()
            } else {
                p.stored.clone()
            };
        }
        p.mode = Mode::Config;
        Ok(())
    }

    fn drain(&mut self) {
        self.pad.lock().out.clear();
    }
}

pub struct FakePorts(pub FakePad);

pub const FAKE_PATH: &str = "fake";
pub const FAKE_SERIAL: &str = "F00D";

impl Ports for FakePorts {
    fn open(&mut self, path: &str) -> Result<Port> {
        if path != FAKE_PATH {
            bail!("no such port {path}");
        }
        let boot = self.0.lock().boot;
        Ok(Box::new(FakePort {
            pad: self.0.clone(),
            boot,
        }))
    }

    fn find(&mut self, serial: &str) -> Option<String> {
        let mut p = self.0.lock();
        if p.away > 0 {
            p.away -= 1;
            return None;
        }
        (serial == FAKE_SERIAL).then(|| FAKE_PATH.to_string())
    }
}
