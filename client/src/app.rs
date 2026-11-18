//! The editor window: Connect, Hardware, Key sets and Tools, with Save,
//! Check, Revert and the presets in the bottom bar.

use eframe::egui;

use crate::config::{
    BK_CONSUMER, BK_KEY, BK_NONE, BK_TOGGLE, Binding, CONSUMER_NAMES, Config, ENC_ENABLED, KEYS,
    KeySet, PIN_NONE, ROLE_BIND, ROLE_KILL, ROLE_NAV, ROLE_NONE, SET_LEGEND, SET_LIVE, SETS,
    TOOL_SLOTS,
};
use crate::keys::{KEY_NAMES, PIN_NAMES, key_name};
use crate::preset::builtin;
use crate::protocol::{ToolInfo, builtin_tools};
use crate::serial::PortInfo;
use crate::worker::{Req, Resp, Session, Worker};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Tab {
    Connect,
    Hardware,
    Sets,
    Tools,
}

pub const TABS: [(Tab, &str); 4] = [
    (Tab::Connect, "Connect"),
    (Tab::Hardware, "Hardware"),
    (Tab::Sets, "Key sets"),
    (Tab::Tools, "Tools"),
];

pub struct App {
    worker: Worker,
    list_ports: fn() -> Vec<PortInfo>,
    ports: Vec<PortInfo>,
    port: usize,
    pub tab: Tab,
    session: Option<Session>,
    tools: Vec<ToolInfo>,
    pub cfg: Config,
    snapshot: Option<Config>,
    /// The USB serial number of the pad the snapshot came from: unsaved
    /// edits carry over a reconnect only to that same pad.
    snapshot_serial: Option<String>,
    /// The serial number of the pad a Connect in flight is for.
    connecting_serial: Option<String>,
    /// The setup a Save in flight sent.
    saving: Option<Config>,
    busy: Option<String>,
    status: String,
    error: Option<String>,
    notice: Option<String>,
}

const MODS: [(u8, &str); 4] = [
    (0x01, "Ctrl"),
    (0x02, "Shift"),
    (0x04, "Alt"),
    (0x08, "Win"),
];

impl App {
    pub fn new(worker: Worker, list_ports: fn() -> Vec<PortInfo>) -> App {
        App {
            worker,
            list_ports,
            ports: list_ports(),
            port: 0,
            tab: Tab::Connect,
            session: None,
            tools: builtin_tools(),
            cfg: builtin("pad").unwrap_or_else(empty_config),
            snapshot: None,
            snapshot_serial: None,
            connecting_serial: None,
            saving: None,
            busy: None,
            status: "Plug in the pad, pick its port and press Connect.".into(),
            error: None,
            notice: None,
        }
    }

    /// Connect to the first listed port (screenshots and `--fake`).
    pub fn connect_first(&mut self) {
        self.port = 0;
        self.connect();
    }

    pub fn connected(&self) -> bool {
        self.session.is_some()
    }

    /// Handle the worker's replies; true while a request is still running.
    pub fn poll(&mut self) -> bool {
        while let Ok(r) = self.worker.rx.try_recv() {
            self.handle(r);
        }
        self.busy.is_some()
    }

    /// Apply one worker reply.
    fn handle(&mut self, r: Resp) {
        match r {
            Resp::Progress(s) => {
                self.busy = Some(s);
                return;
            }
            Resp::Landed(rec) => {
                // An earlier Save landed after all: that record is what the
                // pad holds (so it's out of recovery). The request in flight
                // carries on.
                if let Ok(c) = Config::decode(&rec) {
                    if self.saving.as_ref() == Some(&c) {
                        self.saving = None;
                    }
                    self.snapshot = Some(c);
                }
                if let Some(s) = self.session.as_mut() {
                    s.hello.mode = "config".into();
                    s.hello.cfg = "ok".into();
                }
                return;
            }
            Resp::Connected(s) => self.on_connect(*s),
            Resp::Checked => {
                self.error = None;
                self.status = "The pad accepts this setup.".into();
            }
            Resp::Saved => {
                self.error = None;
                if let Some(sent) = self.saving.take() {
                    self.snapshot = Some(sent);
                }
                // The pad restarted with this record: out of recovery.
                if let Some(s) = self.session.as_mut() {
                    s.hello.mode = "config".into();
                    s.hello.cfg = "ok".into();
                }
                self.status = "Saved. The pad restarted with the new setup.".into();
            }
            Resp::Refused(e) => self.error = Some(format!("The pad refused this setup: {e}")),
            Resp::Failed(m) => self.error = Some(m),
            Resp::Left => {
                self.status = "The pad left config mode. Save will ask it back.".into();
            }
            Resp::Disconnected => {
                // The snapshot stays, so a reconnect can tell whether
                // the editor holds unsaved edits.
                self.session = None;
                self.status = "Disconnected.".into();
            }
        }
        self.busy = None;
    }

    fn on_connect(&mut self, s: Session) {
        let same_pins = s.pins.len() == PIN_NAMES.len()
            && s.pins
                .iter()
                .all(|(code, name)| PIN_NAMES.get(usize::from(*code)) == Some(&name.as_str()));
        if !same_pins {
            self.error =
                Some("This pad's pin table differs from this app's: update the app.".into());
            self.worker.send(Req::Disconnect);
            return;
        }
        match Config::decode(&s.record) {
            Ok(c) => {
                self.cfg = self.carry_over(&c);
                self.snapshot = Some(c);
                self.snapshot_serial = self.connecting_serial.take();
                self.error = None;
            }
            Err(e) => {
                // No session: Save stays off rather than sending the editor's
                // old contents to a pad whose setup couldn't be read.
                self.error = Some(format!("The pad sent a setup this app can't read: {e:#}"));
                self.worker.send(Req::Disconnect);
                return;
            }
        }
        self.tools = s.tools.clone();
        self.status = if s.hello.mode == "recovery" {
            format!(
                "Recovery: the pad's saved setup is unusable ({}). Load a preset, then Save.",
                s.hello.cfg
            )
        } else {
            format!(
                "Connected to firmware {}. The pad's keys are paused while this app is connected.",
                s.hello.fw
            )
        };
        if self.tab == Tab::Connect && s.hello.mode != "recovery" {
            self.tab = Tab::Hardware;
        }
        self.session = Some(s);
    }

    fn connect(&mut self) {
        let Some(p) = self.ports.get(self.port) else {
            self.error = Some("No serial port picked.".into());
            return;
        };
        self.error = None;
        self.busy = Some("Connecting".into());
        // Writes stay off until this Connect succeeds.
        self.session = None;
        self.connecting_serial = p.serial.clone();
        self.worker.send(Req::Connect {
            path: p.path.clone(),
            serial: p.serial.clone(),
        });
    }

    fn encoded(&mut self) -> Option<Vec<u8>> {
        match self.cfg.encode() {
            Ok(r) => Some(r),
            Err(e) => {
                self.error = Some(format!("{e:#}"));
                None
            }
        }
    }

    fn shown_tools(&self) -> Vec<(u8, String)> {
        self.cfg
            .tool_kinds
            .iter()
            .map(|&k| (k, self.tool_name(k)))
            .collect()
    }

    fn tool_name(&self, kind: u8) -> String {
        self.tools
            .iter()
            .find(|t| t.kind == kind)
            .map_or_else(|| format!("tool {kind}"), |t| t.name.clone())
    }

    /// The editor's contents after Connect read `pad`: unsaved edits are
    /// rebased onto it when this is the same pad (by USB serial number)
    /// they were made against; otherwise the pad's setup.
    fn carry_over(&mut self, pad: &Config) -> Config {
        let same_pad =
            self.snapshot_serial.is_some() && self.snapshot_serial == self.connecting_serial;
        // A Save whose outcome was unclear and which the pad now holds landed:
        // it, not the older snapshot, is what the editor's edits stand on.
        if same_pad && self.saving.as_ref() == Some(pad) {
            self.snapshot = self.saving.take();
        }
        let unsaved = self.snapshot.as_ref().is_some_and(|old| *old != self.cfg);
        let Some(old) = self.snapshot.clone().filter(|_| same_pad && unsaved) else {
            if unsaved {
                self.notice = Some("Unsaved edits made for another pad were discarded.".into());
            }
            return pad.clone();
        };
        if self.cfg == *pad {
            return pad.clone(); // the edits are what the pad holds (a Save that landed)
        }
        let rebased = (|| -> anyhow::Result<(Config, Vec<usize>)> {
            let (rec, conflicts) =
                crate::config::merge3(&old.encode()?, &self.cfg.encode()?, &pad.encode()?)?;
            Ok((Config::decode(&rec)?, conflicts))
        })();
        match rebased {
            Ok((merged, conflicts)) if conflicts.is_empty() => {
                self.notice = Some(
                    "Your unsaved edits are kept, on top of the pad's current setup. Save to send them, or Revert."
                        .into(),
                );
                merged
            }
            Ok((merged, conflicts)) => {
                let fields: Vec<String> = conflicts
                    .iter()
                    .map(|&o| crate::config::field_at(o))
                    .collect();
                let mut fields = fields;
                fields.dedup();
                self.notice = Some(format!(
                    "Your unsaved edits are kept on top of the pad's current setup, but the pad also changed {}: \
                     Save would replace those with your values.",
                    fields.join(", ")
                ));
                merged
            }
            Err(_) => {
                self.notice = Some(
                    "Your unsaved edits are kept as they were; the pad's setup changed too, and Save would replace it. \
                     Revert to take the pad's setup."
                        .into(),
                );
                self.cfg.clone()
            }
        }
    }

    pub fn show(&mut self, ui: &mut egui::Ui) {
        if self.poll() {
            ui.ctx()
                .request_repaint_after(std::time::Duration::from_millis(100));
        }
        egui::Panel::top("tabs").show(ui, |ui| {
            ui.horizontal(|ui| {
                for (t, name) in TABS {
                    ui.selectable_value(&mut self.tab, t, name);
                }
            });
        });
        egui::Panel::bottom("bar").show(ui, |ui| self.bar(ui));
        egui::CentralPanel::default_margins().show(ui, |ui| {
            egui::ScrollArea::vertical().show(ui, |ui| match self.tab {
                Tab::Connect => self.connect_tab(ui),
                Tab::Hardware => self.hardware_tab(ui),
                Tab::Sets => self.sets_tab(ui),
                Tab::Tools => self.tools_tab(ui),
            });
        });
    }

    fn bar(&mut self, ui: &mut egui::Ui) {
        ui.add_space(4.0);
        if let Some(n) = self.notice.clone() {
            ui.horizontal(|ui| {
                ui.label(n);
                if ui.small_button("OK").clicked() {
                    self.notice = None;
                }
            });
        }
        if let Some(e) = &self.error {
            ui.colored_label(ui.visuals().error_fg_color, e);
        } else if let Some(b) = &self.busy {
            ui.horizontal(|ui| {
                ui.spinner();
                ui.label(format!("{b}…"));
            });
        } else {
            ui.label(&self.status);
        }
        ui.horizontal(|ui| {
            let idle = self.busy.is_none();
            let online = self.session.is_some();
            let changed = self.snapshot.as_ref().is_some_and(|s| *s != self.cfg);
            if ui
                .add_enabled(idle && online, egui::Button::new("Save to pad"))
                .on_hover_text("Send, check and save; the pad restarts")
                .clicked()
                && let Some(rec) = self.encoded()
            {
                self.error = None;
                self.busy = Some("Saving".into());
                // What the pad will hold if this Save succeeds; the editor
                // may change while it runs.
                self.saving = Some(self.cfg.clone());
                self.worker.send(Req::Save(rec));
            }
            if ui
                .add_enabled(idle && online, egui::Button::new("Check"))
                .on_hover_text("Ask the pad whether it accepts this setup, without saving")
                .clicked()
                && let Some(rec) = self.encoded()
            {
                self.error = None;
                self.busy = Some("Checking".into());
                self.worker.send(Req::Check(rec));
            }
            if ui
                .add_enabled(changed, egui::Button::new("Revert"))
                .on_hover_text("Go back to the setup read at Connect")
                .clicked()
                && let Some(s) = &self.snapshot
            {
                self.cfg = s.clone();
                self.error = None;
            }
            ui.separator();
            ui.label("Load preset:");
            for name in ["pad", "deck"] {
                if ui.button(name).clicked()
                    && let Some(c) = builtin(name)
                {
                    self.cfg = c;
                    self.error = None;
                    self.status = format!("Loaded the {name} preset. Save to send it to the pad.");
                }
            }
            if changed {
                ui.label("Unsaved changes");
            }
        });
        ui.add_space(4.0);
    }

    fn connect_tab(&mut self, ui: &mut egui::Ui) {
        ui.heading("Pad");
        ui.horizontal(|ui| {
            let label = |p: &PortInfo| {
                let kind = if p.pad { "Nano ESP32" } else { "serial port" };
                format!("{} ({kind})", p.path)
            };
            let text = self
                .ports
                .get(self.port)
                .map_or_else(|| "no ports found".into(), label);
            egui::ComboBox::from_id_salt("port")
                .selected_text(text)
                .width(280.0)
                .show_ui(ui, |ui| {
                    for (i, p) in self.ports.iter().enumerate() {
                        ui.selectable_value(&mut self.port, i, label(p));
                    }
                });
            if ui.button("Refresh").clicked() {
                self.ports = (self.list_ports)();
                self.port = 0;
            }
        });
        ui.horizontal(|ui| {
            let idle = self.busy.is_none();
            if ui
                .add_enabled(idle && !self.ports.is_empty(), egui::Button::new("Connect"))
                .clicked()
            {
                self.connect();
            }
            if ui
                .add_enabled(
                    idle && self.session.is_some(),
                    egui::Button::new("Disconnect"),
                )
                .on_hover_text("The pad goes back to work")
                .clicked()
            {
                self.worker.send(Req::Disconnect);
            }
        });
        ui.add_space(8.0);
        let Some(s) = &self.session else {
            ui.label("Not connected. You can still edit and load presets; connect to save them.");
            return;
        };
        egui::Grid::new("hello").num_columns(2).show(ui, |ui| {
            ui.label("Firmware");
            ui.label(&s.hello.fw);
            ui.end_row();
            ui.label("Mode");
            ui.label(&s.hello.mode);
            ui.end_row();
            ui.label("Saved setup");
            ui.label(&s.hello.cfg);
            ui.end_row();
        });
        if s.hello.mode == "recovery" {
            ui.add_space(8.0);
            ui.label("The pad's saved setup can't be used, so it's running its built-in one.");
            if ui.button("Load pad preset").clicked()
                && let Some(c) = builtin("pad")
            {
                self.cfg = c;
                self.tab = Tab::Hardware;
            }
        }
    }

    /// Pins in use, other than the ones a combo is editing.
    fn used_pins(&self) -> Vec<u8> {
        let c = &self.cfg;
        let mut v: Vec<u8> = c.key_pins[..usize::from(c.key_count)].to_vec();
        v.extend(
            c.buttons
                .iter()
                .filter(|b| b.role != ROLE_NONE)
                .map(|b| b.pin),
        );
        if c.enc_flags & ENC_ENABLED != 0 {
            v.extend(c.enc_pins);
        }
        v.retain(|&p| p != PIN_NONE);
        v
    }

    fn free_pin(&self) -> u8 {
        let used = self.used_pins();
        (0..16u8).find(|p| !used.contains(p)).unwrap_or(PIN_NONE)
    }

    fn hardware_tab(&mut self, ui: &mut egui::Ui) {
        ui.heading("Keys");
        let mut count = self.cfg.key_count;
        egui::ComboBox::from_id_salt("count")
            .selected_text(format!("{count} keys"))
            .show_ui(ui, |ui| {
                for n in 1..=u8::try_from(KEYS).unwrap_or(8) {
                    ui.selectable_value(&mut count, n, format!("{n} keys"));
                }
            });
        while count > self.cfg.key_count {
            let p = self.free_pin();
            self.cfg.key_pins[usize::from(self.cfg.key_count)] = p;
            self.cfg.key_count += 1;
        }
        while count < self.cfg.key_count {
            self.cfg.key_count -= 1;
            self.cfg.key_pins[usize::from(self.cfg.key_count)] = PIN_NONE;
        }
        let used = self.used_pins();
        egui::Grid::new("keys").num_columns(2).show(ui, |ui| {
            for i in 0..usize::from(self.cfg.key_count) {
                ui.label(format!("Key {}", i + 1));
                pin_combo(ui, ("key", i), &mut self.cfg.key_pins[i], &used, false);
                ui.end_row();
            }
        });

        ui.add_space(8.0);
        ui.heading("Small buttons");
        let shown = self.shown_tools();
        for b in 0..self.cfg.buttons.len() {
            let used = self.used_pins();
            let free = self.free_pin();
            let btn = &mut self.cfg.buttons[b];
            ui.horizontal(|ui| {
                ui.label(format!("Button {}", b + 1));
                let before = btn.role;
                egui::ComboBox::from_id_salt(("role", b))
                    .selected_text(role_label(btn.role))
                    .show_ui(ui, |ui| {
                        for r in [ROLE_NONE, ROLE_KILL, ROLE_NAV, ROLE_BIND] {
                            ui.selectable_value(&mut btn.role, r, role_label(r));
                        }
                    });
                if btn.role != before {
                    if btn.role == ROLE_NONE {
                        btn.pin = PIN_NONE;
                    } else if before == ROLE_NONE {
                        btn.pin = free;
                    }
                    btn.bind = if btn.role == ROLE_BIND {
                        key_binding(0x68)
                    } else {
                        Binding::default()
                    };
                }
                if btn.role != ROLE_NONE {
                    pin_combo(ui, ("bpin", b), &mut btn.pin, &used, false);
                }
                if btn.role == ROLE_BIND {
                    binding_editor(ui, ("bbind", b), &mut btn.bind, &shown, false);
                }
            });
        }
        ui.label(
            "Kill pauses everything the pad sends. Nav opens the menu; one button must be Nav.",
        );

        ui.add_space(8.0);
        ui.heading("Encoder");
        let mut on = self.cfg.enc_flags & ENC_ENABLED != 0;
        if ui.checkbox(&mut on, "Rotary encoder fitted").changed() {
            if on {
                self.cfg.enc_flags = ENC_ENABLED;
                self.cfg.detent = 4;
                for i in 0..3 {
                    self.cfg.enc_pins[i] = self.free_pin();
                }
            } else {
                self.cfg.enc_flags = 0;
                self.cfg.detent = 0;
                self.cfg.enc_pins = [PIN_NONE; 3];
            }
        }
        if on {
            let used = self.used_pins();
            egui::Grid::new("enc").num_columns(2).show(ui, |ui| {
                for (i, name) in ["A", "B", "Push"].iter().enumerate() {
                    ui.label(*name);
                    pin_combo(ui, ("enc", i), &mut self.cfg.enc_pins[i], &used, i == 2);
                    ui.end_row();
                }
                ui.label("Steps per click");
                egui::ComboBox::from_id_salt("detent")
                    .selected_text(self.cfg.detent.to_string())
                    .show_ui(ui, |ui| {
                        for d in [1u8, 2, 4] {
                            ui.selectable_value(&mut self.cfg.detent, d, d.to_string());
                        }
                    });
                ui.end_row();
            });
        }

        ui.add_space(8.0);
        ui.heading("Timing");
        ui.horizontal(|ui| {
            ui.label("Key release debounce");
            ui.add(
                egui::DragValue::new(&mut self.cfg.release_ms)
                    .range(1..=30)
                    .suffix(" ms"),
            );
        });
    }

    fn sets_tab(&mut self, ui: &mut egui::Ui) {
        let shown = self.shown_tools();
        let keys = usize::from(self.cfg.key_count);
        let enc = self.cfg.enc_flags & ENC_ENABLED != 0;
        ui.horizontal(|ui| {
            ui.label("Shown at start-up:");
            let cur = self
                .cfg
                .sets
                .get(usize::from(self.cfg.selected))
                .map_or("?", |s| s.name.as_str());
            egui::ComboBox::from_id_salt("selected")
                .selected_text(cur.to_string())
                .show_ui(ui, |ui| {
                    for (i, s) in self.cfg.sets.iter().enumerate() {
                        ui.selectable_value(
                            &mut self.cfg.selected,
                            u8::try_from(i).unwrap_or(0),
                            &s.name,
                        );
                    }
                });
        });
        let mut remove = None;
        let n = self.cfg.sets.len();
        for (si, set) in self.cfg.sets.iter_mut().enumerate() {
            egui::CollapsingHeader::new(format!("{} ({})", set.name, set.id))
                .id_salt(("set", si))
                .default_open(si == 0)
                .show(ui, |ui| {
                    ui.horizontal(|ui| {
                        ui.label("Name");
                        ui.add(
                            egui::TextEdit::singleline(&mut set.name)
                                .char_limit(12)
                                .desired_width(110.0),
                        );
                        set.name.retain(|c| (' '..='~').contains(&c));
                        ui.label("Id");
                        ui.add(
                            egui::TextEdit::singleline(&mut set.id)
                                .char_limit(6)
                                .desired_width(60.0),
                        )
                        .on_hover_text(
                            "Lowercase letters and digits; it names the set's saved settings",
                        );
                        set.id
                            .retain(|c| c.is_ascii_lowercase() || c.is_ascii_digit());
                        flag_box(ui, &mut set.flags, SET_LEGEND, "Show labels");
                        flag_box(ui, &mut set.flags, SET_LIVE, "Live key boxes");
                        if n > 1 && ui.button("Remove").clicked() {
                            remove = Some(si);
                        }
                    });
                    egui::Grid::new(("binds", si))
                        .num_columns(2)
                        .show(ui, |ui| {
                            for (k, b) in set.keys.iter_mut().take(keys).enumerate() {
                                ui.label(format!("Key {}", k + 1));
                                binding_editor(ui, ("k", si, k), b, &shown, true);
                                ui.end_row();
                            }
                            if enc {
                                for (e, name) in
                                    ["Turn right", "Turn left", "Push"].iter().enumerate()
                                {
                                    ui.label(*name);
                                    binding_editor(ui, ("e", si, e), &mut set.enc[e], &shown, true);
                                    ui.end_row();
                                }
                            }
                        });
                });
        }
        if let Some(i) = remove {
            remove_set(&mut self.cfg, i);
        }
        if self.cfg.sets.len() < SETS && ui.button("Add key set").clicked() {
            let n = (1..)
                .find(|i| !self.cfg.sets.iter().any(|s| s.id == format!("set{i}")))
                .unwrap_or(1);
            self.cfg.sets.push(KeySet {
                id: format!("set{n}"),
                name: format!("Set {n}"),
                flags: SET_LEGEND,
                ..KeySet::default()
            });
        }
    }

    fn tools_tab(&mut self, ui: &mut egui::Ui) {
        ui.label("Shown tools appear in the pad's menu, in the order you tick them.");
        let tools = self.tools.clone();
        for t in &tools {
            ui.add_space(6.0);
            let mut shown = self.cfg.tool_kinds.contains(&t.kind);
            if ui
                .checkbox(&mut shown, egui::RichText::new(&t.name).strong())
                .changed()
            {
                if shown {
                    if self.cfg.tool_kinds.len() < TOOL_SLOTS {
                        self.cfg.tool_kinds.push(t.kind);
                    }
                } else {
                    self.hide_tool(t.kind, &t.name);
                }
            }
            let Some(slot) = self.cfg.params.get_mut(usize::from(t.kind).wrapping_sub(1)) else {
                continue;
            };
            egui::Grid::new(("params", t.kind))
                .num_columns(2)
                .show(ui, |ui| {
                    for p in &t.params {
                        let Some(v) = slot.get_mut(p.index) else {
                            continue;
                        };
                        ui.label(&p.label);
                        match p.kind.as_str() {
                            "choice" => {
                                let cur = usize::try_from(*v)
                                    .ok()
                                    .and_then(|i| p.names.get(i))
                                    .cloned();
                                egui::ComboBox::from_id_salt(("p", t.kind, p.index))
                                    .selected_text(cur.unwrap_or_else(|| v.to_string()))
                                    .show_ui(ui, |ui| {
                                        for (i, n) in p.names.iter().enumerate() {
                                            ui.selectable_value(
                                                v,
                                                i32::try_from(i).unwrap_or(0),
                                                n,
                                            );
                                        }
                                    });
                            }
                            "key" => {
                                let mut u = u8::try_from(*v).unwrap_or(0);
                                key_combo(ui, ("p", t.kind, p.index), &mut u);
                                *v = i32::from(u);
                            }
                            _ => {
                                let unit = if p.unit.is_empty() {
                                    String::new()
                                } else {
                                    format!(" {}", p.unit)
                                };
                                ui.add(
                                    egui::DragValue::new(v)
                                        .range(p.min..=p.max)
                                        .speed(f64::from(p.step).max(1.0) / 4.0)
                                        .suffix(unit),
                                );
                            }
                        }
                        ui.end_row();
                    }
                });
            // The firmware's clamp: a max below its min becomes the min.
            let lo = t.params.iter().find(|p| p.id == "min").map(|p| p.index);
            let hi = t.params.iter().find(|p| p.id == "max").map(|p| p.index);
            if let (Some(lo), Some(hi)) = (lo, hi)
                && slot[hi] < slot[lo]
            {
                slot[hi] = slot[lo];
            }
        }
    }

    /// Hide a tool, clearing every binding that toggles it (the pad refuses
    /// a toggle for a hidden tool), and say which.
    fn hide_tool(&mut self, kind: u8, name: &str) {
        self.cfg.tool_kinds.retain(|&k| k != kind);
        let mut cleared = Vec::new();
        let hits = |b: &Binding| b.kind == BK_TOGGLE && b.usage == kind;
        for (i, b) in self.cfg.buttons.iter_mut().enumerate() {
            if hits(&b.bind) {
                b.bind = Binding::default();
                cleared.push(format!("button {} (now unbound: pick a binding)", i + 1));
            }
        }
        for s in &mut self.cfg.sets {
            for (i, b) in s.keys.iter_mut().enumerate() {
                if hits(b) {
                    *b = Binding::default();
                    cleared.push(format!("{} key {}", s.name, i + 1));
                }
            }
            for (i, b) in s.enc.iter_mut().enumerate() {
                if hits(b) {
                    *b = Binding::default();
                    cleared.push(format!(
                        "{} {}",
                        s.name,
                        ["turn right", "turn left", "push"][i]
                    ));
                }
            }
        }
        if !cleared.is_empty() {
            self.notice = Some(format!("Hiding {name} cleared: {}.", cleared.join(", ")));
        }
    }
}

impl eframe::App for App {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        self.show(ui);
    }

    fn on_exit(&mut self) {
        // Leave config mode before the window goes; Worker's drop joins.
        self.worker.send(Req::Disconnect);
    }
}

fn empty_config() -> Config {
    Config {
        key_count: 4,
        key_pins: [0, 1, 2, 3, PIN_NONE, PIN_NONE, PIN_NONE, PIN_NONE],
        buttons: Default::default(),
        enc_flags: 0,
        enc_pins: [PIN_NONE; 3],
        detent: 0,
        release_ms: 5,
        sets: vec![KeySet::default()],
        tool_kinds: Vec::new(),
        params: [[0; 8]; TOOL_SLOTS],
        selected: 0,
    }
}

/// Remove a key set, keeping the start-up set on the same set (by identity):
/// a later set moves down a slot; removing the start-up set picks the first.
fn remove_set(cfg: &mut Config, i: usize) {
    cfg.sets.remove(i);
    let sel = usize::from(cfg.selected);
    cfg.selected = match sel.cmp(&i) {
        std::cmp::Ordering::Less => cfg.selected,
        std::cmp::Ordering::Equal => 0,
        std::cmp::Ordering::Greater => cfg.selected - 1,
    };
}

fn role_label(r: u8) -> &'static str {
    match r {
        ROLE_KILL => "Kill",
        ROLE_NAV => "Nav",
        ROLE_BIND => "Sends",
        _ => "Not fitted",
    }
}

fn key_binding(usage: u8) -> Binding {
    Binding {
        kind: BK_KEY,
        mods: 0,
        usage,
        label: String::new(),
    }
}

fn flag_box(ui: &mut egui::Ui, flags: &mut u8, bit: u8, text: &str) {
    let mut on = *flags & bit != 0;
    if ui.checkbox(&mut on, text).changed() {
        *flags ^= bit;
    }
}

fn pin_combo(
    ui: &mut egui::Ui,
    id: impl std::hash::Hash + std::fmt::Debug,
    pin: &mut u8,
    used: &[u8],
    none: bool,
) {
    let name = |p: u8| PIN_NAMES.get(usize::from(p)).map_or("none", |n| n);
    egui::ComboBox::from_id_salt(id)
        .selected_text(name(*pin))
        .width(70.0)
        .show_ui(ui, |ui| {
            if none {
                ui.selectable_value(pin, PIN_NONE, "none");
            }
            for p in 0..16u8 {
                if p == *pin || !used.contains(&p) {
                    ui.selectable_value(pin, p, name(p));
                }
            }
        });
}

fn key_combo(ui: &mut egui::Ui, id: impl std::hash::Hash + std::fmt::Debug, usage: &mut u8) {
    egui::ComboBox::from_id_salt(id)
        .selected_text(key_name(*usage))
        .width(90.0)
        .show_ui(ui, |ui| {
            for (u, n) in KEY_NAMES {
                ui.selectable_value(usage, *u, *n);
            }
        });
}

/// Kind, then what it sends, then a label (shown when the set shows labels).
fn binding_editor(
    ui: &mut egui::Ui,
    id: impl std::hash::Hash + std::fmt::Debug + Copy,
    b: &mut Binding,
    tools: &[(u8, String)],
    none: bool,
) {
    ui.horizontal(|ui| {
        let kinds: &[(u8, &str)] = &[
            (BK_NONE, "Nothing"),
            (BK_KEY, "Key"),
            (BK_CONSUMER, "Media"),
            (BK_TOGGLE, "Tool"),
        ];
        let before = b.kind;
        let label = |k: u8| kinds.iter().find(|(x, _)| *x == k).map_or("?", |(_, n)| n);
        egui::ComboBox::from_id_salt(("kind", id))
            .selected_text(label(b.kind))
            .width(80.0)
            .show_ui(ui, |ui| {
                for (k, n) in kinds {
                    if *k == BK_NONE && !none {
                        continue;
                    }
                    if *k == BK_TOGGLE && tools.is_empty() && b.kind != BK_TOGGLE {
                        continue;
                    }
                    ui.selectable_value(&mut b.kind, *k, *n);
                }
            });
        if b.kind != before {
            b.mods = 0;
            b.usage = match b.kind {
                BK_KEY => 0x68,
                BK_TOGGLE => tools.first().map_or(1, |t| t.0),
                _ => 0,
            };
            if b.kind == BK_NONE {
                b.label.clear();
            }
        }
        match b.kind {
            BK_KEY => {
                key_combo(ui, ("key", id), &mut b.usage);
                for (bit, name) in MODS {
                    flag_box(ui, &mut b.mods, bit, name);
                }
            }
            BK_CONSUMER => {
                let cur = CONSUMER_NAMES
                    .get(usize::from(b.usage))
                    .copied()
                    .unwrap_or("?");
                egui::ComboBox::from_id_salt(("media", id))
                    .selected_text(cur)
                    .show_ui(ui, |ui| {
                        for (i, n) in CONSUMER_NAMES.iter().enumerate() {
                            ui.selectable_value(&mut b.usage, u8::try_from(i).unwrap_or(0), *n);
                        }
                    });
            }
            BK_TOGGLE => {
                let cur = tools
                    .iter()
                    .find(|t| t.0 == b.usage)
                    .map_or_else(|| "hidden tool".to_string(), |t| t.1.clone());
                egui::ComboBox::from_id_salt(("tool", id))
                    .selected_text(cur)
                    .show_ui(ui, |ui| {
                        for (k, n) in tools {
                            ui.selectable_value(&mut b.usage, *k, n);
                        }
                    });
            }
            _ => {}
        }
        if b.kind != BK_NONE {
            ui.add(
                egui::TextEdit::singleline(&mut b.label)
                    .char_limit(8)
                    .desired_width(70.0)
                    .hint_text("label"),
            );
            b.label.retain(|c| (' '..='~').contains(&c));
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::fake::FakePorts;

    fn app() -> App {
        let pad = crate::fake_pad().expect("the pad preset");
        App::new(Worker::spawn(FakePorts(pad), || {}), crate::fake_ports)
    }

    fn toggles(c: &Config, kind: u8) -> usize {
        let hit = |b: &Binding| b.kind == BK_TOGGLE && b.usage == kind;
        c.buttons.iter().filter(|b| hit(&b.bind)).count()
            + c.sets
                .iter()
                .flat_map(|s| s.keys.iter().chain(&s.enc))
                .filter(|b| hit(b))
                .count()
    }

    #[test]
    fn hiding_a_tool_clears_its_toggles_everywhere() {
        let mut a = app();
        a.cfg.key_count = 2; // a toggle past the key count still counts
        a.cfg.sets[0].keys[7] = Binding {
            kind: BK_TOGGLE,
            mods: 0,
            usage: 1,
            label: "x".into(),
        };
        a.cfg.sets[0].enc[2] = a.cfg.sets[0].keys[7].clone();
        assert!(
            toggles(&a.cfg, 1) >= 3,
            "the stream set toggles the autoclicker too"
        );
        a.hide_tool(1, "Autoclicker");
        assert_eq!(toggles(&a.cfg, 1), 0);
        assert!(!a.cfg.tool_kinds.contains(&1));
        assert!(toggles(&a.cfg, 2) > 0, "other tools keep theirs");
        let n = a.notice.expect("a notice");
        assert!(n.contains("key 8") && n.contains("push"), "{n}");
    }

    #[test]
    fn reconnecting_keeps_unsaved_edits() {
        let mut a = app();
        let pad = builtin("pad").expect("pad");
        a.snapshot = Some(pad.clone());
        a.cfg.release_ms = 9; // an unsaved edit
        let mut other = pad.clone();
        other.selected = 1; // the pad's own setup changed meanwhile
        let session = Session {
            hello: crate::protocol::Hello {
                fw: "0.3.0".into(),
                mode: "config".into(),
                cfg: "ok".into(),
                crc: None,
                boot: 1,
                sn: None,
            },
            pins: PIN_NAMES
                .iter()
                .enumerate()
                .map(|(i, n)| (u8::try_from(i).expect("16 pins"), (*n).to_string()))
                .collect(),
            tools: builtin_tools(),
            record: other.encode().expect("encode"),
        };
        a.snapshot_serial = Some("A".into());
        a.connecting_serial = Some("A".into());
        a.on_connect(session.clone());
        assert_eq!(a.cfg.release_ms, 9, "the edit survives");
        assert_eq!(a.cfg.selected, 1, "and so does the pad's own change");
        assert_eq!(a.snapshot.as_ref().expect("snapshot").selected, 1);
        assert!(a.notice.is_some());

        // Another pad: nothing carries over.
        a.snapshot = Some(pad.clone());
        a.cfg = pad.clone();
        a.cfg.release_ms = 9;
        a.snapshot_serial = Some("A".into());
        a.connecting_serial = Some("B".into());
        a.on_connect(session);
        assert_eq!(a.cfg, other, "a different pad shows its own setup");
    }

    #[test]
    fn a_save_snapshots_what_it_sent() {
        let mut a = app();
        let sent = a.cfg.clone();
        a.saving = Some(sent.clone());
        a.cfg.release_ms = 21; // edited while the Save ran
        a.handle(Resp::Saved);
        assert_eq!(a.snapshot.as_ref(), Some(&sent));
        assert_ne!(
            a.snapshot.as_ref(),
            Some(&a.cfg),
            "the later edit still shows as unsaved"
        );
    }

    #[test]
    fn an_unreadable_setup_opens_no_session() {
        let mut a = app();
        let session = Session {
            hello: crate::protocol::Hello {
                fw: "0.3.0".into(),
                mode: "config".into(),
                cfg: "ok".into(),
                crc: None,
                boot: 1,
                sn: None,
            },
            pins: PIN_NAMES
                .iter()
                .enumerate()
                .map(|(i, n)| (u8::try_from(i).expect("16 pins"), (*n).to_string()))
                .collect(),
            tools: builtin_tools(),
            record: vec![0; 1355],
        };
        a.handle(Resp::Connected(Box::new(session)));
        assert!(!a.connected(), "Save stays off");
        assert!(a.error.is_some());
    }

    #[test]
    fn a_landed_save_found_during_another_save_snapshots_the_landed_record() {
        let mut a = app();
        let landed = a.cfg.clone(); // Save A, which timed out but landed
        let mut b = a.cfg.clone();
        b.release_ms = 22; // Save B, now in flight
        a.cfg = b.clone();
        a.saving = Some(b.clone());
        a.busy = Some("Saving".into());
        a.handle(Resp::Landed(landed.encode().expect("encode")));
        assert_eq!(a.snapshot.as_ref(), Some(&landed), "the pad holds A");
        assert!(a.busy.is_some(), "the request in flight carries on");
        a.handle(Resp::Refused(crate::protocol::DeviceError {
            code: 105,
            offset: 0,
            text: "flash write failed".into(),
        }));
        assert_ne!(
            a.snapshot.as_ref(),
            Some(&a.cfg),
            "B still shows as unsaved"
        );
    }

    #[test]
    fn removing_a_set_keeps_the_start_up_set() {
        let mut c = builtin("pad").expect("pad"); // osu, stream, media
        c.selected = 1; // stream
        remove_set(&mut c, 0);
        assert_eq!(c.sets[usize::from(c.selected)].id, "stream");
        c.selected = 0; // stream, now first
        remove_set(&mut c, 0); // remove the start-up set itself
        assert_eq!(c.selected, 0);
        assert_eq!(c.sets[0].id, "media");
    }

    #[test]
    fn an_edit_made_during_a_save_that_landed_survives_a_reconnect() {
        let mut a = app();
        let mut five = a.cfg.clone();
        five.release_ms = 5;
        let mut ten = five.clone();
        ten.release_ms = 10;
        a.snapshot = Some(five.clone());
        a.saving = Some(ten.clone()); // Save of 10 ms: landed, check timed out
        a.cfg = five.clone(); // edited back to 5 ms meanwhile
        a.snapshot_serial = Some("A".into());
        a.connecting_serial = Some("A".into());
        a.handle(Resp::Failed("couldn't confirm the save".into()));
        let session = Session {
            hello: crate::protocol::Hello {
                fw: "0.3.0".into(),
                mode: "config".into(),
                cfg: "ok".into(),
                crc: None,
                boot: 2,
                sn: None,
            },
            pins: PIN_NAMES
                .iter()
                .enumerate()
                .map(|(i, n)| (u8::try_from(i).expect("16 pins"), (*n).to_string()))
                .collect(),
            tools: builtin_tools(),
            record: ten.encode().expect("encode"), // the pad holds 10 ms
        };
        a.on_connect(session);
        assert_eq!(a.cfg.release_ms, 5, "the edit back to 5 ms survives");
        assert_eq!(
            a.snapshot.as_ref(),
            Some(&ten),
            "the pad's 10 ms is the base"
        );
    }

    #[test]
    fn a_landed_recovery_save_leaves_recovery_status() {
        let mut a = app();
        let mut session = Session {
            hello: crate::protocol::Hello {
                fw: "0.3.0".into(),
                mode: "recovery".into(),
                cfg: "invalid 4".into(),
                crc: None,
                boot: 1,
                sn: None,
            },
            pins: PIN_NAMES
                .iter()
                .enumerate()
                .map(|(i, n)| (u8::try_from(i).expect("16 pins"), (*n).to_string()))
                .collect(),
            tools: builtin_tools(),
            record: a.cfg.encode().expect("encode"),
        };
        session.hello.cfg = "invalid 4".into();
        a.session = Some(session);
        a.handle(Resp::Landed(a.cfg.encode().expect("encode")));
        let h = &a.session.as_ref().expect("session").hello;
        assert_eq!((h.mode.as_str(), h.cfg.as_str()), ("config", "ok"));
    }
}
