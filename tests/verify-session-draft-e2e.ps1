#Requires -Version 7
param(
    [string]$ExePath = 'build-win\qcnote.exe',
    [switch]$KeepTemp
)

# =====================================================================
# qcnote M4-B 会话/草稿端到端验证（TC-I05~I07，真实双进程/强杀/重启）
#
#   TC-I05 热退出草稿：新建未命名 + WM_CHAR 注入键入 → 等 1.6s（1000ms
#     防抖 + 200ms timer 余量）→ 强杀（Process.Kill = TerminateProcess，
#     与 taskkill /F 同机制）→ 重启断言未命名带内容恢复 + ● + 激活；
#     再正常退出（CloseMainWindow = WM_CLOSE 热退出免提示）→ 重启断言
#     不重复恢复（恰一份、内容一致、仍脏、仍激活）。
#   TC-I06 会话恢复：3 临时文件命令行参数启动 → 逐文档设光标（SCI_GOTOPOS
#     注入）+ 切活动标签（Ctrl+Tab 真实输入流）→ 移动窗口（SetWindowPos）
#     → 正常退出 → 重启断言 3 标签顺序/激活/逐文档光标/几何
#     （ini [window]/[documents] 读取 + dump 采样的 GetWindowRect 双口径）。
#   TC-I07 关闭现场还原：三阶段构造与断言——
#     phase1：[A] + Ctrl+N 未命名1 键入内容 → 强杀（草稿落盘）；
#     phase2：恢复得 [A, 未命名1@1]，叠加命令行 [A, B]（A 去重激活、B 尾部
#       追加）→ 恰成 [文件A, 未命名1, 文件B] → Ctrl+Tab×2 激活未命名1 →
#       强杀（会话记 uid@1 + activeUid；草稿续写落盘）；
#     phase3：重启断言顺序 [A, 未命名1(带内容), B] 与激活=未命名1 还原
#       （DEC-001/002 按位插入 + activeUid 三级优先一级）。
#
# 测试隔离：--userdir=<path>（exe 测试钩子，重定向用户根目录）——每场景独立
#   用户目录，不触碰 %LOCALAPPDATA%\qcnote 与 exe 旁便携落点。
# 断言辅助：--selftest-dump=<path>（exe 测试钩子）——SendMessageTimeout(
#   WM_APP+6) 同步触发 exe 输出当前文档列表/激活/光标/窗口几何 JSON，
#   处理返回后读文件即完整（临时文件 + 原子替换）。
# 驱动方式：全程 --headless（窗口不显示、停泊离屏——零可见元素、不扰用户
#   前台焦点）。Ctrl 组合键（Ctrl+N/Ctrl+Tab）经 WM_APP+7 命令钩子直触 exe
#   内与快捷键处理完全相同的命令函数（RouteCommand/CycleTab——真实键盘路径
#   的 GetKeyState 判定不认 post/sent 消息，且不可见窗口不能走
#   SetForegroundWindow+keybd_event 真实输入流——那会抢用户前台焦点）；
#   文本键入经 WM_CHAR 直投当前活动 Scintilla 子窗口（与 exe 内
#   --selftest-doc 的 TypeIntoEditor 同路径）；光标经 SCI_GOTOPOS 直投；
#   正常退出经对主窗口直接 SendMessage(WM_CLOSE)（不可见窗口
#   MainWindowHandle==0，CloseMainWindow 不可用）。
# 轮询断言：50ms 间隔、10s 上限；进程清理 try/finally（按 PID 对象，绝不
#   误杀本机其他 qcnote 实例）。
# 用法：pwsh.exe -NoProfile -ExecutionPolicy Bypass -File tests\verify-session-draft-e2e.ps1
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

$TempRoot = 'D:\temp\aiRunner\opencode\qcnote-win32-rewrite-20261110183000\e2e-session-draft'
if (-not $KeepTemp) {
    Remove-Item -Recurse -Force $TempRoot -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null

# ---------------- Win32 P/Invoke ----------------
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Collections.Generic;
public class QcE2e {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h, int idx);
    [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint tid, ref GUITHREADINFO info);
    [StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO {
        public int cbSize; public uint flags; public IntPtr hwndActive; public IntPtr hwndFocus;
        public IntPtr hwndCapture; public IntPtr hwndMenuOwner; public IntPtr hwndMoveSize; public IntPtr hwndCaret;
        public RECT rcCaret;
    }
    // 活动编辑器 = 持有键盘焦点者（exe setActiveDocument 对活动编辑器 SetFocus）。
    // 历史注：曾不能以 WS_VISIBLE 样式位判定——setActiveDocument 曾用
    // IsWindowVisible 守卫隐藏旧实例，--headless 主窗口不可见期间判定失效，
    // 样式位命中多个编辑器；该根因已修复（无条件 SW_HIDE，仅活动编辑器
    // 保留样式位，见 I06 回归断言），焦点定位仍作为权威信号保留
    public static IntPtr FocusedChildOf(IntPtr main) {
        uint tid; GetWindowThreadProcessId(main, out tid);
        var gi = new GUITHREADINFO(); gi.cbSize = Marshal.SizeOf(typeof(GUITHREADINFO));
        if (GetGUIThreadInfo(tid, ref gi)) return gi.hwndFocus;
        return IntPtr.Zero;
    }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    // CharSet=Unicode 必须显式：ANSI 变体的 WM_CHAR 会按 CP_ACP 转换 wParam，
    // 中文（>0xFF）必乱码（实测 U+4E2D → U+5640）
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    public struct RECT { public int Left, Top, Right, Bottom; }
    public static List<IntPtr> WindowsOfPid(uint target, string cls) {
        var list = new List<IntPtr>();
        EnumWindows((h, lp) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid == target) {
                // 不过滤 IsWindowVisible：--headless 下主窗口永不显示（不可见窗口
                // 仍可收消息、GetWindowRect 几何有效）
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

# ---------------- 常量 ----------------
$WM_KEYDOWN = 0x0100; $WM_CHAR = 0x0102; $WM_CLOSE_MSG = 0x0010
$SCI_GOTOPOS = 2025
$MSG_DUMP = 0x8006     # WM_APP + 6（exe 内 kMsgSelftestDump）
$MSG_COMMAND = 0x8007  # WM_APP + 7（exe 内 kMsgSelftestCommand）
$CMD_NEW = 1; $CMD_NEXTTAB = 2; $CMD_CLOSETAB = 3
$GWL_STYLE = -16; $WS_VISIBLE = 0x10000000

# ---------------- 断言收集 ----------------
$script:Results = @()
function Assert-Cond([string]$case, [bool]$cond, [string]$detail = '') {
    $script:Results += [pscustomobject]@{ Case = $case; Pass = $cond; Detail = $detail }
    $tag = if ($cond) { 'PASS' } else { 'FAIL' }
    Write-Host ("  [{0}] {1}{2}" -f $tag, $case, $(if ($detail) { "  ($detail)" } else { '' }))
}

# ---------------- 进程与窗口驱动 ----------------
$script:Procs = @()   # try/finally 统一清理
$script:MainHwnd = @{}  # pid → 主窗口句柄（不可见窗口 CloseMainWindow 不可用）
function Start-App([string]$userDir, [string]$dumpPath, [string[]]$files = @()) {
    $args = @("--headless", "--userdir=$userDir", "--selftest-dump=$dumpPath")
    foreach ($f in $files) { $args += ('"{0}"' -f $f) }
    $p = Start-Process -FilePath $ExePath -ArgumentList $args -PassThru -WorkingDirectory $TempRoot
    $script:Procs += $p
    return $p
}

function Wait-MainWindow([System.Diagnostics.Process]$proc, [int]$timeoutMs = 10000) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        if ($proc.HasExited) { throw "进程提前退出（ExitCode=$($proc.ExitCode)）" }
        $wins = [QcE2e]::WindowsOfPid([uint32]$proc.Id, 'QcnoteMainWindow')
        if ($wins.Count -gt 0) {
            $script:MainHwnd[[uint32]$proc.Id] = $wins[0]
            return $wins[0]
        }
        Start-Sleep -Milliseconds 50
    }
    throw "10s 内未找到主窗口"
}

function Get-DumpJson([IntPtr]$main) {
    $dump = $script:DumpPath
    Remove-Item $dump -Force -ErrorAction SilentlyContinue
    $r = [IntPtr]::Zero
    # 同步采样：SendMessageTimeout 处理返回后文件即完整（临时文件+原子替换）
    [QcE2e]::SendMessageTimeout($main, $MSG_DUMP, [IntPtr]::Zero, [IntPtr]::Zero, 2, 5000, [ref]$r) | Out-Null
    if (Test-Path $dump) {
        return Get-Content $dump -Raw -Encoding utf8 | ConvertFrom-Json
    }
    return $null
}

# Ctrl 组合键等价路径：WM_APP+7 命令钩子直触 exe 内与快捷键处理相同的命令
# 函数（不可见窗口零输入流、不扰前台；真实键盘路径属人工验收）
function Send-Command([IntPtr]$main, [int]$code) {
    $r = [IntPtr]::Zero
    [QcE2e]::SendMessageTimeout($main, $MSG_COMMAND, [IntPtr]$code, [IntPtr]::Zero, 2, 5000, [ref]$r) | Out-Null
    Start-Sleep -Milliseconds 150
}

# 活动编辑器：优先取 --selftest-dump 的 documents[activeIndex].editor（exe
# 输出的该文档专属 Scintilla HWND——权威映射，--headless 下子窗口枚举顺序
# 不可靠；样式位曾因 IsWindowVisible 守卫缺陷命中多个编辑器，修复后虽已
# 可靠，dump 仍是最直接映射）；dump 不可得时兜底首个枚举命中者
function Get-ActiveEditor([IntPtr]$main) {
    $d = Get-DumpJson $main
    if ($d -and $d.activeIndex -ge 0 -and $d.documents.Count -gt $d.activeIndex) {
        $hex = [string]$d.documents[$d.activeIndex].editor
        if ($hex -and $hex -like '0x*') {
            return [IntPtr][Int64]::Parse($hex.Substring(2),
                [System.Globalization.NumberStyles]::HexNumber)
        }
    }
    return [QcE2e]::FindWindowEx($main, [IntPtr]::Zero, 'Scintilla', $null)
}

function Type-Text([IntPtr]$main, [string]$text) {
    $sci = Get-ActiveEditor $main
    if ($sci -eq [IntPtr]::Zero) { throw "未找到活动编辑器子窗口" }
    foreach ($ch in $text.ToCharArray()) {
        [QcE2e]::SendMessage($sci, $WM_CHAR, [IntPtr][int][char]$ch, [IntPtr]::Zero) | Out-Null
    }
    Start-Sleep -Milliseconds 150
}

function Set-EditorCursor([IntPtr]$main, [int]$pos) {
    $sci = Get-ActiveEditor $main
    if ($sci -eq [IntPtr]::Zero) { throw "未找到活动编辑器子窗口" }
    [QcE2e]::SendMessage($sci, $SCI_GOTOPOS, [IntPtr]$pos, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 150
}

# dump 条目 editor 字段（"0x..."）→ IntPtr；格式异常返回 Zero
function Get-EditorHwnd([object]$doc) {
    $hex = [string]$doc.editor
    if ($hex -and $hex -like '0x*') {
        return [IntPtr][Int64]::Parse($hex.Substring(2),
            [System.Globalization.NumberStyles]::HexNumber)
    }
    return [IntPtr]::Zero
}

# 各文档编辑器的 WS_VISIBLE 样式位数组。headless 下主窗口不可见，
# IsWindowVisible（含祖先链）全 FALSE 不可用，直接查样式位是唯一可测口径；
# setActiveDocument 修复（无条件 SW_HIDE）后应恰好只有活动文档为 True
function Get-VisBits($dump) {
    $bits = @()
    if ($dump -and $dump.documents) {
        foreach ($doc in @($dump.documents)) {
            $sci = Get-EditorHwnd $doc
            if ($sci -ne [IntPtr]::Zero) {
                $bits += (([QcE2e]::GetWindowLong($sci, $GWL_STYLE) -band $WS_VISIBLE) -ne 0)
            } else {
                $bits += $null   # 句柄缺失/格式异常 → 上层断言判失败
            }
        }
    }
    return ,$bits
}

function Stop-AppForce([System.Diagnostics.Process]$proc) {
    if ($proc -and -not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit(3000) | Out-Null }
}

function Exit-AppNormal([System.Diagnostics.Process]$proc, [int]$timeoutMs = 5000) {
    # 不可见窗口 MainWindowHandle==0 → CloseMainWindow 不可用；直接对已找到的
    # 主窗口 HWND 发 WM_CLOSE（与 CloseMainWindow 同消息，热退出路径一致）
    $main = $script:MainHwnd[[uint32]$proc.Id]
    if ($main) {
        [QcE2e]::SendMessage($main, $WM_CLOSE_MSG, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    } else {
        $proc.CloseMainWindow() | Out-Null
    }
    if (-not $proc.WaitForExit($timeoutMs)) {
        Stop-AppForce $proc
        return $false
    }
    return ($proc.ExitCode -eq 0)
}

# ---------------- ini 读取 ----------------
function Get-IniMap([string]$path) {
    $map = @{}
    if (-not (Test-Path $path)) { return $map }
    $section = ''
    foreach ($line in (Get-Content $path -Encoding utf8)) {
        if ($line -match '^\[(.+)\]\s*$') { $section = $Matches[1]; $map[$section] = @{}; continue }
        if ($section -and $line -match '^([^=]+)=(.*)$') { $map[$section][$Matches[1]] = $Matches[2] }
    }
    return $map
}

function Url-ValueOf([string]$path) { return $path.Replace('\', '/') }
# dump 的 url 可能是原生反斜杠（命令行直开）或正斜杠（会话恢复——
# std::filesystem::path 保留构造分隔符；MSVC 路径按分量比较两者相等）。
# 统一按正斜杠比较，两种来源均可命中
function Url-Norm([string]$path) { return $path.Replace('\', '/') }

# ---------------- 轮询断言（50ms / 10s） ----------------
function Wait-Assert([scriptblock]$probe, [string]$case, [string]$detail = '') {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $ok = $false
    while ($sw.ElapsedMilliseconds -lt 10000) {
        try { if (& $probe) { $ok = $true; break } } catch { }
        Start-Sleep -Milliseconds 50
    }
    Assert-Cond $case $ok $detail
    return $ok
}

$script:DumpPath = Join-Path $TempRoot 'state-dump.json'
$fgBefore = [QcE2e]::GetForegroundWindow()   # 防扰自证：全程前台焦点不变
$fail = $false
try {
    # ================================================================
    Write-Host "`n=== TC-I05 热退出草稿（强杀 → 恢复；正常退出 → 不重复）==="
    # ================================================================
    $u05 = Join-Path $TempRoot 'i05-user'
    $typed = 'draft-e2e-Hello-中文123'

    $p1 = Start-App $u05 $script:DumpPath
    $main1 = Wait-MainWindow $p1
    Send-Command $main1 $CMD_NEW      # Ctrl+N 等价 → 未命名-1
    $d = Get-DumpJson $main1
    Assert-Cond 'I05/键入前：Ctrl+N 新建未命名-1' ($null -ne $d -and $d.documents.Count -eq 1 -and $d.documents[0].name -eq '未命名-1' -and $d.activeIndex -eq 0) `
        ("docs={0}" -f $(if ($d) { $d.documents.Count } else { -1 }))
    Type-Text $main1 $typed
    $d = Get-DumpJson $main1
    Assert-Cond 'I05/WM_CHAR 注入内容 + 置脏' ($d.documents[0].content -eq $typed -and $d.documents[0].modified -eq $true) `
        ("content='{0}' modified={1}" -f $d.documents[0].content, $d.documents[0].modified)
    # 等 1000ms 草稿防抖 + 200ms timer tick 余量
    Start-Sleep -Milliseconds 1600
    $draftFiles1 = @(Get-ChildItem (Join-Path $u05 'drafts') -Filter '*.json' -File -ErrorAction SilentlyContinue)
    Assert-Cond 'I05/防抖到期草稿落盘（强杀前证据）' ($draftFiles1.Count -eq 1) `
        ("drafts={0}" -f $draftFiles1.Count)

    Stop-AppForce $p1   # 强杀（TerminateProcess，与任务管理器/taskkill /F 同）

    $p2 = Start-App $u05 $script:DumpPath
    $main2 = Wait-MainWindow $p2
    $d = Get-DumpJson $main2
    Assert-Cond 'I05/强杀重启：未命名带内容恢复 + ● + 激活' `
        ($null -ne $d -and $d.documents.Count -eq 1 -and $d.documents[0].name -eq '未命名-1' -and
         $d.documents[0].url -eq '' -and $d.documents[0].content -eq $typed -and
         $d.documents[0].modified -eq $true -and $d.activeIndex -eq 0) `
        ("docs={0} content='{1}' modified={2} active={3}" -f $d.documents.Count, $d.documents[0].content, $d.documents[0].modified, $d.activeIndex)

    $normalExit = Exit-AppNormal $p2   # 热退出免提示：直接退出
    Assert-Cond 'I05/正常退出（WM_CLOSE 热退出免提示）退出码 0' $normalExit

    $p3 = Start-App $u05 $script:DumpPath
    $main3 = Wait-MainWindow $p3
    $d = Get-DumpJson $main3
    Assert-Cond 'I05/再重启：不重复恢复（恰一份、内容一致、仍脏、仍激活）' `
        ($null -ne $d -and $d.documents.Count -eq 1 -and $d.documents[0].content -eq $typed -and
         $d.documents[0].modified -eq $true -and $d.activeIndex -eq 0) `
        ("docs={0} active={1}" -f $d.documents.Count, $d.activeIndex)
    Stop-AppForce $p3

    # ================================================================
    Write-Host "`n=== TC-I06 会话恢复（3 文档顺序/激活/光标/几何）==="
    # ================================================================
    $u06 = Join-Path $TempRoot 'i06-user'
    $files = @()
    $contents = @('alpha-file-one', 'beta-file-two-longer', 'gamma-file-three')
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    for ($i = 0; $i -lt 3; $i++) {
        $f = Join-Path $TempRoot ("tc-i06-{0}.txt" -f $([char](97 + $i)))
        [System.IO.File]::WriteAllText($f, $contents[$i] + "`n", $utf8NoBom)
        $files += $f
    }

    $p1 = Start-App $u06 $script:DumpPath $files
    $main1 = Wait-MainWindow $p1
    $d = Get-DumpJson $main1
    # dump 的 url 为原生路径（反斜杠）；ini 值才经 pathToUrlValue（正斜杠）
    Assert-Cond 'I06/参数启动：3 文档按序打开、激活最后' `
        ($d.documents.Count -eq 3 -and
         (Url-Norm $d.documents[0].url) -eq (Url-Norm $files[0]) -and
         (Url-Norm $d.documents[1].url) -eq (Url-Norm $files[1]) -and
         (Url-Norm $d.documents[2].url) -eq (Url-Norm $files[2]) -and
         $d.activeIndex -eq 2) `
        ("docs={0} active={1}" -f $d.documents.Count, $d.activeIndex)

    # 逐文档设光标（Ctrl+Tab 轮转激活 + SCI_GOTOPOS），最终激活 doc0
    $cursorPlan = @(3, 5, 7)
    for ($i = 0; $i -lt 3; $i++) {
        Send-Command $main1 $CMD_NEXTTAB   # Ctrl+Tab 等价：2→0→1→2
        $d = Get-DumpJson $main1
        Set-EditorCursor $main1 $cursorPlan[$d.activeIndex]
    }
    Send-Command $main1 $CMD_NEXTTAB   # 回到 doc0（恢复目标激活态 ≠ 自然末开）
    $d = Get-DumpJson $main1
    Assert-Cond 'I06/光标与活动标签设定生效' `
        ($d.activeIndex -eq 0 -and
         $d.documents[0].cursor -eq 3 -and $d.documents[1].cursor -eq 5 -and $d.documents[2].cursor -eq 7) `
        ("active={0} cursors=[{1},{2},{3}]" -f $d.activeIndex, $d.documents[0].cursor, $d.documents[1].cursor, $d.documents[2].cursor)

    # 移动窗口（保持尺寸；DPI 无关的纯位移）
    $rect = New-Object QcE2e+RECT
    [QcE2e]::GetWindowRect($main1, [ref]$rect) | Out-Null
    $movedX = $rect.Left + 37; $movedY = $rect.Top + 53
    [QcE2e]::SetWindowPos($main1, [IntPtr]::Zero, $movedX, $movedY, 0, 0, 0x0005) | Out-Null  # SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE
    Start-Sleep -Milliseconds 300
    $d = Get-DumpJson $main1
    Assert-Cond 'I06/窗口移动生效（dump 采样 GetWindowRect 口径）' `
        ($d.window.x -eq $movedX -and $d.window.y -eq $movedY -and $d.window.maximized -eq $false) `
        ("window=({0},{1})" -f $d.window.x, $d.window.y)

    Start-Sleep -Milliseconds 700   # 会话 500ms 防抖落盘（退出 flush 兜底）
    $normalExit = Exit-AppNormal $p1
    Assert-Cond 'I06/正常退出（热退出免提示）退出码 0' $normalExit

    # ini 文件级断言（会话持久化证据）
    $ini = Get-IniMap (Join-Path $u06 'qcnote.ini')
    $w = $ini['window']; $docs = $ini['documents']
    Assert-Cond 'I06/ini [window] 记录移动后几何' `
        ($w -and [int]$w['x'] -eq $movedX -and [int]$w['y'] -eq $movedY) `
        ("ini window x={0} y={1}" -f $(if ($w) { $w['x'] } else { '?' }), $(if ($w) { $w['y'] } else { '?' }))
    Assert-Cond 'I06/ini [documents] 记录顺序与 active' `
        ($docs -and [int]$docs['size'] -eq 3 -and
         $docs['1\url'] -eq (Url-ValueOf $files[0]) -and
         $docs['2\url'] -eq (Url-ValueOf $files[1]) -and
         $docs['3\url'] -eq (Url-ValueOf $files[2]) -and
         $docs['active'] -eq (Url-ValueOf $files[0])) `
        ("size={0} active={1}" -f $(if ($docs) { $docs['size'] } else { '?' }), $(if ($docs) { $docs['active'] } else { '?' }))
    Assert-Cond 'I06/ini [documents] 记录逐文档光标' `
        ($docs -and [int]$docs['1\cursor'] -eq 3 -and [int]$docs['2\cursor'] -eq 5 -and [int]$docs['3\cursor'] -eq 7)

    # 重启（无参数）→ 断言恢复
    $p2 = Start-App $u06 $script:DumpPath
    $main2 = Wait-MainWindow $p2
    $d = Get-DumpJson $main2
    Assert-Cond 'I06/重启：3 标签顺序还原' `
        ($d.documents.Count -eq 3 -and
         (Url-Norm $d.documents[0].url) -eq (Url-Norm $files[0]) -and
         (Url-Norm $d.documents[1].url) -eq (Url-Norm $files[1]) -and
         (Url-Norm $d.documents[2].url) -eq (Url-Norm $files[2])) `
        ("docs={0}" -f $d.documents.Count)
    Assert-Cond 'I06/重启：活动标签还原（activeUrl 命中 doc0）' ($d.activeIndex -eq 0)
    # 回归：仅活动编辑器带 WS_VISIBLE 样式位。曾因 setActiveDocument 用
    # IsWindowVisible 守卫 SW_HIDE——其含祖先链有效可见性，主窗口隐藏恢复期
    # 恒为 FALSE，非活动编辑器残留样式位，主窗口显示时全部重叠、非活动者盖
    # 住活动者（切换一次标签才恢复）；修复为无条件隐藏后样式位应恰好命中
    # 活动文档一个。headless 下 IsWindowVisible 全 FALSE 不可用，直接查样式位
    $docsArr = @()
    if ($d -and $d.documents) { $docsArr = @($d.documents) }
    $visBits = Get-VisBits $d
    $visOk = ($docsArr.Count -gt 0 -and $visBits.Count -eq $docsArr.Count)
    for ($i = 0; $i -lt $visBits.Count; $i++) {
        if ($visBits[$i] -ne ($i -eq $d.activeIndex)) { $visOk = $false }
    }
    Assert-Cond 'I06/重启：仅活动编辑器可见（WS_VISIBLE 样式位回归）' $visOk `
        ("active={0} vis=[{1}] editors=[{2}]" -f $d.activeIndex, ($visBits -join ','),
         (@($docsArr | ForEach-Object { [string]$_.editor }) -join ','))
    Assert-Cond 'I06/重启：逐文档光标还原' `
        ($d.documents[0].cursor -eq 3 -and $d.documents[1].cursor -eq 5 -and $d.documents[2].cursor -eq 7) `
        ("cursors=[{0},{1},{2}]" -f $d.documents[0].cursor, $d.documents[1].cursor, $d.documents[2].cursor)
    Assert-Cond 'I06/重启：窗口几何还原（GetWindowRect == ini 记录）' `
        ($d.window.x -eq $movedX -and $d.window.y -eq $movedY -and
         $d.window.width -eq ([int]$w['width']) -and $d.window.height -eq ([int]$w['height'])) `
        ("window=({0},{1},{2},{3}) ini=({4},{5})" -f $d.window.x, $d.window.y, $d.window.width, $d.window.height, $w['width'], $w['height'])

    # TC-002 闭环：重启恢复（活动=doc0）后依次切换标签（Ctrl+Tab 等价命令），
    # 每次切换后断言仅新活动编辑器带 WS_VISIBLE 样式位——对应用户症状的第二
    # 半"切换一次后才正常"：修复后每次切换（含首屏）均正确，不依赖切换自救。
    # 焦点断言不在此做：本测试执行上下文与被测进程 desktop 隔离，
    # GetGUIThreadInfo/AttachThreadInput 跨进程读取实测均失败（返回无效 0x0，
    # 见测试报告）；焦点语义由 exe 进程内 selftest 覆盖（headless 下进程内
    # GetFocus 有效，verify-doc-selftest.ps1 的 editor focus 场景）
    foreach ($target in 1, 2, 0) {
        Send-Command $main2 $CMD_NEXTTAB   # Ctrl+Tab 等价：0→1→2→0
        $d = Get-DumpJson $main2
        $visBits = Get-VisBits $d
        $visOk = ($d -and $d.documents.Count -gt $target -and
                  $visBits.Count -eq $d.documents.Count)
        for ($j = 0; $j -lt $visBits.Count; $j++) {
            if ($visBits[$j] -ne ($j -eq $target)) { $visOk = $false }
        }
        Assert-Cond ("I06/重启后切换 tab{0}：仅新活动编辑器带样式位" -f $target) `
            ($d.activeIndex -eq $target -and $visOk) `
            ("active={0} vis=[{1}]" -f $d.activeIndex, ($visBits -join ','))
    }
    Stop-AppForce $p2

    # ================================================================
    Write-Host "`n=== TC-I07 关闭现场还原（[A, 未命名1, B] 活动=未命名1）==="
    # ================================================================
    $u07 = Join-Path $TempRoot 'i07-user'
    $mixText = 'mix-draft-未命名内容-7'
    $fileA = Join-Path $TempRoot 'tc-i07-a.txt'
    $fileB = Join-Path $TempRoot 'tc-i07-b.txt'
    [System.IO.File]::WriteAllText($fileA, "file-A-content`n", $utf8NoBom)
    [System.IO.File]::WriteAllText($fileB, "file-B-content`n", $utf8NoBom)

    # phase1：[A] + Ctrl+N + 键入 → 强杀（草稿落盘）
    $p1 = Start-App $u07 $script:DumpPath @($fileA)
    $main1 = Wait-MainWindow $p1
    Send-Command $main1 $CMD_NEW   # Ctrl+N 等价 → [A, 未命名-1]，活动=未命名-1
    Type-Text $main1 $mixText
    $d = Get-DumpJson $main1
    Assert-Cond 'I07/phase1 现场：[A, 未命名-1(带内容)] 活动=未命名-1' `
        ($d.documents.Count -eq 2 -and (Url-Norm $d.documents[0].url) -eq (Url-Norm $fileA) -and
         $d.documents[1].name -eq '未命名-1' -and $d.documents[1].content -eq $mixText -and $d.activeIndex -eq 1) `
        ("docs={0} active={1}" -f $d.documents.Count, $d.activeIndex)
    Start-Sleep -Milliseconds 1600   # 草稿 1s 防抖落盘
    Stop-AppForce $p1

    # phase2：恢复 [A, 未命名1@1] + 参数 [A, B]（A 去重、B 追加）→ [A, U1, B]；
    #         Ctrl+Tab×2 激活 U1 → 强杀（会话记 uid@1 + activeUid；草稿续写）
    $p2 = Start-App $u07 $script:DumpPath @($fileA, $fileB)
    $main2 = Wait-MainWindow $p2
    $d = Get-DumpJson $main2
    Assert-Cond 'I07/phase2 现场：[A, 未命名1(带内容), B] 活动=末开 B' `
        ($d.documents.Count -eq 3 -and
         (Url-Norm $d.documents[0].url) -eq (Url-Norm $fileA) -and
         $d.documents[1].name -eq '未命名-1' -and $d.documents[1].content -eq $mixText -and
         (Url-Norm $d.documents[2].url) -eq (Url-Norm $fileB) -and $d.activeIndex -eq 2) `
        ("docs={0} active={1}" -f $d.documents.Count, $d.activeIndex)
    Send-Command $main2 $CMD_NEXTTAB   # Ctrl+Tab 等价：2 → 0
    Send-Command $main2 $CMD_NEXTTAB   # Ctrl+Tab 等价：0 → 1（未命名1）
    $d = Get-DumpJson $main2
    Assert-Cond 'I07/phase2 活动=未命名1（uid 会话将记 activeUid）' ($d.activeIndex -eq 1)
    Start-Sleep -Milliseconds 1600   # 恢复种子续写（1s）+ 会话防抖（500ms）
    Stop-AppForce $p2

    # ini 证据：条目 2 为 uid（非 url）、activeUid 互斥
    $ini = Get-IniMap (Join-Path $u07 'qcnote.ini')
    $docs07 = $ini['documents']
    Assert-Cond 'I07/ini 记录 uid@2 条目 + activeUid' `
        ($docs07 -and [int]$docs07['size'] -eq 3 -and
         $docs07['1\url'] -eq (Url-ValueOf $fileA) -and
         $null -ne $docs07['2\uid'] -and $docs07['2\uid'].Length -gt 0 -and
         $docs07['3\url'] -eq (Url-ValueOf $fileB) -and
         $null -ne $docs07['activeUid'] -and $docs07['activeUid'] -eq $docs07['2\uid']) `
        ("size={0} activeUid?={1}" -f $(if ($docs07) { $docs07['size'] } else { '?' }), $(if ($docs07) { $docs07['activeUid'] -ne $null } else { '?' }))

    # phase3：重启断言顺序与激活还原
    $p3 = Start-App $u07 $script:DumpPath
    $main3 = Wait-MainWindow $p3
    $d = Get-DumpJson $main3
    Assert-Cond 'I07/重启：顺序 [A, 未命名1(带内容●), B] + 激活=未命名1' `
        ($d.documents.Count -eq 3 -and
         (Url-Norm $d.documents[0].url) -eq (Url-Norm $fileA) -and
         $d.documents[1].name -eq '未命名-1' -and $d.documents[1].content -eq $mixText -and
         $d.documents[1].modified -eq $true -and $d.documents[1].url -eq '' -and
         (Url-Norm $d.documents[2].url) -eq (Url-Norm $fileB) -and
         $d.activeIndex -eq 1) `
        ("docs={0} active={1} midContent='{2}'" -f $d.documents.Count, $d.activeIndex, $d.documents[1].content)
    Stop-AppForce $p3
}
finally {
    foreach ($p in $script:Procs) {
        if ($p -and -not $p.HasExited) {
            $main = $script:MainHwnd[[uint32]$p.Id]
            if ($main) {
                [QcE2e]::SendMessage($main, $WM_CLOSE_MSG, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
            } else {
                $p.CloseMainWindow() | Out-Null
            }
            if (-not $p.WaitForExit(2000)) { Stop-AppForce $p }
            $p.Dispose()
        }
    }
}

Write-Output ''
# 防扰自证（观察项，不计入失败判定：真实用户在测试期间的正常前台切换
# 也会改变该值，只作证据记录；--headless 本身不产生任何前台抢占动作）
$fgAfter = [QcE2e]::GetForegroundWindow()
$fgStable = ($fgBefore -eq $fgAfter)
Write-Output ("foreground-stable: {0}（before=0x{1:X} after=0x{2:X}；若变化通常为用户自身操作）" -f $(if ($fgStable) { 'PASS' } else { 'WARN' }), [int64]$fgBefore, [int64]$fgAfter)
$failed = @($script:Results | Where-Object { -not $_.Pass })
foreach ($r in $script:Results) {
    Write-Output ("scenario={0}: {1}" -f $r.Case, $(if ($r.Pass) { 'PASS' } else { 'FAIL' }))
}
$verdict = if ($failed.Count -eq 0) { 'PASS' } else { 'FAIL' }
$passedCount = $script:Results.Count - $failed.Count
$totalCount = $script:Results.Count
Write-Output ("overall: {0}（{1}/{2} 通过）" -f $verdict, $passedCount, $totalCount)
if ($failed.Count -gt 0) { exit 1 } else { exit 0 }
