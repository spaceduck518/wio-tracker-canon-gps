# Wio Tracker → Canon GPS

Turn a **Seeed Studio Wio Tracker L1 Pro** into a pocket-sized Bluetooth GPS for **Canon EOS
cameras**. The camera believes it is connected to a smartphone running *Canon Camera Connect*
and stores the tracker's position (latitude, longitude, altitude, UTC time) in the EXIF data
of every photo – without a phone, without an app, without draining your phone battery.

Switch it on, put it in your pocket, done.

> Tested with: **Wio Tracker L1 Pro** + **Canon EOS R6 Mark II**.
> Other Canon cameras that support "location information from a smartphone" via Bluetooth
> will very likely work too (the protocol is the same as in the
> [furble](https://github.com/gkoh/furble) project), but they have not been tested here.
> Reports welcome!

---

## Contents

1. [Features](#features)
2. [How it works](#how-it-works)
3. [What you need](#what-you-need)
4. [Quick start (prebuilt firmware)](#quick-start-prebuilt-firmware)
5. [Pair the camera](#pair-the-camera)
6. [Using the device](#using-the-device)
7. [Building from source](#building-from-source)
8. [Troubleshooting](#troubleshooting)
9. [Technical details](#technical-details)
10. [Known limitations](#known-limitations)
11. [Credits, license, disclaimer](#credits-license-disclaimer)

---

## Features

* Sends GPS position to the camera every 5 s (2 / 5 / 10 / 30 s selectable).
* Uses the tracker's built-in **Quectel L76K** GNSS receiver (GPS + BeiDou + GLONASS).
* **Only sends with a valid fix** – the camera never receives a stale or invented position.
* Automatic reconnect after camera power cycles, camera sleep and tracker reboots
  (typically 5 s).
* **Leaves a switched-off camera alone**: detects "camera off" from its Bluetooth advertising
  rate and does not connect – otherwise every connection attempt wakes the camera
  (you would hear the shutter click every few seconds).
* 1.3" OLED with four pages: status, GPS details, device info, settings.
* Coordinates shown as `N 48° 05' 07.2"`, local time (CET/CEST, automatic DST).
* Joystick menu (language EN/DE, send interval, display timeout, key lock, delete pairing).
* Key lock (manual + automatic), battery indicator, status LED.
* Robustness: crash handler + watchdog → automatic restart, last crash location is shown
  on the device page for debugging.
* The LoRa radio of the tracker is put to sleep (not used).

## How it works

```
 ┌──────────────┐  NMEA (UART)  ┌──────────────┐   Bluetooth LE   ┌───────────────┐
 │ L76K GNSS    │ ────────────▶ │ nRF52840     │ ───────────────▶ │ Canon EOS     │
 │ (in tracker) │               │ this firmware│  "smartphone"    │ writes EXIF   │
 └──────────────┘               └──────────────┘   GPS protocol   └───────────────┘
```

The camera is paired once in its **smartphone** pairing mode. From then on the tracker connects
automatically, the camera requests location data and the tracker sends a 20-byte position
packet every few seconds. Protocol details: [docs/PROTOCOL.md](docs/PROTOCOL.md).

## What you need

| Item | Notes |
|---|---|
| **Seeed Studio Wio Tracker L1 Pro** | nRF52840 + L76K GNSS + 1.3" OLED + joystick + battery + case. The other L1 variants are untested (see below). |
| USB-C cable | for flashing and charging |
| Canon EOS camera with Bluetooth and smartphone GPS support | tested: EOS R6 Mark II |
| Computer (Linux, macOS or Windows) | only for flashing |

**Other L1 variants:** the plain *Wio Tracker L1* uses the same board and should work.
The *L1 Lite* has no display (the firmware runs without one but you lose the UI).
The *L1 E-Ink* has a different display and is **not** supported.

> ⚠️ Flashing this firmware **replaces the Meshtastic firmware** the tracker ships with.
> The bootloader is not touched – you can go back to Meshtastic any time
> (see [Going back to Meshtastic](#going-back-to-meshtastic)).

---

## Quick start (prebuilt firmware)

No compiler needed – the tracker has a drag-and-drop (UF2) bootloader.

1. **Download** [`prebuilt/CanonGPS_L1.uf2`](prebuilt/CanonGPS_L1.uf2) from this repository.
2. Connect the tracker to your computer with USB-C.
3. **Double-press the reset button quickly.** A USB drive named **`TRACKER L1`** appears
   (the LED next to the power switch lights up).
4. **Optional but recommended – back up the original firmware:** copy `CURRENT.UF2` from the
   drive to your computer. Copying it back later restores Meshtastic exactly as it was.
5. **Check the bootloader:** open `INFO_UF2.TXT` on the drive. It must say
   `SoftDevice: S140 7.3.0`. (If it shows something else, do not flash – open an issue.)
6. **Copy `CanonGPS_L1.uf2` onto the `TRACKER L1` drive.** The drive disappears, the tracker
   reboots into the new firmware and the display shows *Searching camera...* within a second.
   (Your OS may complain that the drive was removed unexpectedly – that is normal.)

Continue with [pairing the camera](#pair-the-camera).

---

## Pair the camera

Pairing is needed **once**. The tracker remembers the camera (and the camera remembers the
tracker) across power cycles.

1. On the camera, enable Bluetooth and start pairing **as a smartphone** – *not* as a wireless
   remote. On the EOS R6 Mark II:
   `Network menu → Bluetooth settings → Bluetooth: Enable`, then
   `Connect to smartphone (tablet) → Add a device to connect to`.
   When the camera shows a QR code / asks you to install Camera Connect, choose
   **"Do not display"** / skip, so the camera waits for a Bluetooth connection.
2. Switch on the tracker (or reset it). It finds the camera, connects and the display shows
   **`Confirm on camera!`** (the LED blinks fast).
3. Confirm the pairing request for **"Wio GPS"** on the camera.
4. In the camera's GPS settings select the **smartphone** as the GPS source:
   `Network menu → GPS settings → GPS device settings → Smartphone`.
5. With a GNSS fix (outdoors / at a window) the display shows `GPS -> camera` and the camera
   shows the GPS icon. Take a photo and check the GPS info page in playback – done.

If an old entry of the tracker (or another device) already exists on the camera, delete it
first. If the tracker reports **`Remote mode!`**, the camera was paired in remote-control mode –
delete that pairing on the camera and start again with step 1.

To pair a different camera, use **Settings → Delete pairing** on the tracker, then pair again.

---

## Using the device

### Display pages

Move the joystick **left/right** to switch pages (the dots in the top line show which one).

**1 – Status**
```
Canon GPS        • ▪ • •  87%
GPS -> camera
EOSR6m2_XXXXXX
Sat 9/14  18:35:12
N 48° 05' 07.2"
E 10° 51' 42.1"
```
* Line 2: state – `Searching camera...`, `Camera off`, `Camera on, waiting...`,
  `Connecting...`, `Confirm on camera!`, `Connected` (no GPS request yet),
  `GPS -> camera` (sending), `Remote mode!`.
* `Sat 9/14` = satellites **used / in view**. `0/0` = antenna covered;
  `0/9` = satellites visible but no fix yet (wait).
* Time is **local time** (CET/CEST). The camera always receives UTC.

**2 – GPS:** satellites, HDOP, altitude, speed and course, age of the fix.

**3 – Device:** battery voltage, number of packets sent, uptime, Bluetooth address of the paired
camera, and either the device name or – after an automatic restart – the last crash/watchdog
address (see [Troubleshooting](#crash-or-watchdog-message-on-the-device-page)).

**4 – Settings:** move **up/down** to select, **press** the joystick to change:

| Setting | Values | Default |
|---|---|---|
| Language | English / Deutsch | English |
| Interval | 2 / 5 / 10 / 30 s (how often a position is sent) | 5 s |
| Display | 15 / 30 / 60 / 120 s / always on | 60 s |
| Key lock | Auto / Off | Auto |
| Delete pairing | asks for confirmation: press = yes, any other direction = no | – |

Settings are stored in flash and survive power-off.

The display switches off after the configured time. **The first joystick movement or button press
only switches it back on** – nothing else happens, so nothing changes by accident in your pocket.

### Small button (left of the joystick) = key lock

| Action | Unlocked | Locked |
|---|---|---|
| short press | **lock** (display shows *Locked*, padlock icon in the top line) | display on + hint *Locked: button 2 s* |
| hold 2 s | – | **unlock** |

With *Key lock: Auto* the keys also lock automatically 30 s after the last input. The joystick
never unlocks. GPS and Bluetooth keep running while locked.

### Status LED (next to the power switch)

| Pattern | Meaning |
|---|---|
| short blink every second | searching for the camera |
| fast blinking | confirm the pairing on the camera |
| one flash every 5 s | connected, sending GPS |
| two flashes every 5 s | connected, but no GPS fix yet |

### First fix

After the tracker has been off for hours/days, or when started indoors, the GNSS receiver needs
**1–3 minutes** under open sky for the first fix (cold start). Switch the tracker on a few
minutes before you start shooting, or don't put it in your pocket right away. Photos taken
without a fix simply have no GPS data – never wrong data.

---

## Building from source

Only needed if you want to change the firmware. Tested with Arduino IDE 2.3 / arduino-cli 1.5
on Linux.

### 1. Arduino IDE and the Seeed nRF52 package

1. Install the [Arduino IDE 2](https://www.arduino.cc/en/software).
2. `File → Preferences → Additional boards manager URLs`, add
   `https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json`
3. `Tools → Board → Boards Manager`, search **"Seeed nRF52 Boards"**, install version **1.1.13**
   (this is the version the board definition was made for; it uses SoftDevice S140 7.3.0, which
   matches the tracker's bootloader).

### 2. Libraries

`Tools → Manage Libraries`, install:
* **U8g2** (by olikraus) – display
* **TinyGPSPlus** (by Mikal Hart) – NMEA parser

Bluefruit (BLE), LittleFS, Wire and SPI come with the Seeed package.

### 3. adafruit-nrfutil (only for "Upload" from the IDE)

The Seeed package uses `adafruit-nrfutil` to create the DFU package, so compiling **fails** without
it. Install it so that it is in your `PATH`:

* Linux / macOS: `pipx install adafruit-nrfutil` (or `pip install --user adafruit-nrfutil`)
* Windows: `pip install adafruit-nrfutil`

### 4. Install the board definition

Seeed has no Arduino board definition for the Wio Tracker L1, so this repository brings its own
(`board/`). It reuses the core, libraries and tools of the Seeed package and only adds the L1 pin
mapping.

**Linux / macOS:**
```bash
git clone https://github.com/spaceduck518/wio-tracker-canon-gps.git
cd wio-tracker-canon-gps
tools/install-board.sh
```
The script copies `board/WioTrackerL1` into `<sketchbook>/hardware/WioTrackerL1` and creates
three symbolic links into the Seeed package (`libraries`, `tools`, `bootloader`).
Re-run it after updating the Seeed package.

**Windows (manual):**
1. Copy the folder `board\WioTrackerL1` to `Documents\Arduino\hardware\WioTrackerL1`.
2. Copy (or link with `mklink /J`) these three folders from
   `%LOCALAPPDATA%\Arduino15\packages\Seeeduino\hardware\nrf52\1.1.13\`
   into `Documents\Arduino\hardware\WioTrackerL1\nrf52\`:
   `libraries`, `tools`, `bootloader`.

Restart the IDE. Select **`Tools → Board → Seeed Wio Tracker L1 / L1 Pro (eigene Definition)`**.

### 5. Compile and flash

Open `firmware/CanonGPS_L1/CanonGPS_L1.ino`.

**Option A – UF2 (verified):**
1. `Sketch → Export Compiled Binary`, or on the command line `tools/build-uf2.sh`
   (needs `arduino-cli`), which writes `build/CanonGPS_L1.uf2`.
2. If you used the IDE export, convert the `.hex` to UF2 with the Seeed package's converter:
   ```bash
   python3 ~/.arduino15/packages/Seeeduino/hardware/nrf52/1.1.13/tools/uf2conv/uf2conv.py \
     -f 0xADA52840 -c -o CanonGPS_L1.uf2 CanonGPS_L1.ino.hex
   ```
3. Double-press reset and copy the UF2 onto the `TRACKER L1` drive (as in the quick start).

**Option B – Upload button:** select the tracker's serial port and press *Upload*. The IDE resets
the tracker into the bootloader (1200-baud touch) and uploads with `adafruit-nrfutil`. This is how
the Seeed package normally works, but on this project only Option A was used, so treat B as
untested.

**Getting into the bootloader without pressing reset:** while the firmware runs, opening the USB
serial port at **1200 baud** and closing it again restarts the tracker into the bootloader
(Linux: `stty -F /dev/ttyACM0 1200`, with no terminal program holding the port). Combined with
`tools/build-uf2.sh` and a copy to `/media/$USER/TRACKER L1/` this gives a fully command-line
build-and-flash loop.

### Serial console

USB serial, 115200 baud (any terminal; the Arduino serial monitor works). Commands:

| Key | Function |
|---|---|
| `s` | status (connection, battery, GNSS statistics, current fix) |
| `r` | disconnect and reconnect |
| `x` | delete pairing |
| `l` | toggle language |
| `h` | listen-only diagnostic: never connect, print the camera's advert rate every 10 s |
| `Z` | deliberately crash (tests the crash log) |

The log shows every pairing step, GPS indications, sent packets (hex) and detected camera on/off
changes. ⚠️ Logs contain your real position – don't post them unedited.

### Configuration in the source

At the top of `CanonGPS_L1.ino`:

| Constant | Meaning |
|---|---|
| `DEVICE_NAME` | name shown on the camera (`"Wio GPS"`) |
| `DEFAULT_LANG` | language on first start |
| `SEND_INTERVALS_S`, `DISPLAY_TIMES_S` | choices in the settings menu |
| `TZ_STD_OFFSET_S`, `TZ_EU_DST` | display time zone (default CET/CEST with EU DST rules) |
| `ADV_FAST_MIN`, `AFTER_DISC_MS` | camera on/off detection thresholds (see protocol notes) |
| `OX` | left margin of the display in pixels (the case covers the edge) |
| `OLED_SH1106` | display controller (the L1 uses an **SH1106**, not an SSD1306) |

---

## Troubleshooting

**Display shows `Remote mode!`** – the camera was paired as a wireless remote. Delete that
pairing on the camera and pair as a smartphone.

**`Confirm on camera!` but the camera shows nothing** – start smartphone pairing on the camera
first (it has to be waiting for a device).

**Stays at `Searching camera...`** – Bluetooth disabled on the camera, camera out of range, or no
pairing exists. To start over: Settings → Delete pairing on the tracker *and* delete "Wio GPS" on the
camera, then pair again.

**`Camera off` although the camera is on** – the camera is in its power-saving Bluetooth state
(e.g. auto power off). Wake it up (half-press the shutter); the tracker connects within seconds.

**`Sat 0/0`** – the GNSS antenna sees nothing (indoors, covered, upside down).
**`Sat 0/5 no fix`** – satellites visible but signals too weak or almanac missing after a cold
start. Go outside / to a window and wait 1–3 minutes.

**Photos have no GPS data** – check that the camera's GPS source is set to *Smartphone*, that the
tracker shows `GPS -> camera`, and that it had a fix when the photo was taken.

**Camera time / time zone** – the tracker cannot set the camera clock (see limitations). Set the
camera's time zone and daylight saving time correctly; the GPS time in EXIF is UTC anyway.

**The switched-off camera clicks** – this should not happen with the advert-rate detection. If it
does with your camera model, run the `h` diagnostic with the camera off and on and open an issue
with the numbers.

<a id="crash-or-watchdog-message-on-the-device-page"></a>
**Crash or watchdog message on the device page** (`Crash 0002A202` / `WDT 0003DD81`) – the
firmware hit a fault and restarted itself. Please open an issue with the line (a photo is fine),
the firmware version (printed at startup and with `s`), and what the device was doing. The serial console prints more registers on
startup. The address can be mapped to a source line with `arm-none-eabi-addr2line -e <the .elf
of that build>` – for the prebuilt firmware that is [`prebuilt/CanonGPS_L1.elf`](prebuilt/CanonGPS_L1.elf). The crash log survives automatic restarts but not power-off.

**Weird pixels at the right edge / first column cut off** – wrong display driver. The L1 needs
`OLED_SH1106 1` (default in this firmware).

<a id="going-back-to-meshtastic"></a>
### Going back to Meshtastic

Double-press reset and copy either your backed-up `CURRENT.UF2` or a current Meshtastic UF2 for
the "Seeed Wio Tracker L1" (from the Meshtastic web flasher / Seeed wiki) onto the `TRACKER L1`
drive.

---

## Technical details

### Pin mapping (Wio Tracker L1)

Taken from the Meshtastic variant `seeed_wio_tracker_L1` and verified on an L1 Pro where marked ✓.
Arduino pin numbers are those of this board definition.

| Function | Arduino pin | nRF52840 pin | Notes |
|---|---|---|---|
| GNSS UART RX (from GNSS) | D7 | P0.26 | ✓ 9600 baud |
| GNSS UART TX (to GNSS) | D6 | P0.27 | ✓ |
| GNSS wakeup | D0 | P1.09 | ✓ HIGH = running |
| OLED SDA / SCL | D14 / D15 | P0.06 / P0.05 | ✓ SH1106, I²C address **0x3D** |
| User button (small, left of joystick) | D13 | P0.08 | ✓ active low |
| Joystick up / down / left / right / press | D25–D29 | P1.04 / P0.12 / P0.11 / P1.03 / P1.05 | ✓ active low |
| User LED (next to power switch) | D11 | P1.01 | ✓ active high |
| Buzzer | D12 | P1.00 | not used |
| Battery voltage | D16 | P0.31 (AIN7) | ✓ divider ×2, enable via D30 (P0.04) HIGH |
| LoRa SX1262 CS / RST / BUSY / DIO1 / RXEN | D4 / D2 / D3 / D1 / D5 | P1.14 / P1.07 / P1.10 / P0.07 / P1.08 | only used to put the radio to sleep |
| SPI SCK / MISO / MOSI | D8 / D9 / D10 | P0.30 / P0.03 / P0.28 | LoRa |
| QSPI flash CS | D20 | P0.25 | deselected |

Because some Meshtastic comments contradict each other, the firmware still auto-detects the GNSS
UART pin order / wakeup level and the OLED address / pin order at startup and prints the result.

### Firmware structure

* **Bluetooth:** Adafruit Bluefruit (part of the Seeed core), central role, LE "Just Works" bonding.
  Bonds are stored by Bluefruit in internal flash; tracker UUID, paired camera and settings are
  stored in LittleFS (`/canongps.bin`, `/settings.bin`).
* **Camera on/off detection:** passive scan, adverts of the paired camera counted per 3 s
  (≥ 9 = on). See [docs/PROTOCOL.md](docs/PROTOCOL.md).
* **GNSS:** TinyGPSPlus; the L76K is set to GPS+BeiDou+GLONASS and GGA/GSV/RMC output.
  A fix is only used if position and time are less than 3 s old.
* **Robustness:** HardFault handler and watchdog (120 s) store the fault address in a RAM area that
  survives a soft reset (`0x2003E000`; the bootloader clears the lower RAM) and restart the chip.

### Board definition

`board/WioTrackerL1/nrf52/` is an Arduino "platform" that references the core of the Seeed package
(`build.core=Seeeduino:nRF5`). Differences from the Seeed XIAO nRF52840 definition it is based on:
L1 pin mapping (`variants/WioTrackerL1`), RC low-frequency clock (`USE_LFRC`, it is not documented
whether the L1 has a 32 kHz crystal), quoted include path in `platform.txt` (the original breaks on
sketch paths with spaces), and `-DARDUINO_Seeed_XIAO_nRF52840` so that the Seeed `Wire` library
accepts the board.

---

## Known limitations

* Tested on one camera model only (EOS R6 Mark II).
* The camera clock is **not** set by the tracker; in our test the camera's "auto time setting"
  did not take the time from the smartphone GPS data. The time zone cannot be set either.
* Display time zone is fixed in the source (CET/CEST by default).
* After the camera dropped an active connection (e.g. it was switched off), the tracker waits 45 s
  before connecting again – switching the camera off and on within that time delays the reconnect.
* Battery runtime has not been measured yet.
* Code comments are mostly in German; all user-facing text is available in English.

## Credits, license, disclaimer

* Canon BLE GPS protocol: reverse engineered by **[furble](https://github.com/gkoh/furble)**
  (Guo-Rong Koh and contributors, MIT license) – thank you!
* Pin information: **[Meshtastic](https://github.com/meshtastic/firmware)** firmware,
  variant `seeed_wio_tracker_L1`.
* Board support: **[Seeed nRF52 Arduino core](https://github.com/Seeed-Studio/Adafruit_nRF52_Arduino)**
  (fork of the Adafruit nRF52 core). `platform.txt` is a modified copy and stays under that project's
  license.
* Libraries: [U8g2](https://github.com/olikraus/u8g2), [TinyGPSPlus](https://github.com/mikalhart/TinyGPSPlus).

This project is licensed under the **MIT License** – see [LICENSE](LICENSE).

This is an independent hobby project, **not affiliated with or endorsed by Canon or Seeed Studio**.
Canon, EOS and Camera Connect are trademarks of Canon Inc. Use at your own risk; the protocol is
undocumented and may change with camera firmware updates.
