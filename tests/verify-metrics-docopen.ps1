param(
    [string]$ExePath = 'build-win\qcnote.exe',
    [int]$WaitSeconds = 5,
    [double]$ThresholdMB = 30,
    [int]$DocCount = 3
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null
$ProgressPreference = 'SilentlyContinue'

# =====================================================================
# qcnote 开文档内存指标测量（TC-I08 第二项：开典型文档 ≤ 30 MB）
# 姊妹脚本：tools\verify-metrics.ps1（空壳 < 10 MB + 热启动 < 200 ms）。
# 本脚本补齐测试用例 TC-I08 的「打开若干典型小笔记文件后静置读取」：
# - 内存口径 = 私有工作集（Working Set - Private，任务管理器默认内存列）
# - 防扰口径 = --noactivate（SW_SHOWNOACTIVATE：窗口可见不抢前台焦点）
# - 隔离口径 = --userdir 临时目录（不读写真实用户会话/草稿）
# - 典型文档 = 3 个中英混排小笔记（UTF-8，多行，混合换行风格）
# =====================================================================

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($ExePath)) {
    $ExePath = Join-Path $root $ExePath
}
if (-not (Test-Path $ExePath)) {
    Write-Output "[FATAL] exe not found: $ExePath"
    exit 2
}
$ExePath = (Resolve-Path $ExePath).Path

# 临时工作目录：按本脚本实例唯一化（脚本目录\docopen-metrics-<pid>）
$work = Join-Path $env:TEMP ("qcnote-docopen-metrics-{0}" -f $PID)
$userdir = Join-Path $work 'userdir'
New-Item -ItemType Directory -Force -Path $userdir | Out-Null

$fail = $false
$proc = $null
try {
    # 生成典型小笔记（中文多行 / 中英混排 CRLF / 短行 LF——覆盖混合换行风格）
    $n1 = (1..200  | ForEach-Object { "第$_行：中文内容测试 abc123，标点与空格 混合。" }) -join "`n"
    $n2 = (1..150  | ForEach-Object { "note line $_ - mixed EN/中文 content" }) -join "`r`n"
    $n3 = (1..300  | ForEach-Object { "短行$_" }) -join "`n"
    $files = @()
    $i = 0
    foreach ($content in @($n1, $n2, $n3)) {
        $i++
        $p = Join-Path $work ("note$i.txt")
        [System.IO.File]::WriteAllText($p, $content, (New-Object System.Text.UTF8Encoding($false)))
        $files += $p
    }
    $files = $files | Select-Object -First $DocCount

    $args = '--noactivate' + " --userdir=$userdir " + (($files | ForEach-Object { '"{0}"' -f $_ }) -join ' ')
    $proc = [System.Diagnostics.Process]::Start($ExePath, $args)
    Start-Sleep -Seconds $WaitSeconds
    if ($proc.HasExited) {
        Write-Output ("[FAIL] 进程在静置期内退出，ExitCode={0}" -f $proc.ExitCode)
        $fail = $true
    }
    else {
        # 按 PID 匹配性能计数器实例（多实例兜底取最大值，测值偏保守）
        $instances = Get-Counter '\Process(qcnote*)\Working Set - Private' -ErrorAction SilentlyContinue
        $bytes = $null
        if ($instances) {
            $ids = Get-Counter '\Process(qcnote*)\ID Process' -ErrorAction SilentlyContinue
            for ($i = 0; $i -lt $instances.CounterSamples.Count; $i++) {
                $pidVal = if ($ids) { [int64]$ids.CounterSamples[$i].CookedValue } else { -1 }
                if ($pidVal -eq $proc.Id) { $bytes = $instances.CounterSamples[$i].CookedValue; break }
            }
            if (-not $bytes) {
                $bytes = ($instances.CounterSamples | Measure-Object -Property CookedValue -Maximum).Maximum
                Write-Output "（注：未按实例 Id 匹配到目标进程，已取多实例最大值，测值偏保守）"
            }
        }
        if (-not $bytes) {
            $proc.Refresh()
            $bytes = $proc.PrivateMemorySize64
            Write-Output "（注：性能计数器不可用，已使用 Process.PrivateMemorySize64 近似）"
        }
        $mb = [math]::Round($bytes / 1MB, 1)
        $verdict = if ($mb -le $ThresholdMB) { 'PASS' } else { 'FAIL' }
        Write-Output ("开 {0} 文档私有工作集: {1} MB (阈值 {2} MB) -> {3}" -f $files.Count, $mb, $ThresholdMB, $verdict)
        if ($verdict -eq 'FAIL') { $fail = $true }
    }
}
finally {
    if ($proc -and -not $proc.HasExited) {
        $proc.CloseMainWindow() | Out-Null
        if (-not $proc.WaitForExit(2000)) { $proc.Kill() }
        $proc.Dispose()
    }
    if (Test-Path $work) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}

Write-Output ""
if ($fail) { Write-Output '总结: FAIL'; exit 1 } else { Write-Output '总结: PASS'; exit 0 }
