#╔════════════════════════════════════════════════════════════════════════════════╗
#║   build.ps1 - Windows build helper (MSVC + Qt 6 or Qt 5.15)                    ║
#╚════════════════════════════════════════════════════════════════════════════════╝
<#
.SYNOPSIS
    Configures and builds the Qt app with MSVC, finding Visual Studio and Qt on its own.
.EXAMPLE
    .\build.ps1                                   # first Qt found under C:\Qt, else radioconda's Qt 5
    .\build.ps1 -QtDir C:\Qt\6.8.2\msvc2022_64 -Deploy
    .\build.ps1 -Run -- --simulate                # build, then start in demo mode
#>
[CmdletBinding()]
param(
    [string]$QtDir,
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [switch]$Clean,
    [switch]$Deploy,
    [switch]$Run,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$AppArgs
)

$ErrorActionPreference = 'Stop'
$root  = $PSScriptRoot
$build = Join-Path $root 'build'

# ── Visual Studio ────────────────────────────────────────────────────────────────
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'Visual Studio (with the C++ workload) was not found.' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio installation with the MSVC x64 tools.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$ninjaDir = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'

# ── Qt ──────────────────────────────────────────────────────────────────────────
function Get-QtMajor([string]$dir) {
    if (Test-Path (Join-Path $dir 'lib\cmake\Qt6SerialPort')) { return 6 }
    if (Test-Path (Join-Path $dir 'lib\cmake\Qt5SerialPort')) { return 5 }
    return 0
}
if (-not $QtDir) {
    $candidates = @()
    foreach ($base in 'C:\Qt', 'D:\Qt', "$env:USERPROFILE\Qt") {
        if (Test-Path $base) {
            $candidates += Get-ChildItem $base -Directory -Filter '6.*' -ErrorAction Ignore |
                Sort-Object { [version]($_.Name -replace '[^\d.]', '') } -Descending |
                ForEach-Object { Get-ChildItem $_.FullName -Directory -Filter 'msvc*_64' -ErrorAction Ignore }
        }
    }
    $candidates += Get-Item 'C:\Programs\radioconda\Library', "$env:USERPROFILE\radioconda\Library" -ErrorAction Ignore
    $QtDir = ($candidates | Where-Object { (Get-QtMajor $_.FullName) -gt 0 } | Select-Object -First 1).FullName
    if (-not $QtDir) { throw 'Qt with the SerialPort module was not found. Pass -QtDir <path to Qt\6.x\msvc2022_64>.' }
}
$major = Get-QtMajor $QtDir
if ($major -eq 0) { throw "No Qt SerialPort module under $QtDir" }
Write-Host "Qt $major : $QtDir" -ForegroundColor Magenta

# ── configure + build ───────────────────────────────────────────────────────────
if ($Clean -and (Test-Path $build)) { Remove-Item $build -Recurse -Force }
$deployFlag = if ($Deploy) { 'ON' } else { 'OFF' }
$qtCmake = $QtDir -replace '\\', '/'
if (Test-Path $ninjaDir) { $env:PATH = "$ninjaDir;$env:PATH" }   # vcvars keeps what is already on PATH
$cmd = @(
    "call `"$vcvars`" >nul"
    "cmake -S `"$root`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DRFPM_QT_MAJOR=$major -DRFPM_DEPLOY=$deployFlag -DCMAKE_PREFIX_PATH=`"$qtCmake`""
    "cmake --build `"$build`""
    "ctest --test-dir `"$build`" --output-on-failure"
) -join ' && '
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

$exe = Join-Path $build 'rf-powermeter-assistant.exe'
Write-Host "Built $exe" -ForegroundColor Green

if ($Run) {
    # Without -Deploy the Qt DLLs are not next to the exe: put Qt on the PATH for this run
    $env:PATH = (Join-Path $QtDir 'bin') + ';' + $env:PATH
    $env:QT_PLUGIN_PATH = Join-Path $QtDir 'plugins'
    & $exe @AppArgs
}
