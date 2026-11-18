//! The config record (v1.0, 1355 bytes): the Rust mirror of the firmware's
//! `src/config.{h,cpp}`. Layout and rules: docs/protocol.md. The firmware's
//! validator is the authority; this side only encodes, decodes and keeps the
//! limits it needs to build a record.

use anyhow::{Result, bail};

pub const SIZE: usize = 1355;
pub const KEYS: usize = 8;
pub const SETS: usize = 6;
pub const TOOL_SLOTS: usize = 8;
pub const BUTTONS: usize = 2;
pub const PARAM_WORDS: usize = 8;
pub const PIN_NONE: u8 = 0xFF;

pub const ROLE_NONE: u8 = 0;
pub const ROLE_KILL: u8 = 1;
pub const ROLE_NAV: u8 = 2;
pub const ROLE_BIND: u8 = 3;

pub const BK_NONE: u8 = 0;
pub const BK_KEY: u8 = 1;
pub const BK_CONSUMER: u8 = 2;
pub const BK_TOGGLE: u8 = 3;

pub const SET_LEGEND: u8 = 1;
pub const SET_LIVE: u8 = 2;
pub const ENC_ENABLED: u8 = 1;

pub const CONSUMER_NAMES: [&str; 7] = [
    "Volume up",
    "Volume down",
    "Mute",
    "Play/pause",
    "Next",
    "Previous",
    "Stop",
];

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Binding {
    pub kind: u8,
    pub mods: u8,
    pub usage: u8,
    pub label: String,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Button {
    pub pin: u8,
    pub role: u8,
    pub bind: Binding,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct KeySet {
    pub id: String,
    pub name: String,
    pub flags: u8,
    pub keys: [Binding; KEYS],
    /// Encoder clockwise, anticlockwise, push.
    pub enc: [Binding; 3],
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Config {
    pub key_count: u8,
    pub key_pins: [u8; KEYS],
    pub buttons: [Button; BUTTONS],
    pub enc_flags: u8,
    /// A, B, push.
    pub enc_pins: [u8; 3],
    pub detent: u8,
    pub release_ms: u8,
    /// Only the first `set_count` matter; unused slots are encoded as zeros.
    pub sets: Vec<KeySet>,
    pub tool_kinds: Vec<u8>,
    pub params: [[i32; PARAM_WORDS]; TOOL_SLOTS],
    pub selected: u8,
}

/// CRC-32/ISO-HDLC (zlib).
pub fn crc32(data: &[u8]) -> u32 {
    let mut c: u32 = 0xFFFF_FFFF;
    for &b in data {
        c ^= u32::from(b);
        for _ in 0..8 {
            c = (c >> 1) ^ (0xEDB8_8320 & 0u32.wrapping_sub(c & 1));
        }
    }
    c ^ 0xFFFF_FFFF
}

/// The CRC stored in an encoded record's header.
pub fn stored_crc(rec: &[u8]) -> u32 {
    u32::from_le_bytes([rec[12], rec[13], rec[14], rec[15]])
}

struct W(Vec<u8>);

impl W {
    fn u8(&mut self, v: u8) {
        self.0.push(v);
    }
    fn zero(&mut self, n: usize) {
        self.0.resize(self.0.len() + n, 0);
    }
    fn str(&mut self, s: &str, field: usize) {
        let b = s.as_bytes();
        let n = b.len().min(field - 1);
        self.0.extend_from_slice(&b[..n]);
        self.zero(field - n);
    }
    fn binding(&mut self, b: &Binding) {
        self.u8(b.kind);
        self.u8(b.mods);
        self.u8(b.usage);
        self.u8(0);
        self.str(&b.label, 9);
    }
}

struct R<'a> {
    p: &'a [u8],
    at: usize,
}

impl R<'_> {
    fn u8(&mut self) -> u8 {
        let v = self.p[self.at];
        self.at += 1;
        v
    }
    fn skip(&mut self, n: usize) {
        self.at += n;
    }
    fn str(&mut self, field: usize) -> String {
        let raw = &self.p[self.at..self.at + field];
        self.at += field;
        let end = raw.iter().position(|&c| c == 0).unwrap_or(field);
        String::from_utf8_lossy(&raw[..end]).into_owned()
    }
    fn i32(&mut self) -> i32 {
        let v = i32::from_le_bytes([
            self.p[self.at],
            self.p[self.at + 1],
            self.p[self.at + 2],
            self.p[self.at + 3],
        ]);
        self.at += 4;
        v
    }
    fn binding(&mut self) -> Binding {
        let kind = self.u8();
        let mods = self.u8();
        let usage = self.u8();
        self.skip(1);
        Binding {
            kind,
            mods,
            usage,
            label: self.str(9),
        }
    }
}

impl Config {
    /// Encode with header and CRC.
    pub fn encode(&self) -> Result<Vec<u8>> {
        if self.sets.is_empty() || self.sets.len() > SETS {
            bail!("1 to {SETS} key sets");
        }
        if self.tool_kinds.len() > TOOL_SLOTS {
            bail!("at most {TOOL_SLOTS} tools");
        }
        let mut w = W(Vec::with_capacity(SIZE));
        w.0.extend_from_slice(b"MPC1");
        w.u8(1);
        w.u8(0);
        w.0.extend_from_slice(&u16::try_from(SIZE)?.to_le_bytes());
        w.zero(8); // reserved, crc
        w.u8(self.key_count);
        for &p in &self.key_pins {
            w.u8(p);
        }
        for b in &self.buttons {
            w.u8(b.pin);
            w.u8(b.role);
            w.u8(0);
            w.binding(&b.bind);
        }
        w.u8(self.enc_flags);
        for &p in &self.enc_pins {
            w.u8(p);
        }
        w.u8(self.detent);
        w.u8(self.release_ms);
        w.zero(4);
        w.u8(u8::try_from(self.sets.len())?);
        for s in 0..SETS {
            let Some(set) = self.sets.get(s) else {
                w.zero(169);
                continue;
            };
            w.str(&set.id, 7);
            w.str(&set.name, 13);
            w.u8(set.flags);
            w.u8(0);
            for b in set.keys.iter().chain(set.enc.iter()) {
                w.binding(b);
            }
            w.zero(4);
        }
        w.u8(u8::try_from(self.tool_kinds.len())?);
        for i in 0..TOOL_SLOTS {
            w.u8(self.tool_kinds.get(i).copied().unwrap_or(0));
        }
        for slot in &self.params {
            for &v in slot {
                w.0.extend_from_slice(&v.to_le_bytes());
            }
        }
        w.zero(4);
        w.u8(self.selected);
        w.zero(3);
        let mut rec = w.0;
        if rec.len() != SIZE {
            bail!("encoded {} bytes, want {SIZE}", rec.len());
        }
        let crc = crc32(&rec);
        rec[12..16].copy_from_slice(&crc.to_le_bytes());
        Ok(rec)
    }

    /// Decode a record. Checks only what decoding needs (magic, version,
    /// length, CRC); the pad validates the rest.
    pub fn decode(rec: &[u8]) -> Result<Config> {
        if rec.len() != SIZE || &rec[..4] != b"MPC1" {
            bail!("not a config record");
        }
        if rec[4] != 1 || rec[5] != 0 {
            bail!("unsupported record version {}.{}", rec[4], rec[5]);
        }
        if usize::from(u16::from_le_bytes([rec[6], rec[7]])) != SIZE {
            bail!("record length field mismatch");
        }
        let mut copy = rec.to_vec();
        copy[12..16].fill(0);
        if crc32(&copy) != stored_crc(rec) {
            bail!("record CRC mismatch");
        }
        let mut r = R { p: rec, at: 16 };
        let key_count = r.u8();
        let mut key_pins = [0; KEYS];
        for p in &mut key_pins {
            *p = r.u8();
        }
        let mut buttons: [Button; BUTTONS] = Default::default();
        for b in &mut buttons {
            b.pin = r.u8();
            b.role = r.u8();
            r.skip(1);
            b.bind = r.binding();
        }
        let enc_flags = r.u8();
        let enc_pins = [r.u8(), r.u8(), r.u8()];
        let detent = r.u8();
        let release_ms = r.u8();
        r.skip(4);
        let set_count = usize::from(r.u8());
        let mut sets = Vec::new();
        for s in 0..SETS {
            let id = r.str(7);
            let name = r.str(13);
            let flags = r.u8();
            r.skip(1);
            let keys: [Binding; KEYS] = std::array::from_fn(|_| r.binding());
            let enc: [Binding; 3] = std::array::from_fn(|_| r.binding());
            r.skip(4);
            if s < set_count {
                sets.push(KeySet {
                    id,
                    name,
                    flags,
                    keys,
                    enc,
                });
            }
        }
        let tool_count = usize::from(r.u8());
        let mut tool_kinds = Vec::new();
        for i in 0..TOOL_SLOTS {
            let k = r.u8();
            if i < tool_count {
                tool_kinds.push(k);
            }
        }
        let mut params = [[0; PARAM_WORDS]; TOOL_SLOTS];
        for slot in &mut params {
            for v in slot.iter_mut() {
                *v = r.i32();
            }
        }
        r.skip(4);
        let selected = r.u8();
        Ok(Config {
            key_count,
            key_pins,
            buttons,
            enc_flags,
            enc_pins,
            detent,
            release_ms,
            sets,
            tool_kinds,
            params,
            selected,
        })
    }
}

pub fn from_hex(s: &str) -> Result<Vec<u8>> {
    let s = s.trim();
    if !s.len().is_multiple_of(2) {
        bail!("odd hex length");
    }
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).map_err(Into::into))
        .collect()
}

pub fn to_hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

/// The firmware's error names, by code (docs/protocol.md).
pub fn err_name(code: u8) -> &'static str {
    match code {
        1 => "E_MAGIC",
        2 => "E_VERSION",
        3 => "E_LENGTH",
        4 => "E_CRC",
        5 => "E_RESERVED",
        6 => "E_FLAGS",
        7 => "E_STRING",
        8 => "E_ID",
        9 => "E_COUNT",
        10 => "E_PIN",
        11 => "E_ROLE",
        12 => "E_BINDING",
        13 => "E_ENCODER",
        14 => "E_DEBOUNCE",
        15 => "E_TOOLKIND",
        16 => "E_PARAM",
        17 => "E_UNUSED",
        20 => "E_CLAMP",
        30 => "E_DUP_PIN",
        31 => "E_DUP_SET",
        32 => "E_DUP_TOOL",
        33 => "E_TOGGLE",
        34 => "E_SELECTED",
        35 => "E_NO_NAV",
        36 => "E_TWO_NAV",
        37 => "E_TWO_KILL",
        38 => "E_CONTROLS",
        100 => "E_LINE",
        101 => "E_MODE",
        102 => "E_ARGS",
        103 => "E_RANGE",
        104 => "E_HEX",
        105 => "E_WRITE",
        106 => "E_VERIFY",
        _ => "E_UNKNOWN",
    }
}

/// Rebase edits onto a record that changed underneath them: for each byte
/// past the header, the editor's value where it differs from `base`,
/// otherwise `theirs`. Returns the merged record and the offsets both sides
/// changed differently (the editor's value wins there).
pub fn merge3(base: &[u8], mine: &[u8], theirs: &[u8]) -> Result<(Vec<u8>, Vec<usize>)> {
    if base.len() != SIZE || mine.len() != SIZE || theirs.len() != SIZE {
        anyhow::bail!("records of the wrong size");
    }
    let mut out = theirs.to_vec();
    let mut conflicts = Vec::new();
    for i in 16..SIZE {
        if mine[i] != base[i] {
            if theirs[i] != base[i] && theirs[i] != mine[i] {
                conflicts.push(i);
            }
            out[i] = mine[i];
        }
    }
    out[12..16].fill(0);
    let crc = crc32(&out);
    out[12..16].copy_from_slice(&crc.to_le_bytes());
    Ok((out, conflicts))
}

/// Which field of the record a byte offset belongs to, for error messages.
pub fn field_at(off: usize) -> String {
    match off {
        0..16 => "header".into(),
        16 => "key count".into(),
        17..25 => format!("key {} pin", off - 16),
        25..41 => "button 1".into(),
        41..57 => "button 2".into(),
        57..62 => "encoder".into(),
        62 => "release debounce".into(),
        63..67 => "hardware".into(),
        67 => "key set count".into(),
        68..1082 => {
            let s = (off - 68) / 169;
            let r = (off - 68) % 169;
            let part = match r {
                0..7 => "id".into(),
                7..20 => "name".into(),
                20..22 => "flags".into(),
                22..126 => format!("key {}", (r - 22) / 13 + 1),
                126..165 => ["encoder cw", "encoder ccw", "encoder push"][(r - 126) / 13].into(),
                _ => "reserved".into(),
            };
            format!("key set {} {part}", s + 1)
        }
        1082..1091 => "tool list".into(),
        1091..1347 => format!("tool slot {} params", (off - 1091) / 32 + 1),
        1351 => "selected key set".into(),
        _ => "reserved".into(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn vector(name: &str) -> Vec<u8> {
        let path = format!("{}/../test/vectors/{name}", env!("CARGO_MANIFEST_DIR"));
        from_hex(&std::fs::read_to_string(&path).expect("vector file exists")).expect("valid hex")
    }

    #[test]
    fn crc_is_zlib() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
    }

    #[test]
    fn vectors_round_trip_byte_for_byte() {
        for name in ["default.hex", "full.hex", "deck.hex"] {
            let rec = vector(name);
            let c = Config::decode(&rec).expect("decodes");
            assert_eq!(c.encode().expect("encodes"), rec, "{name}");
        }
    }

    #[test]
    fn full_vector_decodes_to_known_values() {
        let c = Config::decode(&vector("full.hex")).expect("decodes");
        assert_eq!(c.key_count, 8);
        assert_eq!(c.key_pins, [15, 14, 13, 12, 11, 10, 9, 8]);
        assert_eq!(c.buttons[0].role, ROLE_BIND);
        assert_eq!(c.buttons[0].bind.mods, 0xA5);
        assert_eq!(c.buttons[0].bind.usage, 0x73);
        assert_eq!(c.buttons[0].bind.label, "F24");
        assert_eq!(c.buttons[1].role, ROLE_NAV);
        assert_eq!(c.enc_flags, ENC_ENABLED);
        assert_eq!(c.enc_pins, [2, 3, 4]);
        assert_eq!(c.detent, 2);
        assert_eq!(c.release_ms, 7);
        assert_eq!(c.sets.len(), 6);
        assert_eq!(c.sets[5].id, "f6");
        assert_eq!(c.sets[5].name, "Sixth set 12");
        assert_eq!(c.sets[0].enc[2].label, "Ctl F13");
        assert_eq!(c.tool_kinds, [4, 3, 1, 2]);
        assert_eq!(c.params[2][..4], [0x6E, 30, 9990, 20]);
        assert_eq!(c.selected, 5);
    }

    #[test]
    fn invalid_table_parses_and_corrupt_crcs_are_refused() {
        let path = format!("{}/../test/vectors/invalid.tsv", env!("CARGO_MANIFEST_DIR"));
        let text = std::fs::read_to_string(path).expect("table exists");
        let mut n = 0;
        for line in text
            .lines()
            .filter(|l| !l.starts_with('#') && !l.is_empty())
        {
            let f: Vec<&str> = line.split('\t').collect();
            assert_eq!(f.len(), 4, "{line}");
            let rec = from_hex(f[1]).expect("hex");
            let code: u8 = f[2].parse().expect("code");
            // Decoding refuses what it can see (magic, version, length, CRC);
            // everything else is the pad's to reject.
            if matches!(code, 1..=4) {
                assert!(Config::decode(&rec).is_err(), "{}", f[0]);
            }
            n += 1;
        }
        assert!(n > 40);
    }

    #[test]
    fn merge3_keeps_both_sides_changes() {
        let base = vector("default.hex");
        let mut mine = Config::decode(&base).expect("decode");
        mine.release_ms = 9;
        let mut theirs = Config::decode(&base).expect("decode");
        theirs.selected = 1;
        let (out, conflicts) = merge3(
            &base,
            &mine.encode().expect("e"),
            &theirs.encode().expect("e"),
        )
        .expect("merge");
        let m = Config::decode(&out).expect("the merge decodes");
        assert_eq!((m.release_ms, m.selected), (9, 1));
        assert!(conflicts.is_empty());
        let mut clash = Config::decode(&base).expect("decode");
        clash.release_ms = 11;
        let (out, conflicts) = merge3(
            &base,
            &mine.encode().expect("e"),
            &clash.encode().expect("e"),
        )
        .expect("merge");
        assert_eq!(
            Config::decode(&out).expect("decodes").release_ms,
            9,
            "the editor wins"
        );
        assert_eq!(conflicts, [62]);
    }
}
