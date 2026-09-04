# hdr_state.ps1 — 每显示器 HDR 状态快速查询（P0 A/B 前后各跑一次，截图留档）
# 用法：powershell -File tools\hdr_state.ps1
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'DisplayConfig.psm1') -Force

$states = Get-DisplayHdrState
$now = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
Write-Output "HDR state @ $now"
Write-Output ('=' * 72)
$states | ForEach-Object {
    $hdr = if ($_.HdrEnabled) { 'ON' } elseif ($_.HdrSupported) { 'supported, off' } else { 'off (not supported)' }
    Write-Output ("Monitor {0}" -f $_.GdiDeviceName)
    Write-Output ("  HDR            : {0}" -f $hdr)
    Write-Output ("  WideColor      : {0}" -f $_.WideColor)
    Write-Output ("  BitsPerColor   : {0}" -f $_.BitsPerColor)
    Write-Output ("  SDR white      : {0} nits" -f $_.SdrWhiteNits)
    Write-Output ("  Refresh        : {0} Hz" -f $_.RefreshHz)
    Write-Output ('-' * 72)
}
