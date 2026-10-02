#╔════════════════════════════════════════════════════════════════════════════════╗
#║                                                                                ║
#║   PowerMeter.Gui.ps1                                                           ║
#║   WinForms clone of the vendor "USB-RF-Power-Meter-V5" application             ║
#║                                                                                ║
#╟────────────────────────────────────────────────────────────────────────────────╢
#║   Guillaume Plante <codegp@icloud.com>                                         ║
#║   Code licensed under the GNU GPL v3.0. See the LICENSE file for details.      ║
#╚════════════════════════════════════════════════════════════════════════════════╝
#
#  Requires PowerMeter.Protocol.ps1 and PowerMeter.Serial.ps1 (dot-sourced first).
#
#  Everything runs on the UI thread: a WinForms timer polls the serial port every
#  30 ms (PowerShell scriptblocks cannot run on the SerialPort's background thread).
#  All shared state lives in one hashtable ($S) that the event handlers mutate.


Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$script:PmScales       = [double[]]@(1, 2, 3, 5, 10, 20)   # dB per division
$script:PmDivisionsY   = 10
$script:PmDivisionsX   = 10
$script:PmSettingsPath = Join-Path $env:LOCALAPPDATA 'rf.powermeter\gui.settings.json'


# ─────────────────────────────────────────────────────────────────────────────
#  settings
# ─────────────────────────────────────────────────────────────────────────────

function Get-PmGuiSettings {
    $defaults = [ordered]@{
        Port          = ''
        BaudRate      = 460800
        Frequency     = 1000
        Offset        = 0.0
        TimebaseIndex = 3          # 1ms - 500kSa/s
        ScaleIndex    = 4          # 10 dB/div
        TopDbm        = 10
        TriggerLevel  = -40
        TriggerMode   = 'Auto'
        Marker1       = 0.15
        Marker2       = 0.55
    }
    if (Test-Path $script:PmSettingsPath) {
        try {
            $saved = Get-Content $script:PmSettingsPath -Raw | ConvertFrom-Json
            foreach ($p in $saved.PSObject.Properties) {
                if ($defaults.Contains($p.Name)) { $defaults[$p.Name] = $p.Value }
            }
        } catch { Write-Verbose "settings ignored: $_" }
    }
    return $defaults
}

function Save-PmGuiSettings {
    param($S)
    try {
        $dir = Split-Path $script:PmSettingsPath
        if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
        [ordered]@{
            Port          = [string]$S.C.Port.SelectedItem
            BaudRate      = $S.BaudRate
            Frequency     = $S.C.Freq.Text
            Offset        = $S.C.Offset.Text
            TimebaseIndex = $S.TbIndex
            ScaleIndex    = $S.ScaleIndex
            TopDbm        = $S.TopDbm
            TriggerLevel  = $S.TriggerLevel
            TriggerMode   = $S.TriggerMode
            Marker1       = $S.M1
            Marker2       = $S.M2
        } | ConvertTo-Json | Set-Content -Path $script:PmSettingsPath -Encoding UTF8
    } catch { Write-Verbose "settings not saved: $_" }
}


# ─────────────────────────────────────────────────────────────────────────────
#  helpers
# ─────────────────────────────────────────────────────────────────────────────

function Write-PmGuiLog {
    param($S, [string]$Text)
    $box = $S.C.Log
    if ($box.Lines.Count -gt 300) { $box.Lines = $box.Lines[-200..-1] }
    $box.AppendText("$Text`r`n")
}

function ConvertFrom-PmUserNumber {
    # accepts '.' or ',' as decimal separator, and a leading '+'
    param([string]$Text)
    $t = $Text.Trim().Replace(',', '.')
    $v = 0.0
    if ([double]::TryParse($t, [System.Globalization.NumberStyles]::Float, [cultureinfo]::InvariantCulture, [ref]$v)) { return $v }
    return [double]::NaN
}

function Format-PmDbm {
    param([double]$dBm)
    if ([double]::IsNaN($dBm)) { return '--dBm' }
    return $dBm.ToString('0.0', [cultureinfo]::InvariantCulture) + 'dBm'
}

function Get-PmTimebase { param($S) $S.Timebases[$S.TbIndex] }

function Add-PmQueue {
    # queue a command; the meter needs a beat between commands (one command per write)
    param($S, [string]$Name, [int]$DelayMs = 0)
    $due = [datetime]::Now.AddMilliseconds($DelayMs)
    if ($S.Queue.Count -gt 0) {
        $minDue = $S.Queue[$S.Queue.Count - 1].Due.AddMilliseconds(300)
        if ($due -lt $minDue) { $due = $minDue }
    }
    $S.Queue.Add([pscustomobject]@{ Name = $Name; Due = $due })
}

function Send-PmGuiCommand {
    param($S, [string]$Command)
    if (-not $S.Port) { Write-PmGuiLog $S 'E-not connected'; return $false }
    try {
        Send-PowerMeterCommand $S.Port $Command
        Write-PmGuiLog $S ('S-' + $Command.TrimEnd())
        return $true
    } catch {
        Write-PmGuiLog $S "E-$($_.Exception.Message)"
        return $false
    }
}

function Invoke-PmQueued {
    param($S, [string]$Name)
    switch ($Name) {
        'K' {
            $k = (Get-PmTimebase $S).K
            if (Send-PmGuiCommand $S (New-PowerMeterCommand -SampleRate $k)) {
                $S.SentK = $k
                $S.Sweep = $null          # old sweep has the wrong time scale
                $S.C.Plot.Invalidate()
            }
        }
        'Read' {
            if (Send-PmGuiCommand $S (New-PowerMeterCommand -Read)) {
                $S.ReadDeadline = [datetime]::Now.AddMilliseconds(2500)
            }
        }
    }
}

function Set-PmConnected {
    param($S, [bool]$Connected)
    $c = $S.C
    $c.Connect.Text   = if ($Connected) { 'Disconnect' } else { 'Connect' }
    $c.Port.Enabled   = -not $Connected
    $c.Refresh.Enabled = -not $Connected
    $c.Send.Enabled   = $Connected
    $c.Read.Enabled   = $Connected
}

function Disconnect-PmGui {
    param($S, [string]$Reason = '')
    if ($S.Port) {
        Close-PowerMeterPort $S.Port
        $S.Port = $null
        Write-PmGuiLog $S ("Disconnected" + $(if ($Reason) { ": $Reason" }))
    }
    $S.Queue.Clear()
    $S.ReadDeadline = $null
    $S.C.Rate.Text = ''
    Set-PmConnected $S $false
}

function Connect-PmGui {
    param($S)
    $name = [string]$S.C.Port.SelectedItem
    if (-not $name) { Write-PmGuiLog $S 'E-no port selected'; return }
    try {
        $S.Port   = Open-PowerMeterPort -Port $name -BaudRate $S.BaudRate
        $S.Parser = New-PowerMeterParser
        $S.SentK  = 0
        $S.LastDataAt  = [datetime]::Now
        $S.RateRecords = 0
        $S.RateAt      = [datetime]::Now
        $S.ReadRetries = 0
        Write-PmGuiLog $S "Connected $name"
        Set-PmConnected $S $true
        # the sample rate is write-only on the meter: set it so we know what it is
        Add-PmQueue $S 'K' 100
        Add-PmQueue $S 'Read' 1500
    } catch {
        $S.Port = $null
        Write-PmGuiLog $S "E-$($_.Exception.Message)"
        Set-PmConnected $S $false
    }
}

function Update-PmPortList {
    param($S)
    $cb = $S.C.Port
    $current = [string]$cb.SelectedItem
    $cb.Items.Clear()
    $ports = @(Get-PowerMeterPort)
    $S.PortInfo = @{}
    foreach ($p in $ports) { [void]$cb.Items.Add($p.Port); $S.PortInfo[$p.Port] = $p }
    $pick = $null
    foreach ($want in @($current, $S.Settings.Port)) {
        if ($want -and $cb.Items.Contains($want)) { $pick = $want; break }
    }
    if (-not $pick) { $pick = ($ports | Where-Object IsPowerMeter | Select-Object -First 1).Port }
    if (-not $pick -and $cb.Items.Count -gt 0) { $pick = $cb.Items[0] }
    if ($pick) { $cb.SelectedItem = $pick }
}

function Reset-PmStats {
    param($S)
    $S.Max = [double]::NaN
    $S.Min = [double]::NaN
    Update-PmReadouts $S
}


# ─────────────────────────────────────────────────────────────────────────────
#  readouts
# ─────────────────────────────────────────────────────────────────────────────

function Get-PmMarkerInfo {
    param($S, [double]$Fraction)
    $tb = Get-PmTimebase $S
    $t  = $Fraction * $tb.Window
    $v  = [double]::NaN
    if ($S.Sweep) {
        $i = [int][math]::Floor($Fraction * $tb.Samples)
        $i = [math]::Max(0, [math]::Min($i, [math]::Min($tb.Samples, $S.Sweep.Length) - 1))
        $v = $S.Sweep[$i]
    }
    [pscustomobject]@{ Time = $t; dBm = $v }
}

function Update-PmReadouts {
    param($S)
    $c = $S.C
    $c.PowerW.Text   = Format-PowerMeterWatt $S.Current
    $c.PowerDbm.Text = Format-PmDbm $S.Current
    $c.MaxW.Text     = Format-PowerMeterWatt $S.Max
    $c.MaxDbm.Text   = Format-PmDbm $S.Max
    $c.MinW.Text     = Format-PowerMeterWatt $S.Min
    $c.MinDbm.Text   = Format-PmDbm $S.Min
    Update-PmMarkerReadouts $S
}

function Update-PmMarkerReadouts {
    param($S)
    $c  = $S.C
    $m1 = Get-PmMarkerInfo $S $S.M1
    $m2 = Get-PmMarkerInfo $S $S.M2
    $c.M1Time.Text = Format-PowerMeterTime $m1.Time
    $c.M2Time.Text = Format-PowerMeterTime $m2.Time
    $c.M1Dbm.Text  = Format-PmDbm $m1.dBm
    $c.M2Dbm.Text  = Format-PmDbm $m2.dBm
    $dt = [math]::Abs($m2.Time - $m1.Time)
    $c.DeltaT.Text = 'T2-T1: ' + (Format-PowerMeterTime $dt)
    $c.InvDeltaT.Text = '1/T2-T1: ' + $(if ($dt -gt 0) { Format-PowerMeterSi (1 / $dt) 'Hz' } else { '--' })
}

function Update-PmTimebaseLabel {
    param($S)
    $S.C.Timebase.Text = (Get-PmTimebase $S).Label
}


# ─────────────────────────────────────────────────────────────────────────────
#  the 30 ms tick: read, parse, trigger, display
# ─────────────────────────────────────────────────────────────────────────────

function Test-PmTrigger {
    # rising edge through the trigger level inside the visible part of the sweep
    param([double[]]$Sweep, [double]$Level, [int]$Count)
    $n = [math]::Min($Count, $Sweep.Length)
    for ($i = 1; $i -lt $n; $i++) {
        if ($Sweep[$i - 1] -lt $Level -and $Sweep[$i] -ge $Level) { return $true }
    }
    return $false
}

function Invoke-PmTick {
    param($S)
    $now = [datetime]::Now

    # queued commands
    while ($S.Queue.Count -gt 0 -and $S.Queue[0].Due -le $now) {
        $item = $S.Queue[0]
        $S.Queue.RemoveAt(0)
        Invoke-PmQueued $S $item.Name
    }

    # debounced timebase change -> K command
    if ($S.KDue -and $S.KDue -le $now) {
        $S.KDue = $null
        if ($S.Port -and (Get-PmTimebase $S).K -ne $S.SentK) { Add-PmQueue $S 'K' }
    }

    if (-not $S.Port) { return }

    # the first Read after an A command is sometimes swallowed: retry once
    if ($S.ReadDeadline -and $S.ReadDeadline -le $now) {
        $S.ReadDeadline = $null
        if ($S.ReadRetries -lt 1) { $S.ReadRetries++; Add-PmQueue $S 'Read' }
        else { Write-PmGuiLog $S 'E-no reply to Read' }
    }

    try {
        $chunk = Read-PowerMeterPort $S.Port
    } catch {
        Disconnect-PmGui $S $_.Exception.Message
        return
    }

    if ($chunk.Length -gt 0) {
        $S.LastDataAt = $now
        $r = ConvertFrom-PowerMeterStream $S.Parser $chunk

        foreach ($v in $r.Samples) {
            if (-not ($v -le $S.Max)) { $S.Max = $v }      # NaN-safe
            if (-not ($v -ge $S.Min)) { $S.Min = $v }
        }

        foreach ($st in $r.Settings) {
            Write-PmGuiLog $S ('R-' + $st.Raw)
            $S.C.Freq.Text   = $st.Frequency.ToString()
            $S.C.Offset.Text = $(if ($st.Offset -lt 0) { '-' } else { '+' }) + [math]::Abs($st.Offset).ToString('00.0', [cultureinfo]::InvariantCulture)
            $S.ReadDeadline  = $null
            $S.ReadRetries   = 0
        }

        $tb = Get-PmTimebase $S
        $redraw = $false
        foreach ($b in $r.Blocks) {
            if ($b.Length -lt 450) { continue }            # partial sweep (just connected / K changed)
            $S.Current = [System.Linq.Enumerable]::Average($b)
            if (-not $S.Running) { continue }
            if ($S.TriggerMode -ne 'Auto' -and -not (Test-PmTrigger $b $S.TriggerLevel $tb.Samples)) { continue }
            $S.Sweep = $b
            $redraw  = $true
            if ($S.TriggerMode -eq 'Single') {
                $S.Running = $false
                $S.C.Stop.Text = 'RUN'
            }
        }

        # numeric readouts at 10 Hz (plenty for a human, and the formatting isn't free);
        # a new sweep always refreshes them so the marker values match the trace
        if ($redraw -or ($now - $S.ReadoutAt).TotalMilliseconds -ge 100) {
            $S.ReadoutAt = $now
            $S.C.Wave.Text = $r.LastRecord
            Update-PmReadouts $S
        }
        if ($redraw) { $S.C.Plot.Invalidate(); $S.C.Strip.Invalidate() }
    } elseif (($now - $S.LastDataAt).TotalSeconds -gt [math]::Max(3, 2 * (Get-PmTimebase $S).Window + 1)) {
        $S.C.Rate.Text = 'no data'
    }

    if (($now - $S.RateAt).TotalSeconds -ge 1) {
        $dt = ($now - $S.RateAt).TotalSeconds
        $S.C.Rate.Text = '{0:0} rec/s' -f (($S.Parser.Records - $S.RateRecords) / $dt)
        $S.RateRecords = $S.Parser.Records
        $S.RateAt = $now
    }
}


# ─────────────────────────────────────────────────────────────────────────────
#  painting
# ─────────────────────────────────────────────────────────────────────────────

function Get-PmY {
    param($S, [double]$dBm, [int]$Height)
    $range = $script:PmScales[$S.ScaleIndex] * $script:PmDivisionsY
    $y = ($S.TopDbm - $dBm) / $range * $Height
    return [float][math]::Max(-10, [math]::Min($Height + 10, $y))
}

function Invoke-PmPaintPlot {
    param($S, [System.Drawing.Graphics]$G, [int]$W, [int]$H)
    $G.Clear([System.Drawing.Color]::Black)

    # graticule
    for ($i = 1; $i -lt $script:PmDivisionsX; $i++) {
        $x = [float]($i * $W / $script:PmDivisionsX)
        $G.DrawLine($S.P.Grid, $x, 0, $x, $H)
    }
    for ($i = 1; $i -lt $script:PmDivisionsY; $i++) {
        $y = [float]($i * $H / $script:PmDivisionsY)
        $G.DrawLine($S.P.Grid, 0, $y, $W, $y)
    }

    # trigger level
    $ty = Get-PmY $S $S.TriggerLevel $H
    $tp = if ($S.TriggerMode -eq 'Auto') { $S.P.TrigOff } else { $S.P.Trig }
    $G.DrawLine($tp, 0, $ty, $W, $ty)

    # trace
    $tb = Get-PmTimebase $S
    if ($S.Sweep) {
        $n = [math]::Min($tb.Samples, $S.Sweep.Length)
        if ($n -ge 2) {
            $pts   = [System.Drawing.PointF[]]::new($n)
            $sx    = $W / $tb.Samples
            $sy    = $H / ($script:PmScales[$S.ScaleIndex] * $script:PmDivisionsY)
            $top   = $S.TopDbm
            $sweep = $S.Sweep
            $lo    = -10.0
            $hi    = $H + 10.0
            for ($i = 0; $i -lt $n; $i++) {
                # same as Get-PmY, inlined: this loop runs for every point of every repaint
                $y = ($top - $sweep[$i]) * $sy
                if ($y -lt $lo) { $y = $lo } elseif ($y -gt $hi) { $y = $hi }
                $pts[$i] = [System.Drawing.PointF]::new([float]($i * $sx), [float]$y)
            }
            $G.DrawLines($S.P.Trace, $pts)
        }
    } else {
        $msg = if ($S.Port) { 'waiting for sweep...' } else { 'not connected' }
        $G.DrawString($msg, $S.F.Small, [System.Drawing.Brushes]::Gray, 8, 8)
    }

    # markers
    foreach ($f in @($S.M1, $S.M2)) {
        $x = [float]($f * $W)
        $G.DrawLine($S.P.Marker, $x, 0, $x, $H)
    }

    if (-not $S.Running) {
        $G.DrawString('STOPPED', $S.F.Small, [System.Drawing.Brushes]::OrangeRed, $W - 70, 6)
    }
}

function Invoke-PmPaintRuler {
    param($S, [System.Drawing.Graphics]$G, [int]$W, [int]$H)
    $G.Clear($S.C.Ruler.BackColor)
    $x = $W - 6
    $G.DrawLine([System.Drawing.Pens]::Black, $x, 0, $x, $H - 1)
    $scale = $script:PmScales[$S.ScaleIndex]
    for ($i = 0; $i -le $script:PmDivisionsY; $i++) {
        $y = [float][math]::Min($H - 1, $i * $H / $script:PmDivisionsY)
        $G.DrawLine([System.Drawing.Pens]::Black, $x - 8, $y, $x, $y)
        if ($i % 2 -eq 1) { continue }
        $label = ($S.TopDbm - $i * $scale).ToString('0.#', [cultureinfo]::InvariantCulture)
        $sz = $G.MeasureString($label, $S.F.Small)
        $ly = [math]::Max(0, [math]::Min($H - $sz.Height, $y - $sz.Height / 2))
        $G.DrawString($label, $S.F.Small, [System.Drawing.Brushes]::Black, $x - 10 - $sz.Width, $ly)
    }
    # minor ticks
    for ($i = 0; $i -lt $script:PmDivisionsY; $i++) {
        $y = [float](($i + 0.5) * $H / $script:PmDivisionsY)
        $G.DrawLine([System.Drawing.Pens]::Black, $x - 4, $y, $x, $y)
    }
}

function Invoke-PmPaintStrip {
    param($S, [System.Drawing.Graphics]$G, [int]$W)
    $G.Clear($S.C.Strip.BackColor)
    $i = 1
    foreach ($f in @($S.M1, $S.M2)) {
        $text = "Marker$i"
        $sz = $G.MeasureString($text, $S.F.Small)
        $x = [math]::Max(0, [math]::Min($W - $sz.Width, $f * $W - $sz.Width / 2))
        $G.DrawString($text, $S.F.Small, [System.Drawing.Brushes]::Black, [float]$x, 3)
        $i++
    }
}


# ─────────────────────────────────────────────────────────────────────────────
#  export
# ─────────────────────────────────────────────────────────────────────────────

function Export-PmSweep {
    param($S)
    if (-not $S.Sweep) { Write-PmGuiLog $S 'E-no sweep to export'; return }
    $dlg = [System.Windows.Forms.SaveFileDialog]::new()
    $dlg.Filter   = 'CSV (*.csv)|*.csv|All files (*.*)|*.*'
    $dlg.FileName = 'powermeter_{0}.csv' -f (Get-Date -Format 'yyyyMMdd_HHmmss')
    if ($dlg.ShowDialog($S.Form) -ne [System.Windows.Forms.DialogResult]::OK) { return }

    $tb  = Get-PmTimebase $S
    $inv = [cultureinfo]::InvariantCulture
    $sb  = [System.Text.StringBuilder]::new()
    [void]$sb.AppendLine('Index,Time_s,dBm,Watt')
    for ($i = 0; $i -lt $S.Sweep.Length; $i++) {
        $v = $S.Sweep[$i]
        [void]$sb.AppendLine(('{0},{1},{2},{3}' -f $i,
            ($i * $tb.Period).ToString('0.#########', $inv),
            $v.ToString('0.0', $inv),
            (ConvertTo-PowerMeterWatt $v).ToString('E4', $inv)))
    }
    [System.IO.File]::WriteAllText($dlg.FileName, $sb.ToString())
    Write-PmGuiLog $S "Exported $($S.Sweep.Length) samples ($($tb.Label)) to $($dlg.FileName)"
}


# ─────────────────────────────────────────────────────────────────────────────
#  form
# ─────────────────────────────────────────────────────────────────────────────

function Show-PowerMeterGui {
    <#
    .SYNOPSIS
        Opens the RF power meter GUI (clone of the vendor USB-RF-Power-Meter-V5 app).
    .PARAMETER Port
        Serial port to preselect (default: last used, else the first STM32 VCP found).
    .PARAMETER Connect
        Connect immediately on startup.
    #>
    [CmdletBinding()]
    param(
        [string]$Port,
        [switch]$Connect
    )

    [System.Windows.Forms.Application]::EnableVisualStyles()

    $settings = Get-PmGuiSettings
    if ($Port) { $settings.Port = $Port }

    $S = @{
        Settings     = $settings
        BaudRate     = [int]$settings.BaudRate
        Timebases    = @(Get-PowerMeterTimebase)
        TbIndex      = [int]$settings.TimebaseIndex
        ScaleIndex   = [int]$settings.ScaleIndex
        TopDbm       = [double]$settings.TopDbm
        TriggerLevel = [double]$settings.TriggerLevel
        TriggerMode  = [string]$settings.TriggerMode
        M1           = [double]$settings.Marker1
        M2           = [double]$settings.Marker2
        Port         = $null
        Parser       = $null
        Sweep        = $null
        Current      = [double]::NaN
        Max          = [double]::NaN
        Min          = [double]::NaN
        Running      = $true
        Queue        = [System.Collections.Generic.List[object]]::new()
        SentK        = 0
        KDue         = $null
        ReadDeadline = $null
        ReadRetries  = 0
        LastDataAt   = [datetime]::Now
        RateAt       = [datetime]::Now
        RateRecords  = 0
        ReadoutAt    = [datetime]::MinValue
        Drag         = 0
        PortInfo     = @{}
        C            = @{}
    }
    $S.TbIndex    = [math]::Max(0, [math]::Min($S.Timebases.Count - 1, $S.TbIndex))
    $S.ScaleIndex = [math]::Max(0, [math]::Min($script:PmScales.Count - 1, $S.ScaleIndex))
    if ($S.TriggerMode -notin 'Auto', 'Normal', 'Single') { $S.TriggerMode = 'Auto' }

    $S.F = @{
        Big    = [System.Drawing.Font]::new('Segoe UI', 18)
        Label  = [System.Drawing.Font]::new('Segoe UI', 12)
        Mid    = [System.Drawing.Font]::new('Segoe UI', 11)
        Small  = [System.Drawing.Font]::new('Segoe UI', 8)
        Normal = [System.Drawing.Font]::new('Segoe UI', 9)
        Red    = [System.Drawing.Font]::new('Segoe UI', 14)
    }
    $gridPen = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(45, 45, 45))
    $gridPen.DashStyle = [System.Drawing.Drawing2D.DashStyle]::Dot
    $trigPen = [System.Drawing.Pen]::new([System.Drawing.Color]::Orange)
    $trigPen.DashStyle = [System.Drawing.Drawing2D.DashStyle]::Dash
    $trigOff = [System.Drawing.Pen]::new([System.Drawing.Color]::FromArgb(90, 70, 20))
    $trigOff.DashStyle = [System.Drawing.Drawing2D.DashStyle]::Dash
    $S.P = @{
        Grid    = $gridPen
        Trace   = [System.Drawing.Pen]::new([System.Drawing.Color]::Yellow, 1.5)
        Marker  = [System.Drawing.Pens]::White
        Trig    = $trigPen
        TrigOff = $trigOff
    }

    $A = [System.Windows.Forms.AnchorStyles]
    $TL  = $A::Top -bor $A::Left
    $TR  = $A::Top -bor $A::Right
    $BR  = $A::Bottom -bor $A::Right
    $TLB = $A::Top -bor $A::Left -bor $A::Bottom
    $TRB = $A::Top -bor $A::Right -bor $A::Bottom
    $TLR = $A::Top -bor $A::Left -bor $A::Right
    $BLR = $A::Bottom -bor $A::Left -bor $A::Right
    $ALL = $A::Top -bor $A::Left -bor $A::Bottom -bor $A::Right

    $tip = [System.Windows.Forms.ToolTip]::new()

    function New-Ctl {
        param([string]$Type, [int]$X, [int]$Y, [int]$W, [int]$H, [string]$Text = '', $Anchor = $TL, $Font = $S.F.Normal, $Parent = $form)
        $ctl = New-Object "System.Windows.Forms.$Type"
        $ctl.SetBounds($X, $Y, $W, $H)
        if ($Text) { $ctl.Text = $Text }
        $ctl.Anchor = $Anchor
        $ctl.Font   = $Font
        $Parent.Controls.Add($ctl)
        return $ctl
    }

    $form = [System.Windows.Forms.Form]::new()
    $form.Text          = 'USB-RF-Power-Meter-V5 (PowerShell)'
    $form.ClientSize    = [System.Drawing.Size]::new(1100, 745)
    $form.MinimumSize   = [System.Drawing.Size]::new(900, 640)
    $form.StartPosition = 'CenterScreen'
    $form.BackColor     = [System.Drawing.Color]::White
    $S.Form = $form
    $C = $S.C

    # ── top: power readout ────────────────────────────────────────────────
    $pnl = New-Ctl Panel 6 6 255 72
    $pnl.BorderStyle = 'FixedSingle'
    [void](New-Ctl Label 4 8 105 24 'Power (W):' -Font $S.F.Label -Parent $pnl)
    $C.PowerW = New-Ctl Label 105 2 145 34 '--W' -Font $S.F.Big -Parent $pnl
    [void](New-Ctl Label 4 42 105 24 'Power (dBm):' -Font $S.F.Label -Parent $pnl)
    $C.PowerDbm = New-Ctl Label 105 36 150 34 '--dBm' -Font $S.F.Big -Parent $pnl

    # ── top: MAX / MIN ────────────────────────────────────────────────────
    $pnl = New-Ctl Panel 266 6 205 72
    $pnl.BorderStyle = 'FixedSingle'
    [void](New-Ctl Label 8 2 90 22 'MAX' -Font $S.F.Label -Parent $pnl)
    [void](New-Ctl Label 104 2 90 22 'MIN' -Font $S.F.Label -Parent $pnl)
    $C.MaxW   = New-Ctl Label 8 24 95 20 '--W' -Font $S.F.Mid -Parent $pnl
    $C.MinW   = New-Ctl Label 104 24 95 20 '--W' -Font $S.F.Mid -Parent $pnl
    $C.MaxDbm = New-Ctl Label 6 44 98 24 '--dBm' -Font $S.F.Label -Parent $pnl
    $C.MinDbm = New-Ctl Label 102 44 100 24 '--dBm' -Font $S.F.Label -Parent $pnl

    # ── top: markers ──────────────────────────────────────────────────────
    $pnl = New-Ctl Panel 476 6 380 72
    $pnl.BorderStyle = 'FixedSingle'
    [void](New-Ctl Label 8 4 80 22 'Marker1' -Font $S.F.Mid -Parent $pnl)
    [void](New-Ctl Label 92 4 80 22 'Marker2' -Font $S.F.Mid -Parent $pnl)
    $C.M1Time = New-Ctl Label 14 30 78 16 '' -Font $S.F.Small -Parent $pnl
    $C.M2Time = New-Ctl Label 98 30 78 16 '' -Font $S.F.Small -Parent $pnl
    $C.M1Dbm  = New-Ctl Label 14 50 78 16 '' -Font $S.F.Small -Parent $pnl
    $C.M2Dbm  = New-Ctl Label 98 50 78 16 '' -Font $S.F.Small -Parent $pnl
    $C.DeltaT    = New-Ctl Label 188 8 186 20 '' -Font $S.F.Normal -Parent $pnl
    $C.InvDeltaT = New-Ctl Label 188 42 186 20 '' -Font $S.F.Normal -Parent $pnl

    # ── top right: STOP / CLEAR / RESET + timebase ───────────────────────
    $C.Stop  = New-Ctl Button 866 8 72 28 'STOP' $TR
    $C.Clear = New-Ctl Button 944 8 72 28 'CLEAR' $TR
    $C.Reset = New-Ctl Button 1022 8 72 28 'RESET' $TR
    $C.Timebase = New-Ctl Label 866 42 228 28 '' $TR $S.F.Red
    $C.Timebase.ForeColor = [System.Drawing.Color]::Red
    $C.Timebase.TextAlign = 'MiddleCenter'
    $C.Rate = New-Ctl Label 866 70 228 16 '' $TR $S.F.Small
    $C.Rate.ForeColor = [System.Drawing.Color]::Gray
    $C.Rate.TextAlign = 'MiddleCenter'
    $tip.SetToolTip($C.Stop,  'Freeze / resume the trace')
    $tip.SetToolTip($C.Clear, 'Clear the trace and the MAX / MIN values')
    $tip.SetToolTip($C.Reset, 'Reset the view: scale, position, trigger, markers, MAX / MIN')

    # ── scope area ────────────────────────────────────────────────────────
    $C.TrigLabel = New-Ctl Label 0 96 90 16 '' -Font $S.F.Small
    $C.TrigLabel.ForeColor = [System.Drawing.Color]::Blue
    $C.TrigLabel.Cursor = [System.Windows.Forms.Cursors]::Hand
    $tip.SetToolTip($C.TrigLabel, 'Click to change the trigger mode: Auto / Normal / Single (rising edge)')
    [void](New-Ctl Label 38 116 32 16 'dBm' -Font $S.F.Small)

    $C.TrigBar = New-Ctl VScrollBar 2 136 17 574 '' $TLB
    $C.TrigBar.Minimum = 0; $C.TrigBar.Maximum = 140; $C.TrigBar.LargeChange = 1; $C.TrigBar.SmallChange = 1
    $tip.SetToolTip($C.TrigBar, 'Trigger level')

    $C.Ruler = New-Ctl PictureBox 20 136 52 574 '' $TLB
    $C.Ruler.BackColor = [System.Drawing.Color]::White

    $C.Strip = New-Ctl PictureBox 72 114 730 22 '' $TLR
    $C.Strip.BackColor = [System.Drawing.Color]::White

    $C.Plot = New-Ctl PictureBox 72 136 730 574 '' $ALL
    $C.Plot.BackColor = [System.Drawing.Color]::Black
    $C.Plot.Cursor = [System.Windows.Forms.Cursors]::SizeWE
    $tip.SetToolTip($C.Plot, 'Drag to move the nearest marker')

    $lbl = New-Ctl Label 803 116 60 16 'V-Position' $TR $S.F.Small
    $lbl.ForeColor = [System.Drawing.Color]::Blue
    $lbl = New-Ctl Label 804 140 40 16 'Scale' $TR $S.F.Small
    $lbl.ForeColor = [System.Drawing.Color]::Blue

    $C.ScaleBar = New-Ctl VScrollBar 808 158 17 552 '' $TRB
    $C.ScaleBar.Minimum = 0; $C.ScaleBar.Maximum = $script:PmScales.Count - 1; $C.ScaleBar.LargeChange = 1
    $C.VPosBar = New-Ctl VScrollBar 836 136 17 574 '' $TRB
    $C.VPosBar.Minimum = 0; $C.VPosBar.Maximum = 120; $C.VPosBar.LargeChange = 1
    $tip.SetToolTip($C.ScaleBar, 'dB per division')
    $tip.SetToolTip($C.VPosBar, 'Top of screen (dBm)')

    $C.TimeBar = New-Ctl HScrollBar 72 716 730 17 '' $BLR
    $C.TimeBar.Minimum = 0; $C.TimeBar.Maximum = $S.Timebases.Count - 1; $C.TimeBar.LargeChange = 1
    $tip.SetToolTip($C.TimeBar, 'Timebase (sets the meter sample rate, K01..K18)')
    $lbl = New-Ctl Label 806 716 60 16 'Time-dev' $BR $S.F.Small
    $lbl.ForeColor = [System.Drawing.Color]::Blue

    # ── right: Data_Set ───────────────────────────────────────────────────
    $grp = New-Ctl GroupBox 866 94 228 250 'Data_Set' $TR
    [void](New-Ctl Label 10 26 90 20 'Port_Select' -Parent $grp)
    $C.Port = New-Ctl ComboBox 110 22 108 24 '' -Parent $grp -Font $S.F.Mid
    $C.Port.DropDownStyle = 'DropDownList'
    $C.Connect = New-Ctl Button 10 56 100 30 'Connect' -Parent $grp -Font $S.F.Mid
    $C.Refresh = New-Ctl Button 118 56 100 30 'Refresh' -Parent $grp -Font $S.F.Mid
    [void](New-Ctl Label 10 104 98 22 'Freq (MHz)' -Parent $grp -Font $S.F.Mid)
    $C.Freq = New-Ctl TextBox 110 100 108 26 ([string]$settings.Frequency) -Parent $grp -Font $S.F.Mid
    $hint = New-Ctl Label 110 128 108 16 ('(' + [char]0x00B1 + '00.0)') -Parent $grp -Font $S.F.Small
    $hint.ForeColor = [System.Drawing.Color]::Red
    $hint.TextAlign = 'MiddleCenter'
    [void](New-Ctl Label 10 150 98 22 'Offset (dB)' -Parent $grp -Font $S.F.Mid)
    $off = ConvertFrom-PmUserNumber ([string]$settings.Offset)
    if ([double]::IsNaN($off)) { $off = 0 }
    $C.Offset = New-Ctl TextBox 110 146 108 26 ($(if ($off -lt 0) { '-' } else { '+' }) + [math]::Abs($off).ToString('00.0', [cultureinfo]::InvariantCulture)) -Parent $grp -Font $S.F.Mid
    $C.Send = New-Ctl Button 20 196 88 34 'Send' -Parent $grp -Font $S.F.Label
    $C.Read = New-Ctl Button 120 196 88 34 'Read' -Parent $grp -Font $S.F.Label
    $tip.SetToolTip($C.Freq, 'Measurement frequency 1..9999 MHz: selects the meter band calibration')
    $tip.SetToolTip($C.Offset, 'External attenuation -99.9..+99.9 dB: the meter adds it to every reading')
    $tip.SetToolTip($C.Send, 'Write frequency and offset to the meter (A command)')
    $tip.SetToolTip($C.Read, 'Read frequency and offset back from the meter')

    $cm = [System.Windows.Forms.ContextMenuStrip]::new()
    $miCycle = $cm.Items.Add('Power-cycle USB device (requires admin)')
    $miInfo  = $cm.Items.Add('Show device info')
    $C.Port.ContextMenuStrip = $cm

    # ── right: Data_Display ───────────────────────────────────────────────
    $grp = New-Ctl GroupBox 866 352 228 340 'Data_Display' $TRB
    $C.Log = New-Ctl TextBox 10 22 208 240 '' $ALL -Parent $grp
    $C.Log.Multiline  = $true
    $C.Log.ReadOnly   = $true
    $C.Log.ScrollBars = 'Vertical'
    $C.Log.BackColor  = [System.Drawing.Color]::White
    $lbl = New-Ctl Label 10 270 208 18 'Waveform data' $BLR -Parent $grp
    $lbl.TextAlign = 'MiddleCenter'
    $C.Wave = New-Ctl TextBox 10 292 208 26 '' $BLR -Parent $grp -Font $S.F.Mid
    $C.Wave.ReadOnly  = $true
    $C.Wave.TextAlign = 'Center'
    $C.Wave.BackColor = [System.Drawing.Color]::White

    $C.Export = New-Ctl Button 920 702 130 34 'Data_Export' $BR -Font $S.F.Mid
    $tip.SetToolTip($C.Export, 'Save the displayed sweep to CSV')

    # ── initial values ────────────────────────────────────────────────────
    $C.TrigLabel.Text = "Trig: $($S.TriggerMode)"
    $C.TrigBar.Value  = [int][math]::Max(0, [math]::Min(140, 30 - $S.TriggerLevel))
    $C.ScaleBar.Value = $script:PmScales.Count - 1 - $S.ScaleIndex   # top = largest dB/div
    $C.VPosBar.Value  = [int][math]::Max(0, [math]::Min(120, 40 - $S.TopDbm))
    $C.TimeBar.Value  = $S.TbIndex
    Update-PmTimebaseLabel $S
    Set-PmConnected $S $false
    Update-PmReadouts $S

    # ── events ────────────────────────────────────────────────────────────
    $C.Plot.Add_Paint({ param($src, $e)
        try { Invoke-PmPaintPlot $S $e.Graphics $src.ClientSize.Width $src.ClientSize.Height } catch { Write-Verbose "$_" } })
    $C.Ruler.Add_Paint({ param($src, $e)
        try { Invoke-PmPaintRuler $S $e.Graphics $src.ClientSize.Width $src.ClientSize.Height } catch { Write-Verbose "$_" } })
    $C.Strip.Add_Paint({ param($src, $e)
        try { Invoke-PmPaintStrip $S $e.Graphics $src.ClientSize.Width } catch { Write-Verbose "$_" } })
    $C.Plot.Add_Resize({ $S.C.Plot.Invalidate(); $S.C.Ruler.Invalidate(); $S.C.Strip.Invalidate() })

    $moveMarker = {
        param($x)
        $w = [math]::Max(1, $S.C.Plot.ClientSize.Width)
        $f = [math]::Max(0.0, [math]::Min(1.0, $x / $w))
        if ($S.Drag -eq 1) { $S.M1 = $f } else { $S.M2 = $f }
        Update-PmMarkerReadouts $S
        $S.C.Plot.Invalidate(); $S.C.Strip.Invalidate()
    }
    $C.Plot.Add_MouseDown({ param($src, $e)
        if ($e.Button -ne 'Left') { return }
        $w = $src.ClientSize.Width
        $S.Drag = if ([math]::Abs($e.X - $S.M1 * $w) -le [math]::Abs($e.X - $S.M2 * $w)) { 1 } else { 2 }
        & $moveMarker $e.X
    })
    $C.Plot.Add_MouseMove({ param($src, $e) if ($S.Drag -gt 0) { & $moveMarker $e.X } })
    $C.Plot.Add_MouseUp({ $S.Drag = 0 })

    $C.TrigLabel.Add_Click({
        $S.TriggerMode = switch ($S.TriggerMode) { 'Auto' { 'Normal' } 'Normal' { 'Single' } default { 'Auto' } }
        $S.C.TrigLabel.Text = "Trig: $($S.TriggerMode)"
        if ($S.TriggerMode -eq 'Single') { $S.Running = $true; $S.C.Stop.Text = 'STOP' }
        $S.C.Plot.Invalidate()
    })
    $C.TrigBar.Add_ValueChanged({
        $S.TriggerLevel = 30 - $S.C.TrigBar.Value
        $tip.SetToolTip($S.C.TrigBar, "Trigger level $($S.TriggerLevel) dBm")
        $S.C.Plot.Invalidate()
    })
    $C.ScaleBar.Add_ValueChanged({
        $S.ScaleIndex = $script:PmScales.Count - 1 - $S.C.ScaleBar.Value
        $tip.SetToolTip($S.C.ScaleBar, "$($script:PmScales[$S.ScaleIndex]) dB/div")
        $S.C.Plot.Invalidate(); $S.C.Ruler.Invalidate()
    })
    $C.VPosBar.Add_ValueChanged({
        $S.TopDbm = 40 - $S.C.VPosBar.Value
        $tip.SetToolTip($S.C.VPosBar, "Top of screen $($S.TopDbm) dBm")
        $S.C.Plot.Invalidate(); $S.C.Ruler.Invalidate()
    })
    $C.TimeBar.Add_ValueChanged({
        $S.TbIndex = $S.C.TimeBar.Value
        Update-PmTimebaseLabel $S
        Update-PmMarkerReadouts $S
        $S.KDue = [datetime]::Now.AddMilliseconds(400)   # debounce while dragging
        $S.C.Plot.Invalidate()
    })

    $C.Stop.Add_Click({
        $S.Running = -not $S.Running
        $S.C.Stop.Text = if ($S.Running) { 'STOP' } else { 'RUN' }
        $S.C.Plot.Invalidate()
    })
    $C.Clear.Add_Click({
        $S.Sweep = $null
        Reset-PmStats $S
        $S.C.Plot.Invalidate()
    })
    $C.Reset.Add_Click({
        $S.C.ScaleBar.Value = $script:PmScales.Count - 1 - 4    # 10 dB/div
        $S.C.VPosBar.Value  = 30                                 # top = +10 dBm
        $S.C.TrigBar.Value  = 70                                 # -40 dBm
        $S.TriggerMode = 'Auto'; $S.C.TrigLabel.Text = 'Trig: Auto'
        $S.M1 = 0.15; $S.M2 = 0.55
        $S.Running = $true; $S.C.Stop.Text = 'STOP'
        Reset-PmStats $S
        $S.C.Plot.Invalidate(); $S.C.Strip.Invalidate(); $S.C.Ruler.Invalidate()
    })

    $C.Refresh.Add_Click({ Update-PmPortList $S })
    $C.Connect.Add_Click({
        if ($S.Port) { Disconnect-PmGui $S } else { Connect-PmGui $S }
    })
    $C.Send.Add_Click({
        $f = ConvertFrom-PmUserNumber $S.C.Freq.Text
        $o = ConvertFrom-PmUserNumber $S.C.Offset.Text
        if ([double]::IsNaN($f) -or $f -lt 1 -or $f -gt 9999 -or $f -ne [math]::Floor($f)) {
            Write-PmGuiLog $S 'E-frequency must be an integer 1..9999 MHz'; return
        }
        if ([double]::IsNaN($o) -or [math]::Abs($o) -gt 99.9) {
            Write-PmGuiLog $S 'E-offset must be -99.9..+99.9 dB'; return
        }
        try { $cmd = New-PowerMeterCommand -Frequency ([int]$f) -Offset ([math]::Round($o, 1)) }
        catch { Write-PmGuiLog $S "E-$($_.Exception.Message)"; return }
        if (Send-PmGuiCommand $S $cmd) {
            # the stream pauses ~1 s while the meter reconfigures; verify with a Read
            $S.ReadRetries = 0
            Add-PmQueue $S 'Read' 1500
        }
    })
    $C.Read.Add_Click({ $S.ReadRetries = 0; Add-PmQueue $S 'Read' })
    $C.Export.Add_Click({ try { Export-PmSweep $S } catch { Write-PmGuiLog $S "E-$($_.Exception.Message)" } })

    $C.Port.Add_SelectedIndexChanged({
        $p = $S.PortInfo[[string]$S.C.Port.SelectedItem]
        if ($p) { $tip.SetToolTip($S.C.Port, $p.FriendlyName) }
    })
    $miInfo.Add_Click({
        $p = $S.PortInfo[[string]$S.C.Port.SelectedItem]
        if ($p) { Write-PmGuiLog $S "$($p.FriendlyName) $($p.InstanceId)" }
    })
    $miCycle.Add_Click({
        $name = [string]$S.C.Port.SelectedItem
        if (-not $name) { return }
        if (-not (Get-Command Invoke-DisconnectDevice -ErrorAction Ignore)) {
            Write-PmGuiLog $S 'E-Invoke-DisconnectDevice not loaded (powermeter.ps1)'; return
        }
        if ($S.Port) { Disconnect-PmGui $S 'power-cycle' }
        Write-PmGuiLog $S "Power-cycling $name ..."
        $S.Form.Cursor = [System.Windows.Forms.Cursors]::WaitCursor
        try {
            $out = Invoke-DisconnectDevice -Port $name -ErrorAction Stop *>&1
            Write-PmGuiLog $S "$out"
        } catch {
            Write-PmGuiLog $S "E-$($_.Exception.Message)"
        } finally {
            $S.Form.Cursor = [System.Windows.Forms.Cursors]::Default
        }
    })

    $timer = [System.Windows.Forms.Timer]::new()
    $timer.Interval = 30
    $timer.Add_Tick({
        try { Invoke-PmTick $S }
        catch { Write-PmGuiLog $S "E-$($_.Exception.Message)" }
    })

    $form.Add_Shown({
        Update-PmPortList $S
        $timer.Start()
        if ($Connect) { Connect-PmGui $S }
    })
    $form.Add_FormClosing({
        $timer.Stop()
        Save-PmGuiSettings $S
        Disconnect-PmGui $S
    })

    try {
        [void]$form.ShowDialog()
    } finally {
        $timer.Dispose()
        $tip.Dispose()
        if ($S.Port) { Close-PowerMeterPort $S.Port }
        foreach ($f in $S.F.Values) { $f.Dispose() }
        $form.Dispose()
    }
}
