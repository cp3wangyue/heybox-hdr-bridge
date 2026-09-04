# module_diff.ps1 — P1 进程模块清单快照与 diff（计划书 §5.1）
# 共享前抓一次 idle，共享中抓一次 sharing，再 diff 出新增/消失/变更的模块。
# 用法：
#   powershell -File tools\module_diff.ps1 -Snapshot -Tag idle    -ProcessName xiaoheihe
#   powershell -File tools\module_diff.ps1 -Snapshot -Tag sharing -ProcessName xiaoheihe
#   powershell -File tools\module_diff.ps1 -Diff idle,sharing
param(
    [switch]$Snapshot,
    [switch]$Diff,
    [string]$Tag = '',
    [string]$ProcessName = 'xiaoheihe',
    [int]$ProcessId = 0,
    [string]$OutDir = ''
)
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$SnapDir = Join-Path $RepoRoot 'docs\recon\module-snapshots'
if (-not (Test-Path $SnapDir)) { New-Item -ItemType Directory -Path $SnapDir -Force | Out-Null }

# 与捕获/编码/色彩相关的候选关键词（§5.1/§5.2）
$CandidateRegex = 'dxgi|d3d11|d3d9|dcomp|capture|mfplat|mfreadwrite|mf\.dll|mfh264|mfvdec|mfhevc|nvencode|nvml|avcodec|avutil|avformat|swscale|ffmpeg|webrtc|libyuv|obs|x264|openh264|nv12|i420|dxva|dwm|color'

function Get-TargetProcess {
    if ($ProcessId -gt 0) { return Get-Process -Id $ProcessId -ErrorAction Stop }
    $procs = @(Get-Process -Name "*$ProcessName*" -ErrorAction SilentlyContinue)
    if ($procs.Count -eq 0) { throw "No process matching name '$ProcessName' is running. Start the client first, or pass -ProcessName/-ProcessId." }
    if ($procs.Count -gt 1) {
        Write-Output "Multiple matching processes:"
        $procs | ForEach-Object { Write-Output ("  pid={0} name={1} mainWindowTitle='{2}'" -f $_.Id, $_.ProcessName, $_.MainWindowTitle) }
        throw "Ambiguous match; pass -ProcessId."
    }
    return $procs[0]
}

if ($Snapshot) {
    if (-not $Tag) { throw '-Snapshot requires -Tag (e.g. idle / sharing).' }
    $proc = Get-TargetProcess
    Write-Output ("Capturing modules of pid={0} ({1}) ..." -f $proc.Id, $proc.ProcessName)
    try {
        $mods = $proc.Modules
    } catch {
        throw "Failed to enumerate modules ($($_.Exception.Message)). If the client runs elevated, run this script as administrator."
    }
    $rows = foreach ($m in $mods) {
        $vi = $m.FileVersionInfo
        [pscustomobject]@{
            ModuleName      = $m.ModuleName
            FileName        = $m.FileName
            BaseAddress     = ('0x{0:X}' -f [int64]$m.BaseAddress)
            ModuleSize      = $m.ModuleMemorySize
            FileDescription = $vi.FileDescription
            FileVersion     = $vi.FileVersion
            CompanyName     = $vi.CompanyName
            ProductName     = $vi.ProductName
        }
    }
    $csv = Join-Path $SnapDir ("{0}-{1}.csv" -f $Tag, (Get-Date -Format 'yyyyMMdd-HHmmss'))
    $rows | Export-Csv -Path $csv -NoTypeInformation -Encoding UTF8
    Write-Output ("Snapshot saved: {0} ({1} modules)" -f $csv, $rows.Count)

    $hits = $rows | Where-Object { $_.ModuleName -match $CandidateRegex }
    if ($hits) {
        Write-Output "Candidate modules (capture/encode/color keywords):"
        $hits | ForEach-Object { Write-Output ("  [CAND] {0}  <- {1}  v{2}" -f $_.ModuleName, $_.CompanyName, $_.FileVersion) }
    }
    exit 0
}

if ($Diff) {
    $parts = $Tag.Split(',')
    if ($parts.Count -ne 2) { throw '-Diff requires -Tag "beforeTag,afterTag" (e.g. idle,sharing).' }
    $aTag, $bTag = $parts
    $aFile = Get-ChildItem $SnapDir -Filter "$aTag-*.csv" | Sort-Object LastWriteTime | Select-Object -Last 1
    $bFile = Get-ChildItem $SnapDir -Filter "$bTag-*.csv" | Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $aFile -or -not $bFile) { throw "Snapshot CSV for '$aTag' and/or '$bTag' not found in $SnapDir" }
    $a = Import-Csv $aFile.FullName
    $b = Import-Csv $bFile.FullName
    $aNames = @{}; foreach ($r in $a) { $aNames[$r.ModuleName.ToLower()] = $r }
    $bNames = @{}; foreach ($r in $b) { $bNames[$r.ModuleName.ToLower()] = $r }

    $added = @($b | Where-Object { -not $aNames.ContainsKey($_.ModuleName.ToLower()) })
    $removed = @($a | Where-Object { -not $bNames.ContainsKey($_.ModuleName.ToLower()) })

    $report = New-Object System.Text.StringBuilder
    [void]$report.AppendLine("# 模块清单 diff：$aTag -> $bTag")
    [void]$report.AppendLine("")
    [void]$report.AppendLine("- before: $($aFile.Name) ($($a.Count) modules)")
    [void]$report.AppendLine("- after : $($bFile.Name) ($($b.Count) modules)")
    [void]$report.AppendLine("")
    [void]$report.AppendLine("## 新增（$($added.Count)）")
    [void]$report.AppendLine("")
    [void]$report.AppendLine("| 模块 | 路径 | 版本 | 厂商 | 候选 |")
    [void]$report.AppendLine("| --- | --- | --- | --- | --- |")
    foreach ($r in $added) {
        $cand = if ($r.ModuleName -match $CandidateRegex) { '**YES**' } else { '' }
        [void]$report.AppendLine("| $($r.ModuleName) | $($r.FileName) | $($r.FileVersion) | $($r.CompanyName) | $cand |")
    }
    [void]$report.AppendLine("")
    [void]$report.AppendLine("## 移除（$($removed.Count)）")
    [void]$report.AppendLine("")
    foreach ($r in $removed) { [void]$report.AppendLine("- $($r.ModuleName)") }
    $md = Join-Path $SnapDir ("diff-{0}-vs-{1}-{2}.md" -f $aTag, $bTag, (Get-Date -Format 'yyyyMMdd-HHmmss'))
    Set-Content -Path $md -Value $report.ToString() -Encoding UTF8

    Write-Output ("Added: {0}  Removed: {1}" -f $added.Count, $removed.Count)
    $added | Where-Object { $_.ModuleName -match $CandidateRegex } |
        ForEach-Object { Write-Output ("  [CAND+] {0}  <- {1}" -f $_.ModuleName, $_.FileName) }
    Write-Output "Diff report: $md"
    exit 0
}

Write-Output 'Nothing to do. Use -Snapshot -Tag <name> or -Diff beforeTag,afterTag.'
