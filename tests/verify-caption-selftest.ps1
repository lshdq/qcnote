#Requires -Version 7
param(
    [string]$ExePath = 'build-win\qcnote.exe',
    [int]$TimeoutSec = 60,
    [switch]$KeepTemp
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null
$ProgressPreference = 'SilentlyContinue'

# =====================================================================
# qcnote「标题栏按钮点击穿透修复」自动化验证（headless 消息驱动）
#
# 覆盖（对齐 .localdoc\测试用例\测试用例-标题栏按钮点击穿透修复.md）：
#   TC-002 按下关闭按钮不销毁窗口：post WM_NCLBUTTONDOWN(HTCLOSE，
#     lParam=关闭按钮中心屏幕坐标) → 进程存活、主窗口 IsWindow、UI 线程
#     持有鼠标捕获（GetGUIThreadInfo.hwndCapture == 主窗口，SetCapture 生效，
#     修复的关键机制）。旧「down 即执行」实现此时已销毁窗口退出。
#   TC-003 松开才发起关闭：紧接 post WM_LBUTTONUP（lParam=关闭按钮中心
#     客户区坐标，捕获会话的客户端消息形态）→ WM_CLOSE 正常发起（草稿/
#     会话落盘后退出），进程正常退出（ExitCode=0）。
#   TC-004 拖离取消语义：post WM_NCLBUTTONDOWN(HTCLOSE) → WM_MOUSEMOVE
#     （按钮外客户区坐标）→ WM_LBUTTONUP（按钮外）→ 进程存活、窗口在、
#     捕获已释放（无动作）。
#   TC-004b 会话复位回归：取消后完整 down+up（同按钮）仍能正常关闭——
#     证明取消路径已清空按下状态、不阻塞后续点击。
#
# 按钮几何数学（与 verify-linenumbers-selftest.ps1 同款）：
#   关闭按钮中心（屏幕）= (窗口右缘 - Scale(44)/2, 窗口顶 + Scale(40)/2)；
#   客户区 = 整窗（WM_NCCALCSIZE 返回 0），客户区坐标按 GetClientRect 直接取。
#
# 防扰约定：全程 --headless（主窗口离屏 -32000、TOOLWINDOW 无任务栏项、
#   SW_SHOWNOACTIVATE 零前台抢占）；--userdir 隔离单实例域与用户数据。
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

$TempRoot = 'D:\temp\aiRunner\opencode\qcnote-caption-selftest-20261124'
if (-not $KeepTemp) {
    Remove-Item -Recurse -Force $TempRoot -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null
$UserDir = Join-Path $TempRoot 'user'

# ---------------- Win32 P/Invoke ----------------
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Collections.Generic;
public class QcCaptionE2e {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint idThread, ref GUITHREADINFO pgi);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO {
        public int cbSize; public uint flags;
        public IntPtr hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret;
        public RECT rcCaret;
    }
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
    // 跨进程查询窗口所属 UI 线程当前持有的鼠标捕获窗口（SetCapture 观测口径；
    // GetCapture 仅查调用线程，跨进程必须走 GetGUIThreadInfo）
    public static IntPtr CaptureOf(IntPtr hwnd) {
        uint pid; uint tid = GetWindowThreadProcessId(hwnd, out pid);
        GUITHREADINFO gi = new GUITHREADINFO();
        gi.cbSize = Marshal.SizeOf(typeof(GUITHREADINFO));
        if (!GetGUIThreadInfo(tid, ref gi)) return IntPtr.Zero;
        return gi.hwndCapture;
    }
}
"@

# ---------------- 常量（与 exe/vendor 头文件对齐） ----------------
$WM_MOUSEMOVE     = 0x0200
$WM_LBUTTONUP     = 0x0202
$WM_NCLBUTTONDOWN = 0x00A1
$HTCLOSE          = 20      # 关闭按钮 NCHITTEST 命中值（titlebar.cpp ComputeCaptionHitTest）

# 标题栏度量（src/ui/theme.h metrics；逻辑 px）
$CaptionBtnWidth = 44
$CaptionHeight   = 40

# ---------------- 纯函数复刻（与 src/ui/theme.{h,cpp} 一致） ----------------
function MulDiv([int]$n, [int]$m, [int]$d) {
    [int][Math]::Round([double]($n * $m) / $d, [MidpointRounding]::AwayFromZero)
}
function ScaleLogical([int]$logical, [uint32]$dpi) { MulDiv $logical ([int]$dpi) 96 }
function MakeLParam([int]$x, [int]$y) {
    $sx = [int][short]($x -band 0xFFFF)
    $sy = [int][short]($y -band 0xFFFF)
    [IntPtr][int]((($sy -shl 16) -bor ($sx -band 0xFFFF)))
}

# ---------------- 断言收集 ----------------
$script:Results = @()
function Assert-Cond([string]$case, [bool]$cond, [string]$detail = '') {
    $script:Results += [pscustomobject]@{ Case = $case; Pass = $cond; Detail = $detail }
    $tag = if ($cond) { 'PASS' } else { 'FAIL' }
    Write-Output ("  [{0}] {1}{2}" -f $tag, $case, $(if ($detail) { "  ($detail)" } else { '' }))
}

# ---------------- 进程与窗口驱动 ----------------
$script:Procs = @()
function Start-App {
    $p = Start-Process -FilePath $ExePath `
        -ArgumentList @('--headless', "--userdir=$UserDir") `
        -PassThru -WorkingDirectory $TempRoot
    $script:Procs += $p
    return $p
}
function Wait-MainWindow([System.Diagnostics.Process]$proc, [int]$timeoutMs = 10000) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        if ($proc.HasExited) { throw "进程提前退出（ExitCode=$($proc.ExitCode)）" }
        $wins = [QcCaptionE2e]::WindowsOfPid([uint32]$proc.Id, 'QcnoteMainWindow')
        if ($wins.Count -gt 0) { return $wins[0] }
        Start-Sleep -Milliseconds 50
    }
    throw "10s 内未找到主窗口"
}
function Wait-CaptureHeld([IntPtr]$main, [int]$timeoutMs = 3000) {
    # 等待 UI 线程捕获建立（SetCapture 在按下处理内执行；轮询至 hwndCapture == 主窗口）
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        $cap = [QcCaptionE2e]::CaptureOf($main)
        if ($cap -eq $main) { return $true }
        Start-Sleep -Milliseconds 30
    }
    return ([QcCaptionE2e]::CaptureOf($main) -eq $main)
}
function Wait-CaptureReleased([IntPtr]$main, [int]$timeoutMs = 3000) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        if ([QcCaptionE2e]::CaptureOf($main) -eq [IntPtr]::Zero) { return $true }
        Start-Sleep -Milliseconds 30
    }
    return ([QcCaptionE2e]::CaptureOf($main) -eq [IntPtr]::Zero)
}
function Get-Geometry([IntPtr]$main, [uint32]$dpi) {
    # 返回：关闭按钮中心（屏幕/客户区两套坐标）+ 标题栏中部按钮外客户区坐标
    $wr = New-Object QcCaptionE2e+RECT
    [QcCaptionE2e]::GetWindowRect($main, [ref]$wr) | Out-Null
    $cr = New-Object QcCaptionE2e+RECT
    [QcCaptionE2e]::GetClientRect($main, [ref]$cr) | Out-Null
    $btnW = ScaleLogical $CaptionBtnWidth $dpi
    $capH = ScaleLogical $CaptionHeight $dpi
    $scr = New-Object QcCaptionE2e+POINT
    $scr.X = $wr.Right - [int]($btnW / 2)
    $scr.Y = $wr.Top + [int]($capH / 2)
    $cli = New-Object QcCaptionE2e+POINT
    $cli.X = $scr.X; $cli.Y = $scr.Y
    [QcCaptionE2e]::ScreenToClient($main, [ref]$cli) | Out-Null
    $outside = New-Object QcCaptionE2e+POINT
    $outside.X = [int](($cr.Left + $cr.Right) / 2)   # 标题栏中部（Tab 区，按钮矩形外）
    $outside.Y = [int]($capH / 2)
    return @{ Screen = $scr; Client = $cli; Outside = $outside }
}
function Post-NcDownClose([IntPtr]$main, $geo) {
    # WM_NCLBUTTONDOWN(HTCLOSE)：lParam = 关闭按钮中心屏幕坐标
    [QcCaptionE2e]::PostMessage($main, $WM_NCLBUTTONDOWN, [IntPtr]$HTCLOSE,
        (MakeLParam $geo.Screen.X $geo.Screen.Y)) | Out-Null
}
function Wait-Exit([System.Diagnostics.Process]$proc, [IntPtr]$main) {
    # WM_CLOSE → 草稿/会话落盘 → DestroyWindow → PostQuitMessage(0)（C4 热退出）
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        if (-not $proc.HasExited) { $proc.Kill() }
        throw "松开后 ${TimeoutSec}s 未退出（关闭动作未生效）"
    }
    return $proc.ExitCode
}

# =====================================================================
# 用例执行
# =====================================================================
$fail = $false
$app1 = $null; $app2 = $null
try {
    # ================= 组 1：TC-002 按下不销毁 + TC-003 松开才关闭 =================
    Write-Output '== 组 1：TC-002 按下关闭按钮不销毁窗口 + TC-003 松开才发起关闭 =='
    $app1 = Start-App
    $main1 = Wait-MainWindow $app1
    $dpi1 = [QcCaptionE2e]::GetDpiForWindow($main1)
    if ($dpi1 -eq 0) { $dpi1 = 96 }
    Write-Output ("主窗口就绪 pid={0} dpi={1}" -f $app1.Id, $dpi1)
    $geo1 = Get-Geometry $main1 $dpi1

    # ---- TC-002：按下（WM_NCLBUTTONDOWN HTCLOSE）只记录 + 捕获，不销毁 ----
    Post-NcDownClose $main1 $geo1
    $capOk = Wait-CaptureHeld $main1
    Start-Sleep -Milliseconds 300   # 留出消息消化裕量（旧实现在此窗口早已销毁退出）
    Assert-Cond 'TC-02 按下后进程存活（未退出 = 未发起关闭）' (-not $app1.HasExited) "exited=$($app1.HasExited)"
    Assert-Cond 'TC-02 按下后主窗口仍有效（IsWindow，未销毁）' ([QcCaptionE2e]::IsWindow($main1)) ''
    Assert-Cond 'TC-02 按下会话持有鼠标捕获（SetCapture 生效 = 穿透修复关键机制）' $capOk ("capture=0x{0:X}" -f ([QcCaptionE2e]::CaptureOf($main1)).ToInt64())

    # ---- TC-003：松开（WM_LBUTTONUP，客户区形态）→ WM_CLOSE → 正常退出 ----
    [QcCaptionE2e]::PostMessage($main1, $WM_LBUTTONUP, [IntPtr]::Zero,
        (MakeLParam $geo1.Client.X $geo1.Client.Y)) | Out-Null
    $code1 = Wait-Exit $app1 $main1
    Assert-Cond 'TC-03 同按钮松开后进程正常退出（关闭动作生效）' ($code1 -eq 0) "exitCode=$code1"

    # ================= 组 2：TC-004 拖离取消 + TC-004b 会话复位回归 =================
    Write-Output ''
    Write-Output '== 组 2：TC-004 拖离取消语义 + 会话复位后完整点击仍可关闭 =='
    $app2 = Start-App
    $main2 = Wait-MainWindow $app2
    $dpi2 = [QcCaptionE2e]::GetDpiForWindow($main2)
    if ($dpi2 -eq 0) { $dpi2 = 96 }
    $geo2 = Get-Geometry $main2 $dpi2

    Post-NcDownClose $main2 $geo2
    if (-not (Wait-CaptureHeld $main2)) { throw 'TC-04 前置：按下会话未建立捕获' }
    # 拖离：移动 + 松开都在按钮外（客户区坐标）
    [QcCaptionE2e]::PostMessage($main2, $WM_MOUSEMOVE, [IntPtr]::Zero,
        (MakeLParam $geo2.Outside.X $geo2.Outside.Y)) | Out-Null
    Start-Sleep -Milliseconds 200
    [QcCaptionE2e]::PostMessage($main2, $WM_LBUTTONUP, [IntPtr]::Zero,
        (MakeLParam $geo2.Outside.X $geo2.Outside.Y)) | Out-Null
    Start-Sleep -Milliseconds 400   # 留出消息消化裕量（若有误触发，退出流程已启动）
    $relOk = Wait-CaptureReleased $main2
    Assert-Cond 'TC-04 拖离后松开：进程存活（无动作）' (-not $app2.HasExited) "exited=$($app2.HasExited)"
    Assert-Cond 'TC-04 拖离后松开：主窗口仍有效' ([QcCaptionE2e]::IsWindow($main2)) ''
    Assert-Cond 'TC-04 拖离后松开：捕获已释放（按下状态收口）' $relOk ("capture=0x{0:X}" -f ([QcCaptionE2e]::CaptureOf($main2)).ToInt64())

    # ---- TC-004b：复位后完整点击（同按钮 down+up）仍正常关闭 ----
    Post-NcDownClose $main2 $geo2
    if (-not (Wait-CaptureHeld $main2)) { throw 'TC-04b 前置：第二次按下会话未建立捕获（会话未复位）' }
    [QcCaptionE2e]::PostMessage($main2, $WM_LBUTTONUP, [IntPtr]::Zero,
        (MakeLParam $geo2.Client.X $geo2.Client.Y)) | Out-Null
    $code2 = Wait-Exit $app2 $main2
    Assert-Cond 'TC-04b 取消后完整 down+up 仍正常关闭（会话复位，无阻塞）' ($code2 -eq 0) "exitCode=$code2"
}
catch {
    Write-Output ("[FATAL] {0}" -f $_.Exception.Message)
    $fail = $true
}
finally {
    foreach ($p in $script:Procs) {
        try {
            if (-not $p.HasExited) {
                $p.CloseMainWindow() | Out-Null
                if (-not $p.WaitForExit(2000)) { $p.Kill() }
            }
            $p.Dispose()
        } catch { }
    }
    if (-not $KeepTemp) {
        Remove-Item -Recurse -Force $TempRoot -ErrorAction SilentlyContinue
    }
}

Write-Output ''
$bad = ($script:Results | Where-Object { -not $_.Pass }).Count
if ($fail -or $bad -gt 0) {
    Write-Output ("总结: FAIL（失败 {0} 项）" -f $bad)
    exit 1
}
Write-Output ("总结: PASS（{0}/{0} 通过）" -f $script:Results.Count)
exit 0
