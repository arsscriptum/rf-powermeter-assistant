#╔════════════════════════════════════════════════════════════════════════════════╗
#║                                                                                ║
#║   PowerMeter.ps1                                                               ║
#║                                                                                ║
#╟────────────────────────────────────────────────────────────────────────────────╢
#║   Guillaume Plante <codegp@icloud.com>                                         ║
#║   Code licensed under the GNU GPL v3.0. See the LICENSE file for details.      ║
#╚════════════════════════════════════════════════════════════════════════════════╝



function Register-DeviceInstance {
    [CmdletBinding()]
    param(
        [Parameter(Position = 0, Mandatory = $true)]
        [string]$Port,
        [Parameter(Mandatory = $false)]
        [switch]$Overwrite
    )
    $available = [System.IO.Ports.SerialPort]::GetPortNames()
    if ($Port -notin $available) {
        Write-Warning "Port $Port not found. Available: $($available -join ', ')"
    }

    $RetDev = Get-PnpDevice -Class Ports -ErrorAction Ignore | Where-Object FriendlyName -like "*($Port)*"

    if (-not $RetDev) {
        Write-Error "No device found for $Port"
        return
    }

    $InstanceIdRaw = $RetDev.InstanceId
    $RegKeyPwrMeter = "HKCU:\Software\arsscriptum\rf-powermeter-assistant"
    $RegKeyPwrMeterUniqueInstance = Join-Path $RegKeyPwrMeter "$Port"

    if ((Test-Path $RegKeyPwrMeterUniqueInstance) -and ($Overwrite)) {
        Remove-Item -Path $RegKeyPwrMeterUniqueInstance -Force -Recurse | Out-Null
    }
    
    if (-not (Test-Path $RegKeyPwrMeterUniqueInstance)) {
        New-Item -Path $RegKeyPwrMeterUniqueInstance -Force | Out-Null
    }

    $InError = ($($RetDev.Status) -eq "Error")
    
    if($InError){
        $ErrCode=$RetDev.ConfigManagerErrorCode
        $IsDisabled=switch($ErrCode){
            'CM_PROB_DISABLED' { $True }
            'CM_PROB_LOCKED' { $True }
            default { $True }
        }
    } else {
        $IsDisabled=$False    
    }
    $IsEnabled=(-not($IsDisabled))


    $Status=$RetDev.Status
    $TmpData=$InstanceIdRaw.Replace('&',"\").Split("\")
    $InstanceType=$TmpData[0]
    $InstanceVid=$TmpData[1].Replace('VID_','')
    $InstancePid=$TmpData[2].Replace('PID_','')
    $InstanceHash=$TmpData[3]

    Set-ItemProperty -Path $RegKeyPwrMeterUniqueInstance -Name "status" -Value $Status -Type String
    Set-ItemProperty -Path $RegKeyPwrMeterUniqueInstance -Name "enabled" -Value $IsEnabled -Type Binary
    Set-ItemProperty -Path $RegKeyPwrMeterUniqueInstance -Name "port" -Value $Port -Type String
    Set-ItemProperty -Path $RegKeyPwrMeterUniqueInstance -Name "type" -Value $InstanceType -Type String
    Set-ItemProperty -Path $RegKeyPwrMeterUniqueInstance -Name "vid" -Value $InstanceVid -Type String
    Set-ItemProperty -Path $RegKeyPwrMeterUniqueInstance -Name "pid" -Value $InstancePid -Type String
    Set-ItemProperty -Path $RegKeyPwrMeterUniqueInstance -Name "hash" -Value $InstanceHash -Type String
    Set-ItemProperty -Path $RegKeyPwrMeterUniqueInstance -Name "raw" -Value $InstanceIdRaw -Type String

    [pscustomobject]@{
        Port = $Port
        InstanceType = $InstanceType
        InstancePid = $InstancePid
        InstanceHash = $InstanceHash
        Status = $Status
        IsEnabled = $IsEnabled
    }
}


function Invoke-DisconnectDeviceFromPort {
    [CmdletBinding(DefaultParameterSetName="Port")]
    param(
        [Parameter(ParameterSetName="Port", Position = 0, Mandatory = $true)]
        [string]$Port
    )

    try {
        $dev = Get-UsbDeviceFromInfo -Port $Port
        if (-not $dev) {
            Write-Error "No device found on port $Port"
            return
        }

        Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false
        Start-Sleep -Seconds 2
        Enable-PnpDevice  -InstanceId $dev.InstanceId -Confirm:$false

        Write-Host "ok"
    } catch {
        Write-Error "$_"
    }
}

function Invoke-DisconnectDeviceFromInstanceId {
    [CmdletBinding(DefaultParameterSetName="InstanceId")]
    param(
        [Parameter(ParameterSetName="InstanceId", Position = 0, Mandatory = $true)]
        [string]$InstanceId
    )

    try {
        $dev = Get-UsbDeviceFromInfo -InstanceId $InstanceId
        if (-not $dev) {
            Write-Error "No device found for InstanceId $InstanceId"
            return
        }

        Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false
        Start-Sleep -Seconds 2
        Enable-PnpDevice  -InstanceId $dev.InstanceId -Confirm:$false

        Write-Host "ok"
    } catch {
        Write-Error "$_"
    }
}

function Get-UsbDeviceFromInfo {
    [CmdletBinding(DefaultParameterSetName="InstanceId")]
    param(
        [Parameter(ParameterSetName="InstanceId", Position = 0, Mandatory = $false)]
        [string]$InstanceId,

        [Parameter(ParameterSetName="Port", Position = 0, Mandatory = $false)]
        [string]$Port
    )

    try {
        $RetDev = $null

        if ($PSCmdlet.ParameterSetName -eq "Port") {
            # match a COMx label inside the FriendlyName, e.g. "... (COM7)"
            $RetDev = Get-PnpDevice -Class Ports -ErrorAction Ignore |
                      Where-Object FriendlyName -like "*($Port)*"
        } else {
            $RetDev = Get-PnpDevice -InstanceId $InstanceId -ErrorAction Ignore
        }

        return $RetDev
    } catch {
        Write-Error "$_"
    }
}


function Invoke-DisconnectDevice {
    [CmdletBinding(DefaultParameterSetName="Port")]
    param(
        [Parameter(ParameterSetName="Port", Position = 0, Mandatory = $true)]
        [string]$Port,

        [Parameter(ParameterSetName="InstanceId", Position = 0, Mandatory = $true)]
        [string]$InstanceId
    )

    try {
        if ($PSCmdlet.ParameterSetName -eq "Port") {
            $dev = Get-UsbDeviceFromInfo -Port $Port
            $what = "port $Port"
        } else {
            $dev = Get-UsbDeviceFromInfo -InstanceId $InstanceId
            $what = "InstanceId $InstanceId"
        }

        if (-not $dev) {
            Write-Error "No device found for $what"
            return
        }

        Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false
        Start-Sleep -Seconds 2
        Enable-PnpDevice  -InstanceId $dev.InstanceId -Confirm:$false

        Write-Host "ok"
    } catch {
        Write-Error "$_"
    }
}

function Invoke-ReadPowerMeter {
    [CmdletBinding()]
    param(
        [Parameter(Position = 0, Mandatory = $false)]
        [ArgumentCompleter({
            param($commandName, $parameterName, $wordToComplete, $commandAst, $fakeBoundParameters)
            [System.IO.Ports.SerialPort]::GetPortNames() |
                Where-Object { $_ -like "$wordToComplete*" } |
                ForEach-Object {
                    [System.Management.Automation.CompletionResult]::new($_, $_, 'ParameterValue', $_)
                }
        })]
        [ValidateScript({
            $ports = [System.IO.Ports.SerialPort]::GetPortNames()
            if ($_ -in $ports) { $true }
            else { throw "Port $_ not found. Available: $($ports -join ', ')" }
        })]
        [string]$Port = "COM7",

        [Parameter(Position = 1, Mandatory = $false)]
        [int]$Size = 50,

        [Parameter(Position = 2, Mandatory = $false)]
        [int]$BaudRate = 115200,

        [Parameter(Position = 3, Mandatory = $false)]
        [double]$DivideBy = 1e6,

        [Parameter(Mandatory = $false)]
        [switch]$Raw
    )

    try {
        $available = [System.IO.Ports.SerialPort]::GetPortNames()
        if ($Port -notin $available) {
            Write-Error "Port $Port not found. Available: $($available -join ', ')"
            return
        }
        Write-Verbose "System.IO.Ports.SerialPort $Port,$BaudRate,None,8,One"
        [System.IO.Ports.SerialPort]$port = [System.IO.Ports.SerialPort]::new($Port,$BaudRate,[System.IO.Ports.Parity]::None,8,[System.IO.Ports.StopBits]::One)
        try{
            $port.DtrEnable  = $true
            $port.RtsEnable  = $true
            $port.ReadTimeout = 2000    
        } catch {
            Write-Warning "$_"
        }
        
        $port.Open()

        $buf = ""
        for ($i = 0; $i -lt $Size; $i++) {
            $buf += $port.ReadExisting()
            $fields = $buf -split 'u'
            $buf = $fields[-1]                      # keep trailing partial

            if ($fields.Count -lt 2) {              # no complete field yet
                Start-Sleep -Milliseconds 100
                continue
            }

            foreach ($f in $fields[0..($fields.Count - 2)]) {
                $f = $f.Trim()
                if ($f -match '^-?\d+$') {
                    [int64]$rawVal = $f               # sign preserved
                    if ($Raw) {
                        [pscustomobject]@{
                            Time = (Get-Date).ToString('HH:mm:ss.fff')
                            Raw  = $rawVal
                        }
                    } else {
                        [pscustomobject]@{
                            Time = (Get-Date).ToString('HH:mm:ss.fff')
                            Raw  = $rawVal
                            dBm  = [math]::Round($rawVal / $DivideBy, 2)
                        }
                    }
                } elseif ($f) {
                    [pscustomobject]@{
                        Time  = (Get-Date).ToString('HH:mm:ss.fff')
                        Token = $f                    # catches Aa and any marker
                    }
                }
            }
            Start-Sleep -Milliseconds 100
        }
    } catch {
        Write-Error "$_"
    } finally {
        if ($port -and $port.IsOpen) { $port.Close() }
    }
}