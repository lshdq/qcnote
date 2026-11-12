param(
    [string]$ExePath = 'build-win\qcnote.exe',
    [int]$TimeoutSec = 40,
    [switch]$KeepTemp
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null
$ProgressPreference = 'SilentlyContinue'

# =====================================================================
# qcnote 边缘命中透传 e2e（Win32界面体验修复 F6 / TC-004，兼采 F3/F6 编辑器态）
#
# 覆盖：
#   TC-004 编辑器子窗口边缘命中透传：headless 启动 + NewDocument（WM_APP+7）
#     后对 Scintilla 子窗口与主窗口分别直投 WM_NCHITTEST——断言子窗口在左/
#     右/底边缘与左下/右下角命中区返回 HTTRANSPARENT(-1)（交还父窗口），
#     主窗口同坐标返回 HTLEFT/HTRIGHT/HTBOTTOM/角值；编辑器内部区域不受
#     透传影响（子窗口正常 HTCLIENT）。最大化态无 resize 命中由 test_ui 的
#     ComputeCaptionHitTest(zoomed=true) 单测覆盖（真窗口 SC_MAXIMIZE 会
#     激活并显示窗口，违反测试防扰约定，故不在本脚本做可见最大化）。
#   TC-003（编辑器态）：SCI_GETELEMENTCOLOUR 采样失焦非活动选区底色，浅/
#     深两主题（WM_APP+7 code4 切换）断言 = BlendOver(内容底, 选区@0x55)
#     预合成且 alpha=0xFF。
#   TC-006（编辑器态）：直投主窗口 WM_LBUTTONDOWN 到换行按钮中心 →
#     SCI_GETWRAPMODE 即时 NONE↔WORD；重启（WM_CLOSE 落盘会话）后
#     NewDocument 编辑器继承 wordWrap=true（会话恢复 + 按钮态同步接线）。
#
# 防扰约定：全程 --headless（窗口不显示/不激活），驱动全为消息直投；
#   --userdir 独立目录隔离单实例域与用户数据，不触碰真实配置。
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

$TempRoot = 'D:\temp\aiRunner\opencode\qcnote-edge-hittest-e2e-20261111'
if (-not $KeepTemp) {
    Remove-Item -Recurse -Force $TempRoot -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null
$UserDir = Join-Path $TempRoot 'profile'
$DumpPath = Join-Path $TempRoot 'state-dump.json'

# ---------------- Win32 P/Invoke ----------------
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Collections.Generic;
public class QcEdgeE2e {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    public struct RECT { public int Left, Top, Right, Bottom; }
    public struct POINT { public int X, Y; }
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
    public static IntPtr Send(IntPtr h, uint msg, IntPtr w, IntPtr l) {
        IntPtr result = IntPtr.Zero;
        SendMessageTimeout(h, msg, w, l, 2 /*SMTO_ABORTIFHUNG*/, 5000, out result);
        return result;
    }
}
"@

# ---------------- 常量（与 exe 侧常量对齐） ----------------
$WM_NCHITTEST    = 0x0084
$WM_LBUTTONDOWN  = 0x0201
$WM_LBUTTONUP    = 0x0202
$WM_CLOSE_MSG    = 0x0010
$MSG_COMMAND     = 0x8007   # WM_APP + 7（kMsgSelftestCommand）
$CMD_NEW         = 1        # NewDocument（=Ctrl+N 命令函数）
$CMD_TOGGLE_DARK = 4        # ToggleDarkTheme
$MK_LBUTTON      = 1

$HT_TRANSPARENT = -1; $HT_CLIENT = 1
$HT_LEFT = 10;  $HT_RIGHT = 11; $HT_TOP = 12
$HT_TOPLEFT = 13; $HT_TOPRIGHT = 14
$HT_BOTTOM = 15; $HT_BOTTOMLEFT = 16; $HT_BOTTOMRIGHT = 17

$SCI_GETELEMENTCOLOUR = 2754
$SC_ELEMENT_SELECTION_INACTIVE_BACK = 17
$SCI_GETWRAPMODE = 2269
$SC_WRAP_NONE = 0; $SC_WRAP_WORD = 1

# 主题 token（src/ui/theme.cpp LightPalette/DarkPalette）
$LightContent = 0xFFFFFF; $LightSel = 0xADD6FF
$DarkContent  = 0x262B34; $DarkSel  = 0x264F78
# 工具栏度量（src/ui/toolbar.h）与标题栏高度（src/ui/theme.h metrics）
$CapH = 40; $TbHeight = 40; $BtnW = 44; $BtnH = 32; $LeftMargin = 4; $Spacing = 2

# ---------------- 纯函数复刻（与 src/ui/theme.{h,cpp} 一致） ----------------
function MulDiv([int]$n, [int]$m, [int]$d) {
    # Windows MulDiv：就近舍入（正数域）
    [int][Math]::Round([double]($n * $m) / $d, [MidpointRounding]::AwayFromZero)
}
function ScaleLogical([int]$logical, [uint32]$dpi) { MulDiv $logical ([int]$dpi) 96 }
function BlendOver([long]$base, [long]$overlay) {
    # src/ui/theme.cpp BlendOver：overlay alpha 通道，lround 混合
    $alpha = (($overlay -shr 24) -band 0xFF)
    if ($alpha -le 0)   { return ($base -bor 0xFF000000) }
    if ($alpha -ge 255) { return ($overlay -bor 0xFF000000) }
    $a = $alpha / 255.0
    $mix = { param($sh) [long][Math]::Round(
        ((($base -shr $sh) -band 0xFF) * (1.0 - $a)) +
        ((($overlay -shr $sh) -band 0xFF) * $a), [MidpointRounding]::AwayFromZero) }
    $r = & $mix 16; $g = & $mix 8; $b = & $mix 0
    return (0xFF000000 -bor ($r -shl 16) -bor ($g -shl 8) -bor $b)
}
function ToColorRef([long]$argb) {
    # src/ui/theme.cpp ToColorRef：0xAARRGGBB → 0x00BBGGRR
    ((($argb -shr 16) -band 0xFF) -bor ((($argb -shr 8) -band 0xFF) -shl 8) -bor (($argb -band 0xFF) -shl 16))
}
function MakeLParam([int]$x, [int]$y) {
    # 宏 MAKELPARAM：short 符号扩展后按 x 低 16 位打包（多显示器负坐标安全）。
    # 注意：[short] 直接 -shl 16 会按 16 位回绕（PS 保留 LHS 类型），必须先升位
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
        -ArgumentList @('--headless', "--userdir=$UserDir", "--selftest-dump=$DumpPath") `
        -PassThru -WorkingDirectory $TempRoot
    $script:Procs += $p
    return $p
}
function Wait-MainWindow([System.Diagnostics.Process]$proc, [int]$timeoutMs = 10000) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        if ($proc.HasExited) { throw "进程提前退出（ExitCode=$($proc.ExitCode)）" }
        $wins = [QcEdgeE2e]::WindowsOfPid([uint32]$proc.Id, 'QcnoteMainWindow')
        if ($wins.Count -gt 0) { return $wins[0] }
        Start-Sleep -Milliseconds 50
    }
    throw "10s 内未找到主窗口"
}
function Wait-Scintilla([IntPtr]$main, [int]$timeoutMs = 10000) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        $sci = [QcEdgeE2e]::FindWindowEx($main, [IntPtr]::Zero, 'Scintilla', $null)
        if ($sci -ne [IntPtr]::Zero) { return $sci }
        Start-Sleep -Milliseconds 50
    }
    throw "10s 内未找到 Scintilla 编辑器子窗口"
}
function Send-Command([IntPtr]$main, [int]$code) {
    [QcEdgeE2e]::Send($main, $MSG_COMMAND, [IntPtr]$code, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 150
}
function HitTest([IntPtr]$hwnd, [int]$sx, [int]$sy) {
    [int][QcEdgeE2e]::Send($hwnd, $WM_NCHITTEST, [IntPtr]::Zero, (MakeLParam $sx $sy))
}
function ClientPointToScreen([IntPtr]$hwnd, [int]$cx, [int]$cy) {
    $pt = New-Object QcEdgeE2e+POINT
    $pt.X = $cx; $pt.Y = $cy
    [QcEdgeE2e]::ClientToScreen($hwnd, [ref]$pt) | Out-Null
    return $pt
}
function Get-ClientSize([IntPtr]$hwnd) {
    $r = New-Object QcEdgeE2e+RECT
    [QcEdgeE2e]::GetClientRect($hwnd, [ref]$r) | Out-Null
    return @{ W = ($r.Right - $r.Left); H = ($r.Bottom - $r.Top) }
}
function Get-WrapMode([IntPtr]$sci) {
    [int][QcEdgeE2e]::Send($sci, $SCI_GETWRAPMODE, [IntPtr]::Zero, [IntPtr]::Zero)
}
function Get-InactiveSelectionRgba([IntPtr]$sci) {
    [long][QcEdgeE2e]::Send($sci, $SCI_GETELEMENTCOLOUR, [IntPtr]$SC_ELEMENT_SELECTION_INACTIVE_BACK, [IntPtr]::Zero)
}
function Stop-App([System.Diagnostics.Process]$proc, [IntPtr]$main) {
    # WM_CLOSE：exe 侧 C4 热退出（PersistenceActive 下 PersistSessionNow 落盘会话）
    [QcEdgeE2e]::Send($main, $WM_CLOSE_MSG, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        if (-not $proc.HasExited) { $proc.Kill() }
        throw "WM_CLOSE 后 ${TimeoutSec}s 未退出"
    }
}

# =====================================================================
# 用例执行
# =====================================================================
$fail = $false
try {
    $proc = Start-App
    $main = Wait-MainWindow $proc
    $dpi = [QcEdgeE2e]::GetDpiForWindow($main)
    if ($dpi -eq 0) { $dpi = 96 }
    Write-Output ("主窗口就绪 pid={0} dpi={1} zoomed={2}" -f $proc.Id, $dpi, ([bool][QcEdgeE2e]::IsZoomed($main)))

    # ---- 准备文档（存在编辑器子窗口；TC-004 前置） ----
    Send-Command $main $CMD_NEW
    $sci = Wait-Scintilla $main

    $cs = Get-ClientSize $main
    $cx = $cs.W; $cy = $cs.H
    # 命中带（逻辑 px，Scale 随 DPI）：edge=ResizeEdge=5，corner=ResizeCorner=12
    $edge = ScaleLogical 5 $dpi
    $corner = ScaleLogical 12 $dpi
    Assert-Cond '前置：边缘带几何有效（2 < edge 且 cy-2 落角区）' (($edge -gt 2) -and ($corner -ge 2)) "edge=$edge corner=$corner client=${cx}x${cy}"

    # 边缘/角采样点（父窗口客户区坐标 → 屏幕坐标）
    $pts = @(
        @{ name = '左边缘'; cxp = 2;      cyp = [int]($cy / 2); expect = $HT_LEFT },
        @{ name = '右边缘'; cxp = $cx - 2; cyp = [int]($cy / 2); expect = $HT_RIGHT },
        @{ name = '底边缘'; cxp = [int]($cx / 2); cyp = $cy - 2; expect = $HT_BOTTOM },
        @{ name = '左下角'; cxp = 2;      cyp = $cy - 2; expect = $HT_BOTTOMLEFT },
        @{ name = '右下角'; cxp = $cx - 2; cyp = $cy - 2; expect = $HT_BOTTOMRIGHT }
    )
    foreach ($p in $pts) {
        $sp = ClientPointToScreen $main $p.cxp $p.cyp
        $childHit = HitTest $sci $sp.X $sp.Y
        $mainHit  = HitTest $main $sp.X $sp.Y
        Assert-Cond ("TC-04 子窗口透传：{0} → HTTRANSPARENT" -f $p.name) ($childHit -eq $HT_TRANSPARENT) "child=$childHit"
        Assert-Cond ("TC-04 父窗口命中：{0} → HT{1}" -f $p.name, $p.expect) ($mainHit -eq $p.expect) "main=$mainHit"
    }
    # 顶边缘（工具栏/标题区，无子窗口遮挡，父窗口直达）：resize 命中
    $spTop = ClientPointToScreen $main ([int]($cx / 2)) 2
    $topHit = HitTest $main $spTop.X $spTop.Y
    Assert-Cond 'TC-04 父窗口命中：顶边缘 → HTTOP' ($topHit -eq $HT_TOP) "main=$topHit"

    # 内部区域不受透传影响：子窗口正常处理（非 HTTRANSPARENT），父窗口 HTCLIENT
    $spMid = ClientPointToScreen $main ([int]($cx / 2)) ([int]($cy / 2))
    $childMid = HitTest $sci $spMid.X $spMid.Y
    $mainMid  = HitTest $main $spMid.X $spMid.Y
    Assert-Cond 'TC-04 内部区域：子窗口正常 HTCLIENT（非透传）' ($childMid -eq $HT_CLIENT) "child=$childMid"
    Assert-Cond 'TC-04 内部区域：父窗口 HTCLIENT' ($mainMid -eq $HT_CLIENT) "main=$mainMid"

    # ---- TC-003（编辑器态）：失焦非活动选区底色，浅色主题 ----
    # 生产公式（scintillaedit.cpp）：BlendOver(contentBg, 0x55<<24 | sel&0xFFFFFF)
    $lExpect = ToColorRef (BlendOver (0xFF000000 -bor $LightContent) (0x55000000 -bor ($LightSel -band 0xFFFFFF)))
    $lRgba = Get-InactiveSelectionRgba $sci
    Assert-Cond 'TC-03 浅色：失焦选区底 = BlendOver(#ffffff, #ADD6FF@0x55) 且 alpha=0xFF' `
        ((($lRgba -band 0xFFFFFF) -eq $lExpect) -and ((($lRgba -shr 24) -band 0xFF) -eq 0xFF)) `
        ("rgba=0x{0:X8} expectColorRef=0x{1:X6}" -f $lRgba, $lExpect)

    # ---- 深色主题重复（WM_APP+7 code4 切换；结束时切回浅色） ----
    Send-Command $main $CMD_TOGGLE_DARK
    $dExpect = ToColorRef (BlendOver (0xFF000000 -bor $DarkContent) (0x55000000 -bor ($DarkSel -band 0xFFFFFF)))
    $dRgba = Get-InactiveSelectionRgba $sci
    Assert-Cond 'TC-03 深色：失焦选区底 = BlendOver(#262b34, #264F78@0x55) 且 alpha=0xFF' `
        ((($dRgba -band 0xFFFFFF) -eq $dExpect) -and ((($dRgba -shr 24) -band 0xFF) -eq 0xFF)) `
        ("rgba=0x{0:X8} expectColorRef=0x{1:X6}" -f $dRgba, $dExpect)
    Send-Command $main $CMD_TOGGLE_DARK  # 切回浅色

    # 深色主题下 TC-004 透传不受主题影响（抽测左边缘一组）
    Send-Command $main $CMD_TOGGLE_DARK
    $spL = ClientPointToScreen $main 2 ([int]($cy / 2))
    $darkChild = HitTest $sci $spL.X $spL.Y
    $darkMain  = HitTest $main $spL.X $spL.Y
    Assert-Cond 'TC-04 深色主题透传：左边缘 child=-1 / main=HTLEFT' `
        (($darkChild -eq $HT_TRANSPARENT) -and ($darkMain -eq $HT_LEFT)) "child=$darkChild main=$darkMain"
    Send-Command $main $CMD_TOGGLE_DARK  # 切回浅色

    # ---- TC-006（编辑器态）：换行按钮点击即时切换 SCI_GETWRAPMODE ----
    # 换行按钮（下标 3）中心（逻辑 px）：前 3 按钮末尾 + 分隔间隔 13（SeparatorMargin*2 + SeparatorWidth，见 src/ui/toolbar.h）+ 半宽
    # x = 4 + 3*(44+2) - 2 + 6*2 + 1 + 22 = 175；y = 40 + 4 + 16 = 60
    $btnX = ScaleLogical ($LeftMargin + 3 * ($BtnW + $Spacing) - $Spacing + 6 + 1 + 6 + [int]($BtnW / 2)) $dpi
    $btnY = ScaleLogical ($CapH + [int](($TbHeight - $BtnH) / 2) + [int]($BtnH / 2)) $dpi
    function Click-WrapButton([IntPtr]$main, [int]$bx, [int]$by) {
        [QcEdgeE2e]::Send($main, $WM_LBUTTONDOWN, [IntPtr]$MK_LBUTTON, (MakeLParam $bx $by)) | Out-Null
        [QcEdgeE2e]::Send($main, $WM_LBUTTONUP, [IntPtr]::Zero, (MakeLParam $bx $by)) | Out-Null
        Start-Sleep -Milliseconds 150
    }
    $wrap0 = Get-WrapMode $sci
    Assert-Cond 'TC-06 初始换行模式 = SC_WRAP_NONE' ($wrap0 -eq $SC_WRAP_NONE) "wrapMode=$wrap0"
    Click-WrapButton $main $btnX $btnY
    $wrap1 = Get-WrapMode $sci
    Assert-Cond 'TC-06 点击换行按钮 → SC_WRAP_WORD（编辑器即时切换）' ($wrap1 -eq $SC_WRAP_WORD) "wrapMode=$wrap1"
    Click-WrapButton $main $btnX $btnY
    $wrap2 = Get-WrapMode $sci
    Assert-Cond 'TC-06 再次点击 → SC_WRAP_NONE' ($wrap2 -eq $SC_WRAP_NONE) "wrapMode=$wrap2"
    Click-WrapButton $main $btnX $btnY   # 置 ON，供重启会话恢复断言
    $wrap3 = Get-WrapMode $sci
    Assert-Cond 'TC-06 第三次点击 → SC_WRAP_WORD（重启前置态）' ($wrap3 -eq $SC_WRAP_WORD) "wrapMode=$wrap3"

    # ---- 重启：会话恢复 wordWrap=true 且新编辑器继承（TC-006 会话恢复项） ----
    Stop-App $proc $main
    $proc2 = Start-App
    $main2 = Wait-MainWindow $proc2
    Send-Command $main2 $CMD_NEW
    $sci2 = Wait-Scintilla $main2
    $wrap4 = Get-WrapMode $sci2
    Assert-Cond 'TC-06 重启后会话恢复：新文档编辑器继承 wordWrap=true' ($wrap4 -eq $SC_WRAP_WORD) "wrapMode=$wrap4"

    Stop-App $proc2 $main2
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
$pass = ($script:Results | Where-Object { -not $_.Pass }).Count
if ($fail -or $pass -gt 0) {
    Write-Output ("总结: FAIL（失败 {0} 项）" -f $pass)
    exit 1
}
Write-Output '总结: PASS'
exit 0
