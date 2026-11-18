//! The config protocol (v1) from the host side. Commands and replies:
//! docs/protocol.md. One request at a time; lines the client doesn't know
//! (logs start with "# ") are skipped.

use std::time::{Duration, Instant};

use anyhow::{Context, Result, anyhow, bail};

use crate::config::{SIZE, err_name, field_at, from_hex, to_hex};

pub trait Transport: Send {
    fn send(&mut self, line: &str) -> Result<()>;
    /// One line, or None when `timeout` passes with no complete line.
    fn recv(&mut self, timeout: Duration) -> Result<Option<String>>;
    /// Hold DTR low with RTS high, then raise DTR: ask a pad in normal mode to
    /// enter config mode.
    fn knock(&mut self) -> Result<()>;
    /// Drop any input already received.
    fn drain(&mut self);
}

impl<T: Transport + ?Sized> Transport for Box<T> {
    fn send(&mut self, line: &str) -> Result<()> {
        (**self).send(line)
    }
    fn recv(&mut self, timeout: Duration) -> Result<Option<String>> {
        (**self).recv(timeout)
    }
    fn knock(&mut self) -> Result<()> {
        (**self).knock()
    }
    fn drain(&mut self) {
        (**self).drain();
    }
}

/// The firmware's `TOOLS` reply, for editing tool settings without a pad.
/// A test checks it against `src/tools/schema.cpp`.
pub const BUILTIN_TOOLS: &str = "TOOL\t1\tclick\tAutoclicker
PARAM\t1\t0\tmin\tMin\tnumber\t20\t5000\t5\tms\t
PARAM\t1\t1\tmax\tMax\tnumber\t20\t5000\t5\tms\t
PARAM\t1\t2\tbtn\tButton\tchoice\t0\t2\t1\t\tLeft,Right,Middle
TOOL\t2\tjitter\tJitter
PARAM\t2\t0\tdist\tDistance\tnumber\t10\t200\t10\tpx\t
PARAM\t2\t1\tmin\tMin pause\tnumber\t100\t60000\t100\tms\t
PARAM\t2\t2\tmax\tMax pause\tnumber\t100\t60000\t100\tms\t
TOOL\t3\tspam\tKeyspam
PARAM\t3\t0\tkey\tKey\tkey\t4\t127\t1\t\t
PARAM\t3\t1\tmin\tMin\tnumber\t20\t10000\t10\tms\t
PARAM\t3\t2\tmax\tMax\tnumber\t20\t10000\t10\tms\t
PARAM\t3\t3\thold\tHold\tnumber\t10\t500\t10\tms\t
TOOL\t4\thold\tMouse hold
PARAM\t4\t0\tbtn\tButton\tchoice\t0\t2\t1\t\tLeft,Right,Both
OK";

/// Parse a `TOOLS` reply.
pub fn parse_tools(lines: &[String]) -> Result<Vec<ToolInfo>> {
    let mut out: Vec<ToolInfo> = Vec::new();
    for l in lines {
        let f: Vec<&str> = l.split('\t').collect();
        match f[0] {
            "TOOL" if f.len() == 4 => out.push(ToolInfo {
                kind: f[1].parse()?,
                id: f[2].into(),
                name: f[3].into(),
                params: Vec::new(),
            }),
            "PARAM" if f.len() == 11 => {
                let p = ParamInfo {
                    index: f[2].parse()?,
                    id: f[3].into(),
                    label: f[4].into(),
                    kind: f[5].into(),
                    min: f[6].parse()?,
                    max: f[7].parse()?,
                    step: f[8].parse()?,
                    unit: f[9].into(),
                    names: f[10]
                        .split(',')
                        .filter(|s| !s.is_empty())
                        .map(String::from)
                        .collect(),
                };
                let kind: u8 = f[1].parse()?;
                out.iter_mut()
                    .find(|t| t.kind == kind)
                    .ok_or_else(|| anyhow!("PARAM for unknown tool {kind}"))?
                    .params
                    .push(p);
            }
            _ => {}
        }
    }
    Ok(out)
}

pub fn builtin_tools() -> Vec<ToolInfo> {
    let lines: Vec<String> = BUILTIN_TOOLS.lines().map(String::from).collect();
    parse_tools(&lines).unwrap_or_default()
}

/// An error the pad reported.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DeviceError {
    pub code: u8,
    pub offset: usize,
    pub text: String,
}

impl std::fmt::Display for DeviceError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.code < 100 {
            write!(
                f,
                "{} in {} (byte {}): {}",
                err_name(self.code),
                field_at(self.offset),
                self.offset,
                self.text
            )
        } else {
            write!(f, "{}: {}", err_name(self.code), self.text)
        }
    }
}

impl std::error::Error for DeviceError {}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Hello {
    pub fw: String,
    pub mode: String,
    pub cfg: String,
    pub crc: Option<u32>,
    pub boot: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ParamInfo {
    pub index: usize,
    pub id: String,
    pub label: String,
    pub kind: String,
    pub min: i32,
    pub max: i32,
    pub step: i32,
    pub unit: String,
    pub names: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ToolInfo {
    pub kind: u8,
    pub id: String,
    pub name: String,
    pub params: Vec<ParamInfo>,
}

pub const TIMEOUT: Duration = Duration::from_secs(2);

pub struct Client<T: Transport> {
    pub t: T,
    pub timeout: Duration,
}

fn parse_err(line: &str) -> DeviceError {
    let f: Vec<&str> = line.split('\t').collect();
    DeviceError {
        code: f.get(1).and_then(|s| s.parse().ok()).unwrap_or(0),
        offset: f.get(2).and_then(|s| s.parse().ok()).unwrap_or(0),
        // "E_NAME: text": the name comes from the code already.
        text: f
            .get(3)
            .map_or("", |t| t.split_once(": ").map_or(*t, |(_, rest)| rest))
            .to_string(),
    }
}

impl<T: Transport> Client<T> {
    pub fn new(t: T) -> Self {
        Client {
            t,
            timeout: TIMEOUT,
        }
    }

    /// Send a command; return the lines before its final OK (and the OK line
    /// itself last). A reply ERR becomes a DeviceError.
    pub fn request(&mut self, cmd: &str) -> Result<Vec<String>> {
        self.t.send(cmd)?;
        let deadline = Instant::now() + self.timeout;
        let mut lines = Vec::new();
        loop {
            let left = deadline.saturating_duration_since(Instant::now());
            if left.is_zero() {
                bail!(
                    "no reply to {} within {:?}",
                    cmd.split('\t').next().unwrap_or(cmd),
                    self.timeout
                );
            }
            let Some(line) = self.t.recv(left)? else {
                continue;
            };
            // A HELLO reply that arrived late (after its retry's drain) must
            // not end another command.
            let stray_hello = line.starts_with("OK\tmacropad\t") && !cmd.starts_with("HELLO");
            if !stray_hello && (line == "OK" || line.starts_with("OK\t")) {
                lines.push(line);
                return Ok(lines);
            }
            if line.starts_with("ERR\t") {
                return Err(parse_err(&line).into());
            }
            if ["DATA\t", "PIN\t", "TOOL\t", "PARAM\t"]
                .iter()
                .any(|p| line.starts_with(p))
            {
                lines.push(line);
            }
            // anything else (logs, the boot banner) is skipped
        }
    }

    pub fn hello(&mut self) -> Result<Hello> {
        let lines = self.request("HELLO")?;
        let ok = lines.last().context("empty reply")?;
        let mut h = Hello {
            fw: String::new(),
            mode: String::new(),
            cfg: String::new(),
            crc: None,
            boot: 0,
        };
        for (i, f) in ok.split('\t').enumerate() {
            match i {
                2 => h.fw = f.into(),
                _ => {
                    if let Some(v) = f.strip_prefix("mode ") {
                        h.mode = v.into();
                    } else if let Some(v) = f.strip_prefix("cfg ") {
                        h.cfg = v.into();
                    } else if let Some(v) = f.strip_prefix("crc ") {
                        h.crc = u32::from_str_radix(v, 16).ok();
                    } else if let Some(v) = f.strip_prefix("boot ") {
                        h.boot = u32::from_str_radix(v, 16).context("boot nonce")?;
                    } else if let Some(v) = f.strip_prefix("proto ")
                        && v != "1"
                    {
                        bail!("the pad speaks protocol {v}; this app knows 1");
                    }
                }
            }
        }
        Ok(h)
    }

    /// HELLO up to 3 times, draining input before each, so a late reply
    /// can't pair with the next command.
    pub fn hello_retried(&mut self) -> Result<Hello> {
        let mut last = anyhow!("no reply");
        for _ in 0..3 {
            self.t.drain();
            match self.hello() {
                Ok(h) => return Ok(h),
                Err(e) => last = e,
            }
        }
        Err(last)
    }

    /// One quick HELLO (a pad already in config mode answers at once; a knock
    /// would cancel its session), then knock and retry, twice.
    pub fn connect(&mut self) -> Result<Hello> {
        self.t.drain();
        let saved = self.timeout;
        self.timeout = saved.min(Duration::from_millis(300));
        let quick = self.hello();
        self.timeout = saved;
        if let Ok(h) = quick {
            return Ok(h);
        }
        for _ in 0..2 {
            self.t.knock()?;
            if let Ok(h) = self.hello_retried() {
                return Ok(h);
            }
        }
        bail!("the pad isn't answering (is it a macropad with firmware 0.3 or later?)")
    }

    pub fn pins(&mut self) -> Result<Vec<(u8, String)>> {
        let mut out = Vec::new();
        for l in self.request("PINS")? {
            let f: Vec<&str> = l.split('\t').collect();
            if f[0] == "PIN" && f.len() == 3 {
                out.push((f[1].parse()?, f[2].to_string()));
            }
        }
        Ok(out)
    }

    pub fn tools(&mut self) -> Result<Vec<ToolInfo>> {
        parse_tools(&self.request("TOOLS")?)
    }

    /// The staging record, 64 bytes at a time.
    pub fn get_record(&mut self) -> Result<Vec<u8>> {
        let mut rec = Vec::with_capacity(SIZE);
        while rec.len() < SIZE {
            let n = (SIZE - rec.len()).min(64);
            let lines = self.request(&format!("GET\t{}\t{n}", rec.len()))?;
            let data = lines
                .iter()
                .find_map(|l| l.strip_prefix("DATA\t"))
                .context("GET without DATA")?;
            let bytes = from_hex(data)?;
            if bytes.len() != n {
                bail!("GET returned {} bytes, asked for {n}", bytes.len());
            }
            rec.extend_from_slice(&bytes);
        }
        Ok(rec)
    }

    pub fn put_record(&mut self, rec: &[u8]) -> Result<()> {
        for (i, chunk) in rec.chunks(64).enumerate() {
            self.request(&format!("PUT\t{}\t{}", i * 64, to_hex(chunk)))?;
        }
        Ok(())
    }

    pub fn check(&mut self) -> Result<()> {
        self.request("CHECK").map(|_| ())
    }

    /// COMMIT; the pad replies OK with its CRC, then restarts.
    pub fn commit(&mut self) -> Result<u32> {
        let lines = self.request("COMMIT")?;
        let ok = lines.last().context("empty reply")?;
        let crc = ok
            .split('\t')
            .nth(1)
            .context("COMMIT reply without a CRC")?;
        Ok(u32::from_str_radix(crc, 16)?)
    }

    pub fn exit(&mut self) -> Result<()> {
        self.request("EXIT").map(|_| ())
    }
}

#[cfg(test)]
pub mod tests {
    use super::*;
    use std::collections::VecDeque;

    /// Replies scripted per command.
    #[derive(Default)]
    pub struct Fake {
        pub sent: Vec<String>,
        pub replies: VecDeque<Vec<String>>,
        pub inbox: VecDeque<String>,
        pub knocks: usize,
    }

    impl Transport for Fake {
        fn send(&mut self, line: &str) -> Result<()> {
            self.sent.push(line.into());
            if let Some(r) = self.replies.pop_front() {
                self.inbox.extend(r);
            }
            Ok(())
        }
        fn recv(&mut self, _: Duration) -> Result<Option<String>> {
            Ok(self.inbox.pop_front())
        }
        fn knock(&mut self) -> Result<()> {
            self.knocks += 1;
            Ok(())
        }
        fn drain(&mut self) {}
    }

    fn lines(v: &[&str]) -> Vec<String> {
        v.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn hello_parses_and_skips_logs() {
        let mut f = Fake::default();
        f.replies.push_back(lines(&[
            "# a log line",
            "OK\tmacropad\t0.3.0\tproto 1\tmode config\tcfg ok\tcrc 0000abcd\tboot deadbeef",
        ]));
        let h = Client::new(f).hello().expect("hello");
        assert_eq!(h.fw, "0.3.0");
        assert_eq!(h.mode, "config");
        assert_eq!(h.crc, Some(0xabcd));
        assert_eq!(h.boot, 0xdead_beef);
    }

    #[test]
    fn errors_carry_code_and_field() {
        let mut f = Fake::default();
        f.replies
            .push_back(lines(&["ERR\t38\t16\tE_CONTROLS: invalid config"]));
        let e = Client::new(f).check().expect_err("error");
        let d = e.downcast_ref::<DeviceError>().expect("device error");
        assert_eq!((d.code, d.offset), (38, 16));
        assert!(d.to_string().contains("key count"));
    }

    #[test]
    fn records_go_in_64_byte_chunks() {
        let rec = vec![7u8; SIZE];
        let mut f = Fake::default();
        for _ in 0..22 {
            f.replies.push_back(lines(&["OK"]));
        }
        let mut c = Client::new(f);
        c.put_record(&rec).expect("put");
        assert_eq!(c.t.sent.len(), 22);
        assert!(c.t.sent[21].starts_with("PUT\t1344\t"));
    }

    #[test]
    fn a_late_hello_reply_doesnt_end_another_command() {
        let mut f = Fake::default();
        f.replies.push_back(lines(&[
            "OK\tmacropad\t0.3.0\tproto 1\tmode config\tcfg ok\tcrc 0000abcd\tboot deadbeef",
            "DATA\t00",
            "OK",
        ]));
        let lines = Client::new(f).request("GET\t0\t1").expect("reply");
        assert_eq!(lines, ["DATA\t00", "OK"]);
    }

    #[test]
    fn tools_parse() {
        let mut f = Fake::default();
        f.replies.push_back(lines(&[
            "TOOL\t1\tclick\tAutoclicker",
            "PARAM\t1\t0\tmin\tMin\tnumber\t20\t5000\t5\tms\t",
            "PARAM\t1\t2\tbtn\tButton\tchoice\t0\t2\t1\t\tLeft,Right,Middle",
            "OK",
        ]));
        let t = Client::new(f).tools().expect("tools");
        assert_eq!(t[0].params.len(), 2);
        assert_eq!(t[0].params[1].names, ["Left", "Right", "Middle"]);
    }

    #[test]
    fn builtin_tools_match_the_firmware() {
        let path = format!("{}/../src/tools/schema.cpp", env!("CARGO_MANIFEST_DIR"));
        let src = std::fs::read_to_string(path).expect("schema.cpp exists");
        let tools = builtin_tools();
        assert_eq!(tools.len(), 4);
        for t in &tools {
            let row = format!(
                "{{TK_{}, \"{}\", \"{}\", {},",
                t.id.to_uppercase(),
                t.id,
                t.name,
                t.params.len()
            );
            assert!(src.contains(&row), "{row}");
            for p in &t.params {
                let kind = match p.kind.as_str() {
                    "number" => "P_NUMBER",
                    "choice" => "P_CHOICE",
                    _ => "P_KEY",
                };
                let min = if p.kind == "key" {
                    format!("0x{:02X}", p.min)
                } else {
                    p.min.to_string()
                };
                let max = if p.kind == "key" {
                    format!("0x{:02X}", p.max)
                } else {
                    p.max.to_string()
                };
                let row = format!(
                    "{{\"{}\", \"{}\", {kind}, {min}, {max}, {},",
                    p.id, p.label, p.step
                );
                assert!(src.contains(&row), "{row}");
                if !p.names.is_empty() {
                    let names: Vec<String> = p.names.iter().map(|n| format!("\"{n}\"")).collect();
                    assert!(
                        src.contains(&format!("{{{}, nullptr}}", names.join(", "))),
                        "{:?}",
                        p.names
                    );
                }
            }
        }
    }

    #[test]
    fn connect_knocks_when_unanswered() {
        let mut f = Fake::default();
        // The quick HELLO goes unanswered; after the knock, an answer.
        f.replies.push_back(vec![]);
        f.replies.push_back(lines(&[
            "OK\tmacropad\t0.3.0\tproto 1\tmode config\tcfg none\tcrc none\tboot 00000001",
        ]));
        let mut c = Client::new(f);
        c.timeout = Duration::from_millis(10);
        let h = c.connect().expect("connects");
        assert_eq!(c.t.knocks, 1);
        assert_eq!(h.crc, None);
    }
}
