#╔════════════════════════════════════════════════════════════════════════════════╗
#║                                                                                ║
#║   Start-PowerMeterGui.ps1                                                      ║
#║   Launches the PowerShell GUI for the USB RF Power Meter V5                    ║
#║                                                                                ║
#╟────────────────────────────────────────────────────────────────────────────────╢
#║   Guillaume Plante <codegp@icloud.com>                                         ║
#║   Code licensed under the GNU GPL v3.0. See the LICENSE file for details.      ║
#╚════════════════════════════════════════════════════════════════════════════════╝

[CmdletBinding()]
param(
    [Parameter(Position = 0, Mandatory = $false)]
    [string]$Port,

    [Parameter(Mandatory = $false)]
    [switch]$Connect
)

# WinForms dialogs (Data_Export's SaveFileDialog) need a single-threaded apartment
if ([System.Threading.Thread]::CurrentThread.ApartmentState -ne 'STA') {
    $exe  = (Get-Process -Id $PID).Path
    $argv = @('-NoProfile', '-STA', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath)
    if ($Port)    { $argv += @('-Port', $Port) }
    if ($Connect) { $argv += '-Connect' }
    & $exe @argv
    return
}

. "$PSScriptRoot\utils\PowerMeter.Utils.ps1"
. "$PSScriptRoot\gui\PowerMeter.Protocol.ps1"
. "$PSScriptRoot\gui\PowerMeter.Serial.ps1"
. "$PSScriptRoot\gui\PowerMeter.Gui.ps1"

Show-PowerMeterGui @PSBoundParameters
