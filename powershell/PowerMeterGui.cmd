@echo off
rem Double-click launcher for the PowerShell RF power meter GUI
start "" powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0Start-PowerMeterGui.ps1" %*
