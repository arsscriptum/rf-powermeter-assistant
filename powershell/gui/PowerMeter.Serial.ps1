#╔════════════════════════════════════════════════════════════════════════════════╗
#║                                                                                ║
#║   PowerMeter.Serial.ps1                                                        ║
#║   Serial port helpers for the USB RF Power Meter V5                            ║
#║                                                                                ║
#╟────────────────────────────────────────────────────────────────────────────────╢
#║   Guillaume Plante <codegp@icloud.com>                                         ║
#║   Code licensed under the GNU GPL v3.0. See the LICENSE file for details.      ║
#╚════════════════════════════════════════════════════════════════════════════════╝
#
#  The meter enumerates as "STMicroelectronics Virtual COM Port" (USB\VID_0483&PID_5740).
#  It is a native USB CDC device, so the baud rate is not critical. DTR/RTS are asserted
#  like the original Invoke-ReadPowerMeter does.


$script:PowerMeterUsbId = 'VID_0483&PID_5740'

function Get-PowerMeterPort {
    <#
    .SYNOPSIS
        Lists the serial ports with their friendly names; IsPowerMeter flags the meter's USB id.
    #>
    [CmdletBinding()]
    param()

    $pnp = @(Get-CimInstance -ClassName Win32_PnPEntity -Filter "Name LIKE '%(COM%'" -ErrorAction Ignore)

    foreach ($name in ([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object { [int]($_ -replace '\D', '') } -Unique)) {
        $dev = $pnp | Where-Object Name -like "*($name)" | Select-Object -First 1
        [pscustomobject]@{
            Port         = $name
            FriendlyName = if ($dev) { $dev.Name } else { $name }
            InstanceId   = if ($dev) { $dev.PNPDeviceID } else { $null }
            IsPowerMeter = [bool]($dev -and $dev.PNPDeviceID -like "*$($script:PowerMeterUsbId)*")
        }
    }
}

function Open-PowerMeterPort {
    [CmdletBinding()]
    param(
        [Parameter(Position = 0, Mandatory = $true)]
        [string]$Port,

        [Parameter(Position = 1)]
        [int]$BaudRate = 460800
    )

    $sp = [System.IO.Ports.SerialPort]::new($Port, $BaudRate, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $sp.Handshake    = [System.IO.Ports.Handshake]::None
    $sp.DtrEnable    = $true
    $sp.RtsEnable    = $true
    $sp.ReadTimeout  = 500
    $sp.WriteTimeout = 1000
    $sp.Encoding     = [System.Text.Encoding]::ASCII
    $sp.ReadBufferSize = 262144
    try {
        $sp.Open()
        $sp.DiscardInBuffer()
    } catch {
        $sp.Dispose()
        throw
    }
    return $sp
}

function Close-PowerMeterPort {
    [CmdletBinding()]
    param(
        [Parameter(Position = 0)]
        [AllowNull()]
        [System.IO.Ports.SerialPort]$SerialPort
    )
    if (-not $SerialPort) { return }
    try { if ($SerialPort.IsOpen) { $SerialPort.Close() } } catch { Write-Verbose "$_" }
    $SerialPort.Dispose()
}

function Read-PowerMeterPort {
    <#
    .SYNOPSIS
        Non-blocking read of everything currently buffered on the port.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Position = 0, Mandatory = $true)]
        [System.IO.Ports.SerialPort]$SerialPort
    )
    if ($SerialPort.BytesToRead -le 0) { return '' }
    return $SerialPort.ReadExisting()
}

function Send-PowerMeterCommand {
    <#
    .SYNOPSIS
        Writes one command (from New-PowerMeterCommand) as a single atomic write.
    .EXAMPLE
        Send-PowerMeterCommand $sp (New-PowerMeterCommand -Frequency 1000 -Offset 0)
    #>
    [CmdletBinding()]
    param(
        [Parameter(Position = 0, Mandatory = $true)]
        [System.IO.Ports.SerialPort]$SerialPort,

        [Parameter(Position = 1, Mandatory = $true)]
        [string]$Command
    )
    if (-not $Command.EndsWith("`r`n")) { $Command += "`r`n" }
    if ($Command -match '^A\d{4}\r\n$') { throw "Refusing to send short A command (corrupts the meter offset)" }
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($Command)
    $SerialPort.Write($bytes, 0, $bytes.Length)
}

function Read-PowerMeterSweep {
    <#
    .SYNOPSIS
        Command-line reader: returns complete 500-sample sweeps as objects (no GUI needed).
    .EXAMPLE
        Read-PowerMeterSweep -Port COM7 -Count 3 -SampleRate 1 | Select-Object Time, Samples, Avg, Max, Min
    #>
    [CmdletBinding()]
    param(
        [Parameter(Position = 0)]
        [string]$Port = 'COM7',

        [Parameter(Position = 1)]
        [int]$Count = 1,

        [ValidateRange(1, 18)]
        [int]$SampleRate,

        [int]$TimeoutSeconds = 20
    )

    $sp = Open-PowerMeterPort -Port $Port
    try {
        if ($PSBoundParameters.ContainsKey('SampleRate')) {
            Send-PowerMeterCommand $sp (New-PowerMeterCommand -SampleRate $SampleRate)
        }
        $period = if ($SampleRate) { Get-PowerMeterSamplePeriod $SampleRate } else { [double]::NaN }
        $parser = New-PowerMeterParser
        $first  = $true      # the first block after opening is usually partial: skip it
        $got    = 0
        $sw     = [System.Diagnostics.Stopwatch]::StartNew()
        while ($got -lt $Count -and $sw.Elapsed.TotalSeconds -lt $TimeoutSeconds) {
            $r = ConvertFrom-PowerMeterStream $parser (Read-PowerMeterPort $sp)
            foreach ($b in $r.Blocks) {
                if ($first) { $first = $false; continue }
                if ($got -ge $Count) { break }
                $m = $b | Measure-Object -Average -Maximum -Minimum
                [pscustomobject]@{
                    Time    = (Get-Date).ToString('HH:mm:ss.fff')
                    Samples = $b.Count
                    Period  = $period
                    Avg     = [math]::Round($m.Average, 2)
                    Max     = $m.Maximum
                    Min     = $m.Minimum
                    dBm     = $b
                }
                $got++
            }
            Start-Sleep -Milliseconds 20
        }
    } finally {
        Close-PowerMeterPort $sp
    }
}
