param(
    [string]$ExePath = 'build-win\qcnote.exe',
    [int]$WaitSeconds = 5,
    [double]$ThresholdMB = 10,
    # M4 起正常运行会读写用户目录会话/草稿（%LOCALAPPDATA%\qcnote）；
    # 传入临时目录经 --userdir 测试钩子隔离，防污染真实用户数据（可省略）
    [string]$UserDir = ''
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null
$ProgressPreference = 'SilentlyContinue'

# =====================================================================
# qcnote 内存 / 热启动指标测量脚本
#
# 口径说明：
# - 内存口径 = 私有工作集（Working Set - Private），即任务管理器默认
#   「内存」列的口径。非阈值判定依据需求：
#   空闲空壳 < 10MB、开文档 <= 30MB（见 docs/需求文档/需求-Win32全量重写.md）。
# - 热启动口径 = 同一 exe 预热 1 次后，正式测量 5 次取中值。
#   计时：System.Diagnostics.Stopwatch，从 Process.Start 到
#   「主窗口就绪」——判定条件为 EnumWindows 轮询主窗口可见
#   （IsWindowVisible，5ms 粒度，超时 3s 记为失败）。
# - 冷启动（重启系统后首次启动，含杀软扫描）需单独人工测量，
#   本脚本只覆盖热启动。
# - 防扰口径：全程 --noactivate（SW_SHOWNOACTIVATE——窗口显示但不抢前台
#   焦点）。启动计时「窗口就绪」判定改为 EnumWindows 轮询主窗口可见
#   （IsWindowVisible）——原 WaitForInputIdle+MainWindowHandle 双条件在
#   未激活窗口下返回偏慢（实测 ~2 倍：非前台线程的 idle 语义延迟），
#   可见性轮询直接度量「进程启动 → 主窗口可见」，对激活状态不敏感，
#   口径更诚实且不打扰用户；任务栏图标短暂出现几秒，不抢焦点。
# =====================================================================

$ErrorActionPreference = 'Stop'
$fail = $false
$root = Split-Path -Parent $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($ExePath)) {
    $ExePath = Join-Path $root $ExePath
}
if (-not (Test-Path $ExePath)) {
    Write-Output "[FATAL] exe not found: $ExePath"
    exit 2
}
$ExePath = (Resolve-Path $ExePath).Path

# ---------------- 内存测量 ----------------
Write-Output "=== 内存测量（私有工作集 = 任务管理器默认内存列口径）==="
$proc = $null
try {
    $memStartArgs = '--noactivate' + $(if ($UserDir) { " --userdir=$UserDir" } else { '' })
    $proc = [System.Diagnostics.Process]::Start($ExePath, $memStartArgs)
    Start-Sleep -Seconds $WaitSeconds
    if ($proc.HasExited) {
        Write-Output ("[FAIL] 进程在静置期内退出，ExitCode={0}" -f $proc.ExitCode)
        $fail = $true
    }
    else {
        # 读取计数器；可能有多个 qcnote 实例，按实例 Id 匹配启动的那个，
        # 匹配不到时取最大值并注明（此时含其他实例，测值偏保守偏大）。
        $instances = Get-Counter '\Process(qcnote*)\Working Set - Private' -ErrorAction SilentlyContinue
        $bytes = $null
        $matched = $false
        if ($instances) {
            $ids = Get-Counter '\Process(qcnote*)\ID Process' -ErrorAction SilentlyContinue
            for ($i = 0; $i -lt $instances.CounterSamples.Count; $i++) {
                $pidVal = if ($ids) { [int64]$ids.CounterSamples[$i].CookedValue } else { -1 }
                if ($pidVal -eq $proc.Id) {
                    $bytes = $instances.CounterSamples[$i].CookedValue
                    $matched = $true
                    break
                }
            }
            if (-not $matched) {
                # pdh 实例顺序兜底：取最大值（多实例时含其他进程，值偏大、判定保守）
                $bytes = ($instances.CounterSamples | Measure-Object -Property CookedValue -Maximum).Maximum
                Write-Output "（注：未按实例 Id 匹配到目标进程，已取多实例最大值，测值偏保守）"
            }
        }
        if (-not $bytes) {
            # 最终兜底：直接读进程对象（WorkingSet64 不完全等于私有工作集，仅注明）
            $proc.Refresh()
            $bytes = $proc.PrivateMemorySize64
            $matched = $true
            Write-Output "（注：性能计数器不可用，已使用 Process.PrivateMemorySize64 近似）"
        }
        $mb = [math]::Round($bytes / 1MB, 1)
        $verdict = if ($mb -lt $ThresholdMB) { 'PASS' } else { 'FAIL' }
        Write-Output ("私有工作集: {0} MB (阈值 {1} MB) -> {2}" -f $mb, $ThresholdMB, $verdict)
        if ($verdict -eq 'FAIL') { $fail = $true }
    }
}
finally {
    if ($proc -and -not $proc.HasExited) {
        $proc.CloseMainWindow() | Out-Null
        if (-not $proc.WaitForExit(2000)) { $proc.Kill() }
        $proc.Dispose()
    }
}

# ---------------- 热启动计时 ----------------
Write-Output ""
Write-Output "=== 热启动计时（预热 1 次 + 正式 5 次取中值，<200ms PASS）==="
$startArgsText = '--noactivate' + $(if ($UserDir) { " --userdir=$UserDir" } else { '' })

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class QcMetrics {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    // 主窗口可见判定（启动「窗口就绪」口径：进程启动 → 主窗口 IsWindowVisible）
    public static bool HasVisibleMain(uint pid) {
        bool found = false;
        EnumWindows((h, lp) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p == pid && IsWindowVisible(h)) {
                var c = new StringBuilder(256); GetClassName(h, c, 256);
                if (c.ToString() == "QcnoteMainWindow") { found = true; return false; }
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@

function Measure-OneStartup([string]$path) {
    # 返回耗时毫秒（int），失败返回 -1
    # 判定：EnumWindows 轮询主窗口可见（5ms 粒度；见文件头「防扰口径」注释——
    # WaitForInputIdle+MainWindowHandle 在 --noactivate 未激活态下偏慢 ~2 倍）
    $p = $null
    try {
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        $p = [System.Diagnostics.Process]::Start($path, $startArgsText)
        $deadline = [DateTime]::UtcNow.AddSeconds(3)
        while ([DateTime]::UtcNow -lt $deadline) {
            if ([QcMetrics]::HasVisibleMain([uint32]$p.Id)) {
                $sw.Stop()
                return [int]$sw.ElapsedMilliseconds
            }
            if ($p.HasExited) { return -1 }
            Start-Sleep -Milliseconds 5
        }
        return -1 # 3s 超时记失败
    }
    finally {
        if ($p -and -not $p.HasExited) {
            $p.CloseMainWindow() | Out-Null
            if (-not $p.WaitForExit(2000)) { $p.Kill() }
            $p.Dispose()
        }
    }
}

# 预热 1 次（不计数）
$warm = Measure-OneStartup $ExePath
Write-Output ("预热: {0} ms" -f $warm)

$samples = @()
for ($i = 1; $i -le 5; $i++) {
    $ms = Measure-OneStartup $ExePath
    $samples += $ms
    Write-Output ("第 {0} 次: {1} ms" -f $i, $ms)
}
$valid = $samples | Where-Object { $_ -ge 0 }
if ($valid.Count -lt 5) {
    Write-Output "[FAIL] 存在启动失败/超时样本，无法给出有效中值"
    $fail = $true
}
else {
    $sorted = $valid | Sort-Object
    $median = $sorted[[int](($sorted.Count - 1) / 2)]
    $verdict = if ($median -lt 200) { 'PASS' } else { 'FAIL' }
    Write-Output ("启动中值: {0} ms (阈值 200 ms) -> {1}" -f $median, $verdict)
    if ($verdict -eq 'FAIL') { $fail = $true }
}

Write-Output ""
if ($fail) { Write-Output '总结: FAIL'; exit 1 } else { Write-Output '总结: PASS'; exit 0 }
