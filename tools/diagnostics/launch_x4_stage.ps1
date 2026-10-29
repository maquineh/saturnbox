param(
    [string]$DiscImage = 'H:/Isos/Rockman X4 (J)(Saturn)(ISO).mdf',
    [string]$Renderer = 'auto',
    [long]$VBlankBlocks = 10000,
    [long]$LiveAfterBlocks = 160000000,
    [string]$ExecutablePath,
    [string]$DebugSession,
    [string]$AudioOutput,
    [string]$ScspRequests,
    [switch]$ScspDriver,
    [switch]$CaptureOnBrightBars
)

$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$executable = if ([string]::IsNullOrWhiteSpace($ExecutablePath)) {
    Join-Path $taskRoot 'build/tools/saturn-sh2/Release/saturn-sh2.exe'
} else {
    $candidate = if ([System.IO.Path]::IsPathRooted($ExecutablePath)) {
        $ExecutablePath
    } else {
        Join-Path $taskRoot $ExecutablePath
    }
    (Resolve-Path -LiteralPath $candidate).Path
}
# The launcher default points at the archive layout used on the original
# machine; fall back to the usual Downloads copy so a fresh machine works.
$discCandidates = @(
    $DiscImage,
    (Join-Path $env:USERPROFILE 'Downloads/rockmanx4/Rockman X4 (J)(Saturn)(ISO).mdf')
)
$disc = $null
foreach ($candidate in $discCandidates) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        $disc = (Resolve-Path -LiteralPath $candidate).Path
        break
    }
}
if ($null -eq $disc) {
    throw "Disc image not found. Tried: $($discCandidates -join '; ')"
}
$aip = Join-Path $taskRoot 'build/rockman/aip.bin'
$ip = Join-Path $taskRoot 'build/rockman/ip.bin'
$program = Join-Path $taskRoot 'build/rockman/0.BIN'
foreach ($path in @($executable, $aip, $ip, $program, $disc)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required X4 launch input not found: $path"
    }
}
if ($Renderer -notin @('auto', 'cpu', 'vulkan')) {
    throw 'Renderer must be auto, cpu or vulkan.'
}
if ($VBlankBlocks -le 0) {
    throw 'VBlankBlocks must be positive.'
}

# This reproduces the verified title/menu prefix that reaches X's first-stage
# scene, then gives port 1 to keyboard/XInput after the READY transition.
# Keyboard controls: arrows, Z/X/C (A/B/C), Enter (Start); controller: XInput.
$padSequence = @(
    '13700000:start', '14000000:release',
    '18000000:start', '18300000:release',
    '21000000:a', '21300000:release',
    '24000000:a', '24300000:release',
    '46000000:start', '46300000:release',
    '61000000:a', '61300000:release',
    '82000000:a', '82300000:release',
    '94000000:a', '94300000:release',
    '102000000:a', '102300000:release',
    '110000000:a', '110300000:release'
) -join ';'

$runnerArguments = @(
    'launch', $aip,
    '--aip-base', '0x06002E40',
    '--ip', $ip, '--ip-base', '0x06002000',
    '--program', $program, '--program-base', '0x06009000',
    '--entry', '0x06002E40', '--expected-entry', '0x06010000',
    '--disc', $disc, '--disc-sector-size', '2448',
    '--vblank-blocks', "$VBlankBlocks",
    '--pad-sequence', $padSequence,
    '--pad-live-after-blocks', "$LiveAfterBlocks",
    '--renderer', $Renderer
)
if (-not [string]::IsNullOrWhiteSpace($DebugSession)) {
    $runnerArguments += @('--debug-session', [System.IO.Path]::GetFullPath($DebugSession))
}
if ($CaptureOnBrightBars) {
    $runnerArguments += '--capture-on-bright-bars'
}
if ($ScspDriver) {
    $runnerArguments += '--scsp-driver'
    # With the real driver it is worth keeping the audio and the sequence
    # requests: F8 captures then document which cue each moment triggered.
    $captureRoot = if (-not [string]::IsNullOrWhiteSpace($DebugSession)) {
        [System.IO.Path]::GetFullPath($DebugSession)
    } else {
        Join-Path $taskRoot 'build/tmp'
    }
    New-Item -ItemType Directory -Force -Path $captureRoot | Out-Null
    if ([string]::IsNullOrWhiteSpace($AudioOutput)) {
        $AudioOutput = Join-Path $captureRoot 'x4-audio.wav'
    }
    if ([string]::IsNullOrWhiteSpace($ScspRequests)) {
        $ScspRequests = Join-Path $captureRoot 'x4-requests.csv'
    }
    $runnerArguments += @('--audio-output', [System.IO.Path]::GetFullPath($AudioOutput))
    $runnerArguments += @('--scsp-requests', [System.IO.Path]::GetFullPath($ScspRequests))
    Write-Output "Audio: $AudioOutput"
    Write-Output "Sequence requests: $ScspRequests"
}

Write-Output "Opening X4. The known menu route and READY transition run first; live controls take over at $LiveAfterBlocks blocks."
Write-Output 'Once the stage appears, use arrows / Z-X-C / Enter or an XInput controller.'
# The emulator writes diagnostics to stderr (for example the live debugger's
# "snapshots dropped" warning). With $ErrorActionPreference = 'Stop' PowerShell
# turns that into a NativeCommandError and would terminate the session, so the
# game runs with the preference relaxed and the exit code checked afterwards.
$ErrorActionPreference = 'Continue'
& $executable @runnerArguments
$runnerExit = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
exit $runnerExit
