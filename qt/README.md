# RF Power Meter V5 — Assistant (Qt / C++)

Cross-platform (Windows + Linux) companion for the Chinese **USB RF Power Meter V5**
(STM32 + AD8317/AD8318, USB `0483:5740`). It merges the vendor app's oscilloscope view
with the live monitor of the C# companion, in the same "nebula purple" style.

| Tab / card | What it does |
|---|---|
| **Readout** | Big dBm value, watts, level bar, peak hold, noise-floor tare (net power above the floor), over-range warning |
| **Statistics** | Min / max / avg / sample count since reset |
| **Meter settings** | Frequency (band calibration) + attenuation with band presets, **Apply** verifies the echo; timebase K01..K18; *Re-apply on connect* restores the band after a replug (the meter forgets it) |
| **Threshold events** | Timestamp each time the level rises above a threshold (debounced, 2 dB hysteresis) |
| **CSV log** | 20 rows/s to `Documents/rfmeter/logs` |
| **Monitor** tab | Rolling level chart (10 s … 15 min), colored by strength, max trace, peak-hold and floor lines, hover readout |
| **Sweep** tab | One 500-sample sweep, scope style: markers M1/M2 (ΔT, 1/ΔT, ΔdB), trigger Auto/Normal/Single with rising/falling edge, scale/top, auto-fit, CSV/PNG export |
| **Log** tab | Every command (`S-`), reply (`R-`) and error (`E-`), plus the last raw record |

The scope is driven with the mouse: click or drag to move the nearest marker, drag the
amber **T** handle to set the trigger level, wheel to move the view, Ctrl+wheel to change
dB/div, double-click to auto-fit.

**Options ▾** holds the baud rate, DTR/RTS, auto-connect, auto-reconnect when the meter is
plugged back in, and **Demo mode**: a built-in simulated meter that speaks the real wire
protocol, so the whole UI can be tried without hardware.

## Build

Needs CMake ≥ 3.16, a C++17 compiler and Qt 6 (or Qt 5.15) with **Widgets** and **SerialPort**.

### Windows

Install Visual Studio 2022+ (Desktop C++) and Qt from the Qt online installer
(`Qt 6.x › MSVC 2022 64-bit` + `Additional Libraries › Qt Serial Port`), then:

```powershell
.\build.ps1 -Deploy                  # finds VS and Qt, builds, runs the tests, copies the Qt DLLs next to the exe
.\build.ps1 -Run -- --simulate       # build, then start in demo mode
.\build.ps1 -QtDir C:\Qt\6.8.2\msvc2022_64
```

`build.ps1` also picks up the Qt 5.15 shipped with radioconda (`C:\Programs\radioconda\Library`)
when no Qt installation is found. The result is `build\rf-powermeter-assistant.exe`.

### Linux

```bash
# Debian / Ubuntu
sudo apt install build-essential cmake qt6-base-dev libqt6serialport6-dev
# Fedora:  sudo dnf install gcc-c++ cmake qt6-qtbase-devel qt6-qtserialport-devel
# Arch:    sudo pacman -S base-devel cmake qt6-base qt6-serialport

./build.sh                # Release build + tests
./build.sh --run          # build and start
sudo cmake --install build   # optional: binary, .desktop entry and icon
```

Serial-port access: add yourself to the `dialout` group (`uucp` on Arch), or install the udev
rule, which also stops ModemManager from probing the meter (its AT commands confuse the
meter's command parser):

```bash
sudo cp packaging/linux/99-rf-powermeter.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

## Command line

```text
rf-powermeter-assistant [--port COM7|ttyACM0] [--connect] [--simulate] [--tab 0|1|2] [--size 1280x820]
```

`--screenshot file.png` saves the window after 5 s and quits (used for docs and testing).

## Keyboard

| Keys | Action |
|---|---|
| Ctrl+K | Connect / disconnect |
| F5 | Rescan ports |
| Ctrl+1 / 2 / 3 | Monitor / Sweep / Log |
| Ctrl+Space | Run / stop the sweep view |
| Ctrl+T | Single sweep |
| Ctrl+F | Auto-fit the sweep view |
| Ctrl+E | Export the sweep to CSV |
| Ctrl+S | Save the current chart as PNG |
| Ctrl+L | Start / stop the CSV log |

## Files

| File | Content |
|---|---|
| `src/MeterProtocol.*` | Plain C++ (no Qt): stream parser, safe command builders, timebase table, formatting |
| `src/MeterLink.*` | Serial port (or simulator), parsing, paced command queue with Read retry |
| `src/MeterSimulator.*` | Demo mode: produces the real byte stream, obeys Read / A / K |
| `src/TimeSeriesChart.*` | Monitor chart |
| `src/SweepScope.*` | Sweep scope with markers and trigger |
| `src/MainWindow.*` | The window, live-data pipeline, settings |
| `src/Theme.*`, `src/Widgets.*` | Palette, style sheet, glow readout, level bar, flow layout |
| `tests/test_protocol.cpp` | Parser / command / formatting tests (`ctest`) |

Settings are saved in `%APPDATA%\rf-powermeter\assistant.ini` (Windows) or
`~/.config/rf-powermeter/assistant.ini` (Linux).

## Safety notes built in

- The truncated `A<freq>` command (which pegs the meter at −99.9 dBm) can't be built or sent:
  every command passes an exact-shape check before it reaches the port.
- One command per write, ≥ 300 ms apart; a `Read` that gets no reply is retried once.
- Readings ≥ +5 dBm raise a warning: the AD8317 compresses above about −5 dBm and is damaged at about +12 dBm.
