param(
    [string]$DiscImage = 'H:/Isos/Rockman X4 (J)(Saturn)(ISO).mdf',
    [long]$Steps = 108000000,
    [long]$CaptureAfterBlocks = 106000000,
    [long]$RightPressAfterBlocks = 122000000,
    [long]$RightReleaseAfterBlocks = 145000000,
    [string]$Name = ('player-stage-x-' + (Get-Date -Format 'yyyyMMdd-HHmmss')),
    [switch]$TraceGame
)

$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if ($Steps -le 0 -or $CaptureAfterBlocks -lt 0 -or $CaptureAfterBlocks -ge $Steps) {
    throw 'Use 0 <= CaptureAfterBlocks < Steps.'
}
if ($RightPressAfterBlocks -lt 0 -or $RightReleaseAfterBlocks -le $RightPressAfterBlocks) {
    throw 'Use RightPressAfterBlocks < RightReleaseAfterBlocks.'
}
if ($Name -notmatch '^[a-zA-Z0-9_-]+$') {
    throw 'Name must contain only letters, digits, underscores or hyphens.'
}

$executable = Join-Path $taskRoot 'build/tools/saturn-sh2/Release/saturn-sh2.exe'
$capture = Join-Path $taskRoot "build/artifacts/$Name"
$log = Join-Path $taskRoot "build/tmp/$Name.log"
$trace = Join-Path $taskRoot "build/tmp/control-trace-$Name.csv"
$manifest = Join-Path $taskRoot "build/tmp/$Name.json"
foreach ($path in @($capture, $log, $trace, $manifest)) {
    if (Test-Path -LiteralPath $path) { throw "Output already exists: $path" }
}

# The rebuilt runner enters attract mode before the former 20M Start pulse.
# This earlier menu prefix was verified against SMPC reads and X1.CPK loading.
# Explicit releases preserve separate menu confirmation edges.
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
    '110000000:a', '110300000:release',
    "${RightPressAfterBlocks}:right", "${RightReleaseAfterBlocks}:release",
    '330000000:right', '335000000:release'
) -join ';'

$runnerArguments = @(
    'boot', (Join-Path $taskRoot 'build/rockman/aip.bin'),
    '--aip-base', '0x06002E40',
    '--ip', (Join-Path $taskRoot 'build/rockman/ip.bin'), '--ip-base', '0x06002000',
    '--program', (Join-Path $taskRoot 'build/rockman/0.BIN'), '--program-base', '0x06009000',
    '--entry', '0x06002E40', '--expected-entry', '0x06010000',
    '--disc', (Resolve-Path -LiteralPath $DiscImage).Path, '--disc-sector-size', '2448',
    '--continue-after-execdmp', '--vblank-blocks', '14000', '--steps', "$Steps",
    '--pad-sequence', $padSequence,
    '--control-trace-oreg2-only', '--control-trace', $trace,
    '--frame-dir', $capture, '--frame-capture-after-blocks', "$CaptureAfterBlocks",
    '--renderer', 'cpu', '--trace-cd-sectors'
)
if ($TraceGame) { $runnerArguments += '--trace-game' }
$inputFiles = @($executable, (Join-Path $taskRoot 'build/rockman/aip.bin'),
    (Join-Path $taskRoot 'build/rockman/ip.bin'), (Join-Path $taskRoot 'build/rockman/0.BIN'),
    (Resolve-Path -LiteralPath $DiscImage).Path)
$hashes = @($inputFiles | ForEach-Object {
    @{ path = $_; sha256 = (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash }
})
New-Item -ItemType Directory -Force -Path (Split-Path $log) | Out-Null
$record = @{ executable = $executable; arguments = $runnerArguments; inputs = $hashes }
$record | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifest -Encoding utf8
Write-Output "Replay log: $log"
Write-Output "Replay manifest: $manifest"
& $executable @runnerArguments *> $log
$record.native_exit_code = $LASTEXITCODE
$summary = Get-Content -LiteralPath $log -Raw
$record.status = [regex]::Match($summary, '(?m)^  status: (.+)$').Groups[1].Value.Trim()
$record.completed_steps = [regex]::Match($summary, '(?m)^  completed block steps: (\d+)').Groups[1].Value
$record | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifest -Encoding utf8
if ($record.native_exit_code -ne 0 -or $record.status -ne 'step-limit' -or
    $record.completed_steps -ne "$Steps") {
    throw "Replay did not reach the requested step limit. See $log"
}
Write-Output "Replay finished. Capture: $capture"
