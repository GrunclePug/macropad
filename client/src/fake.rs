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
    pub stored: Vec<u8>,
    pub staging: Vec<u8>,
    pub boot: u32,
    /// Finds that fail after a reboot (the pad is re-enumerating).
    pub away: u32,
    /// Don't restart on COMMIT (a pad that lost power, say).
    pub no_reboot: bool,
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
            stored: rec,
            boot: 0x1000,
            away: 0,
            no_reboot: false,
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
    fn reply(&mut self, s: impl Into<String>) {
        self.out.push_back(s.into());
    }

    fn err(&mut self, code: u8, off: usize, text: &str) {
        self.reply(format!("ERR\t{code}\t{off}\t{text}"));
    }

    fn hello(&mut self) {
        let crc = stored_crc(&self.stored);
        self.reply(format!(
            "OK\tmacropad\t0.3.0\tproto 1\tmode config\tcfg ok\tcrc {crc:08x}\tboot {:08x}",
            self.boot
        ));
    }

    fn command(&mut self, line: &str) {
        if self.mode != Mode::Config {
            return; // normal mode reads nothing
        }
        let f: Vec<&str> = line.split('\t').collect();
        match f[0] {
            "HELLO" => self.hello(),
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
                self.stored = self.staging.clone();
                self.commits += 1;
                let crc = stored_crc(&self.stored);
                self.reply(format!("OK\t{crc:08x}"));
                if !self.no_reboot {
                    self.mode = Mode::Normal;
                    self.boot = self.boot.wrapping_mul(2_654_435_761).wrapping_add(1);
                    self.away = 2;
                }
            }
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
