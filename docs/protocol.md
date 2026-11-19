# Config record and protocol

A pad's whole setup is one record, kept in flash and edited over USB serial by `macropad-config` (in `client/`). This file is the reference for both sides. Ids and codes in it are frozen and only ever appended to.

## The record (v1.0, 1355 bytes)

Little-endian, no padding, encoded field by field on both sides. Stored as the NVS blob `cfg` in namespace `macropad` (`macropad-test` in self-test builds).

- **Strings** are fixed fields padded with NULs; bytes after the first NUL are 0. Labels and names are printable ASCII (0x20–0x7E); ids are `[a-z0-9]`.
- **Reserved bytes** and **unused flag bits** are 0.
- **Versions:** firmware accepts major 1 with a minor at or below its own.

| Section | Offset | Size |
| --- | --- | --- |
| Header | 0 | 16 |
| Hardware | 16 | 51 |
| Key sets | 67 | 1015 |
| Tools | 1082 | 269 |
| State | 1351 | 4 |

### Header

| Offset | Field | Size | Value |
| --- | --- | --- | --- |
| 0 | magic | 4 | `MPC1` |
| 4 | major | 1 | 1 |
| 5 | minor | 1 | 0 |
| 6 | length | 2 | 1355 |
| 8 | reserved | 4 | 0 |
| 12 | crc32 | 4 | CRC-32/ISO-HDLC (zlib) over all 1355 bytes, with this field taken as 0 |

### Hardware

| Offset | Field | Size | Notes |
| --- | --- | --- | --- |
| 16 | key count | 1 | 1–8 |
| 17 | key pins | 8 | pin codes; slots at or past the count are 0xFF |
| 25 | button 1 | 16 | pin, role, reserved, binding (13) |
| 41 | button 2 | 16 | the same |
| 57 | encoder flags | 1 | bit 0: fitted |
| 58 | encoder pins | 3 | A, B, push (push may be 0xFF) |
| 61 | detent | 1 | 1, 2 or 4 when fitted, else 0 |
| 62 | release debounce | 1 | 1–30 ms |
| 63 | reserved | 4 | |

A button with role none has pin 0xFF and a zero binding. Only a Bind button has a binding, and it isn't none.

### Binding (13 bytes)

| Offset | Field | Size | Notes |
| --- | --- | --- | --- |
| 0 | kind | 1 | 0 none, 1 key, 2 consumer, 3 toggle tool |
| 1 | mods | 1 | HID modifier bits, key bindings only |
| 2 | usage | 1 | key: HID usage 0x04–0x7F; consumer: a code 0–6; toggle: a tool kind 1–4 |
| 3 | reserved | 1 | |
| 4 | label | 9 | up to 8 characters |

A none binding is all zeros, label included.

### Key sets

A set count (1–6) at 67, then six 169-byte sets from 68. Unused slots are all zeros.

| Offset in set | Field | Size | Notes |
| --- | --- | --- | --- |
| 0 | id | 7 | `[a-z0-9]{1,6}`, unique |
| 7 | name | 13 | 1–12 characters |
| 20 | flags | 1 | bit 0: show labels; bit 1: live key boxes |
| 21 | reserved | 1 | |
| 22 | keys | 8 × 13 | bindings |
| 126 | encoder | 3 × 13 | clockwise, anticlockwise, push |
| 165 | reserved | 4 | |

Bindings past the key count, and encoder bindings while no encoder is fitted, are validated and kept.

### Tools

| Offset | Field | Size | Notes |
| --- | --- | --- | --- |
| 1082 | tool count | 1 | 0–4 |
| 1083 | kinds | 8 | menu order, unique; 0 past the count |
| 1091 | params | 8 × 8 × int32 | slot = kind − 1 |
| 1347 | reserved | 4 | |

Slots 0–3 always hold valid params, shown or not, so hiding and showing a tool keeps its settings. Slots 4–7 and words past a kind's param count are 0. Each slot must be unchanged by its tool's clamp (for example, a max below its min is refused).

### State

| Offset | Field | Size |
| --- | --- | --- |
| 1351 | key set shown at start-up | 1 |
| 1352 | reserved | 3 |

## Ids and codes

**Pin codes:** 0–10 = D2–D12, 11 = A0, 12 = A1, 13 = A3, 14 = A6, 15 = A7, 0xFF = none. Other pins (A2, D0, D1, A4, A5, D13, the LED and boot pins) have no code.

**Button roles:** 0 none, 1 Kill, 2 Nav, 3 Bind.

**Consumer codes:** 0 volume up, 1 volume down, 2 mute, 3 play/pause, 4 next, 5 previous, 6 stop.

**Tool kinds and params** (param order is append-only):

| Kind | Id | Params |
| --- | --- | --- |
| 1 | `click` | `min` ms (20–5000), `max` ms (20–5000), `btn` (Left, Right, Middle) |
| 2 | `jitter` | `dist` px (10–200), `min` ms (100–60000), `max` ms (100–60000) |
| 3 | `spam` | `key` (a named key), `min` ms (20–10000), `max` ms (20–10000), `hold` ms (10–500) |
| 4 | `hold` | `btn` (Left, Right, Both) |

**Error codes:**

| Code | Name | Code | Name |
| --- | --- | --- | --- |
| 1 | `E_MAGIC` | 20 | `E_CLAMP` |
| 2 | `E_VERSION` | 30 | `E_DUP_PIN` |
| 3 | `E_LENGTH` | 31 | `E_DUP_SET` |
| 4 | `E_CRC` | 32 | `E_DUP_TOOL` |
| 5 | `E_RESERVED` | 33 | `E_TOGGLE` (toggles a hidden tool) |
| 6 | `E_FLAGS` | 34 | `E_SELECTED` |
| 7 | `E_STRING` | 35 | `E_NO_NAV` |
| 8 | `E_ID` | 36 | `E_TWO_NAV` |
| 9 | `E_COUNT` | 37 | `E_TWO_KILL` |
| 10 | `E_PIN` | 38 | `E_CONTROLS` (fewer than 4 keys and no encoder push) |
| 11 | `E_ROLE` | 100 | `E_LINE` (too long) |
| 12 | `E_BINDING` | 101 | `E_MODE` (unknown here) |
| 13 | `E_ENCODER` | 102 | `E_ARGS` |
| 14 | `E_DEBOUNCE` | 103 | `E_RANGE` |
| 15 | `E_TOOLKIND` | 104 | `E_HEX` |
| 16 | `E_PARAM` | 105 | `E_WRITE` (flash write failed) |
| 17 | `E_UNUSED` | 106 | `E_VERIFY` (read-back differs) |

## Validation

One validator runs at boot, on `CHECK` and on `COMMIT`, and the first failure wins: single-field rules in byte order, then the clamp rule, then the cross-field rules (duplicate pins, duplicate set ids, duplicate tool kinds, toggle targets, the start-up set, controls). The offset is the failing field's byte; a duplicate reports the later one. `test/vectors/` holds valid records and an invalid table (`name, hex, code, offset`) that both the firmware and the client tests check.

## Boot

| Situation | Mode | Runs | Pad-side saves | `HELLO` cfg |
| --- | --- | --- | --- | --- |
| A key pin held for 1 s at power-up | normal | the default | off | `default safe` |
| Reset by a crash or watchdog | normal | the default | off | `default crash` |
| Stored record invalid | recovery | the default, keys gated | off | `invalid <code>` |
| Nothing stored | normal | the default | on | `none` |
| Valid record | normal | the record | on | `ok` |

A stored record that exists but can't be read back counts as invalid (recovery), never as absent. An NVS error while looking the record up counts the same way. A record the flash layer itself dropped (one whose own checksum failed, say) can't be told from none, so that pad boots with no stored record. The safe-boot pin is the stored layout's first key pin, plus D2 when there's no valid record or D2 is in use. A stored record longer than 1355 bytes (from a newer firmware, say) can't be read and shows as `invalid 1`.

## Serial

USB serial (CDC) is used only in config and recovery mode. In normal mode the firmware never touches the CDC endpoints, because the core's USB driver (arduino-esp32 2.0.18) can't safely interleave CDC and HID transfers. Unread bytes wait in the host's buffers.

**The knock** asks a pad in normal mode to enter config mode: hold DTR low with RTS high for 300–1000 ms, then raise DTR with RTS still high. Opening a port raises both lines together, and closing it drops both, so neither is a knock. A pad already in config mode treats a DTR drop as the end of the session, so a client sends `HELLO` first and knocks only if it gets no answer.

**Config mode** gates key output, engages the killswitch if a tool is enabled, and shows "Configure" on the screen. It ends with `EXIT`, a commit, a Kill tap or Nav hold on the pad, USB loss, or, once a client has sent a valid command, a DTR drop or 60 s without a command. Entered by a knock, it also ends if no valid command arrives within 10 s.

**Recovery mode** (an invalid stored record) stays until a commit or a cancel on the pad; session breaks only reset staging to the default.

## Protocol v1

Lines are tab-separated, end in `\n` (a trailing `\r` is ignored) and are at most 160 bytes. Numbers are decimal; hex is lowercase on output and either case on input. One request at a time: every reply ends with `OK[\t...]` or `ERR\t<code>\t<offset>\t<NAME>: <text>`. Lines starting with `# ` are logs; a client ignores lines it doesn't know.

| Command | Reply |
| --- | --- |
| `HELLO` | `OK\tmacropad\t<fw>\tproto 1\tmode <config\|recovery>\tcfg <status>\tcrc <8 hex\|none>\tboot <8 hex>\tsn <USB serial>` |
| `PINS` | `PIN\t<code>\t<name>` per pin, then `OK` |
| `TOOLS` | per kind `TOOL\t<kind>\t<id>\t<name>`, then per param `PARAM\t<kind>\t<index>\t<id>\t<label>\t<number\|choice\|key>\t<min>\t<max>\t<step>\t<unit>\t<names>`, then `OK` |
| `GET\t<offset>\t<len>` | `DATA\t<hex>` (len ≤ 64) from staging, then `OK` |
| `PUT\t<offset>\t<hex>` | writes ≤ 64 bytes into staging, `OK` |
| `CHECK` | validates staging: `OK` or `ERR` |
| `COMMIT` | validates, writes, reads back, `OK\t<crc>`, then restarts |
| `EXIT` | `OK`, then back to normal mode (not in recovery) |

Staging starts as the stored record (or the running setup if none validates; the default in recovery); edits made on the pad and not yet saved are saved first. `boot` is a random number picked at each boot, so a changed value proves a restart. `crc` is the stored record's, kept current when the pad saves its own edits, and `cfg` changes from `none` to `ok` when the first save creates the record. `cfg ok` with `crc none` (`cfg ok default` on a safe or crash boot) means the pad couldn't read its stored record back (a flash fault: after `E_WRITE` or `E_VERIFY` on a commit or on the entry flush of pad-side edits, the read-back finds the record unreadable, or a record verified before gone; or at config entry a record verified before is unreadable or gone); the client then writes nothing, on this connection or the next. Once reported, the fault stands until the pad restarts, even if a later read works again. The repair is a restart: at boot an unreadable record counts as invalid, so the pad comes up in recovery (or with no stored record, if the flash layer dropped it), where a Save replaces it; or the record reads again and the pad runs it, without the pad-side edits made since its last save. The client rebases the editor onto the setup the faulted Connect read (the running setup, with those edits) and keeps it as a draft through every later Connect to that pad without a crc (another faulted Connect, a recovery boot), until a verified Connect shows it or a Save stores it (Revert drops it, except when it goes back to the running setup a faulted Connect read, which is the draft). On a safe or crash boot the pad reports the fault as `cfg ok default` (it runs the compiled default), which the client refuses the same way but doesn't rebase onto: the editor keeps its setup. `pull` refuses a pad without a crc, so a fallback never replaces a backup. On the same pad, the client keeps the editor's setup when the pad has no usable one (`HELLO` without a crc: recovery, nothing stored, unreadable, or a safe or crash boot on any of those), so that Save restores it. The pad reports this whatever its boot row (a fresh pad, `default safe` or `default crash` too, once something is stored), and it stops saving its own edits until it restarts.

**Self-test builds only:** `TESTCORRUPT` stores a record with a bad CRC and restarts; `TESTCRASH` aborts; `TESTDOUBLETAP` leaves the core's double-tap words in PSRAM and restarts (the firmware must clear them and boot normally), and `TESTDOUBLETAP\tcleared` answers `OK\tcleared` only if this boot cleared them (else `ERR\t101`; any other argument gives `ERR\t102`); `TESTRUN` leaves config mode and runs the self-test again (config mode only, like `TESTTAP` and `TESTACK`); `TESTTAP` taps Nav once the pad is back in normal mode (an unsaved pad edit, for testing the save on entry); `TESTACK` confirms the self-test report was read (until then it's re-sent at every config entry); `TESTERASE` removes the stored record and restarts (a fresh pad; `TESTERASE\tnorun` also keeps the self-test from starting on that boot).

### Saving, as the client does it

1. `HELLO` (knock first if there's no answer), then `PUT` the record in 64-byte chunks and `CHECK`.
2. `COMMIT`. An `ERR` ends the attempt; it saved nothing, except `E_WRITE` and `E_VERIFY`, after which the record may be in flash (see below).
3. Close the port, then every 250 ms for up to 12 s look for the same USB serial number, open it, knock and `HELLO`.
4. Success needs a changed `boot`, mode config, cfg `ok` and the crc of the record sent. Then `EXIT`.

Before every Check or Save, the client also confirms it's still talking to the same session:
- any reported crc must be the one read at Connect, or the crc of any Save whose outcome is still open (one that reached `COMMIT` and whose reboot check timed out, or got `E_WRITE` or `E_VERIFY`, may still have landed); anything else was edited on the pad (a fresh pad whose save on entry failed reports its edits' crc while still `cfg none`), and the client refuses and asks to connect again;
- a restart in between (a changed `boot`) is accepted with such a known record, or when there was no stored record then or now (`crc none`) and `cfg` is what it was at Connect (a fresh pad, or one in recovery);
- the pad is opened and reopened only by its USB serial number, never by port name, and the `sn` in its `HELLO` must match before anything is written (a port name can change hands between finding it and opening it); a port without a serial number can be read and checked, but Save refuses it (a knock that reaches the wrong pad still puts it in config mode before `HELLO` identifies it: it engages its killswitch if a tool was enabled and saves its own pending edits; the client then sends `EXIT` and writes nothing);
- a COMMIT refused with `ERR` didn't land, except `E_WRITE` and `E_VERIFY`: the NVS library reports any error as a failed write, including one after the new record was written, so both are open (the pad first reads flash once more, and if it holds exactly the record sent, the save ends the normal way); one whose outcome was unclear counts as landed only once a later `HELLO` shows its crc, and the app then marks that record saved. After a restart, a `HELLO` showing the Connect crc, or still no stored record, settles it as not landed. Every such open Save is kept as a candidate, so a later retry can't hide an earlier one that landed. Otherwise `HELLO`'s crc comes from that second read (in recovery it stays `none`). At every entry to config mode that stages the stored record, `HELLO` reports its crc, and the client checks at Connect that the record it read matches `HELLO`'s crc. Only a Save that reached `COMMIT` and stayed open is a candidate; one that failed before `COMMIT` isn't.

After such a refusal, connecting again to the same pad (same USB serial number) keeps the editor's unsaved edits rebased onto the pad's new setup, field by field (each tool param, string and binding is one field; a set's flags merge bit by bit): a field the editor changed keeps the editor's value, every other field takes the pad's. Key sets merge by identity (their id), never by slot: if only one side added, removed or reordered sets, that side's list wins and every set both sides have merges field by field; if both did, the editor's list wins (named, unless both made the same change) and every set both still have merges field by field. A set one side deleted and the other edited stays deleted; if the pad's side deleted it, the app names the set and says the editor's edits to it were dropped. If the start-up set is gone after the merge, the pad's choice is kept if it survives, else the editor's earlier one, else the first set, and the app says where the pad will start. A record with duplicate set ids (a draft mid-rename) isn't merged: the editor keeps its draft. The start-up set follows its set (the editor's choice if it made one, else the pad's). Where both changed the same field, the app names it, and its value wins on Save. Edits never carry over to a different pad; the app says when it drops them.

If saving the pad's own edits fails when config mode starts, the pad reads flash back: if the write landed, it counts; if flash still holds the old record or none, `HELLO` reports the crc of the staged record (the edits), so a client that read the old record is refused rather than overwriting them; if the record is unreadable, or a record verified before is gone, it's a flash fault (`cfg ok`, `crc none`).
