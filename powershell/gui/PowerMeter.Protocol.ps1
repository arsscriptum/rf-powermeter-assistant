#╔════════════════════════════════════════════════════════════════════════════════╗
#║                                                                                ║
#║   PowerMeter.Protocol.ps1                                                      ║
#║   Wire protocol of the "USB RF Power Meter V5" (STM32 + AD8317/AD8318)         ║
#║                                                                                ║
#╟────────────────────────────────────────────────────────────────────────────────╢
#║   Guillaume Plante <codegp@icloud.com>                                         ║
#║   Code licensed under the GNU GPL v3.0. See the LICENSE file for details.      ║
#╚════════════════════════════════════════════════════════════════════════════════╝
#
#  Stream record (10 bytes), e.g. "-72400000u":
#
#     [+|-] D D d D D D d d <unit>
#       |   dBm   |  watts  |  'u' = uW, 'm' = mW, 'w' = W (also the record terminator)
#
#     -72400000u  = -72.4 dBm, 000.00 uW
#     +03600229m  =  +3.6 dBm, 002.29 mW
#
#  The dBm field is the finished, calibrated value (band calibration + offset are
#  applied by the firmware). Every 500 records the meter emits the block marker "Aa":
#  one block = one 500-sample sweep taken at the sample period set with K01..K18.
#
#  Commands (CRLF terminated, one command per write):
#     Read                 -> reply "R<freq:4><+-##.#>" embedded in the stream (between A and a)
#     A<freq:4><+-##.#>    -> set band-cal frequency (MHz) and offset (dB). NEVER send the short
#                             "A<freq:4>" form: it corrupts the meter state (stream pegs at -99.9).
#     K01..K18             -> set the sweep sample rate (write-only)
#
#  Reference: https://github.com/LostInNovo/rf-power-meter-v5-companion/blob/main/PROTOCOL.md


# Sample period (seconds) for each K command. Duplicates are real (the vendor app maps
# several of its timebase positions to the same hardware rate). Index = K, [0] unused.
$script:PowerMeterSamplePeriods = [double[]]@(
    0,
    2e-6,    4e-6,    8e-6,    16e-6,   32e-6,    # K01..K05
    64e-6,   128e-6,  256e-6,  256e-6,  512e-6,   # K06..K10
    512e-6,  1024e-6, 2048e-6, 4096e-6, 4096e-6,  # K11..K15
    8192e-6, 8192e-6, 16384e-6                    # K16..K18
)

$script:PowerMeterSamplesPerBlock = 500

$script:PowerMeterTokenRegex = [regex]::new(
    '(?<rec>[+-]\d{8}[umw])|R(?<freq>\d{4})(?<atten>[+-]\d\d\.\d)|(?<end>a)',
    [System.Text.RegularExpressions.RegexOptions]::Compiled)


function Get-PowerMeterSamplePeriod {
    [CmdletBinding()]
    param(
        [Parameter(Position = 0, Mandatory = $true)]
        [ValidateRange(1, 18)]
        [int]$K
    )
    return $script:PowerMeterSamplePeriods[$K]
}

function Get-PowerMeterTimebase {
    <#
    .SYNOPSIS
        Timebase table used by the GUI's Time-dev control: the visible window and the K
        command (hardware sample rate) that produces it. Windows shorter than one 500-sample
        sweep at K01 are zoomed views of the K01 sweep.
    #>
    [CmdletBinding()]
    param()

    $windows = @(
        @{ Window = 100e-6;  K = 1 },  @{ Window = 200e-6; K = 1 },  @{ Window = 500e-6; K = 1 }
        @{ Window = 1e-3;    K = 1 },  @{ Window = 2e-3;   K = 2 },  @{ Window = 4e-3;   K = 3 }
        @{ Window = 8e-3;    K = 4 },  @{ Window = 16e-3;  K = 5 },  @{ Window = 32e-3;  K = 6 }
        @{ Window = 64e-3;   K = 7 },  @{ Window = 128e-3; K = 8 },  @{ Window = 256e-3; K = 10 }
        @{ Window = 512e-3;  K = 12 }, @{ Window = 1.024;  K = 13 }, @{ Window = 2.048;  K = 14 }
        @{ Window = 4.096;   K = 16 }, @{ Window = 8.192;  K = 18 }
    )

    foreach ($w in $windows) {
        $period = Get-PowerMeterSamplePeriod -K $w.K
        [pscustomobject]@{
            Window  = [double]$w.Window
            K       = [int]$w.K
            Period  = $period
            Samples = [int][math]::Min([double]$script:PowerMeterSamplesPerBlock, [math]::Round($w.Window / $period))
            Label   = '{0} - {1}Sa/s' -f (Format-PowerMeterTime $w.Window), (Format-PowerMeterSi (1 / $period) -Digits 1)
        }
    }
}

function New-PowerMeterParser {
    <#
    .SYNOPSIS
        Creates a stream parser state object for ConvertFrom-PowerMeterStream.
    #>
    [CmdletBinding()]
    param()

    [pscustomobject]@{
        Pending    = ''                                              # unparsed tail of the previous chunk
        Block      = [System.Collections.Generic.List[double]]::new(512)
        LastRecord = ''
        Records    = [int64]0
    }
}

function ConvertFrom-PowerMeterStream {
    <#
    .SYNOPSIS
        Feeds a chunk of raw serial text to the parser.
    .OUTPUTS
        A result object with:
          Samples    - every dBm value parsed from this chunk (in order)
          Blocks     - completed 500-sample sweeps (double[] each) closed by an 'a' marker
          Settings   - settings replies (Frequency, Offset, Raw) found in this chunk
          LastRecord - the last raw record text seen, e.g. "-72400000u"
    #>
    [CmdletBinding()]
    param(
        [Parameter(Position = 0, Mandatory = $true)]
        [psobject]$Parser,

        [Parameter(Position = 1, Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Chunk
    )

    $samples  = [System.Collections.Generic.List[double]]::new()
    $blocks   = [System.Collections.Generic.List[object]]::new()
    $settings = [System.Collections.Generic.List[object]]::new()

    $text = $Parser.Pending + $Chunk
    $consumed = 0
    $block    = $Parser.Block
    $last     = $null

    foreach ($m in $script:PowerMeterTokenRegex.Matches($text)) {
        $consumed = $m.Index + $m.Length
        $rec = $m.Groups['rec']
        if ($rec.Success) {
            $last = $rec.Value
            # chars 1..3 = DDd -> DD.d dBm ; the watts field is ignored (recomputed from dBm)
            $dbm = [int]$last.Substring(1, 3) / 10.0
            if ($last[0] -eq '-') { $dbm = -$dbm }
            $samples.Add($dbm)
            $block.Add($dbm)
        } elseif ($m.Groups['end'].Success) {
            if ($block.Count -gt 0) {
                $blocks.Add($block.ToArray())
                $block.Clear()
            }
        } else {
            $settings.Add([pscustomobject]@{
                Frequency = [int]$m.Groups['freq'].Value
                Offset    = [double]::Parse($m.Groups['atten'].Value, [cultureinfo]::InvariantCulture)
                Raw       = $m.Value
            })
        }
    }
    if ($last) { $Parser.LastRecord = $last }
    $Parser.Records += $samples.Count

    # keep a short unparsed tail (a record or reply split across two reads); drop runaway garbage
    $tail = $text.Substring($consumed)
    if ($tail.Length -gt 32) { $tail = $tail.Substring($tail.Length - 32) }
    $Parser.Pending = $tail

    # a sweep never exceeds 500 samples; if a marker got lost, don't let the block grow forever
    if ($Parser.Block.Count -gt (2 * $script:PowerMeterSamplesPerBlock)) { $Parser.Block.Clear() }

    [pscustomobject]@{
        Samples    = $samples
        Blocks     = $blocks
        Settings   = $settings
        LastRecord = $Parser.LastRecord
    }
}

function New-PowerMeterCommand {
    <#
    .SYNOPSIS
        Builds a validated command string (including CRLF) for the meter.
    .EXAMPLE
        New-PowerMeterCommand -Frequency 1000 -Offset 10   # "A1000+10.0`r`n"
        New-PowerMeterCommand -SampleRate 1                # "K01`r`n"
        New-PowerMeterCommand -Read                        # "Read`r`n"
    #>
    [CmdletBinding(DefaultParameterSetName = 'Read')]
    param(
        [Parameter(ParameterSetName = 'Set', Mandatory = $true)]
        [ValidateRange(1, 9999)]
        [int]$Frequency,

        [Parameter(ParameterSetName = 'Set', Mandatory = $true)]
        [ValidateRange(-99.9, 99.9)]
        [double]$Offset,

        [Parameter(ParameterSetName = 'Rate', Mandatory = $true)]
        [ValidateRange(1, 18)]
        [int]$SampleRate,

        [Parameter(ParameterSetName = 'Read')]
        [switch]$Read
    )

    switch ($PSCmdlet.ParameterSetName) {
        'Set' {
            $sign = if ($Offset -lt 0) { '-' } else { '+' }
            $cmd  = 'A{0:0000}{1}{2}' -f $Frequency, $sign, [math]::Abs($Offset).ToString('00.0', [cultureinfo]::InvariantCulture)
            # the short "A####" form corrupts the meter; refuse anything but the full form
            if ($cmd -notmatch '^A\d{4}[+-]\d\d\.\d$') { throw "Refusing to build malformed command '$cmd'" }
        }
        'Rate' { $cmd = 'K{0:00}' -f $SampleRate }
        default { $cmd = 'Read' }
    }
    return "$cmd`r`n"
}

function ConvertTo-PowerMeterWatt {
    [CmdletBinding()]
    param(
        [Parameter(Position = 0, Mandatory = $true, ValueFromPipeline = $true)]
        [double]$dBm
    )
    process { [math]::Pow(10, ($dBm - 30) / 10) }
}

# p n u m (none) k M G  ->  10^-12 .. 10^9 ; index = exponent/3 + 4
$script:PowerMeterSiPrefixes = @('p', 'n', [string][char]0x00B5, 'm', '', 'k', 'M', 'G')

# The Format-* helpers are plain (non-advanced) functions on purpose: the GUI calls them
# on every refresh and advanced-function binding costs ~10x more.
function Format-PowerMeterSi {
    <#
    .SYNOPSIS
        Formats a value with an SI prefix: 4.7e-10 -> "470p", 12170 -> "12.17k".
    #>
    param(
        [double]$Value,
        [string]$Unit = '',
        [int]$Digits = 2
    )

    if ([double]::IsNaN($Value) -or [double]::IsInfinity($Value)) { return "--$Unit" }
    $abs = [math]::Abs($Value)
    $e = 0
    if ($abs -gt 0) {
        # 0.9995 keeps 999.96 from printing as "1000" in the lower prefix
        $e = [int][math]::Floor([math]::Log10($abs / 0.9995) / 3)
        if ($e -lt -4) { $e = -4 } elseif ($e -gt 3) { $e = 3 }
    }
    $num = [math]::Round($Value / [math]::Pow(1000, $e), $Digits)
    $fmt = if ($Digits -gt 0) { '0.' + ('#' * $Digits) } else { '0' }
    return $num.ToString($fmt, [cultureinfo]::InvariantCulture) + $script:PowerMeterSiPrefixes[$e + 4] + $Unit
}

function Format-PowerMeterWatt {
    param([double]$dBm)
    if ([double]::IsNaN($dBm)) { return '--W' }
    return Format-PowerMeterSi ([math]::Pow(10, ($dBm - 30) / 10)) 'W' 2
}

function Format-PowerMeterTime {
    param([double]$Seconds)
    $t = Format-PowerMeterSi $Seconds 's' 2
    return $t.Replace(([string][char]0x00B5) + 's', 'us')
}
