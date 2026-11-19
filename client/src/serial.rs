//! The pad's USB serial port: finding it, opening it, the knock.

use std::io::{Read, Write};
use std::time::{Duration, Instant};

use anyhow::{Context, Result};
use serialport::{SerialPort, SerialPortType};

use crate::protocol::Transport;

pub const VID: u16 = 0x2341;
pub const PID: u16 = 0x0070;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PortInfo {
    pub path: String,
    pub serial: Option<String>,
    pub pad: bool, // a Nano ESP32
}

/// Serial ports, Nano ESP32 ports first.
pub fn ports() -> Vec<PortInfo> {
    let mut v: Vec<PortInfo> = serialport::available_ports()
        .unwrap_or_default()
        .into_iter()
        .map(|p| match p.port_type {
            SerialPortType::UsbPort(u) => PortInfo {
                pad: u.vid == VID && u.pid == PID,
                serial: u.serial_number,
                path: p.port_name,
            },
            _ => PortInfo {
                path: p.port_name,
                serial: None,
                pad: false,
            },
        })
        .collect();
    v.sort_by_key(|p| !p.pad);
    v
}

pub struct Serial {
    port: Box<dyn SerialPort>,
    buf: Vec<u8>,
}

impl Serial {
    /// Open at 115200 (never 1200, which some boards treat as a reset
    /// request) with DTR and RTS up.
    pub fn open(path: &str) -> Result<Serial> {
        let mut port = serialport::new(path, 115_200)
            .timeout(Duration::from_millis(100))
            .dtr_on_open(true)
            .open()
            .with_context(|| format!("open {path}"))?;
        port.write_request_to_send(true)?;
        port.write_data_terminal_ready(true)?;
        Ok(Serial {
            port,
            buf: Vec::new(),
        })
    }
}

impl Drop for Serial {
    /// Discard unsent output: otherwise closing waits (up to 30 s on Linux)
    /// for a pad in normal mode, which never reads, to take it.
    fn drop(&mut self) {
        let _ = self.port.clear(serialport::ClearBuffer::All);
    }
}

impl Transport for Serial {
    fn send(&mut self, line: &str) -> Result<()> {
        // No flush: it waits for the device to read (tcdrain), and a pad in
        // normal mode never reads.
        self.port.write_all(format!("{line}\n").as_bytes())?;
        Ok(())
    }

    fn recv(&mut self, timeout: Duration) -> Result<Option<String>> {
        let deadline = Instant::now() + timeout;
        loop {
            if let Some(i) = self.buf.iter().position(|&c| c == b'\n') {
                let line: Vec<u8> = self.buf.drain(..=i).collect();
                let s = String::from_utf8_lossy(&line[..line.len() - 1]);
                return Ok(Some(s.trim_end_matches('\r').to_string()));
            }
            if Instant::now() >= deadline {
                return Ok(None);
            }
            let mut chunk = [0u8; 256];
            match self.port.read(&mut chunk) {
                Ok(n) => self.buf.extend_from_slice(&chunk[..n]),
                Err(e) if e.kind() == std::io::ErrorKind::TimedOut => {}
                Err(e) => return Err(e.into()),
            }
        }
    }

    fn knock(&mut self) -> Result<()> {
        // Commands a pad in normal mode didn't read would arrive after it
        // enters config mode: drop them.
        self.port.clear(serialport::ClearBuffer::All)?;
        self.port.write_request_to_send(true)?;
        self.port.write_data_terminal_ready(false)?;
        std::thread::sleep(Duration::from_millis(500));
        self.port.write_data_terminal_ready(true)?;
        std::thread::sleep(Duration::from_millis(50));
        self.drain();
        Ok(())
    }

    fn drain(&mut self) {
        self.buf.clear();
        let _ = self.port.clear(serialport::ClearBuffer::Input);
    }
}
