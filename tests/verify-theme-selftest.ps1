#Requires -Version 7
param(
    [string]$ExePath = 'build-win\qcnote.exe',
    [int]$TimeoutSec = 20
)

# =====================================================================
# qcnote M6 双主题自动化验证（E5）
#
# 第一部分 --selftest-theme（进程内自测，离屏 TOOLWINDOW + SW_SHOWNOACTIVATE，
#   全程零可见元素零前台抢占）：
#   - 浅→深→浅一轮：IsDarkTheme 翻转、主窗口/基础对话框/查找对话框的
#     DWMWA_USE_IMMERSIVE_DARK_MODE 读回断言；
#   - 像素采样：PrintWindow(PW_RENDERFULLCONTENT) 抓离屏窗口，标题栏/
#     工具栏/内容区（欢迎页）/状态栏四点命中两套 token（浅 #dde2ec/#f2f4f8/
#     #ffffff/#e8ebf1 ↔ 深 #171c24/#232833/#262b34/#1d222b）；
#   - Scintilla 配色（STYLEGETFORE/BACK、GETCARETFORE、INDICGETFORE/ALPHA）
#     按主题重设。
#
# 第二部分 持久化 e2e（--headless + --userdir 隔离，双进程重启）：
#   TC 切换深色 → 热退出（WM_CLOSE）→ 重启 → 断言仍深色（dump darkTheme +
#   ini [editor] darkTheme=true）；再切回浅色 → 重启 → 断言浅色（显式 false
#   落盘阻却旧 true 残留）。主题切换经 WM_APP+7 命令钩子（wParam=4，与视图
#   菜单「深色主题」同一 RouteCommand 路径）。
#
# 用法：pwsh.exe -NoProfile -ExecutionPolicy Bypass -File tests\verify-theme-selftest.ps1
# 退出码：0 = 通过；1 = 失败；2 = 前置缺失
# =====================================================================

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null
$ProgressPreference = 'SilentlyContinue'

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

$script:Fail = $false
function Assert-Cond([string]$case, [bool]$cond, [string]$detail = '') {
    $tag = if ($cond) { 'PASS' } else { 'FAIL' }
    if (-not $cond) { $script:Fail = $true }
    Write-Output ("[{0}] {1}{2}" -f $tag, $case, $(if ($detail) { "  ($detail)" } else { '' }))
}

# ---------------- Win32 P/Invoke（会话 e2e 复用同款） ----------------
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Collections.Generic;
public class QcThemeE2e {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
    public static List<IntPtr> WindowsOfPid(uint target, string cls) {
        var list = new List<IntPtr>();
        EnumWindows((h, lp) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid == target) {
                var sb = new StringBuilder(256);
                GetClassName(h, sb, 256);
                if (sb.ToString() == cls) list.Add(h);
            }
            return true;
        }, IntPtr.Zero);
        return list;
    }
}
"@

$WM_CLOSE_MSG = 0x0010
$MSG_DUMP = 0x8006      # WM_APP + 6（kMsgSelftestDump）
$MSG_COMMAND = 0x8007   # WM_APP + 7（kMsgSelftestCommand）
$CMD_DARK = 4           # ToggleDarkTheme（= 视图菜单「深色主题」）

# =====================================================================
# 第一部分：--selftest-theme 进程内自测
# =====================================================================
Write-Output '== part 1: --selftest-theme (in-process, offscreen) =='
$result = Join-Path $root 'build-win\theme-selftest-result.txt'
if (Test-Path $result) { Remove-Item $result -Force }
$proc = $null
try {
    $proc = Start-Process -FilePath $ExePath -ArgumentList @('--headless', ('--selftest-theme="{0}"' -f $result)) -PassThru
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        Write-Output "[FAIL] selftest 超时（${TimeoutSec}s 未退出），强制 Kill"
        if (-not $proc.HasExited) { $proc.Kill() }
        $script:Fail = $true
    }
    elseif (-not (Test-Path $result)) {
        Write-Output ("[FAIL] 结果文件未生成（进程退出码 {0}）" -f $proc.ExitCode)
        $script:Fail = $true
    }
    else {
        foreach ($line in Get-Content $result) { Write-Output $line }
        $last = (Get-Content $result | Select-Object -Last 1)
        if ($last -notmatch 'overall: PASS') { $script:Fail = $true }
    }
}
finally {
    if ($proc -and -not $proc.HasExited) {
        $proc.CloseMainWindow() | Out-Null
        if (-not $proc.WaitForExit(2000)) { $proc.Kill() }
        $proc.Dispose()
    }
}

# =====================================================================
# 第二部分：持久化 e2e（切换深色 → 重启仍深色；切回浅色 → 重启仍浅色）
# =====================================================================
Write-Output ''
Write-Output '== part 2: theme persistence across restart (--headless + isolated --userdir) =='

$TempRoot = 'D:\temp\aiRunner\opencode\qcnote-win32-rewrite-20261110183000\e2e-theme'
Remove-Item -Recurse -Force $TempRoot -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null
$userDir = Join-Path $TempRoot 'user'
$dumpPath = Join-Path $TempRoot 'dump.json'
$iniPath = Join-Path $userDir 'qcnote.ini'

function Wait-MainWindow([System.Diagnostics.Process]$p, [int]$timeoutMs = 10000) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        if ($p.HasExited) { throw "进程提前退出（ExitCode=$($p.ExitCode)）" }
        $wins = [QcThemeE2e]::WindowsOfPid([uint32]$p.Id, 'QcnoteMainWindow')
        if ($wins.Count -gt 0) { return $wins[0] }
        Start-Sleep -Milliseconds 50
    }
    throw "10s 内未找到主窗口"
}

function Get-DumpJson([IntPtr]$main) {
    Remove-Item $dumpPath -Force -ErrorAction SilentlyContinue
    $r = [IntPtr]::Zero
    [QcThemeE2e]::SendMessageTimeout($main, $MSG_DUMP, [IntPtr]::Zero, [IntPtr]::Zero, 2, 5000, [ref]$r) | Out-Null
    if (Test-Path $dumpPath) {
        return Get-Content $dumpPath -Raw -Encoding utf8 | ConvertFrom-Json
    }
    return $null
}

function Send-DarkToggle([IntPtr]$main) {
    $r = [IntPtr]::Zero
    [QcThemeE2e]::SendMessageTimeout($main, $MSG_COMMAND, [IntPtr]$CMD_DARK, [IntPtr]::Zero, 2, 5000, [ref]$r) | Out-Null
    Start-Sleep -Milliseconds 150
}

function Invoke-App([string[]]$extraArgs = @()) {
    $args = @('--headless', "--userdir=$userDir", "--selftest-dump=$dumpPath") + $extraArgs
    $p = Start-Process -FilePath $ExePath -ArgumentList $args -PassThru -WorkingDirectory $TempRoot
    $main = Wait-MainWindow $p
    return @{ Proc = $p; Main = $main }
}

function Stop-App([hashtable]$app, [int]$timeoutMs = 5000) {
    [QcThemeE2e]::SendMessage($app.Main, $WM_CLOSE_MSG, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    if (-not $app.Proc.WaitForExit($timeoutMs)) {
        $app.Proc.Kill()
        $app.Proc.WaitForExit(3000) | Out-Null
        throw "进程未在 ${timeoutMs}ms 内退出"
    }
}

function Get-IniDarkTheme() {
    if (-not (Test-Path $iniPath)) { return $null }
    $inEditor = $false
    foreach ($line in Get-Content $iniPath -Encoding UTF8) {
        if ($line -match '^\s*\[(.+)\]\s*$') { $inEditor = ($Matches[1] -eq 'editor'); continue }
        if ($inEditor -and $line -match '^\s*darkTheme\s*=\s*(\S+)') { return $Matches[1] }
    }
    return $null
}

$app1 = $null
$app2 = $null
$app3 = $null
try {
    # —— 启动 1：默认浅色 → 切深色 → 断言 → 热退出 ——
    $app1 = Invoke-App
    $d1a = Get-DumpJson $app1.Main
    Assert-Cond 'fresh start is light (dump darkTheme=false)' ($d1a -and $d1a.darkTheme -eq $false)
    Send-DarkToggle $app1.Main
    $d1b = Get-DumpJson $app1.Main
    Assert-Cond 'toggle flips theme (dump darkTheme=true)' ($d1b -and $d1b.darkTheme -eq $true)
    Stop-App $app1

    # —— 启动 2：重启 → 断言仍深色（dump + ini 双口径）——
    $app2 = Invoke-App
    $d2 = Get-DumpJson $app2.Main
    Assert-Cond 'restart restores dark theme (dump darkTheme=true)' ($d2 -and $d2.darkTheme -eq $true)
    $iniDark = Get-IniDarkTheme
    Assert-Cond 'session ini records darkTheme=true' ($iniDark -eq 'true') "ini darkTheme=$iniDark"
    # 切回浅色再退出
    Send-DarkToggle $app2.Main
    $d2b = Get-DumpJson $app2.Main
    Assert-Cond 'toggle back to light (dump darkTheme=false)' ($d2b -and $d2b.darkTheme -eq $false)
    Stop-App $app2

    # —— 启动 3：重启 → 断言浅色（显式 false 落盘，无旧 true 残留）——
    $app3 = Invoke-App
    $d3 = Get-DumpJson $app3.Main
    Assert-Cond 'restart stays light after switching back (dump darkTheme=false)' ($d3 -and $d3.darkTheme -eq $false)
    $iniLight = Get-IniDarkTheme
    Assert-Cond 'session ini records darkTheme=false explicitly' ($iniLight -eq 'false') "ini darkTheme=$iniLight"
    Stop-App $app3
    $app3 = $null
}
finally {
    foreach ($a in @($app1, $app2, $app3)) {
        if ($a -and $a.Proc -and -not $a.Proc.HasExited) {
            $a.Proc.Kill()
            $a.Proc.WaitForExit(3000) | Out-Null
        }
    }
}

Write-Output ''
if ($script:Fail) { Write-Output '总结: FAIL'; exit 1 } else { Write-Output '总结: PASS'; exit 0 }
