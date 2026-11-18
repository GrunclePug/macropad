#![cfg_attr(all(windows, not(debug_assertions)), windows_subsystem = "windows")]

mod app;
mod config;
mod fake;
mod keys;
mod preset;
mod protocol;
mod screenshot;
mod serial;
mod worker;

use std::path::PathBuf;
use std::time::Duration;

use anyhow::{Context, Result, anyhow, bail};
use clap::{Parser, Subcommand};
use eframe::egui;

use crate::app::App;
use crate::config::Config;
use crate::fake::{FAKE_PATH, FAKE_SERIAL, FakePad, FakePorts};
use crate::preset::Preset;
use crate::serial::PortInfo;
use crate::worker::{Req, Resp, SerialPorts, Worker};

/// Configure a macropad over USB serial. With no command, opens the editor.
#[derive(Parser)]
#[command(version, about)]
struct Cli {
    #[command(subcommand)]
    cmd: Option<Cmd>,
    /// Use a pretend pad, to try the app without hardware.
    #[arg(long)]
    fake: bool,
    /// Render each tab against the pretend pad into PNGs in DIR, then exit.
    #[arg(long, value_name = "DIR")]
    screenshot: Option<PathBuf>,
}

#[derive(Subcommand)]
enum Cmd {
    /// List serial ports, Nano ESP32s first.
    Ports,
    /// Read the pad's setup into a TOML file.
    Pull {
        file: PathBuf,
        /// The pad's port (default: the only Nano ESP32 plugged in).
        #[arg(short, long)]
        port: Option<String>,
    },
    /// Check a TOML file on the pad and save it there.
    Push {
        file: PathBuf,
        #[arg(short, long)]
        port: Option<String>,
    },
}

fn main() -> Result<()> {
    env_logger::init();
    let cli = Cli::parse();
    if let Some(dir) = cli.screenshot {
        return screenshot::run(&dir);
    }
    match cli.cmd {
        Some(Cmd::Ports) => {
            for p in serial::ports() {
                let kind = if p.pad { "Nano ESP32" } else { "" };
                println!("{}\t{}\t{kind}", p.path, p.serial.unwrap_or_default());
            }
            Ok(())
        }
        Some(Cmd::Pull { file, port }) => pull(&file, port.as_deref()),
        Some(Cmd::Push { file, port }) => push(&file, port.as_deref()),
        None => gui(cli.fake),
    }
}

pub fn fake_ports() -> Vec<PortInfo> {
    vec![PortInfo {
        path: FAKE_PATH.into(),
        serial: Some(FAKE_SERIAL.into()),
        pad: true,
    }]
}

pub fn fake_pad() -> Result<FakePad> {
    let c = preset::builtin("pad").context("the pad preset")?;
    Ok(FakePad::new(c.encode()?))
}

fn gui(fake: bool) -> Result<()> {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([860.0, 660.0])
            .with_title("Macropad"),
        ..Default::default()
    };
    let pad = if fake { Some(fake_pad()?) } else { None };
    eframe::run_native(
        "macropad-config",
        options,
        Box::new(move |cc| {
            let ctx = cc.egui_ctx.clone();
            let wake = move || ctx.request_repaint();
            let app = match pad {
                Some(p) => App::new(Worker::spawn(FakePorts(p), wake), fake_ports),
                None => App::new(Worker::spawn(SerialPorts, wake), serial::ports),
            };
            Ok(Box::new(app))
        }),
    )
    .map_err(|e| anyhow!("{e}"))
}

/// The port to use from the command line: the one named, or the only pad.
fn pick_port(name: Option<&str>) -> Result<PortInfo> {
    let ports = serial::ports();
    if let Some(n) = name {
        return Ok(ports.into_iter().find(|p| p.path == n).unwrap_or(PortInfo {
            path: n.into(),
            serial: None,
            pad: false,
        }));
    }
    let pads: Vec<PortInfo> = ports.into_iter().filter(|p| p.pad).collect();
    match pads.len() {
        1 => Ok(pads.into_iter().next().context("one pad")?),
        0 => bail!("no Nano ESP32 found; plug the pad in or pass --port"),
        _ => bail!(
            "several Nano ESP32s: pass --port ({})",
            pads.iter()
                .map(|p| p.path.as_str())
                .collect::<Vec<_>>()
                .join(", ")
        ),
    }
}

/// The worker's next answer, with progress on stderr.
fn wait(w: &Worker) -> Result<Resp> {
    loop {
        match w.rx.recv_timeout(Duration::from_secs(60))? {
            Resp::Progress(s) => eprintln!("{s}…"),
            Resp::Disconnected | Resp::Landed(_) | Resp::Unresolved(_) | Resp::Settled => {} // the result follows
            Resp::Refused(e) => bail!("the pad refused it: {e}"),
            Resp::Failed(m) => bail!("{m}"),
            r => return Ok(r),
        }
    }
}

fn connect(port: Option<&str>) -> Result<(Worker, worker::Session)> {
    let p = pick_port(port)?;
    let w = Worker::spawn(SerialPorts, || {});
    w.send(Req::Connect {
        path: p.path,
        serial: p.serial,
    });
    match wait(&w)? {
        Resp::Connected(s) => Ok((w, *s)),
        r => bail!("unexpected answer {r:?}"),
    }
}

fn pull(file: &std::path::Path, port: Option<&str>) -> Result<()> {
    let (_w, s) = connect(port)?;
    let c = Config::decode(&s.record).context("the pad's setup")?;
    preset::write_file(file, &Preset::from_config(&c).to_toml()?)?;
    eprintln!("wrote {}", file.display());
    Ok(())
}

fn push(file: &std::path::Path, port: Option<&str>) -> Result<()> {
    let text = std::fs::read_to_string(file).with_context(|| format!("read {}", file.display()))?;
    let rec = Preset::from_toml(&text)
        .and_then(|p| p.to_config())
        .and_then(|c| c.encode())
        .with_context(|| format!("{}", file.display()))?;
    let (w, _) = connect(port)?;
    w.send(Req::Save(rec));
    match wait(&w)? {
        Resp::Saved => {
            eprintln!("saved; the pad restarted with it");
            Ok(())
        }
        r => bail!("unexpected answer {r:?}"),
    }
}
