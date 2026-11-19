//! Named keys and the fixed tool kinds, mirroring the firmware's
//! `src/tools/schema.cpp` (a test checks the key table against it).

/// HID keyboard usages with names, in picker order.
pub const KEY_NAMES: &[(u8, &str)] = &[
    (0x04, "A"),
    (0x05, "B"),
    (0x06, "C"),
    (0x07, "D"),
    (0x08, "E"),
    (0x09, "F"),
    (0x0A, "G"),
    (0x0B, "H"),
    (0x0C, "I"),
    (0x0D, "J"),
    (0x0E, "K"),
    (0x0F, "L"),
    (0x10, "M"),
    (0x11, "N"),
    (0x12, "O"),
    (0x13, "P"),
    (0x14, "Q"),
    (0x15, "R"),
    (0x16, "S"),
    (0x17, "T"),
    (0x18, "U"),
    (0x19, "V"),
    (0x1A, "W"),
    (0x1B, "X"),
    (0x1C, "Y"),
    (0x1D, "Z"),
    (0x1E, "1"),
    (0x1F, "2"),
    (0x20, "3"),
    (0x21, "4"),
    (0x22, "5"),
    (0x23, "6"),
    (0x24, "7"),
    (0x25, "8"),
    (0x26, "9"),
    (0x27, "0"),
    (0x28, "Enter"),
    (0x29, "Esc"),
    (0x2A, "Backspace"),
    (0x2B, "Tab"),
    (0x2C, "Space"),
    (0x3A, "F1"),
    (0x3B, "F2"),
    (0x3C, "F3"),
    (0x3D, "F4"),
    (0x3E, "F5"),
    (0x3F, "F6"),
    (0x40, "F7"),
    (0x41, "F8"),
    (0x42, "F9"),
    (0x43, "F10"),
    (0x44, "F11"),
    (0x45, "F12"),
    (0x68, "F13"),
    (0x69, "F14"),
    (0x6A, "F15"),
    (0x6B, "F16"),
    (0x6C, "F17"),
    (0x6D, "F18"),
    (0x6E, "F19"),
    (0x6F, "F20"),
    (0x70, "F21"),
    (0x71, "F22"),
    (0x72, "F23"),
    (0x73, "F24"),
];

pub fn key_name(usage: u8) -> String {
    KEY_NAMES
        .iter()
        .find(|(u, _)| *u == usage)
        .map_or_else(|| format!("0x{usage:02X}"), |(_, n)| (*n).to_string())
}

pub fn key_usage(name: &str) -> Option<u8> {
    if let Some(hex) = name.strip_prefix("0x") {
        return u8::from_str_radix(hex, 16).ok();
    }
    KEY_NAMES
        .iter()
        .find(|(_, n)| n.eq_ignore_ascii_case(name))
        .map(|(u, _)| *u)
}

/// Tool kinds by id, append-only: index + 1 is the kind.
pub const TOOL_IDS: [&str; 4] = ["click", "jitter", "spam", "hold"];

pub fn tool_kind(id: &str) -> Option<u8> {
    TOOL_IDS
        .iter()
        .position(|t| *t == id)
        .and_then(|i| u8::try_from(i + 1).ok())
}

pub fn tool_id(kind: u8) -> Option<&'static str> {
    TOOL_IDS.get(usize::from(kind).checked_sub(1)?).copied()
}

/// Pin codes and names (the firmware's `PINS` reply lists the same).
pub const PIN_NAMES: [&str; 16] = [
    "D2", "D3", "D4", "D5", "D6", "D7", "D8", "D9", "D10", "D11", "D12", "A0", "A1", "A3", "A6",
    "A7",
];

pub fn pin_name(code: u8) -> String {
    PIN_NAMES
        .get(usize::from(code))
        .map_or_else(|| "none".into(), |n| (*n).to_string())
}

pub fn pin_code(name: &str) -> Option<u8> {
    if name == "none" {
        return Some(crate::config::PIN_NONE);
    }
    PIN_NAMES
        .iter()
        .position(|n| *n == name)
        .and_then(|i| u8::try_from(i).ok())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn key_table_matches_the_firmware() {
        let path = format!("{}/../src/tools/schema.cpp", env!("CARGO_MANIFEST_DIR"));
        let src = std::fs::read_to_string(path).expect("schema.cpp exists");
        let start = src.find("const KeyName KEY_NAMES[] = {").expect("table");
        let end = start + src[start..].find("};").expect("end of table");
        let mut fw = Vec::new();
        for entry in src[start..end].split('{').skip(2) {
            let (hex, rest) = entry.split_once(',').expect("usage");
            let name = rest.split('"').nth(1).expect("name");
            let usage = u8::from_str_radix(hex.trim().trim_start_matches("0x"), 16).expect("hex");
            fw.push((usage, name.to_string()));
        }
        let ours: Vec<(u8, String)> = KEY_NAMES
            .iter()
            .map(|(u, n)| (*u, (*n).to_string()))
            .collect();
        assert_eq!(ours, fw);
    }

    #[test]
    fn names_round_trip() {
        assert_eq!(key_usage("f13"), Some(0x68));
        assert_eq!(key_name(0x2D), "0x2D");
        assert_eq!(key_usage("0x2D"), Some(0x2D));
        assert_eq!(tool_kind("spam"), Some(3));
        assert_eq!(tool_id(4), Some("hold"));
        assert_eq!(pin_code("A7"), Some(15));
    }
}
