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
# qcnote「显示行号」自动化验证（需求-显示行号 TC-001~TC-007 e2e 部分）
#
# 覆盖（对齐 .localdoc\测试用例\测试用例-显示行号.md）：
#   TC-001 默认显示：空会话启动 + NewDocument → margin 0 类型
#     SC_MARGIN_NUMBER(=1)、宽度 > 0、行数 1。
#   TC-002 视图菜单开关：跨进程驱动菜单（主窗口 WM_NCLBUTTONDOWN(HTBORDER)
#     点击菜单按钮 → QcnoteMenuPopup 弹出层按行高数学点击「视图▸显示行号」，
#     与真实菜单同一路径 RouteCommand(ToggleLineNumbers)）→ 关：宽度=0；
#     开：恢复 > 0。
#   TC-003 全局生效 + 新文档继承：多文档逐一采样 dump 暴露的编辑器 HWND；
#     切换后新建文档继承当前开关态。
#   TC-004 宽度自适应：SCI_NEWLINE 逐行加到 999 → 宽度 == 1 行宽（最少
#     3 位）；1000 行 → 加宽；SCI_SELECTALL+SCI_CLEAR 清空 → 回缩。
#   TC-005 会话持久化往返：开→关→开各落盘重启，dump 采样编辑器 +
#     ini [editor] lineNumbers=false/true 双口径。
#   TC-006 前向兼容：真实会话 ini 移除 lineNumbers 键（旧格式）→ 启动
#     恢复文档且行号默认显示。
#   TC-007 双主题配色：SCI_STYLEGETFORE/BACK(STYLE_LINENUMBER) ==
#     ToColorRef(pal.textSecondary / contentBackground)，浅/深两轮。
#   TC-P01/TC-P02 行号边距（需求-行号边距）：开启态
#     SCI_GETMARGINLEFT == Scale(LineNumberPaddingX)、边宽 == 数字宽 + P
#     （等宽字体由 3 位宽 - P 推得一位数字宽自洽验证）；关闭态左内边距
#     恢复默认 1；999→1000 跨档差值 == 一位数字宽。
#
# 菜单行高数学（src/ui/menu.h menu_metrics + main.cpp 场景 C 同款）：
#   行中心 y = 1 + Σ(前置行高) + 行高/2；item=32 / sep=9 逻辑 px（随 DPI）。
#   鼠标消息一律直投根弹出层（捕获语义：坐标按根层客户区解释，
#   经 ClientToScreen→hitTest 跨层命中第二级条目）。
#
# 防扰约定：全程 --headless（主窗口离屏 -32000，弹出层随锚点离屏，
#   无任务栏项、零前台抢占）；--userdir 隔离单实例域与用户数据。
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

$TempRoot = 'D:\temp\aiRunner\opencode\qcnote-linenumbers-e2e-20261119'
if (-not $KeepTemp) {
    Remove-Item -Recurse -Force $TempRoot -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null
$UserDir  = Join-Path $TempRoot 'user'
$UserDir2 = Join-Path $TempRoot 'user-old-ini'
$DumpPath = Join-Path $TempRoot 'state-dump.json'
$IniPath  = Join-Path $UserDir 'qcnote.ini'

# ---------------- Win32 P/Invoke ----------------
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Collections.Generic;
public class QcLineNumE2e {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
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

# ---------------- 常量（与 exe/vendor 头文件对齐） ----------------
$WM_MOUSEMOVE    = 0x0200
$WM_LBUTTONDOWN  = 0x0201
$WM_LBUTTONUP    = 0x0202
$WM_NCLBUTTONDOWN = 0x00A1
$WM_CLOSE_MSG    = 0x0010
$MK_LBUTTON      = 1
$HTBORDER        = 18      # 菜单按钮 NCHITTEST 命中值（titlebar.cpp ComputeCaptionHitTest）

$MSG_DUMP    = 0x8006      # WM_APP + 6（kMsgSelftestDump）
$MSG_COMMAND = 0x8007      # WM_APP + 7（kMsgSelftestCommand）
$CMD_NEW         = 1       # NewDocument
$CMD_TOGGLE_DARK = 4       # ToggleDarkTheme

# Scintilla 消息号（vendor/scintilla/include/Scintilla.h；跨进程仅用无指针参数消息）
$SCI_GETMARGINTYPEN = 2241
$SCI_GETMARGINWIDTHN = 2243
$SCI_GETMARGINLEFT  = 2156
$SCI_GETLINECOUNT   = 2154
$SCI_STYLEGETFORE   = 2481
$SCI_STYLEGETBACK   = 2482
$SCI_NEWLINE        = 2329
$SCI_SELECTALL      = 2013
$SCI_CLEAR          = 2180
$STYLE_LINENUMBER   = 33
$SC_MARGIN_NUMBER   = 1

# 行号左右边距（src/editor/scintillaedit.h editor_metrics::LineNumberPaddingX，逻辑 px）
$LineNumberPaddingX = 4

# 主题 token（src/ui/theme.cpp LightPalette/DarkPalette：textSecondary/contentBackground）
$LightTextSecondaryArgb = 0xFF5C6678
$LightContentBgArgb     = 0xFFFFFFFF
$DarkTextSecondaryArgb  = 0xFF9AA3B5
$DarkContentBgArgb      = 0xFF262B34

# 菜单/标题栏度量（src/ui/menu.h menu_metrics、src/ui/theme.h metrics；逻辑 px）
$MenuItemHeight  = 32
$MenuSepHeight   = 9
$CaptionBtnWidth = 44
$CaptionHeight   = 40

# 根菜单行序（mainmenu.cpp BuildMainMenu）：文件/编辑/搜索/──/视图/──/退出
$RootMenuKinds = @('item','item','item','sep','item','sep','item')
$RootRowView   = 4    # 视图
# 视图子菜单行序：深色主题/自动换行/显示行号（无分隔线）
$ViewMenuKinds = @('item','item','item')
$ViewRowLineNumbers = 2

# ---------------- 纯函数复刻（与 src/ui/theme.{h,cpp} 一致） ----------------
function MulDiv([int]$n, [int]$m, [int]$d) {
    [int][Math]::Round([double]($n * $m) / $d, [MidpointRounding]::AwayFromZero)
}
function ScaleLogical([int]$logical, [uint32]$dpi) { MulDiv $logical ([int]$dpi) 96 }
function ToColorRef([long]$argb) {
    # src/ui/theme.cpp ToColorRef：0xAARRGGBB → 0x00BBGGRR
    ((($argb -shr 16) -band 0xFF) -bor ((($argb -shr 8) -band 0xFF) -shl 8) -bor (($argb -band 0xFF) -shl 16))
}
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
function Start-App([string]$UserDir) {
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
        $wins = [QcLineNumE2e]::WindowsOfPid([uint32]$proc.Id, 'QcnoteMainWindow')
        if ($wins.Count -gt 0) { return $wins[0] }
        Start-Sleep -Milliseconds 50
    }
    throw "10s 内未找到主窗口"
}
function Wait-Scintilla([IntPtr]$main, [int]$timeoutMs = 10000) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        $sci = [QcLineNumE2e]::FindWindowEx($main, [IntPtr]::Zero, 'Scintilla', $null)
        if ($sci -ne [IntPtr]::Zero) { return $sci }
        Start-Sleep -Milliseconds 50
    }
    throw "10s 内未找到 Scintilla 编辑器子窗口"
}
function Send-Command([IntPtr]$main, [int]$code) {
    [QcLineNumE2e]::Send($main, $MSG_COMMAND, [IntPtr]$code, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 150
}
function Stop-App([System.Diagnostics.Process]$proc, [IntPtr]$main) {
    # WM_CLOSE：C4 热退出（PersistenceActive 下 PersistSessionNow 落盘会话）
    [QcLineNumE2e]::Send($main, $WM_CLOSE_MSG, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        if (-not $proc.HasExited) { $proc.Kill() }
        throw "WM_CLOSE 后 ${TimeoutSec}s 未退出"
    }
}

# ---------------- dump / Scintilla 采样 ----------------
function Get-DumpJson([IntPtr]$main) {
    Remove-Item $DumpPath -Force -ErrorAction SilentlyContinue
    $r = [IntPtr]::Zero
    [QcLineNumE2e]::SendMessageTimeout($main, $MSG_DUMP, [IntPtr]::Zero, [IntPtr]::Zero, 2, 5000, [ref]$r) | Out-Null
    if (Test-Path $DumpPath) {
        return Get-Content $DumpPath -Raw -Encoding utf8 | ConvertFrom-Json
    }
    return $null
}
function Get-DumpEditors([IntPtr]$main) {
    # dump documents[].editor（"0x%llX" 字符串）→ IntPtr 数组（仅非 null）
    $d = Get-DumpJson $main
    $list = @()
    foreach ($doc in $d.documents) {
        if ($doc.editor -and $doc.editor -ne 'null') {
            $list += [IntPtr][Convert]::ToInt64(($doc.editor -replace '^0x', ''), 16)
        } else {
            $list += [IntPtr]::Zero
        }
    }
    return ,$list
}
function Get-MarginType([IntPtr]$sci) {
    [int][QcLineNumE2e]::Send($sci, $SCI_GETMARGINTYPEN, [IntPtr]::Zero, [IntPtr]::Zero)
}
function Get-MarginWidth([IntPtr]$sci) {
    [int][QcLineNumE2e]::Send($sci, $SCI_GETMARGINWIDTHN, [IntPtr]::Zero, [IntPtr]::Zero)
}
function Get-MarginLeft([IntPtr]$sci) {
    [int][QcLineNumE2e]::Send($sci, $SCI_GETMARGINLEFT, [IntPtr]::Zero, [IntPtr]::Zero)
}
function Get-LineCount([IntPtr]$sci) {
    [int][QcLineNumE2e]::Send($sci, $SCI_GETLINECOUNT, [IntPtr]::Zero, [IntPtr]::Zero)
}
function Get-StyleFore([IntPtr]$sci) {
    [int][QcLineNumE2e]::Send($sci, $SCI_STYLEGETFORE, [IntPtr]$STYLE_LINENUMBER, [IntPtr]::Zero)
}
function Get-StyleBack([IntPtr]$sci) {
    [int][QcLineNumE2e]::Send($sci, $SCI_STYLEGETBACK, [IntPtr]$STYLE_LINENUMBER, [IntPtr]::Zero)
}
function Wait-LineCount([IntPtr]$sci, [int]$expect, [int]$timeoutMs = 10000) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        if ((Get-LineCount $sci) -eq $expect) { return $true }
        Start-Sleep -Milliseconds 30
    }
    return ((Get-LineCount $sci) -eq $expect)
}

# ---------------- 菜单跨进程驱动（视图▸显示行号 = RouteCommand 唯一入口） ----------------
function Get-MenuPopups([uint32]$procId) {
    [QcLineNumE2e]::WindowsOfPid($procId, 'QcnoteMenuPopup')
}
function Wait-MenuPopupCount([uint32]$procId, [int]$count, [int]$timeoutMs = 5000) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        if ((Get-MenuPopups $procId).Count -eq $count) { return $true }
        Start-Sleep -Milliseconds 30
    }
    return ((Get-MenuPopups $procId).Count -eq $count)
}
function Get-MenuRowCenter([IntPtr]$levelHwnd, [int]$rowIndex, [string[]]$kinds, [uint32]$dpi) {
    # 行中心（该弹出层客户坐标 → 屏幕坐标）：y = 1 + Σ(前置行高) + 行高/2
    $ih = ScaleLogical $MenuItemHeight $dpi
    $sh = ScaleLogical $MenuSepHeight $dpi
    $y = 1
    for ($i = 0; $i -lt $rowIndex; ++$i) { $y += if ($kinds[$i] -eq 'sep') { $sh } else { $ih } }
    $h  = if ($kinds[$rowIndex] -eq 'sep') { $sh } else { $ih }
    $cy = $y + [int]($h / 2)
    $r = New-Object QcLineNumE2e+RECT
    [QcLineNumE2e]::GetWindowRect($levelHwnd, [ref]$r) | Out-Null
    $pt = New-Object QcLineNumE2e+POINT
    $pt.X = $r.Left + [int](($r.Right - $r.Left) / 2)
    $pt.Y = $r.Top + $cy
    return $pt   # 屏幕坐标
}
function Post-MenuClick([IntPtr]$rootHwnd, $screenPt) {
    # 鼠标消息一律直投根弹出层，坐标按根层客户区换算（捕获语义同款）
    $pt = New-Object QcLineNumE2e+POINT
    $pt.X = $screenPt.X; $pt.Y = $screenPt.Y
    [QcLineNumE2e]::ScreenToClient($rootHwnd, [ref]$pt) | Out-Null
    $lp = MakeLParam $pt.X $pt.Y
    [QcLineNumE2e]::PostMessage($rootHwnd, $WM_MOUSEMOVE, [IntPtr]::Zero, $lp) | Out-Null
    [QcLineNumE2e]::PostMessage($rootHwnd, $WM_LBUTTONDOWN, [IntPtr]$MK_LBUTTON, $lp) | Out-Null
    [QcLineNumE2e]::PostMessage($rootHwnd, $WM_LBUTTONUP, [IntPtr]::Zero, $lp) | Out-Null
}
function Invoke-ToggleLineNumbers([System.Diagnostics.Process]$proc, [IntPtr]$main, [uint32]$dpi) {
    # 返回 $true = 菜单路径完整走通（打开 → 视图 → 显示行号 → 关闭）
    if (-not (Wait-MenuPopupCount ([uint32]$proc.Id) 0 2000)) {
        Write-Output '  [WARN] 菜单驱动前置：存在残留弹出层'
        return $false
    }
    # 1) 点击标题栏菜单按钮（WM_NCLBUTTONDOWN + HTBORDER，down 即执行）
    $r = New-Object QcLineNumE2e+RECT
    [QcLineNumE2e]::GetWindowRect($main, [ref]$r) | Out-Null
    $btnPt = New-Object QcLineNumE2e+POINT
    $btnPt.X = $r.Left + [int](ScaleLogical ($CaptionBtnWidth / 2) $dpi)
    $btnPt.Y = $r.Top + [int](ScaleLogical ($CaptionHeight / 2) $dpi)
    [QcLineNumE2e]::PostMessage($main, $WM_NCLBUTTONDOWN, [IntPtr]$HTBORDER, (MakeLParam $btnPt.X $btnPt.Y)) | Out-Null
    if (-not (Wait-MenuPopupCount ([uint32]$proc.Id) 1 5000)) { return $false }

    # 2) 点击根菜单「视图」行（Submenu：DOWN 立即展开第二级）
    $popups = Get-MenuPopups ([uint32]$proc.Id)
    $root = $popups[0]
    $viewPt = Get-MenuRowCenter $root $RootRowView $RootMenuKinds $dpi
    Post-MenuClick $root $viewPt
    if (-not (Wait-MenuPopupCount ([uint32]$proc.Id) 2 5000)) { return $false }

    # 3) 点击第二级「显示行号」行（Action：UP 触发命令并整体关闭）
    $popups2 = Get-MenuPopups ([uint32]$proc.Id)
    $level2 = $popups[1]                       # 后展开的在枚举序尾部不稳，按列表第 2 个兜底
    foreach ($h in $popups2) { if ($h -ne $root) { $level2 = $h } }
    $lnPt = Get-MenuRowCenter $level2 $ViewRowLineNumbers $ViewMenuKinds $dpi
    Post-MenuClick $root $lnPt
    if (-not (Wait-MenuPopupCount ([uint32]$proc.Id) 0 5000)) { return $false }

    Start-Sleep -Milliseconds 250   # track 返回 → onCommand → RouteCommand 完成
    return $true
}

# ---------------- ini 读取 ----------------
function Get-IniEditorValue([string]$path, [string]$key) {
    if (-not (Test-Path $path)) { return $null }
    $inEditor = $false
    foreach ($line in Get-Content $path -Encoding UTF8) {
        if ($line -match '^\s*\[(.+)\]\s*$') { $inEditor = ($Matches[1] -eq 'editor'); continue }
        if ($inEditor -and $line -match ('^\s*' + $key + '\s*=\s*(\S+)')) { return $Matches[1] }
    }
    return $null
}

# =====================================================================
# 用例执行
# =====================================================================
$fail = $false
$app1 = $null; $app2 = $null; $app3 = $null; $app4 = $null; $appOld = $null
try {
    # ================= Phase A：TC-001/004/007/002/003（单进程） =================
    Write-Output '== Phase A：默认显示 / 宽度自适应 / 双主题 / 菜单开关 / 全局与继承 =='
    $app1 = Start-App $UserDir
    $main1 = Wait-MainWindow $app1
    $dpi = [QcLineNumE2e]::GetDpiForWindow($main1)
    if ($dpi -eq 0) { $dpi = 96 }
    Write-Output ("主窗口就绪 pid={0} dpi={1}" -f $app1.Id, $dpi)

    Send-Command $main1 $CMD_NEW
    $sci = Wait-Scintilla $main1

    # ---- TC-001：默认显示（空会话 + 新文档） ----
    $type1 = Get-MarginType $sci
    $w1 = Get-MarginWidth $sci
    $lines1 = Get-LineCount $sci
    Assert-Cond 'TC-01 新建文档 margin 0 类型 = SC_MARGIN_NUMBER' ($type1 -eq $SC_MARGIN_NUMBER) "type=$type1"
    Assert-Cond 'TC-01 新建文档行号边宽度 > 0' ($w1 -gt 0) "width=$w1"
    Assert-Cond 'TC-01 新建文档行数 = 1' ($lines1 -eq 1) "lines=$lines1"

    # ---- TC-P01：开启态左右边距对称且等于 P（需求-行号边距） ----
    $sciDpi = [QcLineNumE2e]::GetDpiForWindow($sci)
    if ($sciDpi -eq 0) { $sciDpi = $dpi }
    $padPx = ScaleLogical $LineNumberPaddingX $sciDpi
    $ml1 = Get-MarginLeft $sci
    Assert-Cond 'TC-P01 开启态：SCI_GETMARGINLEFT == Scale(P, dpi)（右边距）' ($ml1 -eq $padPx) "marginLeft=$ml1 expect=$padPx (dpi=$sciDpi)"
    # 边宽 = 数字实测宽 + P：等宽字体下 3 位数字宽 = 3 × 一位数字宽，据此
    # 自洽验证（跨进程无法安全传指针做 SCI_TEXTWIDTH 独立采样）
    $digitW = [int](($w1 - $padPx) / 3)
    $digitRem = ($w1 - $padPx) % 3
    Assert-Cond 'TC-P01 开启态：边宽 - P == 3 × 一位数字宽（左边距并入边宽）' ($digitW -gt 0 -and $digitRem -eq 0) "w=$w1 P=$padPx digit=$digitW rem=$digitRem"

    # ---- TC-004：宽度自适应（3 位最小 / 999→1000 加宽 / 清空回缩） ----
    # 999 行仍 3 位：宽度应与 1 行时相同（最少按 3 位数预留）
    for ($i = 0; $i -lt 998; ++$i) {
        [QcLineNumE2e]::PostMessage($sci, $SCI_NEWLINE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    }
    $ok999 = Wait-LineCount $sci 999
    $w999 = Get-MarginWidth $sci
    Assert-Cond 'TC-04 999 行（3 位）宽度 == 1 行宽（最少 3 位预留）' ($ok999 -and ($w999 -eq $w1)) "lines=$(Get-LineCount $sci) w1=$w1 w999=$w999"
    # 1000 行跨 4 位：加宽
    [QcLineNumE2e]::PostMessage($sci, $SCI_NEWLINE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $ok1000 = Wait-LineCount $sci 1000
    $w1000 = Get-MarginWidth $sci
    Assert-Cond 'TC-04 跨 999→1000 行宽度加宽（4 位 > 3 位）' ($ok1000 -and ($w1000 -gt $w999)) "lines=$(Get-LineCount $sci) w999=$w999 w1000=$w1000"
    # 跨档差值 == 一位数字宽（边宽两档均含同一 P，差值即纯数字宽差；与 TC-P01
    # 推得的一位数字宽自洽）
    Assert-Cond 'TC-04 跨档差值 == 一位数字宽（边距不随位数档缩放）' ($ok1000 -and (($w1000 - $w999) -eq $digitW)) "diff=$($w1000 - $w999) digit=$digitW"
    # 清空回缩
    [QcLineNumE2e]::PostMessage($sci, $SCI_SELECTALL, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    [QcLineNumE2e]::PostMessage($sci, $SCI_CLEAR, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $okBack = Wait-LineCount $sci 1
    $wBack = Get-MarginWidth $sci
    Assert-Cond 'TC-04 清空删行后宽度回缩 == 1 行宽' ($okBack -and ($wBack -eq $w1)) "lines=$(Get-LineCount $sci) wBack=$wBack w1=$w1"

    # ---- TC-007：双主题行号配色（textSecondary/contentBackground 语义 token） ----
    $lFore = Get-StyleFore $sci; $lBack = Get-StyleBack $sci
    Assert-Cond 'TC-07 浅色：行号前景 = textSecondary(#5C6678)' ($lFore -eq (ToColorRef $LightTextSecondaryArgb)) ("fore=0x{0:X6} expect=0x{1:X6}" -f $lFore, (ToColorRef $LightTextSecondaryArgb))
    Assert-Cond 'TC-07 浅色：行号背景 = contentBackground(#FFFFFF)' ($lBack -eq (ToColorRef $LightContentBgArgb)) ("back=0x{0:X6} expect=0x{1:X6}" -f $lBack, (ToColorRef $LightContentBgArgb))
    Send-Command $main1 $CMD_TOGGLE_DARK
    $dFore = Get-StyleFore $sci; $dBack = Get-StyleBack $sci
    Assert-Cond 'TC-07 深色：行号前景 = textSecondary(#9AA3B5)' ($dFore -eq (ToColorRef $DarkTextSecondaryArgb)) ("fore=0x{0:X6} expect=0x{1:X6}" -f $dFore, (ToColorRef $DarkTextSecondaryArgb))
    Assert-Cond 'TC-07 深色：行号背景 = contentBackground(#262B34)' ($dBack -eq (ToColorRef $DarkContentBgArgb)) ("back=0x{0:X6} expect=0x{1:X6}" -f $dBack, (ToColorRef $DarkContentBgArgb))
    Assert-Cond 'TC-07 深色行号前景 ≠ 正文色（与浅色 token 可辨识区分）' ($dFore -ne $lFore) ("dark=0x{0:X6} light=0x{1:X6}" -f $dFore, $lFore)
    Send-Command $main1 $CMD_TOGGLE_DARK   # 切回浅色

    # ---- TC-002：视图菜单开关切换（真实菜单路径 RouteCommand） ----
    $menuOk1 = Invoke-ToggleLineNumbers $app1 $main1 $dpi
    $wOff = Get-MarginWidth $sci
    Assert-Cond 'TC-02 菜单取消勾选 → 行号边完全消失（宽度 = 0）' ($menuOk1 -and ($wOff -eq 0)) "menuPath=$menuOk1 width=$wOff"
    # ---- TC-P02：关闭态恢复默认左内边距（需求-行号边距） ----
    $mlOff = Get-MarginLeft $sci
    Assert-Cond 'TC-P02 关闭态：SCI_GETMARGINLEFT == 1（Scintilla 默认，无残留空隙）' ($menuOk1 -and ($mlOff -eq 1)) "marginLeft=$mlOff"
    $menuOk2 = Invoke-ToggleLineNumbers $app1 $main1 $dpi
    $wOn = Get-MarginWidth $sci
    Assert-Cond 'TC-02 重新勾选 → 行号恢复显示（宽度 > 0 且与关前一致）' ($menuOk2 -and ($wOn -eq $w1)) "menuPath=$menuOk2 width=$wOn expect=$w1"
    $mlOn = Get-MarginLeft $sci
    Assert-Cond 'TC-P02 重新开启：SCI_GETMARGINLEFT 恢复 == Scale(P, dpi)' ($menuOk2 -and ($mlOn -eq $padPx)) "marginLeft=$mlOn expect=$padPx"

    # ---- TC-003：全局生效（所有已打开文档）+ 新文档继承 ----
    Send-Command $main1 $CMD_NEW
    Send-Command $main1 $CMD_NEW
    $editors3 = Get-DumpEditors $main1
    Assert-Cond 'TC-03 前置：3 个文档均有编辑器实例' ($editors3.Count -eq 3 -and -not ($editors3 -contains [IntPtr]::Zero)) "editors=$($editors3.Count)"
    $menuOk3 = Invoke-ToggleLineNumbers $app1 $main1 $dpi   # 关闭
    $widthsOff = @(); foreach ($e in $editors3) { $widthsOff += Get-MarginWidth $e }
    Assert-Cond 'TC-03 关闭：所有已打开文档行号边同步消失' ($menuOk3 -and (($widthsOff | Where-Object { $_ -ne 0 }).Count -eq 0)) "widths=[$($widthsOff -join ',')]"
    Send-Command $main1 $CMD_NEW   # 关闭态新建
    $editors4 = Get-DumpEditors $main1
    $newSciOff = $editors4[-1]
    $wNewOff = Get-MarginWidth $newSciOff
    Assert-Cond 'TC-03 关闭态新建文档继承：新编辑器宽度 = 0' ($wNewOff -eq 0) "width=$wNewOff"
    $menuOk4 = Invoke-ToggleLineNumbers $app1 $main1 $dpi   # 开启
    $widthsOn = @(); foreach ($e in $editors4) { $widthsOn += Get-MarginWidth $e }
    Assert-Cond 'TC-03 开启：所有文档（含关闭态新建的）行号恢复显示' ($menuOk4 -and (($widthsOn | Where-Object { $_ -le 0 }).Count -eq 0)) "widths=[$($widthsOn -join ',')]"

    # 会话恢复要有实际对象：向活动文档注入内容（WM_CHAR 真实键入路径，Document
    # 层同步）——空且未修改的未命名文档不落盘（重启恢复语义），Phase B/C 依赖 ≥1 文档
    $WM_CHAR = 0x0102
    foreach ($ch in @([int][char]'L', [int][char]'N')) {
        [QcLineNumE2e]::Send($editors4[-1], $WM_CHAR, [IntPtr]$ch, [IntPtr]::Zero) | Out-Null
    }
    Start-Sleep -Milliseconds 200
    $dumpA = Get-DumpJson $main1
    Assert-Cond 'TC-05 前置：活动文档带内容（会话将记录该文档）' ($dumpA.documents.Count -ge 1 -and $dumpA.documents[-1].content.Length -gt 0) "docs=$($dumpA.documents.Count) content='$($dumpA.documents[-1].content)'"

    Stop-App $app1 $main1    # 落盘：lineNumbers=true（TC-05 前置）
    $iniA = Get-IniEditorValue $IniPath 'lineNumbers'
    Assert-Cond 'TC-05 前置：开启态落盘 ini [editor] lineNumbers=true' ($iniA -eq 'true') "ini lineNumbers=$iniA"

    # ================= Phase B：TC-005 持久化往返（重启 ×3） =================
    Write-Output ''
    Write-Output '== Phase B：会话持久化往返（重启恢复 + ini 双口径） =='
    # 重启 1：恢复 true → 菜单关 → 落盘 false
    $app2 = Start-App $UserDir
    $main2 = Wait-MainWindow $app2
    $editorsR1 = Get-DumpEditors $main2
    Assert-Cond 'TC-05 前置：重启恢复出带内容文档（会话恢复链路可用）' ($editorsR1.Count -ge 1) "docs=$($editorsR1.Count)"
    $widthsR1 = @(); foreach ($e in $editorsR1) { $widthsR1 += Get-MarginWidth $e }
    Assert-Cond 'TC-05 重启恢复 lineNumbers=true：全部恢复文档行号立即显示' ($editorsR1.Count -ge 1 -and (($widthsR1 | Where-Object { $_ -le 0 }).Count -eq 0)) "docs=$($editorsR1.Count) widths=[$($widthsR1 -join ',')]"
    Send-Command $main2 $CMD_NEW
    $newSciR1 = (Get-DumpEditors $main2)[-1]
    Assert-Cond 'TC-05 true 态重启后新建文档继承显示' ((Get-MarginWidth $newSciR1) -gt 0) "width=$(Get-MarginWidth $newSciR1)"
    $menuR1 = Invoke-ToggleLineNumbers $app2 $main2 $dpi
    $wR1Off = Get-MarginWidth $newSciR1
    Assert-Cond 'TC-05 true 态重启后菜单关闭仍即时生效' ($menuR1 -and ($wR1Off -eq 0)) "menuPath=$menuR1 width=$wR1Off"
    Stop-App $app2 $main2
    $iniB = Get-IniEditorValue $IniPath 'lineNumbers'
    Assert-Cond 'TC-05 关闭态落盘 ini [editor] lineNumbers=false' ($iniB -eq 'false') "ini lineNumbers=$iniB"

    # 重启 2：恢复 false → 菜单开 → 落盘 true
    $app3 = Start-App $UserDir
    $main3 = Wait-MainWindow $app3
    $editorsR2 = Get-DumpEditors $main3
    $widthsR2 = @(); foreach ($e in $editorsR2) { $widthsR2 += Get-MarginWidth $e }
    Assert-Cond 'TC-05 重启恢复 lineNumbers=false：全部恢复文档行号关闭' ($editorsR2.Count -ge 1 -and (($widthsR2 | Where-Object { $_ -ne 0 }).Count -eq 0)) "docs=$($editorsR2.Count) widths=[$($widthsR2 -join ',')]"
    Send-Command $main3 $CMD_NEW
    $newSciR2 = (Get-DumpEditors $main3)[-1]
    Assert-Cond 'TC-05 false 态重启后新建文档继承关闭' ((Get-MarginWidth $newSciR2) -eq 0) "width=$(Get-MarginWidth $newSciR2)"
    $menuR2 = Invoke-ToggleLineNumbers $app3 $main3 $dpi
    Stop-App $app3 $main3
    $iniC = Get-IniEditorValue $IniPath 'lineNumbers'
    Assert-Cond 'TC-05 重新开启落盘 ini [editor] lineNumbers=true' ($iniC -eq 'true') "ini lineNumbers=$iniC"

    # 重启 3：恢复 true（回到开启态）
    $app4 = Start-App $UserDir
    $main4 = Wait-MainWindow $app4
    $editorsR3 = Get-DumpEditors $main4
    $widthsR3 = @(); foreach ($e in $editorsR3) { $widthsR3 += Get-MarginWidth $e }
    Assert-Cond 'TC-05 再次重启恢复 lineNumbers=true（true/false 往返闭环）' ($editorsR3.Count -ge 1 -and (($widthsR3 | Where-Object { $_ -le 0 }).Count -eq 0)) "docs=$($editorsR3.Count) widths=[$($widthsR3 -join ',')]"
    Stop-App $app4 $main4

    # ================= Phase C：TC-006 前向兼容（旧 ini 无 lineNumbers 键） =================
    Write-Output ''
    Write-Output '== Phase C：旧会话文件（无 lineNumbers 键）前向兼容 =='
    # 复制完整用户目录（ini + drafts 草稿——未命名文档内容恢复依赖草稿缓存），
    # 仅移除 ini 中 lineNumbers 行 = 旧版（无该键）会话文件
    New-Item -ItemType Directory -Force -Path $UserDir2 | Out-Null
    Copy-Item -Path (Join-Path $UserDir '*') -Destination $UserDir2 -Recurse -Force
    $oldIni = Join-Path $UserDir2 'qcnote.ini'
    $lines = Get-Content $oldIni -Encoding UTF8 | Where-Object { $_ -notmatch '^\s*lineNumbers\s*=' }
    Set-Content -Path $oldIni -Value $lines -Encoding UTF8
    Assert-Cond 'TC-06 前置：旧格式 ini 无 lineNumbers 键' ((Get-IniEditorValue $oldIni 'lineNumbers') -eq $null)

    $appOld = Start-App $UserDir2
    $mainOld = Wait-MainWindow $appOld
    $dumpOld = Get-DumpJson $mainOld
    Assert-Cond 'TC-06 旧 ini 启动无报错（文档正常恢复）' ($dumpOld -and $dumpOld.documents.Count -gt 0) "docs=$($dumpOld.documents.Count)"
    $editorsOld = Get-DumpEditors $mainOld
    $firstOld = @($editorsOld | Where-Object { $_ -ne [IntPtr]::Zero }) | Select-Object -First 1
    if ($firstOld) {
        $typeOld = Get-MarginType $firstOld
        $wOld = Get-MarginWidth $firstOld
        Assert-Cond 'TC-06 无 lineNumbers 键 → 默认显示（类型/宽度）' (($typeOld -eq $SC_MARGIN_NUMBER) -and ($wOld -gt 0)) "type=$typeOld width=$wOld"
    } else {
        Assert-Cond 'TC-06 无 lineNumbers 键 → 默认显示（类型/宽度）' $false '无可采样编辑器（文档未恢复）'
    }
    Stop-App $appOld $mainOld
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
Write-Output ("总结: PASS（{0}/{0} 通过）" -f $script:Results.Count)
exit 0
