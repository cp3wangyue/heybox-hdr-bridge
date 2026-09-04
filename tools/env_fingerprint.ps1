# env_fingerprint.ps1 — P0 环境指纹采集（计划书 §4.1）
# 记录 Windows 版本、GPU/驱动、显示器 HDR 状态、分辨率/刷新率/缩放，
# 以及小黑盒客户端版本、主 EXE / 关键 DLL 的文件版本与 SHA-256。
# 用法：powershell -File tools\env_fingerprint.ps1 [-ClientPath "C:\...\小黑盒"] [-OutDir docs\recon]
param(
    [string]$ClientPath = '',
    [string]$OutDir = ''
)
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'DisplayConfig.psm1') -Force

$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'docs\recon' }
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$jsonPath = Join-Path $OutDir "env-fingerprint-$stamp.json"
$mdPath = Join-Path $OutDir "env-fingerprint-$stamp.md"

$fp = [ordered]@{}
$fp.collectedAt = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')

# --- OS ---
$os = Get-CimInstance Win32_OperatingSystem
$cv = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$fp.os = [ordered]@{
    caption        = $os.Caption
    version        = $os.Version
    buildFull      = "$($cv.CurrentBuild).$($cv.UBR)"
    displayVersion = $cv.DisplayVersion
    architecture   = $os.OSArchitecture
}

# --- CPU / GPU ---
$fp.cpu = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name
$gpus = Get-CimInstance Win32_VideoController | ForEach-Object {
    [ordered]@{
        name        = $_.Name
        driverVer   = $_.DriverVersion
        driverDate  = if ($_.DriverDate) { $_.DriverDate.ToString('yyyy-MM-dd') } else { '' }
        resolution  = if ($_.CurrentHorizontalResolution) { "$($_.CurrentHorizontalResolution)x$($_.CurrentVerticalResolution)" } else { '' }
        refreshHz   = $_.CurrentRefreshRate
        colorDepth  = $_.CurrentBitsPerPixel
    }
}
$fp.gpus = @($gpus)

# --- Displays (HDR / SDR white) ---
try {
    $fp.displays = @(Get-DisplayHdrState)
} catch {
    Write-Warning "DisplayConfig query failed: $_"
    $fp.displays = @()
}

# --- DPI scaling (primary) ---
try {
    $dpi = (Get-ItemProperty 'HKCU:\Control Panel\Desktop\WindowMetrics' -ErrorAction Stop).AppliedDPI
    $fp.scalingPrimaryPercent = [math]::Round($dpi / 96.0 * 100)
} catch { $fp.scalingPrimaryPercent = $null }

# --- Client discovery ---
function Get-ClientDir {
    param([string]$Explicit)
    if ($Explicit) {
        if (Test-Path $Explicit) { return (Resolve-Path $Explicit).Path }
        throw "ClientPath not found: $Explicit"
    }
    $keys = @(
        'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
        'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*'
    )
    foreach ($k in $keys) {
        $hit = Get-ItemProperty $k -ErrorAction SilentlyContinue | Where-Object {
            ($_.DisplayName -match 'xiaoheihe|小黑盒') -and $_.InstallLocation
        } | Select-Object -First 1
        if ($hit -and (Test-Path $hit.InstallLocation)) { return $hit.InstallLocation }
    }
    return ''
}

$clientDir = Get-ClientDir -Explicit $ClientPath
$fp.client = [ordered]@{ installDir = $clientDir; files = @() }
if ($clientDir) {
    $files = Get-ChildItem -Path $clientDir -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Extension -match '^\.(exe|dll)$' }
    foreach ($f in $files) {
        $ver = $f.VersionInfo
        $hash = (Get-FileHash -Path $f.FullName -Algorithm SHA256).Hash
        $fp.client.files += [ordered]@{
            name           = $f.Name
            sizeBytes      = $f.Length
            fileVersion    = $ver.FileVersion
            productVersion = $ver.ProductVersion
            description    = $ver.FileDescription
            company        = $ver.CompanyName
            sha256         = $hash
        }
    }
} else {
    Write-Warning 'Client install dir not found; re-run with -ClientPath pointing to the install folder.'
}

# --- Persist ---
$json = $fp | ConvertTo-Json -Depth 6
Set-Content -Path $jsonPath -Value $json -Encoding UTF8

$md = New-Object System.Text.StringBuilder
[void]$md.AppendLine("# 环境指纹（P0，§4.1）")
[void]$md.AppendLine("")
[void]$md.AppendLine("- 采集时间：$($fp.collectedAt)")
[void]$md.AppendLine("- OS：$($fp.os.caption) build $($fp.os.buildFull)（$($fp.os.displayVersion)）")
[void]$md.AppendLine("- CPU：$($fp.cpu)")
foreach ($g in $fp.gpus) { [void]$md.AppendLine("- GPU：$($g.name) 驱动 $($g.driverVer)（$($g.driverDate)）@ $($g.resolution) $($g.refreshHz)Hz") }
foreach ($d in $fp.displays) {
    if ($d.HdrEnabled) {
        [void]$md.AppendLine("- 显示器 $($d.GdiDeviceName)：HDR ON，SDR white $($d.SdrWhiteNits) nits，$($d.BitsPerColor)bit，$($d.RefreshHz)Hz")
    } else {
        [void]$md.AppendLine("- 显示器 $($d.GdiDeviceName)：HDR 状态未知/关闭（高级色彩信息查询失败时以此标注；权威判定用 capture_format_spy 看 colorspace 是否 G2084），SDR white $($d.SdrWhiteNits) nits，$($d.RefreshHz)Hz")
    }
}
[void]$md.AppendLine("- 主屏缩放：$($fp.scalingPrimaryPercent)%")
[void]$md.AppendLine("- 客户端目录：$($fp.client.installDir)")
[void]$md.AppendLine("")
[void]$md.AppendLine("| 文件 | 版本 | 大小 | SHA-256 | 说明 |")
[void]$md.AppendLine("| --- | --- | --- | --- | --- |")
foreach ($f in $fp.client.files) {
    [void]$md.AppendLine("| $($f.name) | $($f.fileVersion) | $($f.sizeBytes) | $($f.sha256) | $($f.description) |")
}
[void]$md.AppendLine("")
[void]$md.AppendLine("原始数据：$(Split-Path $jsonPath -Leaf)")
Set-Content -Path $mdPath -Value $md.ToString() -Encoding UTF8

Write-Output "Environment fingerprint written:"
Write-Output "  $mdPath"
Write-Output "  $jsonPath"
if (-not $clientDir) { Write-Output '  (client dir missing — check -ClientPath)' }
