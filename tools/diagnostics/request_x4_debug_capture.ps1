param(
    [Parameter(Mandatory = $true)][string]$SessionDirectory,
    [string]$Reason = 'sprite/audio glitch observed'
)

$ErrorActionPreference = 'Stop'
$sessionRoot = (Resolve-Path -LiteralPath $SessionDirectory).Path
$latestPath = Join-Path $sessionRoot 'latest-session.txt'
if (Test-Path -LiteralPath $latestPath) {
    $sessionRoot = (Resolve-Path -LiteralPath (Get-Content -LiteralPath $latestPath -Raw).Trim()).Path
}
$requestPath = Join-Path $sessionRoot 'capture.request'
if (Test-Path -LiteralPath $requestPath) {
    throw 'A capture request is already pending in this session.'
}
$temporaryPath = Join-Path $sessionRoot ('request-' + [guid]::NewGuid().ToString('N') + '.tmp')
[System.IO.File]::WriteAllText($temporaryPath, $Reason, [System.Text.UTF8Encoding]::new($false))
try {
    Move-Item -LiteralPath $temporaryPath -Destination $requestPath
} finally {
    if (Test-Path -LiteralPath $temporaryPath) { Remove-Item -LiteralPath $temporaryPath }
}
Write-Output "Capture requested: $requestPath"
