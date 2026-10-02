# RF POWERMETER SCRIPTS

PowerShell tools for the Chinese **USB RF Power Meter V5** (STM32 + AD8317/AD8318,
enumerates as `STMicroelectronics Virtual COM Port`, `USB\VID_0483&PID_5740`),
including a WinForms GUI that replicates the vendor `USB-RF-Power-Meter-V5` app.

## Qt app (Windows + Linux)

[`qt/`](qt/README.md) holds a C++/Qt rewrite of the GUI: live readout, rolling monitor chart,
oscilloscope-style sweep view with markers and trigger, CSV logging, and a demo mode with a
simulated meter. Build it with `qt\build.ps1` (Windows) or `qt/build.sh` (Linux).

## Files

| File | Content |
|---|---|
| `Start-PowerMeterGui.ps1` | Launches the GUI (relaunches itself in STA if needed) |
| `PowerMeterGui.cmd` | Double-click launcher (Windows PowerShell 5.1, console hidden) |
| `powermeter.ps1` | Device helpers: `Invoke-ReadPowerMeter`, `Get-UsbDeviceFromInfo`, `Invoke-DisconnectDevice` ... |
| `gui_scripts/PowerMeter.Protocol.ps1` | Stream parser, command builder, timebase table, unit formatting |
| `gui_scripts/PowerMeter.Serial.ps1` | Port discovery, open/close/read/send, `Read-PowerMeterSweep` |
| `gui_scripts/PowerMeter.Gui.ps1` | `Show-PowerMeterGui` |

## GUI

```powershell
.\Start-PowerMeterGui.ps1                 # last used port, or the first STM32 VCP found
.\Start-PowerMeterGui.ps1 -Port COM7 -Connect
```

| Control | What it does |
|---|---|
| Power (W) / Power (dBm) | Mean of the latest 500-sample sweep |
| MAX / MIN | Extremes of every sample since CLEAR / RESET |
| Marker1 / Marker2 | Drag them on the trace. Shows the time and dBm at each marker, plus T2-T1 and 1/(T2-T1) |
| STOP / RUN | Freezes the trace (the readouts stay live) |
| CLEAR | Clears the trace and MAX / MIN |
| RESET | Resets scale, position, trigger, markers and MAX / MIN |
| red label | Current timebase, e.g. `1ms - 500kSa/s` |
| Time-dev (bottom scrollbar) | Timebase from 100 us to 8.19 s. Sends `K01`..`K18` to the meter |
| Scale / V-Position | dB per division / dBm at the top of the screen |
| Trig (left scrollbar + label) | Trigger level. Click the label to cycle Auto / Normal / Single (rising edge) |
| Connect / Refresh | Open the port / rescan ports. Right-click the port box to power-cycle the USB device (admin) |
| Freq (MHz) / Offset (dB) + Send | `A<freq><±##.#>`: selects the band calibration and the external attenuation |
| Read | `Read`: the reply `R1000+00.0` fills the fields |
| Data_Display | Command log: `S-` = sent, `R-` = received, `E-` = error |
| Waveform data | The last raw record, e.g. `-63300000u` |
| Data_Export | Saves the displayed sweep to CSV (`Index,Time_s,dBm,Watt`) |

Settings (port, frequency, timebase, scale, markers ...) are saved in
`%LOCALAPPDATA%\rf.powermeter\gui.settings.json`.

Windows PowerShell 5.1 is recommended. The GUI also runs on PowerShell 7, but there
it uses about 4x more CPU at the full 3300 records/s stream rate (about 46% of a core
vs 11%).

## Command line

```powershell
. .\gui_scripts\PowerMeter.Protocol.ps1
. .\gui_scripts\PowerMeter.Serial.ps1

Get-PowerMeterPort
Read-PowerMeterSweep -Port COM7 -Count 3 -SampleRate 1 | Select-Object Time, Samples, Avg, Max, Min

$sp = Open-PowerMeterPort COM7
Send-PowerMeterCommand $sp (New-PowerMeterCommand -Frequency 2400 -Offset 30)   # 30 dB pad
Send-PowerMeterCommand $sp (New-PowerMeterCommand -Read)
Close-PowerMeterPort $sp
```

```powershell
Invoke-ReadPowerMeter -Port COM7 -Size 20
```

## Protocol summary

Verified on the hardware (COM7) and consistent with
[LostInNovo/rf-power-meter-v5-companion PROTOCOL.md](https://github.com/LostInNovo/rf-power-meter-v5-companion/blob/main/PROTOCOL.md).

- Each record is 10 ASCII bytes: `[+|-]DDd DDDdd <unit>`. `-63300000u` = -63.3 dBm and
  000.00 uW. The unit char (`u` uW, `m` mW, `w` W) is the record terminator, so a parser
  must split on all three, not only on `u`.
- The dBm value is already calibrated by the firmware, with the frequency band and
  offset applied.
- `Aa` marks the end of a 500-sample sweep taken at the K sample period. At K01 that is
  2 us/sample, a 1 ms sweep, about 6 sweeps/s and about 3300 records/s.
- Commands are CRLF terminated, one per write:
  - `Read` gets the reply `R<freq:4><±##.#>`, injected between the `A` and `a` of the
    next block marker.
  - `A<freq:4><±##.#>` sets frequency and offset.
  - `K01`..`K18` sets the sample rate. It is write-only: the meter never reports it.
- Never send the short form `A<freq:4>` without the offset. It corrupts the meter state
  (the stream pegs at -99.9 dBm) until a full `A` command or a power cycle.
  `New-PowerMeterCommand` and `Send-PowerMeterCommand` refuse to emit it.
- All settings are RAM-only. After a replug the meter is back at 1 MHz / +00.0.
