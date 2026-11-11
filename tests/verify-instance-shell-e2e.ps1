#Requires -Version 7
param(
    [string]$ExePath = 'build-win\qcnote.exe',
    [switch]$KeepTemp
)

# =====================================================================
# qcnote M5 单实例 / shell 集成 / 便携模式 / 命令行端到端验证
# （测试用例文档 TC-I03 / TC-I04 / TC-I10 / TC-I11，真实多进程）
#
#   TC-I04 单实例转发（D1）：
#     实例 A（--headless + --userdir 隔离域）运行中 → 实例 B 同域带文件参数
#     启动 → 断言 B 立即退出（退出码 0）、A 的 dump 显示文件已打开（打开队列
#     消化轮询）；实例 C 带 [已开文件, 新文件] → 断言两个都打开且重复路径
#     去重激活（2 文档、活动=新文件）；实例 D 不带参数 → 空列表转发仅激活
#     （A 文档数不变）。互斥量域由 --userdir 派生（exe 内 FNV-1a64 哈希），
#     不同 userdir = 不同域——与同机真实运行实例（基础域）互不干扰。
#     降级路径（窗口枚举失败/发送失败 → 独立运行）在单测覆盖
#     （tests/test_platform.cpp：SelectPrimaryWindow 注入 + TryForwardToPrimary
#     真实无窗口域 → false），e2e 层不模拟。
#   TC-I03 命令行参数（D2）：多文件 / 含空格 / 中文路径启动（--headless）→
#     dump 断言按序打开、激活末位、内容正确；不存在文件 → 打开失败弹错误框
#     （模态：WM_CLOSE 被拒 + dump 仍响应 + 不产生文档），强杀清理。
#   TC-I10 便携模式（D5）：exe 复制到临时目录 + qcnote.portable 标记 → 启动
#     （--headless，无 --userdir：单实例不参与）→ 断言 ini 与 drafts/ 落 exe
#     旁（Ctrl+N + 键入 → 1s 防抖草稿落盘）；删标记 → 启动（--userdir）→
#     断言 ini 落 userdir 且 exe 旁无落点。
#   TC-I11 shell 注册（D3）：exe 测试钩子 --selftest-shell-register/unregister/
#     query=<suffix> 于临时键名（HKCU\Software\Classes\*\shell\qcnote-selftest-
#     <sfx>，全程不碰真实键 qcnote）→ 断言注册表值（命令串 "<exe>" "%1" /
#     图标 / 显示文本 / 首启标记 DWORD）→ unregister → 断言清除 + query 退出码
#     翻转。finally 兜底删除测试子树。
#
# 测试隔离（用户测试约束）：
# - 全程 --headless（窗口不显示、停泊离屏）或立即退出的钩子进程——零可见
#   元素、零前台抢占；错误对话框以离屏主窗口为锚渲染在屏幕外。
# - --userdir 每场景独立目录（单实例域 + 用户数据双重隔离），不触碰
#   %LOCALAPPDATA%\qcnote 与 exe 旁便携落点（TC-I10 专门场景除外，其落点在
#   任务临时区内）。
# - shell 场景只写 HKCU 下 qcnote-selftest-<sfx> 临时键，真实键（Software\
#   Classes\*\shell\qcnote 与 Software\qcnote\qcnote）零读写；首启提示在
#   --headless 形态下被 exe 侧跳过（不读不写注册表）。
# 断言辅助：--selftest-dump=<path>（WM_APP+6 同步采样 JSON）；Ctrl+N 经
#   WM_APP+7 命令钩子（与快捷键同命令函数）；键入经 WM_CHAR 直投活动编辑器。
# 用法：pwsh.exe -NoProfile -ExecutionPolicy Bypass -File tests\verify-instance-shell-e2e.ps1
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

$TempRoot = 'D:\temp\aiRunner\opencode\qcnote-win32-rewrite-20261110183000\e2e-instance-shell'
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
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
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

# ---------------- 常量 ----------------
$WM_CHAR = 0x0102; $WM_CLOSE_MSG = 0x0010
$MSG_DUMP = 0x8006     # WM_APP + 6（exe 内 kMsgSelftestDump）
$MSG_COMMAND = 0x8007  # WM_APP + 7（exe 内 kMsgSelftestCommand）
$CMD_NEW = 1

# ---------------- 断言收集 ----------------
$script:Results = @()
function Assert-Cond([string]$case, [bool]$cond, [string]$detail = '') {
    $script:Results += [pscustomobject]@{ Case = $case; Pass = $cond; Detail = $detail }
    $tag = if ($cond) { 'PASS' } else { 'FAIL' }
    Write-Host ("  [{0}] {1}{2}" -f $tag, $case, $(if ($detail) { "  ($detail)" } else { '' }))
}
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

# ---------------- 进程与窗口驱动 ----------------
$script:Procs = @()        # try/finally 统一清理
$script:MainHwnd = @{}     # pid → 主窗口句柄
$script:DumpPath = Join-Path $TempRoot 'state-dump.json'

function Start-App([string]$userDir, [string[]]$files = @()) {
    $args = @('--headless', "--userdir=$userDir", "--selftest-dump=$script:DumpPath")
    foreach ($f in $files) { $args += ('"{0}"' -f $f) }
    $p = Start-Process -FilePath $ExePath -ArgumentList $args -PassThru -WorkingDirectory $TempRoot
    $script:Procs += $p
    return $p
}

# 二次实例（转发后退出；不需要 dump 钩子——退出码即断言对象）
function Start-SecondInstance([string]$userDir, [string[]]$files = @()) {
    $args = @('--headless', "--userdir=$userDir")
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
    Remove-Item $script:DumpPath -Force -ErrorAction SilentlyContinue
    $r = [IntPtr]::Zero
    [QcE2e]::SendMessageTimeout($main, $MSG_DUMP, [IntPtr]::Zero, [IntPtr]::Zero, 2, 5000, [ref]$r) | Out-Null
    if (Test-Path $script:DumpPath) {
        return Get-Content $script:DumpPath -Raw -Encoding utf8 | ConvertFrom-Json
    }
    return $null
}

function Send-Command([IntPtr]$main, [int]$code) {
    $r = [IntPtr]::Zero
    [QcE2e]::SendMessageTimeout($main, $MSG_COMMAND, [IntPtr]$code, [IntPtr]::Zero, 2, 5000, [ref]$r) | Out-Null
    Start-Sleep -Milliseconds 150
}

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

function Stop-AppForce([System.Diagnostics.Process]$proc) {
    if ($proc -and -not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit(3000) | Out-Null }
}

# 二次实例断言：限时退出 + 退出码 0（「转发成功即退出」契约）
function Assert-ForwardedExit([System.Diagnostics.Process]$proc, [string]$case) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $exited = $proc.WaitForExit(10000)
    $ms = $sw.ElapsedMilliseconds
    Assert-Cond $case ($exited -and $proc.ExitCode -eq 0) `
        ("exited={0} code={1} elapsedMs={2}" -f $exited, $(if ($exited) { $proc.ExitCode } else { '-' }), $ms)
}

# dump 的 url 兼容原生反斜杠 / 正斜杠两种来源（同会话 e2e 脚本口径）
function Url-Norm([string]$path) { return $path.Replace('\', '/') }
# 内容比较归一：磁盘文件带结尾 LF，Document 内容含它——剥离行尾比较
function Content-Norm([string]$s) { return $s.TrimEnd("`r", "`n") }

# ---------------- 注册表读取（TC-I11） ----------------
function Get-RegDefault([string]$subKey) {
    $k = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($subKey)
    if (-not $k) { return $null }
    try { return $k.GetValue($null) } finally { $k.Dispose() }
}
function Get-RegValue([string]$subKey, [string]$name) {
    $k = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($subKey)
    if (-not $k) { return $null }
    try { return $k.GetValue($name) } finally { $k.Dispose() }
}

# shell 测试钩子进程（注册/反注册/查询后立即退出；退出码 = 操作结果）
function Invoke-ShellHook([string]$verb, [string]$sfx) {
    $p = Start-Process -FilePath $ExePath -ArgumentList @("--selftest-shell-$verb=$sfx") -PassThru -WorkingDirectory $TempRoot
    $script:Procs += $p
    if (-not $p.WaitForExit(10000)) { Stop-AppForce $p; return -1 }
    return $p.ExitCode
}

$fgBefore = [QcE2e]::GetForegroundWindow()   # 防扰自证：全程前台焦点不变
try {
    # ================================================================
    Write-Host "`n=== TC-I04 单实例转发（A 运行中，B/C/D 二次启动）==="
    # ================================================================
    $u04 = Join-Path $TempRoot 'i04-user'
    $file1 = Join-Path $TempRoot 'tc-i04-one.txt'
    $file2 = Join-Path $TempRoot 'tc-i04-带 空 格 二号.md'
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($file1, "instance-one`n", $utf8NoBom)
    [System.IO.File]::WriteAllText($file2, "实例二 内容`n", $utf8NoBom)

    $a = Start-App $u04
    $mainA = Wait-MainWindow $a
    $d = Get-DumpJson $mainA
    Assert-Cond 'I04/A 首实例启动（隔离域，欢迎页 0 文档）' ($null -ne $d -and $d.documents.Count -eq 0) `
        ("docs={0}" -f $(if ($d) { $d.documents.Count } else { -1 }))

    # B：带 file1 二次启动 → 转发后立即退出（码 0）；A 打开队列消化
    $b = Start-SecondInstance $u04 @($file1)
    Assert-ForwardedExit $b 'I04/B 二次启动立即退出且退出码 0（转发成功）'
    Wait-Assert { ($d = Get-DumpJson $mainA) -and $d.documents.Count -eq 1 -and
                  (Url-Norm $d.documents[0].url) -eq (Url-Norm $file1) -and
                  $d.activeIndex -eq 0 -and (Content-Norm $d.documents[0].content) -eq 'instance-one' } `
        'I04/A 经打开队列消化转发参数（file1 打开 + 激活）'

    # C：带 [file1（已开）, file2] → 去重激活 + 新开；最终 [file1, file2] 活动=file2
    $c = Start-SecondInstance $u04 @($file1, $file2)
    Assert-ForwardedExit $c 'I04/C 二次启动（重复+新文件）退出码 0'
    Wait-Assert { ($d = Get-DumpJson $mainA) -and $d.documents.Count -eq 2 -and
                  (Url-Norm $d.documents[0].url) -eq (Url-Norm $file1) -and
                  (Url-Norm $d.documents[1].url) -eq (Url-Norm $file2) -and
                  $d.activeIndex -eq 1 -and (Content-Norm $d.documents[1].content) -eq '实例二 内容' } `
        'I04/重复路径去重激活 + 新文件打开（2 文档，活动=末开）'

    # D：不带参数 → 空列表转发仅激活（文档集不变）
    $d2 = Start-SecondInstance $u04
    Assert-ForwardedExit $d2 'I04/D 无参数二次启动退出码 0（空列表转发=仅激活）'
    Start-Sleep -Milliseconds 300
    $d = Get-DumpJson $mainA
    Assert-Cond 'I04/D 转发后 A 文档集不变（仅激活不开新标签）' `
        ($null -ne $d -and $d.documents.Count -eq 2) `
        ("docs={0}" -f $(if ($d) { $d.documents.Count } else { -1 }))
    Stop-AppForce $a

    # ================================================================
    Write-Host "`n=== TC-I03 命令行参数（多文件/空格/中文/不存在）==="
    # ================================================================
    $u03 = Join-Path $TempRoot 'i03-user'
    $f1 = Join-Path $TempRoot 'tc-i03-a.txt'
    $f2 = Join-Path $TempRoot 'tc-i03-b c d.txt'          # 多空格
    $f3 = Join-Path $TempRoot 'tc-i03-中 文 件.md'        # 中文 + 空格
    [System.IO.File]::WriteAllText($f1, "one`n", $utf8NoBom)
    [System.IO.File]::WriteAllText($f2, "two spaces`n", $utf8NoBom)
    [System.IO.File]::WriteAllText($f3, "中文内容三`n", $utf8NoBom)

    $p3 = Start-App $u03 @($f1, $f2, $f3)
    $main3 = Wait-MainWindow $p3
    $d = Get-DumpJson $main3
    Assert-Cond 'I03/多文件（空格/中文路径）按序打开、激活末位、内容正确' `
        ($null -ne $d -and $d.documents.Count -eq 3 -and
         (Url-Norm $d.documents[0].url) -eq (Url-Norm $f1) -and
         (Url-Norm $d.documents[1].url) -eq (Url-Norm $f2) -and
         (Url-Norm $d.documents[2].url) -eq (Url-Norm $f3) -and
         $d.activeIndex -eq 2 -and
         (Content-Norm $d.documents[1].content) -eq 'two spaces' -and
         (Content-Norm $d.documents[2].content) -eq '中文内容三') `
        ("docs={0} active={1}" -f $(if ($d) { $d.documents.Count } else { -1 }),
         $(if ($d) { $d.activeIndex } else { -1 }))
    Stop-AppForce $p3

    # 相对路径 → 绝对化（D2 健壮性接线验证）：工作目录 = $TempRoot 传相对名，
    # dump 断言文档 url 已按当前目录解析为绝对形态（旧版 fromUserInput 语义）
    $relName = 'tc-i03-rel 相对路径.md'
    $relAbs = Join-Path $TempRoot $relName
    [System.IO.File]::WriteAllText($relAbs, "relative`n", $utf8NoBom)
    $u03r = Join-Path $TempRoot 'i03r-user'
    $p3r = Start-App $u03r @($relName)
    $main3r = Wait-MainWindow $p3r
    Wait-Assert { ($d = Get-DumpJson $main3r) -and $d.documents.Count -eq 1 -and
                  $d.documents[0].url -like '*:*' -and
                  (Url-Norm $d.documents[0].url) -eq (Url-Norm $relAbs) } `
        'I03/相对路径启动 → 绝对化打开（去重/会话持久化基础）'
    Stop-AppForce $p3r

    # 不存在文件：打开失败弹错误框（模态）——不崩溃、不产生文档、dump 仍响应、
    # WM_CLOSE 被拒（模态语义），强杀清理
    $u03b = Join-Path $TempRoot 'i03b-user'
    $missing = Join-Path $TempRoot '不存在的 文件-自测.txt'
    $p3b = Start-App $u03b @($missing)
    $main3b = Wait-MainWindow $p3b
    $d = Get-DumpJson $main3b
    Assert-Cond 'I03/不存在文件：无文档产生 + dump 仍响应（错误框模态中）' `
        ($null -ne $d -and $d.documents.Count -eq 0) `
        ("docs={0}" -f $(if ($d) { $d.documents.Count } else { -1 }))
    [QcE2e]::SendMessage($main3b, $WM_CLOSE_MSG, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 400
    Assert-Cond 'I03/不存在文件：模态错误框期间 WM_CLOSE 被拒（进程存活）' `
        (-not $p3b.HasExited) ("exited={0}" -f $p3b.HasExited)
    Stop-AppForce $p3b

    # ================================================================
    Write-Host "`n=== TC-I10 便携模式（qcnote.portable 标记切换落点）==="
    # ================================================================
    $portableDir = Join-Path $TempRoot 'portable'
    New-Item -ItemType Directory -Force -Path $portableDir | Out-Null
    $portableExe = Join-Path $portableDir 'qcnote.exe'
    Copy-Item $ExePath $portableExe -Force
    New-Item -ItemType File -Path (Join-Path $portableDir 'qcnote.portable') -Force | Out-Null

    # 便携态：ini/drafts 落 exe 旁（--headless 无 --userdir：单实例不参与，
    # 与真实实例基础域互不干扰）
    $argsP = @('--headless', "--selftest-dump=$script:DumpPath")
    $pp = Start-Process -FilePath $portableExe -ArgumentList $argsP -PassThru -WorkingDirectory $TempRoot
    $script:Procs += $pp
    $mainP = Wait-MainWindow $pp
    Wait-Assert { Test-Path (Join-Path $portableDir 'qcnote.ini') } `
        'I10/便携：会话 ini 落 exe 旁'
    # 草稿落点：Ctrl+N + 键入 → 1s 防抖 + 200ms timer 余量 → exe 旁 drafts/
    Send-Command $mainP $CMD_NEW
    Type-Text $mainP 'portable-draft-便携草稿'
    Start-Sleep -Milliseconds 1600
    $draftsPortable = @(Get-ChildItem (Join-Path $portableDir 'drafts') -Filter '*.json' -File -ErrorAction SilentlyContinue)
    Assert-Cond 'I10/便携：草稿条目落 exe 旁 drafts/' ($draftsPortable.Count -eq 1) `
        ("drafts={0}" -f $draftsPortable.Count)
    Stop-AppForce $pp

    # 非便携态（删标记）：落点切回用户目录（--userdir 注入验证）
    Remove-Item (Join-Path $portableDir 'qcnote.portable') -Force
    Remove-Item (Join-Path $portableDir 'qcnote.ini') -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $portableDir 'drafts') -Recurse -Force -ErrorAction SilentlyContinue
    $u10 = Join-Path $TempRoot 'i10-user'
    $argsU = @('--headless', "--userdir=$u10", "--selftest-dump=$script:DumpPath")
    $pu = Start-Process -FilePath $portableExe -ArgumentList $argsU -PassThru -WorkingDirectory $TempRoot
    $script:Procs += $pu
    $null = Wait-MainWindow $pu
    Wait-Assert { (Test-Path (Join-Path $u10 'qcnote.ini')) -and
                  -not (Test-Path (Join-Path $portableDir 'qcnote.ini')) } `
        'I10/删标记：ini 落用户目录且 exe 旁无落点'
    Stop-AppForce $pu

    # ================================================================
    Write-Host "`n=== TC-I11 shell 集成（临时键名注入，不碰真实键）==="
    # ================================================================
    $sfx = 'e2e-{0}-{1:x}' -f $PID, (Get-Random -Maximum 0xFFFF)
    $menuKey = "Software\Classes\*\shell\qcnote-selftest-$sfx"
    $commandKey = "$menuKey\command"
    $markerKey = "Software\qcnote-selftest-$sfx"
    $quotedExe = '"{0}"' -f $ExePath

    Assert-Cond 'I11/注册前：临时菜单键不存在' ($null -eq (Get-RegDefault $menuKey))

    $rc = Invoke-ShellHook 'register' $sfx
    Assert-Cond 'I11/register 钩子退出码 0' ($rc -eq 0) ("exit={0}" -f $rc)
    Assert-Cond 'I11/菜单键默认值 = 显示文本' ((Get-RegDefault $menuKey) -eq '用 qcnote 打开') `
        ("value='{0}'" -f (Get-RegDefault $menuKey))
    Assert-Cond 'I11/Icon 值 = 带引号 exe 绝对路径' ((Get-RegValue $menuKey 'Icon') -eq $quotedExe) `
        ("icon='{0}'" -f (Get-RegValue $menuKey 'Icon'))
    Assert-Cond 'I11/command 默认值 = `"<exe>`" `"%1`"' `
        ((Get-RegDefault $commandKey) -eq ($quotedExe + ' "%1"')) `
        ("command='{0}'" -f (Get-RegDefault $commandKey))
    Assert-Cond 'I11/首启标记 contextMenuPrompted=1 (REG_DWORD)' `
        ((Get-RegValue $markerKey 'contextMenuPrompted') -eq 1)

    $rc = Invoke-ShellHook 'query' $sfx
    Assert-Cond 'I11/query 已注册 → 退出码 0' ($rc -eq 0) ("exit={0}" -f $rc)

    $rc = Invoke-ShellHook 'unregister' $sfx
    Assert-Cond 'I11/unregister 退出码 0' ($rc -eq 0) ("exit={0}" -f $rc)
    Assert-Cond 'I11/反注册后菜单键整树清除（含 command）' ($null -eq (Get-RegDefault $commandKey))

    $rc = Invoke-ShellHook 'query' $sfx
    Assert-Cond 'I11/query 未注册 → 退出码 1' ($rc -eq 1) ("exit={0}" -f $rc)

    # unregister 语义边界：只删菜单树，首启标记保留（不再重复询问属用户决策，
    # 全量清理归测试 finally 兜底/生产卸载场景）
    Assert-Cond 'I11/unregister 后首启标记保留（仅清菜单树）' `
        ((Get-RegValue $markerKey 'contextMenuPrompted') -eq 1)
}
finally {
    # 进程清理（按 PID 对象，绝不误杀本机其他 qcnote 实例）
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
    # 注册表测试子树兜底清理（unregister 场景已删菜单树，此处防御中途失败残留；
    # throw 不含 $sfx（局部变量在 finally 可见），直接按已知命名模式清）：
    # 仅删除 qcnote-selftest-* 命名的键，绝不触碰真实键（Software\Classes\*\
    # shell\qcnote 与 Software\qcnote\qcnote）
    try {
        $shellRoot = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Classes\*\shell', $true)
        if ($shellRoot) {
            foreach ($name in @($shellRoot.GetSubKeyNames() | Where-Object { $_ -like 'qcnote-selftest-*' })) {
                $shellRoot.DeleteSubKeyTree($name, $false)
            }
            $shellRoot.Dispose()
        }
        $softwareRoot = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software', $true)
        if ($softwareRoot) {
            foreach ($name in @($softwareRoot.GetSubKeyNames() | Where-Object { $_ -like 'qcnote-selftest-*' })) {
                $softwareRoot.DeleteSubKeyTree($name, $false)
            }
            $softwareRoot.Dispose()
        }
    } catch { Write-Output ("[WARN] 注册表测试子树清理异常: {0}" -f $_.Exception.Message) }
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
