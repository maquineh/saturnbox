param(
    [string]$DiscImage = 'H:/Isos/Rockman X4 (J)(Saturn)(ISO).mdf',
    [string]$Renderer = 'cpu',
    [long]$VBlankBlocks = 14000,
    [string]$SessionDirectory,
    [switch]$ScspDriver
)

$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if ([string]::IsNullOrWhiteSpace($SessionDirectory)) {
    $SessionDirectory = Join-Path $taskRoot ('build/sessions/x4-debug-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
Write-Output "Debug output: $SessionDirectory"
Write-Output 'Press F8 when a glitch appears. The game keeps running while captures are written.'
& (Join-Path $PSScriptRoot 'launch_x4_stage.ps1') -DiscImage $DiscImage -Renderer $Renderer `
    -VBlankBlocks $VBlankBlocks -DebugSession $SessionDirectory -ScspDriver:$ScspDriver
