//! A readable TOML form of the config record, for `pull`, `push` and keeping a
//! pad's setup in git. Pins, keys and tools are by name.

use anyhow::{Context, Result, bail};
use serde::{Deserialize, Serialize};

use crate::config::{
    BK_CONSUMER, BK_KEY, BK_TOGGLE, Binding, Button, Config, ENC_ENABLED, KEYS, KeySet,
    PARAM_WORDS, PIN_NONE, ROLE_BIND, ROLE_KILL, ROLE_NAV, ROLE_NONE, SET_LEGEND, SET_LIVE,
    TOOL_SLOTS,
};
use crate::keys::{key_name, key_usage, pin_code, pin_name, tool_id, tool_kind};

#[derive(Debug, Serialize, Deserialize, Default, Clone)]
pub struct PBinding {
    pub kind: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub key: Option<String>,
    #[serde(default, skip_serializing_if = "is_zero")]
    pub mods: u8,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub code: Option<u8>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tool: Option<String>,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub label: String,
}

fn is_zero(v: &u8) -> bool {
    *v == 0
}

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct PButton {
    pub pin: String,
    pub role: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub bind: Option<PBinding>,
}

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct PEncoder {
    pub enabled: bool,
    pub a: String,
    pub b: String,
    pub push: String,
    pub detent: u8,
}

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct PSet {
    pub id: String,
    pub name: String,
    pub legend: bool,
    pub live: bool,
    pub keys: Vec<PBinding>,
    pub encoder: Vec<PBinding>,
}

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct Preset {
    pub key_pins: Vec<String>,
    pub release_ms: u8,
    pub selected: u8,
    pub tools: Vec<String>,
    pub buttons: Vec<PButton>,
    pub encoder: PEncoder,
    pub sets: Vec<PSet>,
    pub params: std::collections::BTreeMap<String, Vec<i32>>,
}

fn binding_to(b: &Binding) -> PBinding {
    let mut p = PBinding {
        label: b.label.clone(),
        ..Default::default()
    };
    match b.kind {
        BK_KEY => {
            p.kind = "key".into();
            p.key = Some(key_name(b.usage));
            p.mods = b.mods;
        }
        BK_CONSUMER => {
            p.kind = "consumer".into();
            p.code = Some(b.usage);
        }
        BK_TOGGLE => {
            p.kind = "toggle".into();
            p.tool = Some(tool_id(b.usage).unwrap_or("?").into());
        }
        _ => p.kind = "none".into(),
    }
    p
}

fn binding_from(p: &PBinding) -> Result<Binding> {
    let label = p.label.clone();
    Ok(match p.kind.as_str() {
        "none" | "" => Binding::default(),
        "key" => {
            let name = p.key.as_deref().context("key binding needs `key`")?;
            Binding {
                kind: BK_KEY,
                mods: p.mods,
                usage: key_usage(name).with_context(|| format!("unknown key {name}"))?,
                label,
            }
        }
        "consumer" => Binding {
            kind: BK_CONSUMER,
            mods: 0,
            usage: p.code.context("consumer binding needs `code`")?,
            label,
        },
        "toggle" => {
            let id = p.tool.as_deref().context("toggle binding needs `tool`")?;
            Binding {
                kind: BK_TOGGLE,
                mods: 0,
                usage: tool_kind(id).with_context(|| format!("unknown tool {id}"))?,
                label,
            }
        }
        k => bail!("unknown binding kind {k}"),
    })
}

fn role_name(r: u8) -> &'static str {
    match r {
        ROLE_KILL => "kill",
        ROLE_NAV => "nav",
        ROLE_BIND => "bind",
        _ => "none",
    }
}

fn role_from(s: &str) -> Result<u8> {
    Ok(match s {
        "none" => ROLE_NONE,
        "kill" => ROLE_KILL,
        "nav" => ROLE_NAV,
        "bind" => ROLE_BIND,
        r => bail!("unknown role {r}"),
    })
}

fn pin(s: &str) -> Result<u8> {
    pin_code(s).with_context(|| format!("unknown pin {s}"))
}

impl Preset {
    pub fn from_config(c: &Config) -> Preset {
        let mut params = std::collections::BTreeMap::new();
        for kind in 1..=4u8 {
            if let Some(id) = tool_id(kind) {
                let slot = &c.params[usize::from(kind - 1)];
                let n = slot.iter().rposition(|&v| v != 0).map_or(0, |i| i + 1);
                params.insert(id.to_string(), slot[..n].to_vec());
            }
        }
        Preset {
            key_pins: c.key_pins[..usize::from(c.key_count)]
                .iter()
                .map(|&p| pin_name(p))
                .collect(),
            release_ms: c.release_ms,
            selected: c.selected,
            tools: c
                .tool_kinds
                .iter()
                .map(|&k| tool_id(k).unwrap_or("?").to_string())
                .collect(),
            buttons: c
                .buttons
                .iter()
                .map(|b| PButton {
                    pin: pin_name(b.pin),
                    role: role_name(b.role).into(),
                    bind: (b.role == ROLE_BIND).then(|| binding_to(&b.bind)),
                })
                .collect(),
            encoder: PEncoder {
                enabled: c.enc_flags & ENC_ENABLED != 0,
                a: pin_name(c.enc_pins[0]),
                b: pin_name(c.enc_pins[1]),
                push: pin_name(c.enc_pins[2]),
                detent: c.detent,
            },
            sets: c
                .sets
                .iter()
                .map(|s| PSet {
                    id: s.id.clone(),
                    name: s.name.clone(),
                    legend: s.flags & SET_LEGEND != 0,
                    live: s.flags & SET_LIVE != 0,
                    keys: s.keys.iter().map(binding_to).collect(),
                    encoder: s.enc.iter().map(binding_to).collect(),
                })
                .collect(),
            params,
        }
    }

    pub fn to_config(&self) -> Result<Config> {
        if self.key_pins.is_empty() || self.key_pins.len() > KEYS {
            bail!("1 to {KEYS} key pins");
        }
        let mut key_pins = [PIN_NONE; KEYS];
        for (i, p) in self.key_pins.iter().enumerate() {
            key_pins[i] = pin(p)?;
        }
        if self.buttons.len() != 2 {
            bail!("exactly 2 buttons (use role = \"none\" for an unused one)");
        }
        let mut buttons: [Button; 2] = Default::default();
        for (b, p) in buttons.iter_mut().zip(&self.buttons) {
            b.role = role_from(&p.role)?;
            b.pin = if b.role == ROLE_NONE {
                PIN_NONE
            } else {
                pin(&p.pin)?
            };
            if b.role == ROLE_BIND {
                b.bind = binding_from(p.bind.as_ref().context("a bind button needs `bind`")?)?;
            }
        }
        let e = &self.encoder;
        let (enc_flags, enc_pins, detent) = if e.enabled {
            (
                ENC_ENABLED,
                [pin(&e.a)?, pin(&e.b)?, pin(&e.push)?],
                e.detent,
            )
        } else {
            (0, [PIN_NONE; 3], 0)
        };
        let mut sets = Vec::new();
        for s in &self.sets {
            let mut keys: [Binding; KEYS] = Default::default();
            for (k, b) in keys.iter_mut().zip(&s.keys) {
                *k = binding_from(b)?;
            }
            let mut enc: [Binding; 3] = Default::default();
            for (k, b) in enc.iter_mut().zip(&s.encoder) {
                *k = binding_from(b)?;
            }
            let flags = if s.legend { SET_LEGEND } else { 0 } | if s.live { SET_LIVE } else { 0 };
            sets.push(KeySet {
                id: s.id.clone(),
                name: s.name.clone(),
                flags,
                keys,
                enc,
            });
        }
        let tool_kinds = self
            .tools
            .iter()
            .map(|t| tool_kind(t).with_context(|| format!("unknown tool {t}")))
            .collect::<Result<Vec<u8>>>()?;
        let mut params = [[0; PARAM_WORDS]; TOOL_SLOTS];
        for (id, vals) in &self.params {
            let kind = tool_kind(id).with_context(|| format!("unknown tool {id} in params"))?;
            if vals.len() > PARAM_WORDS {
                bail!("too many params for {id}");
            }
            params[usize::from(kind - 1)][..vals.len()].copy_from_slice(vals);
        }
        Ok(Config {
            key_count: u8::try_from(self.key_pins.len())?,
            key_pins,
            buttons,
            enc_flags,
            enc_pins,
            detent,
            release_ms: self.release_ms,
            sets,
            tool_kinds,
            params,
            selected: self.selected,
        })
    }

    pub fn to_toml(&self) -> Result<String> {
        Ok(toml::to_string_pretty(self)?)
    }

    pub fn from_toml(s: &str) -> Result<Preset> {
        Ok(toml::from_str(s)?)
    }
}

/// The embedded presets: the firmware's default pad and the deck.
pub fn builtin(name: &str) -> Option<Config> {
    let hex = match name {
        "pad" => include_str!("../../test/vectors/default.hex"),
        "deck" => include_str!("../../test/vectors/deck.hex"),
        _ => return None,
    };
    let rec = crate::config::from_hex(hex).ok()?;
    Config::decode(&rec).ok()
}

/// Write a file atomically: a new temporary file beside it (created
/// exclusively, so it never overwrites another file), then a rename.
pub fn write_file(path: &std::path::Path, text: &str) -> Result<()> {
    use std::io::Write;
    let dir = path.parent().filter(|d| !d.as_os_str().is_empty());
    if path.file_name().is_none() {
        anyhow::bail!("{} names no file", path.display());
    }
    for n in 0..100 {
        // Short, so any legal file name still fits; create_new skips a name
        // that's taken (the destination's own, too).
        let tmp_name = format!(".macropad-{}-{n}.tmp", std::process::id());
        if path.file_name() == Some(std::ffi::OsStr::new(&tmp_name)) {
            continue; // never the destination itself, even when it doesn't exist yet
        }
        let tmp = dir.map_or_else(|| tmp_name.clone().into(), |d| d.join(&tmp_name));
        let mut f = match std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&tmp)
        {
            Ok(f) => f,
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
            Err(e) => return Err(e).with_context(|| format!("create {}", tmp.display())),
        };
        let written = f.write_all(text.as_bytes()).and_then(|()| f.sync_all());
        drop(f); // closed before the rename (Windows)
        let done = written
            .with_context(|| format!("write {}", tmp.display()))
            .and_then(|()| {
                std::fs::rename(&tmp, path).with_context(|| format!("rename to {}", path.display()))
            });
        if done.is_err() {
            let _ = std::fs::remove_file(&tmp);
        }
        return done;
    }
    anyhow::bail!("no free temporary name beside {}", path.display())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn writing_leaves_other_files_alone() {
        let dir = std::env::temp_dir().join(format!("macropad-write-{}", std::process::id()));
        std::fs::create_dir_all(&dir).expect("dir");
        let other = dir.join("pad.tmp");
        std::fs::write(&other, "someone else's").expect("write");
        write_file(&dir.join("pad.toml"), "new").expect("write pad.toml");
        assert_eq!(
            std::fs::read_to_string(dir.join("pad.toml")).expect("read"),
            "new"
        );
        assert_eq!(
            std::fs::read_to_string(&other).expect("read"),
            "someone else's"
        );
        write_file(&other, "into pad.tmp").expect("write pad.tmp");
        assert_eq!(
            std::fs::read_to_string(&other).expect("read"),
            "into pad.tmp"
        );
        let long = dir.join(format!("{}.toml", "p".repeat(250))); // 255 bytes
        write_file(&long, "long").expect("a legal long name");
        std::fs::remove_file(&long).expect("rm");
        let left: Vec<_> = std::fs::read_dir(&dir).expect("ls").collect();
        assert_eq!(left.len(), 2, "no temporary file left behind");
        std::fs::remove_dir_all(&dir).expect("clean up");
    }

    #[test]
    fn presets_round_trip_through_toml() {
        for name in ["pad", "deck"] {
            let c = builtin(name).expect("builtin");
            let text = Preset::from_config(&c).to_toml().expect("toml");
            let back = Preset::from_toml(&text)
                .expect("parse")
                .to_config()
                .expect("config");
            assert_eq!(
                back.encode().expect("encode"),
                c.encode().expect("encode"),
                "{name}"
            );
        }
    }

    #[test]
    fn full_vector_round_trips_through_toml() {
        let path = format!("{}/../test/vectors/full.hex", env!("CARGO_MANIFEST_DIR"));
        let rec =
            crate::config::from_hex(&std::fs::read_to_string(path).expect("file")).expect("hex");
        let c = Config::decode(&rec).expect("decode");
        let text = Preset::from_config(&c).to_toml().expect("toml");
        let back = Preset::from_toml(&text)
            .expect("parse")
            .to_config()
            .expect("config");
        assert_eq!(back.encode().expect("encode"), rec);
    }
}
