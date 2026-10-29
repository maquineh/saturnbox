param(
    [string]$ExecutablePath,
    [string]$DiscImage = 'H:/Isos/Rockman X4 (J)(Saturn)(ISO).mdf',
    [ValidateSet('cpu', 'vulkan', 'auto')]
    [string]$Renderer = 'cpu',
    [long]$Steps = 25000000,
    [int]$Runs = 1,
    [string]$OutputDirectory,
    [switch]$Capture,
    [switch]$CaptureAudio,
    [switch]$HleAudio,
    [switch]$Realtime,
    [switch]$Profile,
    [switch]$ExportAot
)

$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if ($Steps -lt 1000000 -or $Runs -lt 1) { throw 'Steps must be >= 1M and Runs >= 1.' }
if ([string]::IsNullOrWhiteSpace($ExecutablePath)) {
    $ExecutablePath = Join-Path $taskRoot 'build/tools/saturn-sh2/Release/saturn-sh2.exe'
}
$ExecutablePath = (Resolve-Path -LiteralPath $ExecutablePath).Path
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $taskRoot ('build/perf/x4-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$OutputDirectory = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw "Output already exists: $OutputDirectory" }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$route = '13700000:start;14000000:release;18000000:start;18300000:release;' +
    '21000000:a;21300000:release;24000000:a;24300000:release;' +
    '46000000:start;46300000:release;61000000:a;61300000:release;' +
    '82000000:a;82300000:release;94000000:a;94300000:release;' +
    '102000000:a;102300000:release;110000000:a;110300000:release;' +
    '160000000:right;170000000:release;330000000:right;335000000:release'
$baseArguments = @(
    'boot', (Join-Path $taskRoot 'build/rockman/aip.bin'), '--aip-base', '0x06002E40',
    '--ip', (Join-Path $taskRoot 'build/rockman/ip.bin'), '--ip-base', '0x06002000',
    '--program', (Join-Path $taskRoot 'build/rockman/0.BIN'), '--program-base', '0x06009000',
    '--entry', '0x06002E40', '--expected-entry', '0x06010000',
    '--disc', $DiscImage, '--disc-sector-size', '2448', '--continue-after-execdmp',
    '--vblank-blocks', '14000', '--steps', "$Steps", '--pad-sequence', $route,
    '--renderer', $Renderer
)
if (-not $HleAudio) { $baseArguments += '--scsp-driver' }
if ($Realtime) { $baseArguments[0] = 'launch' }
if ($Profile) { $baseArguments += '--profile' }
$results = @()
for ($run = 1; $run -le $Runs; $run++) {
    $runArguments = @($baseArguments)
    if ($ExportAot) {
        $runArguments += @('--aot-module-output', (Join-Path $OutputDirectory "module-$run.cpp"),
            '--aot-entry-output', (Join-Path $OutputDirectory "entries-$run.txt"))
    }
    if ($Capture) {
        $runArguments += @('--frame-dir', (Join-Path $OutputDirectory "frames-$run"),
            '--frame-capture-after-blocks', "$($Steps - 1000000)",
            '--audio-output', (Join-Path $OutputDirectory "audio-$run.wav"))
    } elseif ($CaptureAudio) {
        $runArguments += @('--audio-output', (Join-Path $OutputDirectory "audio-$run.wav"))
    }
    $logPath = Join-Path $OutputDirectory "run-$run.log"
    $watch = [Diagnostics.Stopwatch]::StartNew()
    & $ExecutablePath @runArguments > $logPath 2>&1
    $exitCode = $LASTEXITCODE
    $watch.Stop()
    $log = Get-Content -LiteralPath $logPath -Raw
    if ($exitCode -ne 0 -or $log -notmatch 'status: step-limit') {
        throw "Benchmark failed (exit $exitCode). See $logPath"
    }
    $vblankMatch = [regex]::Match($log, 'SCU scheduler:.*?vblank=(\d+)')
    if (-not $vblankMatch.Success) { throw "Missing VBlank statistics: $logPath" }
    $frames = [long]$vblankMatch.Groups[1].Value
    $seconds = $watch.Elapsed.TotalSeconds
    $results += [pscustomobject]@{
        run = $run; seconds = $seconds; vblanks = $frames
        vblanks_per_second = $frames / $seconds
        milliseconds_per_vblank = $seconds * 1000 / $frames
        frontend_statistics = [regex]::Match($log, '(?m)^\s*frontend frames:.*$').Value.Trim()
        arguments = $runArguments
    }
    Write-Output ('Run {0}: {1:N2} VBlank/s, {2:N2} ms/VBlank, {3:N2}s' -f
        $run, ($frames / $seconds), ($seconds * 1000 / $frames), $seconds)
}
[pscustomobject]@{
    executable = $ExecutablePath
    executable_sha256 = (Get-FileHash -LiteralPath $ExecutablePath -Algorithm SHA256).Hash
    renderer = $Renderer
    steps = $Steps; scsp_driver = -not [bool]$HleAudio; capture = [bool]$Capture
    capture_audio = [bool]($Capture -or $CaptureAudio)
    realtime = [bool]$Realtime
    profile = [bool]$Profile
    export_aot = [bool]$ExportAot
    note = if ($Realtime) {
        'Window, device audio and pacing. Includes startup and final reports; see frontend_statistics for presentation FPS.'
    } else {
        'Unthrottled renderer, complete emulation including audio; excludes window paint and device playback. Includes startup and final reports.'
    }
    runs = $results
} | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $OutputDirectory 'results.json')
Write-Output "Report: $OutputDirectory/results.json"
