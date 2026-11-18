# macropad

A handheld USB macropad for game automation, rhythm games and stream-deck shortcuts.

## Overview

macropad is firmware for a small USB HID pad built on an Arduino Nano ESP32, with mechanical keys, two small buttons and an SSD1306 OLED. It runs automation tools (autoclicker, mouse jitter, mouse hold) next to plain key layouts, with a global killswitch that stops all output. It needs no software on the host: it shows up as a standard keyboard and mouse.

The firmware is being rewritten. This README describes the current firmware, `v0.1.0`.

## Features

* **Killswitch**: pauses every script at once; a long press switches them all off.
* **Script menu**: pick a script on the OLED and toggle it with the nav button.
* **Gamepad**: the four keys send D, F, J and K.
* **Autoclicker**: clicks at a random interval between an adjustable minimum and maximum.
* **Jitter**: moves the mouse back and forth at an adjustable interval.
* **Mouse hold**: holds the left or right mouse button down.
* **Concurrent scripts**: scripts don't block, and several can run at once.
* **Pinned build**: a `sketch.yaml` profile pins the board core and libraries, so every machine builds the same firmware.

## Installation

### Requirements

* [arduino-cli](https://arduino.github.io/arduino-cli/) 1.5 or later, and Make
* Arduino Nano ESP32
* 128×64 SSD1306 OLED on I2C (address `0x3C`)
* 4 key switches and 2 small push buttons

The profile installs the `arduino:esp32` core and the Adafruit GFX, BusIO and SSD1306 libraries on the first build.

### Build

```sh
git clone https://github.com/GrunclePug/macropad.git
cd macropad
make
make upload PORT=/dev/ttyACM0
```

## Usage

Plug the pad into a computer. It starts with every script off.

| Key | Action |
| --- | --- |
| Killswitch, tap | Pause or resume all scripts |
| Killswitch, hold | Switch every script off |
| Nav, hold | Open the menu, or confirm the selected script |
| Nav, tap (menu) | Select the next script |
| Nav, tap | Toggle the selected script |
| Keys 1–4 | Script controls: gamepad keys, or delay down and up for the autoclicker and jitter, or left and right hold for mouse hold |

### Configuration

| Variable | Flag | Default | Description |
| --- | --- | --- | --- |
| `VARIANT` | `make VARIANT=...` | `pad` | Hardware variant to build |
| `PORT` | `make upload PORT=...` | none | The board's serial device; use the real device (`/dev/ttyACM0`), not a `/dev/serial/by-id/` link |

## Wiring

Every input uses the Nano's internal pull-up and switches to GND. No other parts are needed.

| Pin | Connects to |
| --- | --- |
| D2, D3, D4, D5 | Keys 1 to 4 |
| D6 | Killswitch |
| D7 | Nav button |
| A4 (SDA), A5 (SCL) | OLED |
| 3V3, GND | OLED power |

## Development

* Build: `make`
* Build one variant: `make VARIANT=pad`
* Flash: `make upload PORT=/dev/ttyACM0`
* Clean: `make clean`

## Contributing

Fork the repository and open a pull request.

## License

MIT. See [LICENSE](LICENSE).

## Author

Chad Humphries | [Website](https://grunclepug.com/) | [GitHub](https://github.com/GrunclePug) | [More projects](https://github.com/GrunclePug?tab=repositories)
