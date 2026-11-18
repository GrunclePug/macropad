# macropad

A handheld USB macropad for game automation, rhythm games and stream-deck shortcuts.

## Overview

macropad is firmware for a small USB HID pad built on an Arduino Nano ESP32, with mechanical keys, two small buttons and an SSD1306 OLED. It runs automation tools (autoclicker, mouse jitter, key spam, mouse hold) in the background while the keys play osu!mania or act as a stream deck, and a global killswitch stops all output at once. It needs no software on the host: it shows up as a standard keyboard, mouse and media-key device.

## Features

* **Key layouts**: an osu!mania layout (D, F, J, K) and a stream-deck layout (F13 and up, tool toggles, encoder volume).
* **Autoclicker**: clicks a mouse button at a random interval between a minimum and a maximum.
* **Jitter**: moves the pointer out and back, then pauses for a random time.
* **Key spam**: presses a chosen key at a random interval.
* **Mouse hold**: holds a mouse button down.
* **Background tools**: tools keep running while you play or use the deck.
* **Global killswitch**: one press stops every key, click and movement; the menu and settings stay usable.
* **Arming**: switch tools on while the killswitch is on, then release the killswitch to start them together.
* **USB safety**: when the computer sleeps or the link drops, the killswitch turns on and armed tools wait for a tap.
* **Settings on the device**: each tool's settings are edited on the OLED and kept across power cycles.
* **N-key rollover**: every key can be held at once; chords go out in a single report.
* **Low latency**: input is scanned tens of thousands of times a second and USB reports never wait on the screen.
* **Build variants**: one codebase builds different pads from small tables of pins, layouts and tools.
* **Tests**: native unit tests for the logic, and an on-device self-test that drives the real input pins.
* **Pinned build**: a `sketch.yaml` profile pins the board core and libraries, so every machine builds the same firmware.

## Installation

### Requirements

* [arduino-cli](https://arduino.github.io/arduino-cli/) 1.5 or later, and Make
* g++ for the native tests
* Arduino Nano ESP32
* 128×64 SSD1306 OLED on I2C (address `0x3C`)
* 4 to 8 key switches and 2 small push buttons; a rotary encoder with a push button is optional

The profile installs the `arduino:esp32` core and the Adafruit GFX, BusIO and SSD1306 libraries on the first build.

### Build

```sh
git clone https://github.com/GrunclePug/macropad.git
cd macropad
make
make upload PORT=/dev/ttyACM0
```

## Usage

Plug the pad into a computer. It starts on the osu!mania layout with every tool off.

| Key | Action |
| --- | --- |
| Killswitch, press | Stop all output (red LED) |
| Killswitch, tap while on | Resume; tools that were on start again |
| Killswitch, hold | Switch every tool off |
| Nav, hold | Open the menu; in the menu, go back |
| Nav, tap | On a tool's screen, switch the tool on or off; in the menu, open the entry |
| Keys 1 and 2 (menu) | Move up and down |
| Keys 3 and 4 (menu) | Open the entry, or go back |
| Keys 1 and 2 (tool screen) | Select a setting |
| Keys 3 and 4 (tool screen) | Decrease or increase it (hold to repeat) |
| Encoder (when fitted) | Scroll and adjust; push to confirm; volume on the deck |

A key held through a screen change does nothing until it's released and pressed again.

### Configuration

| Variable | Flag | Default | Description |
| --- | --- | --- | --- |
| `VARIANT` | `make VARIANT=...` | `pad` | Pad to build: `pad` or `deck` |
| `PORT` | `make upload PORT=...` | none | The board's serial device; use the real device (`/dev/ttyACM0`), not a `/dev/serial/by-id/` link |
| `SELFTEST` | `make SELFTEST=1 ...` | unset | Build the on-device self-test instead of the normal firmware |

## Variants

A variant is a pin list (`src/variants/<name>_pins.h`) plus tables of layouts, tools and button roles (`src/variants/<name>.h`).

* **pad**: 4 keys, killswitch and nav buttons, the osu!mania and deck layouts, and all four tools.
* **deck**: 8 keys, an encoder for volume, a bindable small button and no tools. Its bindings are placeholders for a future stream deck.

## Wiring

Every input uses the Nano's internal pull-up and switches to GND. No other parts are needed.

| Pin | pad (breadboard) | Planned 8-key build |
| --- | --- | --- |
| D2–D5 | Keys 1–4 | Keys 1–4 |
| D6, D7 | Killswitch, nav | Keys 5–6 |
| D8, D9 | | Keys 7–8 |
| D10, D11 | | Killswitch (or bound button), nav |
| D12, A0, A1 | | Encoder A, B and push |
| A4 (SDA), A5 (SCL) | OLED | OLED |
| 3V3, GND | OLED power | OLED power |

Leave A2 (a boot strapping pin), D0 and D1 free. Keep the Nano's reset button reachable in a case: a double tap on it is the recovery path.

## Self-test

`make SELFTEST=1 upload PORT=/dev/ttyACM0` flashes a build that drives the real input pins low itself, the way the switches do, and reports over serial. It runs:

* the arming workflow
* 200 osu presses and 20 chords while jitter and the autoclicker run, with the screen redrawing
* the killswitch with a held key and during clicks
* mouse hold, key spam and saving settings
* a USB disconnect and reconnect

It only sends F13 to F17 and mouse button 8, which nothing binds by default. Flash the normal build afterwards.

## Development

* Build: `make` (or `make VARIANT=deck`)
* Test: `make test`
* Flash: `make upload PORT=/dev/ttyACM0`
* Self-test: `make SELFTEST=1 upload PORT=/dev/ttyACM0`, then read the serial output
* Clean: `make clean`

## Contributing

Fork the repository and open a pull request.

## License

MIT. See [LICENSE](LICENSE).

## Author

Chad Humphries | [Website](https://grunclepug.com/) | [GitHub](https://github.com/GrunclePug) | [More projects](https://github.com/GrunclePug?tab=repositories)
