# module_diff.ps1 — 进程模块清单快照与 diff
# 共享前抓一次 idle，共享中抓一次 sharing，再 diff 出新增/消失的模块。
# 支持多进程客户端（Electron 等）：快照覆盖所有同名进程，每行带 Pid。
# 用法：
#   powershell -File tools\module_diff.ps1 -Snapshot -Tag idle    -ProcessName HeyboxChat
#   powershell -File tools\module_diff.ps1 -Snapshot -Tag sharing -ProcessName HeyboxChat
#   powershell -File tools\module_diff.ps1 -Diff idle,sharing
param(
    [switch]$Snapshot,
    [switch]$Diff,
    [string]$Tag = '',
    [string]$ProcessName = 'HeyboxChat',
    [int]$ProcessId = 0,
    [string]$OutDir = ''
)
$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$SnapDir = Join-Path $RepoRoot 'docs\recon\module-snapshots'
if (-not (Test-Path $SnapDir)) { New-Item -ItemType Directory -Path $SnapDir -Force | Out-Null }

# 与捕获/编码/色彩相关的候选关键词（RTC SDK 场景）
$CandidateRegex = 'dxgi|d3d11|d3d9|dcomp|capture|mfplat|mfreadwrite|mf\.dll|mfh264|mfvdec|mfhevc|nvencode|nvml|avcodec|avutil|avformat|swscale|ffmpeg|webrtc|libyuv|obs|x264|openh264|nv12|i420|dxva|dwm|color|volcengine|vertc|bytertc|liteav|rtc|live_kit|overlay|liteav_screen'

function Get-TargetProcesses {
    if ($ProcessId -gt 0) { return @(Get-Process -Id $ProcessId -ErrorAction Stop) }
    $procs = @(Get-Process -Name "*$ProcessName*" -ErrorAction SilentlyContinue)
    if ($procs.Count -eq 0) { throw "No process matching name '$ProcessName' is running. Start the client first, or pass -ProcessName/-ProcessId." }
    return $procs
}

function Get-ModuleRows {
    param($Process)
    try {
        $mods = $Process.Modules
    } catch {
        Write-Warning ("pid={0}: failed to enumerate modules ({1}). If elevated, run as administrator." -f $Process.Id, $_.Exception.Message)
        return @()
    }
    $rows = @()
    foreach ($m in $mods) {
        $vi = $m.FileVersionInfo
        $rows += [pscustomobject]@{
            Pid             = $Process.Id
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
    return $rows
}

if ($Snapshot) {
    if (-not $Tag) { throw '-Snapshot requires -Tag (e.g. idle / sharing).' }
    $procs = Get-TargetProcesses
    Write-Output ("Capturing modules of {0} process(es) matching '{1}'..." -f $procs.Count, $ProcessName)
    $allRows = @()
    foreach ($proc in $procs) {
        $allRows += Get-ModuleRows -Process $proc
    }
    if ($allRows.Count -eq 0) { throw 'No modules captured from any process.' }
    $csv = Join-Path $SnapDir ("{0}-{1}.csv" -f $Tag, (Get-Date -Format 'yyyyMMdd-HHmmss'))
    $allRows | Export-Csv -Path $csv -NoTypeInformation -Encoding UTF8
    $pidCount = ($allRows | Select-Object -ExpandProperty Pid -Unique).Count
    Write-Output ("Snapshot saved: {0} ({1} modules across {2} processes)" -f $csv, $allRows.Count, $pidCount)

    $hits = $allRows | Where-Object { $_.ModuleName -match $CandidateRegex } |
        Select-Object ModuleName, CompanyName, FileVersion -Unique
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
    # 跨进程按模块名去重比较（同一 DLL 会加载进多个 Electron 进程）
    $aNames = @{}; foreach ($r in $a) { $aNames[$r.ModuleName.ToLower()] = $r }
    $bNames = @{}; foreach ($r in $b) { $bNames[$r.ModuleName.ToLower()] = $r }

    $added = @($b | Where-Object { -not $aNames.ContainsKey($_.ModuleName.ToLower()) } |
        Sort-Object ModuleName -Unique)
    $removed = @($a | Where-Object { -not $bNames.ContainsKey($_.ModuleName.ToLower()) } |
        Sort-Object ModuleName -Unique)

    $report = New-Object System.Text.StringBuilder
    [void]$report.AppendLine("# 模块清单 diff：$aTag -> $bTag")
    [void]$report.AppendLine("")
    [void]$report.AppendLine("- before: $($aFile.Name) ($($a.Count) 行, $(($a | Select-Object -ExpandProperty Pid -Unique).Count) 进程)")
    [void]$report.AppendLine("- after : $($bFile.Name) ($($b.Count) 行, $(($b | Select-Object -ExpandProperty Pid -Unique).Count) 进程)")
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
    [void]$report.AppendLine("| 模块 | 路径 |")
    [void]$report.AppendLine("| --- | --- |")
    foreach ($r in $removed) { [void]$report.AppendLine("| $($r.ModuleName) | $($r.FileName) |") }
    $md = Join-Path $SnapDir ("diff-{0}-vs-{1}-{2}.md" -f $aTag, $bTag, (Get-Date -Format 'yyyyMMdd-HHmmss'))
    Set-Content -Path $md -Value $report.ToString() -Encoding UTF8

    Write-Output ("Added: {0}  Removed: {1}" -f $added.Count, $removed.Count)
    $added | Where-Object { $_.ModuleName -match $CandidateRegex } |
        ForEach-Object { Write-Output ("  [CAND+] {0}  <- {1}" -f $_.ModuleName, $_.FileName) }
    Write-Output "Diff report: $md"
    exit 0
}

Write-Output 'Nothing to do. Use -Snapshot -Tag <name> or -Diff beforeTag,afterTag.'
