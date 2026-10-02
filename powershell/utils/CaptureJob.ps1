

$CaptureScript = {
    param([int]$BaudRate = 115200, [int]$Seconds = 5, [bool]$Dtr = $True, [string]$Send = "", [string]$OutFile = "")

    try {
        $ErrorActionPreference = 'Stop'
        [System.IO.Ports.SerialPort]$port = [System.IO.Ports.SerialPort]::new("COM7", $BaudRate, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
        $port.Handshake = 'None'
        $port.DtrEnable = [bool]$Dtr
        $port.RtsEnable = $false
        $port.ReadTimeout = 200
        $port.Open()
        Start-Sleep -Milliseconds 200
        $port.DiscardInBuffer()

        if ($Send -ne "") {
            $sendBytes = [System.Text.Encoding]::ASCII.GetBytes($Send)
            $port.Write($sendBytes, 0, $sendBytes.Length)
            Write-Output ("SENT: " + (($sendBytes | ForEach-Object { $_.ToString('X2') }) -join ' '))
        }

        [System.IO.MemoryStream]$buf = [System.IO.MemoryStream]::new()
        $deadline = (Get-Date).AddSeconds($Seconds)
        while ((Get-Date) -lt $deadline) {
            $n = $port.BytesToRead
            if ($n -gt 0) {
                [byte[]]$tmp = [byte[]]::new($n)
                $read = $port.Read($tmp, 0, $n)
                if ($read -gt 0) { $buf.Write($tmp, 0, $read) }
            } else {
                Start-Sleep -Milliseconds 20
            }
        }
        $port.Close()
        $bytes = $buf.ToArray()
        Write-Output ("BYTES RECEIVED: " + $bytes.Length + " in " + $Seconds + "s (" + [math]::Round($bytes.Length / $Seconds) + " B/s)")

        if ($OutFile -ne "" -and $bytes.Length -gt 0) {
            [System.IO.File]::WriteAllBytes($OutFile, $bytes)
            Write-Output ("SAVED: " + $OutFile)
        }

        # Hex + ASCII dump of first 512 bytes
        $limit = [math]::Min($bytes.Length, 512)
        for ($i = 0; $i -lt $limit; $i += 16) {
            $chunk = $bytes[$i..([math]::Min($i + 15, $limit - 1))]
            $hex = ($chunk | ForEach-Object { $_.ToString('X2') }) -join ' '
            $ascii = ($chunk | ForEach-Object { if ($_ -ge 32 -and $_ -le 126) { [char]$_ } else { '.' } }) -join ''
            Write-Output ("{0:X4}  {1,-47}  {2}" -f $i, $hex, $ascii)
        }

    } catch {
        Write-Warning $_
    } finally {
        Write-verbose ".."
    } }.GetNewClosure()

[scriptblock]$CaptureScriptBlock = [scriptblock]::Create($CaptureScript)




function Capture-UsingJob {
    [CmdletBinding(SupportsShouldProcess)]
    param()

    try {
        $id = ((date -UFormat %s) -as [string]).Substring(7, 3)
        $JobName = "Capture-UsingJob-{0}" -f $id
        Write-Host "Start job `"$JobName`" Asynchronous $Asynchronous"
        $jobby = Start-Job -Name $JobName -ScriptBlock $CaptureScriptBlock -ArgumentList (115200, 5, $True, "", "")
        $Capturing = $True

        $JobName

    } catch {
        Show-ExceptionDetails $_ -ShowStack
    }
}

function Receive-CaptureJob {
    [CmdletBinding(SupportsShouldProcess)]
    param(
        [Parameter(Mandatory = $true, Position = 0)]
        [string]$JobName,
        [Parameter(Mandatory = $False)]
        [switch]$Wait
    )

    try {
        $Capturing = $True
        $j=Get-Job -Name $JobName -ErrorAction Ignore
        if($j -eq $Null){
            Write-Warning "no such job"
            return
        }
    
        Write-verbose "JobState: $JobState"
        $Output = ''
        $ProgressTitle = "MODE: HTTPJOB $JobName"
        if ($Wait) {
            while ($Capturing) {
                try {
                    $JobState = $j.State

                    Write-verbose "JobState: $JobState"
                    if ($JobState -eq 'Completed') {
                        $Capturing = $False
                        $Output = Receive-Job -Name $JobName

                        $j | Remove-Job
                    }

                } catch {
                    Write-Error $_
                }
            }
            Write-Output $Output
        } else {
            $JobState = (Get-Job -Name $JobName).State

            Write-verbose "JobState: $JobState"
            if ($JobState -eq 'Completed') {
                $Capturing = $False
                $Output = Receive-Job -Name $JobName

                Get-Job $JobName | Remove-Job
                Write-Output $Output
            } else {
                Write-Host "In progress..." -f darkcyan
            }
        }

    } catch {
        Write-Error $_
    }
}

