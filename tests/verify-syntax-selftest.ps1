#Requires -Version 7
param(
    [string]$ExePath = 'build-win\qcnote.exe',
    [int]$TimeoutSec = 20
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null
$ProgressPreference = 'SilentlyContinue'

# =====================================================================
# qcnote 代码着色 headless 自测（需求「代码着色」阶段 6 独立测试产物）
# 由 T3 轮临时冒烟脚本（.localdoc/审查记录/附件-代码着色-T3-headless-smoke.ps1）
# 转正并按测试用例文档补齐覆盖：
#   TC-003 手动指定优先于自动识别（命令链 + dump/槽色断言）
#   TC-004 「自动」项恢复识别（+ 退出后 ini 不再记录手动语言键）
#   TC-005 纯文本强制不着色（词法器卸下 + 槽色复位默认文字色）
#   TC-007 会话持久化手动语言（ini 落盘 + 重启恢复装配）
#   TC-008 自动语言不入会话（ini 无 language 键——前向兼容）
#   TC-009 旧会话无键兼容（手工构造旧版 ini 重启按扩展名识别）
#   TC-010 未命名文档手动语言随 uid 恢复（草稿 + 会话链路）
#   TC-011 未命名文档默认纯文本（dump language/lexer）
#   TC-012 lexer 装配与槽色（21 语言矩阵 + 未知/txt 负例 + STYLEGETFORE）
#   TC-013 双主题重着色（浅→深→浅往返 + 词法器不动）
#   TC-015 16MB 阈值跳过着色（>16MiB 装配跳过、身份保留、手动仍跳过）
#   TC-016 另存为换扩展名重识别（e2e 等价：同内容 .txt/.py 双文件按 url 识别）
#   TC-018 查找高亮与着色共存（indicator 8 与词法装配互不干扰）
# 菜单勾选态（TC-006）由 ctest menu 套件勾选态四象限用例覆盖；映射表完整性
# （TC-014）与扩展名识别（TC-001/002）、会话键读写单元级（TC-007/008/009）
# 由 ctest core/session 套件覆盖，本脚本不重复实现。
#
# 驱动方式：全程 --headless（窗口不显示、停泊离屏——零可见元素不抢前台
#   焦点，测试防扰约定）+ --userdir=<隔离目录>（每场景独立用户目录，不触碰
#   %LOCALAPPDATA%\qcnote）+ --selftest-dump 状态采样（WM_APP+6 同步写
#   JSON）。语言切换/主题切换经 WM_APP+7 命令钩子直触 exe 内与菜单完全
#   相同的 RouteCommand 路径：5=自动、6=纯文本、4=深色主题、100+i=语言项
#   i（0..20，注册表/菜单顺序：100=C … 106=Python … 110=JSON … 120=YAML）。
#   槽位色经 editor HWND 发 SCI_STYLEGETFORE（2481）断言 COLORREF
#   （0x00BBGGRR）；查找高亮共存经 SCI_SETINDICATORCURRENT/
#   INDICATORFILLRANGE/INDICATORVALUEAT（2500/2504/2507）——与
#   SearchController::refreshHighlights/clearHighlights 同消息形态（kIndicator-
#   SearchMatch=8）。多进程场景串行执行（单实例机制 + 共享 dump 文件路径）。
#
# TC-016 等价性说明：另存为的系统保存对话框（模态）无法 headless 自动化，
#   采用等价链路断言——生效语言是 url 的计算属性（T2 设计：url 变化经
#   setUrl 的 metadata 通知链传播重算），同内容文件按 .txt/.py 两种 url 打开
#   分别得到 plaintext/python 装配，即「另存为换扩展名后的最终状态」；
#   saveAs 直调链路（url 变化→重装配）由 --selftest-doc saveAs 阶段持续
#   回归覆盖（该阶段固定 saveAs 到 .txt：装配卸下方向的同一通知链）。
#
# 用法：pwsh -NoProfile -ExecutionPolicy Bypass -File tests\verify-syntax-selftest.ps1
# 退出码：0 = 通过；1 = 失败；2 = 前置缺失
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

$TempRoot = 'D:\temp\aiRunner\opencode\qcnote-syntax-selftest-20261125\e2e-syntax'
Remove-Item -Recurse -Force $TempRoot -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $TempRoot | Out-Null
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

# ---------------- Win32 P/Invoke ----------------
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Collections.Generic;
public class QcSyntaxE2e {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, IntPtr title);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
    // CharSet=Unicode 必须显式：ANSI 变体的 WM_CHAR 会按 CP_ACP 转换 wParam，
    // 中文（>0xFF）必乱码（实测 U+4E2D → U+5640）
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageTimeout(IntPtr h, uint msg, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    public static List<IntPtr> WindowsOfPid(uint target, string cls) {
        var list = new List<IntPtr>();
        EnumWindows((h, lp) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid == target) {
                // 不过滤 IsWindowVisible：--headless 下主窗口永不显示（不可见
                // 窗口仍可收消息）；按 pid 过滤避免误绑其他 qcnote 实例
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
$WM_CLOSE_MSG = 0x0010
$WM_CHAR      = 0x0102
$MSG_DUMP     = 0x8006      # WM_APP + 6（kMsgSelftestDump）
$MSG_COMMAND  = 0x8007      # WM_APP + 7（kMsgSelftestCommand，仅 --headless 响应）
$CMD_NEW      = 1           # = Ctrl+N
$CMD_DARK     = 4           # ToggleDarkTheme（= 视图菜单「深色主题」）
$CMD_LANG_AUTO      = 5     # 语言▸自动
$CMD_LANG_PLAINTEXT = 6     # 语言▸纯文本
# 语言项 i（0..20）→ 100+i；106=Python、110=JSON
$CMD_LANG_PYTHON = 106
$CMD_LANG_JSON   = 110

# Scintilla 消息（vendor/scintilla/include/Scintilla.h）
$SCI_GETLENGTH            = 2006
$SCI_GETLEXERLANGUAGE     = 4012
$SCI_STYLEGETFORE         = 2481
$SCI_SETINDICATORCURRENT  = 2500
$SCI_INDICATORFILLRANGE   = 2504   # wParam=start, lParam=length
$SCI_INDICATORCLEARRANGE  = 2505   # wParam=start, lParam=length
$SCI_INDICATORVALUEAT     = 2507   # wParam=position → 0/1
$INDIC_SEARCH_MATCH       = 8      # editor::kIndicatorSearchMatch（查找全部匹配底色）

# 语义色板断言值（src/ui/theme.cpp 两套色板，COLORREF=0x00BBGGRR 形态）
$FORE_LIGHT_KEYWORD = 0xFF0000    # #0000FF（浅色 syntaxKeyword）
$FORE_LIGHT_NUMBER  = 0x588609    # #098658（浅色 syntaxNumber）
$FORE_LIGHT_STRING  = 0x1515A3    # #A31515（浅色 syntaxString）
$FORE_DARK_KEYWORD  = 0xD69C56    # #569CD6（深色 syntaxKeyword）
$FORE_DARK_NUMBER   = 0xA8CEB5    # #B5CEA8（深色 syntaxNumber）
$FORE_DARK_STRING   = 0x7891CE    # #CE9178（深色 syntaxString）
$FORE_DEFAULT_TEXT  = 0x342721    # #212734（浅色 textPrimary——纯文本槽色复位目标）

# ---------------- 断言收集 ----------------
$script:Results = @()
function Assert-Cond([string]$case, [bool]$cond, [string]$detail = '') {
    $script:Results += [pscustomobject]@{ Case = $case; Pass = $cond; Detail = $detail }
    $tag = if ($cond) { 'PASS' } else { 'FAIL' }
    Write-Output ("  [{0}] {1}{2}" -f $tag, $case, $(if ($detail) { "  ($detail)" } else { '' }))
}

# ---------------- 进程与窗口驱动 ----------------
$script:Procs = @()
$script:MainHwnd = @{}   # pid → 主窗口句柄（不可见窗口 CloseMainWindow 不可用）

function Start-App([string]$userDir, [string]$dumpPath, [string[]]$files = @()) {
    $args = @('--headless', "--userdir=$userDir", "--selftest-dump=$dumpPath")
    foreach ($f in $files) { $args += ('"{0}"' -f $f) }
    $p = Start-Process -FilePath $ExePath -ArgumentList $args -PassThru -WorkingDirectory $TempRoot
    $script:Procs += $p
    return $p
}

function Wait-MainWindow([System.Diagnostics.Process]$proc, [int]$timeoutMs = 10000) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        if ($proc.HasExited) { throw "进程提前退出（ExitCode=$($proc.ExitCode)）" }
        $wins = [QcSyntaxE2e]::WindowsOfPid([uint32]$proc.Id, 'QcnoteMainWindow')
        if ($wins.Count -gt 0) {
            $script:MainHwnd[[uint32]$proc.Id] = $wins[0]
            return $wins[0]
        }
        Start-Sleep -Milliseconds 50
    }
    throw "主窗口等待超时（${timeoutMs}ms）"
}

function Get-DumpJson([IntPtr]$main, [string]$dumpPath) {
    Remove-Item $dumpPath -Force -ErrorAction SilentlyContinue
    $r = [IntPtr]::Zero
    # 同步采样：SendMessageTimeout 处理返回后文件即完整（临时文件+原子替换）
    [QcSyntaxE2e]::SendMessageTimeout($main, $MSG_DUMP, [IntPtr]::Zero, [IntPtr]::Zero, 2, 5000, [ref]$r) | Out-Null
    if (Test-Path $dumpPath) {
        return Get-Content $dumpPath -Raw -Encoding utf8 | ConvertFrom-Json
    }
    return $null
}

# WM_APP+7 命令钩子直触 exe 内与菜单/快捷键相同的 RouteCommand 路径
function Send-Command([IntPtr]$main, [int]$code) {
    $r = [IntPtr]::Zero
    [QcSyntaxE2e]::SendMessageTimeout($main, $MSG_COMMAND, [IntPtr]$code, [IntPtr]::Zero, 2, 5000, [ref]$r) | Out-Null
    Start-Sleep -Milliseconds 150
}

# 对 Scintilla editor 发消息（同步）
function Send-Sci([IntPtr]$sci, [int]$msg, [long]$w, [long]$l) {
    return [QcSyntaxE2e]::SendMessage($sci, [uint32]$msg, [IntPtr]$w, [IntPtr]$l)
}

# dump 条目 editor 字段（"0x..."）→ IntPtr
function Get-EditorHwnd([object]$doc) {
    $hex = [string]$doc.editor
    if ($hex -and $hex -like '0x*') {
        return [IntPtr][Int64]::Parse($hex.Substring(2),
            [System.Globalization.NumberStyles]::HexNumber)
    }
    return [IntPtr]::Zero
}

# WM_CHAR 逐字符注入活动编辑器（Unicode 路径；置脏触发草稿防抖）
function Type-Text([IntPtr]$sci, [string]$text) {
    foreach ($ch in $text.ToCharArray()) {
        [QcSyntaxE2e]::SendMessage($sci, $WM_CHAR, [IntPtr][int][char]$ch, [IntPtr]::Zero) | Out-Null
    }
    Start-Sleep -Milliseconds 150
}

function Stop-AppForce([System.Diagnostics.Process]$proc) {
    if ($proc -and -not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit(3000) | Out-Null }
}

# 热退出（WM_CLOSE 免提示路径；退出时 flush 会话 + 草稿）
function Exit-AppNormal([System.Diagnostics.Process]$proc, [int]$timeoutMs = 8000) {
    $main = $script:MainHwnd[[uint32]$proc.Id]
    if ($main) {
        [QcSyntaxE2e]::SendMessage($main, $WM_CLOSE_MSG, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    } else {
        $proc.CloseMainWindow() | Out-Null
    }
    if (-not $proc.WaitForExit($timeoutMs)) {
        Stop-AppForce $proc
        return $false
    }
    return ($proc.ExitCode -eq 0)
}

# ---------------- ini 读取（[documents] 键分隔符为反斜杠 N\language） ----------------
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

$script:DumpPath = Join-Path $TempRoot 'state-dump.json'
$fgBefore = [QcSyntaxE2e]::GetForegroundWindow()   # 防扰自证：全程前台焦点不变
try {
    # ================================================================
    Write-Output "`n=== 场景 1：TC-012 lexer 装配矩阵（21 语言 + 未知/txt 负例）==="
    # ================================================================
    # 21 语言每语言一个代表扩展名 + .xyz（未知）+ .txt（已知非代码）= 23 文件
    $matrix = @(
        @{ Ext = 'c';   Lang = 'c';           Lexer = 'cpp' }
        @{ Ext = 'cpp'; Lang = 'cpp';         Lexer = 'cpp' }
        @{ Ext = 'cs';  Lang = 'csharp';      Lexer = 'cpp' }
        @{ Ext = 'java'; Lang = 'java';       Lexer = 'cpp' }
        @{ Ext = 'js';  Lang = 'javascript';  Lexer = 'cpp' }
        @{ Ext = 'ts';  Lang = 'typescript';  Lexer = 'cpp' }
        @{ Ext = 'go';  Lang = 'go';          Lexer = 'cpp' }   # DEC-009：go 走 cpp 词法器
        @{ Ext = 'py';  Lang = 'python';      Lexer = 'python' }
        @{ Ext = 'rs';  Lang = 'rust';        Lexer = 'rust' }
        @{ Ext = 'lua'; Lang = 'lua';         Lexer = 'lua' }
        @{ Ext = 'json'; Lang = 'json';       Lexer = 'json' }
        @{ Ext = 'md';  Lang = 'markdown';    Lexer = 'markdown' }
        @{ Ext = 'html'; Lang = 'html';       Lexer = 'hypertext' }
        @{ Ext = 'xml'; Lang = 'xml';         Lexer = 'xml' }
        @{ Ext = 'css'; Lang = 'css';         Lexer = 'css' }
        @{ Ext = 'sql'; Lang = 'sql';         Lexer = 'sql' }
        @{ Ext = 'sh';  Lang = 'bash';        Lexer = 'bash' }
        @{ Ext = 'ps1'; Lang = 'powershell';  Lexer = 'powershell' }
        @{ Ext = 'bat'; Lang = 'batch';       Lexer = 'batch' }
        @{ Ext = 'ini'; Lang = 'ini';         Lexer = 'props' } # DEC-008：ini 走 props 词法器
        @{ Ext = 'yaml'; Lang = 'yaml';       Lexer = 'yaml' }
        @{ Ext = 'xyz'; Lang = 'plaintext';   Lexer = '' }      # 未知扩展名 → 纯文本
        @{ Ext = 'txt'; Lang = 'plaintext';   Lexer = '' }      # 已知非代码 → 纯文本
    )
    $pyContent = "def hello(name):`n    # comment`n    return `"hi `" + name`n"
    $matrixFiles = @()
    for ($i = 0; $i -lt $matrix.Count; $i++) {
        $f = Join-Path $TempRoot ("s12-{0:D2}.{1}" -f $i, $matrix[$i].Ext)
        $text = if ($matrix[$i].Ext -eq 'py') { $pyContent } else { "# syntax matrix sample $($matrix[$i].Ext)`n" }
        [System.IO.File]::WriteAllText($f, $text, $utf8NoBom)
        $matrixFiles += $f
    }

    $u12 = Join-Path $TempRoot 's12-user'
    $p1 = Start-App $u12 $script:DumpPath $matrixFiles
    $main1 = Wait-MainWindow $p1
    Start-Sleep -Milliseconds 400   # 23 文档装载 + 词法装配稳定余量
    $d = Get-DumpJson $main1 $script:DumpPath
    Assert-Cond 'TC-012/启动：23 文档全部打开' ($null -ne $d -and $d.documents.Count -eq 23) `
        ("docs={0}" -f $(if ($d) { $d.documents.Count } else { -1 }))

    $matrixOk = $true
    for ($i = 0; $i -lt $matrix.Count; $i++) {
        $doc = $d.documents[$i]
        if (-not $doc -or $doc.language -ne $matrix[$i].Lang -or $doc.lexer -ne $matrix[$i].Lexer) {
            $matrixOk = $false
            Assert-Cond ("TC-012/.{0}：language={1} + lexer={2}" -f $matrix[$i].Ext, $matrix[$i].Lang, $matrix[$i].Lexer) $false `
                ("actual: lang={0} lexer={1}" -f $doc.language, $doc.lexer)
        }
    }
    if ($matrixOk) {
        Assert-Cond ("TC-012/21 语言矩阵 + 未知/txt 负例：{0} 文件 language+lexer 全部命中（含 DEC-008 ini→props、DEC-009 go→cpp）" -f $matrix.Count) $true
    }

    # 槽色断言（python 词法槽：5=SCE_P_WORD 关键字、2=SCE_P_NUMBER、3=SCE_P_STRING）
    $sciPy = Get-EditorHwnd $d.documents[7]   # 索引 7 = s12-07.py（矩阵序 0..6 为 cpp 系 7 语言）
    Assert-Cond 'TC-012/编辑器 HWND 可得（dump editor 字段）' ($sciPy -ne [IntPtr]::Zero)
    $fore5 = (Send-Sci $sciPy $SCI_STYLEGETFORE 5 0).ToInt64()
    $fore2 = (Send-Sci $sciPy $SCI_STYLEGETFORE 2 0).ToInt64()
    $fore3 = (Send-Sci $sciPy $SCI_STYLEGETFORE 3 0).ToInt64()
    Assert-Cond 'TC-012/python 关键字槽(5)色 = 浅色板 #0000FF' ($fore5 -eq $FORE_LIGHT_KEYWORD) ("got=0x{0:X6}" -f $fore5)
    Assert-Cond 'TC-012/python 数字槽(2)色 = 浅色板 #098658' ($fore2 -eq $FORE_LIGHT_NUMBER) ("got=0x{0:X6}" -f $fore2)
    Assert-Cond 'TC-012/python 字符串槽(3)色 = 浅色板 #A31515' ($fore3 -eq $FORE_LIGHT_STRING) ("got=0x{0:X6}" -f $fore3)
    Stop-AppForce $p1

    # ================================================================
    Write-Output "`n=== 场景 2：TC-003/004/005 手动优先 / 自动恢复 / 纯文本卸词法 ==="
    # ================================================================
    $u2 = Join-Path $TempRoot 's2-user'
    $aPy = Join-Path $TempRoot 'a.py'
    [System.IO.File]::WriteAllText($aPy, $pyContent, $utf8NoBom)
    $p2 = Start-App $u2 $script:DumpPath @($aPy)
    $main2 = Wait-MainWindow $p2
    Start-Sleep -Milliseconds 300
    $d = Get-DumpJson $main2 $script:DumpPath
    $sci = Get-EditorHwnd $d.documents[0]
    Assert-Cond 'TC-003/前置：打开 .py 自动识别 python + lexer 装配' ($d.documents[0].language -eq 'python' -and $d.documents[0].lexer -eq 'python') `
        ("lang={0} lexer={1}" -f $d.documents[0].language, $d.documents[0].lexer)

    # TC-003：手动切 JSON（100+10）—— cpp 系之外的词法器装配 + 槽色
    Send-Command $main2 $CMD_LANG_JSON
    $d = Get-DumpJson $main2 $script:DumpPath
    Assert-Cond 'TC-003/手动切 JSON：language=json + lexer=json（手动优先于扩展名）' ($d.documents[0].language -eq 'json' -and $d.documents[0].lexer -eq 'json') `
        ("lang={0} lexer={1}" -f $d.documents[0].language, $d.documents[0].lexer)
    $fore11 = (Send-Sci $sci $SCI_STYLEGETFORE 11 0).ToInt64()   # SCE_JSON_KEYWORD
    Assert-Cond 'TC-003/JSON 关键字槽(11)色 = 浅色 #0000FF' ($fore11 -eq $FORE_LIGHT_KEYWORD) ("got=0x{0:X6}" -f $fore11)

    # TC-005：切「纯文本」—— 词法器卸下 + 槽色复位默认文字色
    Send-Command $main2 $CMD_LANG_PLAINTEXT
    $d = Get-DumpJson $main2 $script:DumpPath
    Assert-Cond 'TC-005/切「纯文本」：language=plaintext + lexer 卸下（空）' ($d.documents[0].language -eq 'plaintext' -and $d.documents[0].lexer -eq '') `
        ("lang={0} lexer='{1}'" -f $d.documents[0].language, $d.documents[0].lexer)
    $fore5plain = (Send-Sci $sci $SCI_STYLEGETFORE 5 0).ToInt64()
    Assert-Cond 'TC-005/槽位色复位默认文字色 #212734' ($fore5plain -eq $FORE_DEFAULT_TEXT) ("got=0x{0:X6}" -f $fore5plain)

    # TC-004：切「自动」—— 回到按扩展名识别的 Python
    Send-Command $main2 $CMD_LANG_AUTO
    $d = Get-DumpJson $main2 $script:DumpPath
    Assert-Cond 'TC-004/切「自动」：恢复按扩展名识别 python' ($d.documents[0].language -eq 'python' -and $d.documents[0].lexer -eq 'python') `
        ("lang={0} lexer={1}" -f $d.documents[0].language, $d.documents[0].lexer)
    $fore5again = (Send-Sci $sci $SCI_STYLEGETFORE 5 0).ToInt64()
    Assert-Cond 'TC-004/恢复装配后槽色回浅色板 #0000FF' ($fore5again -eq $FORE_LIGHT_KEYWORD) ("got=0x{0:X6}" -f $fore5again)

    # ================================================================
    Write-Output "`n=== 场景 3：TC-018 查找高亮（indicator 8）与词法着色共存 ==="
    # ================================================================
    # 与 SearchController::refreshHighlights 同消息形态注入全部匹配底色
    # （indicator 8 覆盖位置 4..6 = "hel"）；断言两个体系互不干扰：
    #   a) indicator 生效且不着色槽色不变；
    #   b) 语言重装配（STYLECLEARALL+SETILEXER+COLOURISE）不清 indicator 填充
    $len = (Send-Sci $sci $SCI_GETLENGTH 0 0).ToInt64()
    [void](Send-Sci $sci $SCI_SETINDICATORCURRENT $INDIC_SEARCH_MATCH 0)
    [void](Send-Sci $sci $SCI_INDICATORFILLRANGE 4 3)
    # SCI_INDICATORVALUEAT(int indicator, Position position)：wParam=indicator 号、
    # lParam=位置（与 main.cpp search selftest 调用形态一致）
    $vIn  = (Send-Sci $sci $SCI_INDICATORVALUEAT $INDIC_SEARCH_MATCH 5).ToInt64()
    $vOut = (Send-Sci $sci $SCI_INDICATORVALUEAT $INDIC_SEARCH_MATCH 2).ToInt64()
    $vEnd = (Send-Sci $sci $SCI_INDICATORVALUEAT $INDIC_SEARCH_MATCH 7).ToInt64()
    Assert-Cond 'TC-018/indicator 8 填充生效（范围内=1、范围外=0）' ($vIn -eq 1 -and $vOut -eq 0 -and $vEnd -eq 0) `
        ("in={0} out={1} end={2}" -f $vIn, $vOut, $vEnd)
    $fore5ind = (Send-Sci $sci $SCI_STYLEGETFORE 5 0).ToInt64()
    Assert-Cond 'TC-018/着色槽色不受 indicator 影响（仍浅色 #0000FF）' ($fore5ind -eq $FORE_LIGHT_KEYWORD) ("got=0x{0:X6}" -f $fore5ind)

    # 语言重装配（python → json → python）后 indicator 保留
    Send-Command $main2 $CMD_LANG_JSON
    $d = Get-DumpJson $main2 $script:DumpPath
    $vAfterJson = (Send-Sci $sci $SCI_INDICATORVALUEAT $INDIC_SEARCH_MATCH 5).ToInt64()
    Assert-Cond 'TC-018/切 JSON 重装配后：词法器换 json 且 indicator 高亮保留' ($d.documents[0].lexer -eq 'json' -and $vAfterJson -eq 1) `
        ("lexer={0} indic={1}" -f $d.documents[0].lexer, $vAfterJson)
    Send-Command $main2 $CMD_LANG_AUTO
    $d = Get-DumpJson $main2 $script:DumpPath
    $vAfterBack = (Send-Sci $sci $SCI_INDICATORVALUEAT $INDIC_SEARCH_MATCH 5).ToInt64()
    Assert-Cond 'TC-018/切回自动重装配后：词法器回 python 且 indicator 高亮保留' ($d.documents[0].lexer -eq 'python' -and $vAfterBack -eq 1) `
        ("lexer={0} indic={1}" -f $d.documents[0].lexer, $vAfterBack)
    # 清理（与 clearHighlights 同形态），避免影响后续场景断言口径
    [void](Send-Sci $sci $SCI_INDICATORCLEARRANGE 0 $len)

    # ================================================================
    Write-Output "`n=== 场景 4：TC-013 双主题重着色（浅→深→浅）==="
    # ================================================================
    Send-Command $main2 $CMD_DARK
    $d = Get-DumpJson $main2 $script:DumpPath
    $f5d = (Send-Sci $sci $SCI_STYLEGETFORE 5 0).ToInt64()
    $f2d = (Send-Sci $sci $SCI_STYLEGETFORE 2 0).ToInt64()
    $f3d = (Send-Sci $sci $SCI_STYLEGETFORE 3 0).ToInt64()
    Assert-Cond 'TC-013/切深色：关键字槽 = 深色板 #569CD6' ($f5d -eq $FORE_DARK_KEYWORD) ("got=0x{0:X6}" -f $f5d)
    Assert-Cond 'TC-013/切深色：数字槽 = 深色板 #B5CEA8' ($f2d -eq $FORE_DARK_NUMBER) ("got=0x{0:X6}" -f $f2d)
    Assert-Cond 'TC-013/切深色：字符串槽 = 深色板 #CE9178' ($f3d -eq $FORE_DARK_STRING) ("got=0x{0:X6}" -f $f3d)
    Assert-Cond 'TC-013/切深色：词法器不动（retheme 只重放槽位色）' ($d.documents[0].lexer -eq 'python' -and $d.darkTheme -eq $true)

    Send-Command $main2 $CMD_DARK   # 切回浅色
    $d = Get-DumpJson $main2 $script:DumpPath
    $f5back = (Send-Sci $sci $SCI_STYLEGETFORE 5 0).ToInt64()
    Assert-Cond 'TC-013/切回浅色：槽色恢复浅色板 #0000FF' ($f5back -eq $FORE_LIGHT_KEYWORD -and $d.darkTheme -eq $false -and $d.documents[0].lexer -eq 'python') `
        ("got=0x{0:X6}" -f $f5back)

    # ================================================================
    Write-Output "`n=== 场景 5：TC-007 会话持久化手动语言（落盘 + 重启恢复）==="
    # ================================================================
    Send-Command $main2 $CMD_LANG_JSON   # 手动 JSON 后热退出
    $normalExit = Exit-AppNormal $p2
    Assert-Cond 'TC-007/热退出正常（WM_CLOSE flush）' $normalExit
    $ini = Get-IniMap (Join-Path $u2 'qcnote.ini')
    $docs2 = $ini['documents']
    Assert-Cond 'TC-007/ini 落盘 1\language=json（手动语言记录）' ($docs2 -and $docs2['1\language'] -eq 'json' -and $docs2['1\url'] -like '*a.py') `
        ("language={0}" -f $(if ($docs2) { $docs2['1\language'] } else { '?' }))

    $p2b = Start-App $u2 $script:DumpPath
    $main2b = Wait-MainWindow $p2b
    Start-Sleep -Milliseconds 400
    $d = Get-DumpJson $main2b $script:DumpPath
    Assert-Cond 'TC-007/重启恢复：会话重开 a.py 且手动 JSON 生效（装配 json）' `
        ($null -ne $d -and $d.documents.Count -eq 1 -and $d.documents[0].language -eq 'json' -and $d.documents[0].lexer -eq 'json') `
        ("n={0} lang={1} lexer={2}" -f $d.documents.Count, $d.documents[0].language, $d.documents[0].lexer)

    # TC-004 会话侧：恢复后切「自动」→ 退出 → ini 不再记录手动语言
    Send-Command $main2b $CMD_LANG_AUTO
    $d = Get-DumpJson $main2b $script:DumpPath
    Assert-Cond 'TC-004/恢复现场切「自动」：回到 python 识别' ($d.documents[0].language -eq 'python' -and $d.documents[0].lexer -eq 'python')
    $normalExit = Exit-AppNormal $p2b
    Assert-Cond 'TC-004/热退出正常' $normalExit
    $ini = Get-IniMap (Join-Path $u2 'qcnote.ini')
    $docs2b = $ini['documents']
    $langKeyGone = ($docs2b -and -not $docs2b.ContainsKey('1\language') -and $docs2b['1\url'] -like '*a.py')
    Assert-Cond 'TC-004/ini 不再记录手动语言键（条目仍按 url 记录）' $langKeyGone `
        ("language key?={0} url?={1}" -f $(if ($docs2b) { $docs2b.ContainsKey('1\language') } else { '?' }), $(if ($docs2b) { $null -ne $docs2b['1\url'] } else { '?' }))

    # ================================================================
    Write-Output "`n=== 场景 6：TC-008 自动模式语言不入会话（前向兼容）==="
    # ================================================================
    $u8 = Join-Path $TempRoot 's8-user'
    $bPy = Join-Path $TempRoot 'b.py'
    [System.IO.File]::WriteAllText($bPy, $pyContent, $utf8NoBom)
    $p8 = Start-App $u8 $script:DumpPath @($bPy)
    $main8 = Wait-MainWindow $p8
    $d = Get-DumpJson $main8 $script:DumpPath
    Assert-Cond 'TC-008/前置：b.py 自动识别 python（未手动切换）' ($d.documents[0].language -eq 'python' -and $d.documents[0].lexer -eq 'python')
    $normalExit = Exit-AppNormal $p8
    Assert-Cond 'TC-008/热退出正常' $normalExit
    $ini = Get-IniMap (Join-Path $u8 'qcnote.ini')
    $docs8 = $ini['documents']
    Assert-Cond 'TC-008/自动模式：ini 无 1\language 键（旧版可读）' ($docs8 -and -not $docs8.ContainsKey('1\language') -and $docs8['1\url'] -like '*b.py') `
        ("language key?={0}" -f $(if ($docs8) { $docs8.ContainsKey('1\language') } else { '?' }))

    # ================================================================
    Write-Output "`n=== 场景 7：TC-009 旧会话无 language 键兼容（按扩展名自动识别）==="
    # ================================================================
    $u9 = Join-Path $TempRoot 's9-user'
    New-Item -ItemType Directory -Force -Path $u9 | Out-Null
    $oldPy = Join-Path $TempRoot 's09-old.py'
    [System.IO.File]::WriteAllText($oldPy, $pyContent, $utf8NoBom)
    # 构造旧版会话（代码着色功能之前）：[documents] 无 language 键
    $oldUrl = $oldPy.Replace('\', '/')
    [System.IO.File]::WriteAllText((Join-Path $u9 'qcnote.ini'),
        "[editor]`r`nwordWrap=false`r`nlineNumbers=true`r`n" +
        "[documents]`r`nsize=1`r`n1\url=$oldUrl`r`n1\cursor=2`r`nactive=$oldUrl`r`n", $utf8NoBom)
    $p9 = Start-App $u9 $script:DumpPath
    $main9 = Wait-MainWindow $p9
    Start-Sleep -Milliseconds 400
    $d = Get-DumpJson $main9 $script:DumpPath
    Assert-Cond 'TC-009/旧会话恢复：文档正常重开 + 光标还原（无键零影响）' `
        ($null -ne $d -and $d.documents.Count -eq 1 -and $d.documents[0].url.Replace('\', '/') -eq $oldUrl -and $d.documents[0].cursor -eq 2) `
        ("n={0} cursor={1}" -f $d.documents.Count, $d.documents[0].cursor)
    Assert-Cond 'TC-009/旧会话无键：按扩展名自动识别 python + 装配' ($d.documents[0].language -eq 'python' -and $d.documents[0].lexer -eq 'python') `
        ("lang={0} lexer={1}" -f $d.documents[0].language, $d.documents[0].lexer)
    Stop-AppForce $p9

    # ================================================================
    Write-Output "`n=== 场景 8：TC-011 未命名纯文本 / TC-010 未命名手动语言 uid 恢复 ==="
    # ================================================================
    $u10 = Join-Path $TempRoot 's10-user'
    $p10 = Start-App $u10 $script:DumpPath
    $main10 = Wait-MainWindow $p10
    Send-Command $main10 $CMD_NEW   # Ctrl+N 等价 → 未命名-1
    $d = Get-DumpJson $main10 $script:DumpPath
    Assert-Cond 'TC-011/新建未命名：纯文本渲染（language=plaintext + lexer 空）' `
        ($null -ne $d -and $d.documents.Count -eq 1 -and $d.documents[0].name -eq '未命名-1' -and
         $d.documents[0].language -eq 'plaintext' -and $d.documents[0].lexer -eq '') `
        ("name={0} lang={1} lexer='{2}'" -f $d.documents[0].name, $d.documents[0].language, $d.documents[0].lexer)

    # 置脏（草稿防抖触发前置）→ 手动 Python → 热退出
    $sciU = Get-EditorHwnd $d.documents[0]
    $draftText = 'draft-syntax-未命名-着色'
    Type-Text $sciU $draftText
    $d = Get-DumpJson $main10 $script:DumpPath
    Assert-Cond 'TC-010/前置：WM_CHAR 注入内容 + 置脏（草稿种子）' ($d.documents[0].content -eq $draftText -and $d.documents[0].modified -eq $true) `
        ("modified={0}" -f $d.documents[0].modified)
    Start-Sleep -Milliseconds 1600   # 1000ms 草稿防抖 + 余量
    Send-Command $main10 $CMD_LANG_PYTHON
    $d = Get-DumpJson $main10 $script:DumpPath
    Assert-Cond 'TC-010/未命名手动切 Python：language=python + lexer 装配' ($d.documents[0].language -eq 'python' -and $d.documents[0].lexer -eq 'python') `
        ("lang={0} lexer={1}" -f $d.documents[0].language, $d.documents[0].lexer)
    $normalExit = Exit-AppNormal $p10
    Assert-Cond 'TC-010/热退出正常（会话 + 草稿 flush）' $normalExit

    $p10b = Start-App $u10 $script:DumpPath
    $main10b = Wait-MainWindow $p10b
    Start-Sleep -Milliseconds 400
    $d = Get-DumpJson $main10b $script:DumpPath
    Assert-Cond 'TC-010/重启：未命名草稿恢复（uid 匹配）+ 手动 Python 生效（装配）' `
        ($null -ne $d -and $d.documents.Count -eq 1 -and $d.documents[0].name -eq '未命名-1' -and
         $d.documents[0].content -eq $draftText -and
         $d.documents[0].language -eq 'python' -and $d.documents[0].lexer -eq 'python') `
        ("name={0} content='{1}' lang={2} lexer={3}" -f $d.documents[0].name, $d.documents[0].content, $d.documents[0].language, $d.documents[0].lexer)
    Stop-AppForce $p10b

    # ================================================================
    Write-Output "`n=== 场景 9：TC-016 另存为换扩展名重识别（e2e 等价链路）==="
    # ================================================================
    # 另存为系统对话框（模态）无法 headless 自动化；生效语言是 url 的计算属性
    # （url 变化经 metadata 通知链重装配），等价断言「换扩展名后的最终状态」：
    # 同一内容按 .txt / .py 两种 url 打开 → plaintext / python 两种装配。
    # saveAs 直调链路由 verify-doc-selftest 的 saveAs 阶段持续回归。
    $u16 = Join-Path $TempRoot 's16-user'
    $sameTxt = Join-Path $TempRoot 's16-same.txt'
    $samePy  = Join-Path $TempRoot 's16-same.py'
    [System.IO.File]::WriteAllText($sameTxt, $pyContent, $utf8NoBom)
    [System.IO.File]::WriteAllText($samePy, $pyContent, $utf8NoBom)
    $p16 = Start-App $u16 $script:DumpPath @($sameTxt, $samePy)
    $main16 = Wait-MainWindow $p16
    Start-Sleep -Milliseconds 300
    $d = Get-DumpJson $main16 $script:DumpPath
    Assert-Cond 'TC-016/同内容 .txt url：纯文本（无装配）' `
        ($d.documents.Count -eq 2 -and $d.documents[0].language -eq 'plaintext' -and $d.documents[0].lexer -eq '') `
        ("lang={0} lexer='{1}'" -f $d.documents[0].language, $d.documents[0].lexer)
    Assert-Cond 'TC-016/同内容 .py url：识别 Python 并装配（url 计算属性）' `
        ($d.documents[1].language -eq 'python' -and $d.documents[1].lexer -eq 'python') `
        ("lang={0} lexer={1}" -f $d.documents[1].language, $d.documents[1].lexer)
    Stop-AppForce $p16

    # ================================================================
    Write-Output "`n=== 场景 10：TC-015 >16MiB 阈值跳过着色 ==="
    # ================================================================
    # 生成 >16MiB（kSyntaxColoringMaxBytes）python 文件：装载按纯文本渲染
    # （身份判定不受阈值影响），手动再指定语言仍跳过（DEC-005 阈值优先）。
    # 边界（恰 16MiB 不跳过 / +1 跳过）由 ctest core 套件单元级覆盖。
    $u15 = Join-Path $TempRoot 's15-user'
    $bigPy = Join-Path $TempRoot 's15-big.py'
    $line = "# large file threshold padding line 0123456789 abcdefghijklmnopqrstuvwxyz"
    $lineBytes = [System.Text.Encoding]::UTF8.GetByteCount($line) + 1   # + LF
    $needLines = [int][math]::Ceiling((16777216 + 1048576) / $lineBytes)   # > 16MiB + 1MiB 余量
    $sw = [System.IO.StreamWriter]::new($bigPy, $false, $utf8NoBom)
    try {
        $sw.NewLine = "`n"
        for ($i = 0; $i -lt $needLines; $i++) { $sw.WriteLine($line) }
    } finally { $sw.Close() }
    $bigSize = (Get-Item $bigPy).Length
    Assert-Cond 'TC-015/前置：测试文件 > 16MiB（16777216 字节）' ($bigSize -gt 16777216) ("size={0}" -f $bigSize)

    $sw15 = [System.Diagnostics.Stopwatch]::StartNew()
    $p15 = Start-App $u15 $script:DumpPath @($bigPy)
    $main15 = Wait-MainWindow $p15 ($TimeoutSec * 1000)
    Start-Sleep -Milliseconds 800   # 大文档装载稳定余量
    $openMs = $sw15.ElapsedMilliseconds
    # 单文档场景：FindWindowEx 首个 Scintilla 子窗口即唯一编辑器（dump 为
    # 权威映射但此处触发会全量序列化 17MB content，用枚举兜底避免开销）
    $sciBig = [QcSyntaxE2e]::FindWindowEx($main15, [IntPtr]::Zero, 'Scintilla', $null)
    $bigLen = (Send-Sci $sciBig $SCI_GETLENGTH 0 0).ToInt64()
    Assert-Cond 'TC-015/大文件打开：文档内容完整（消息口径）' ($bigLen -gt 16777216) ("len={0}" -f $bigLen)

    # 轻量字段提取：dump 触发后直接对原始文本正则取 documents[0] 的
    # language/lexer（ConvertFrom-Json 对 17MB 字符串解析慢且不稳定）
    function Get-DumpLangLexer([IntPtr]$main, [string]$dumpPath) {
        Remove-Item $dumpPath -Force -ErrorAction SilentlyContinue
        $r = [IntPtr]::Zero
        [QcSyntaxE2e]::SendMessageTimeout($main, $MSG_DUMP, [IntPtr]::Zero, [IntPtr]::Zero, 2, 5000, [ref]$r) | Out-Null
        if (Test-Path $dumpPath) {
            $raw = Get-Content $dumpPath -Raw -Encoding utf8
            if ($raw -match '"language": "([^"]*)", "lexer": "([^"]*)"') {
                return @{ Language = $Matches[1]; Lexer = $Matches[2] }
            }
        }
        return $null
    }
    $f15 = Get-DumpLangLexer $main15 $script:DumpPath
    Assert-Cond 'TC-015/>16MiB 装配跳过：lexer 空 + 身份保留 language=python' `
        ($null -ne $f15 -and $f15.Language -eq 'python' -and $f15.Lexer -eq '') `
        ("lang={0} lexer='{1}'" -f $(if ($f15) { $f15.Language } else { '?' }), $(if ($f15) { $f15.Lexer } else { '?' }))
    Assert-Cond 'TC-015/打开耗时无明显劣化（<20s 宽松上界）' ($openMs -lt 20000) ("open={0}ms" -f $openMs)

    Send-Command $main15 $CMD_LANG_PYTHON   # 手动再指定：阈值优先仍跳过（DEC-005）
    $f15 = Get-DumpLangLexer $main15 $script:DumpPath
    Assert-Cond 'TC-015/手动再指定 Python：阈值优先仍不装配（lexer 空）+ 身份 python' `
        ($null -ne $f15 -and $f15.Language -eq 'python' -and $f15.Lexer -eq '') `
        ("lang={0} lexer='{1}'" -f $(if ($f15) { $f15.Language } else { '?' }), $(if ($f15) { $f15.Lexer } else { '?' }))
    Stop-AppForce $p15
}
finally {
    foreach ($p in $script:Procs) {
        if ($p -and -not $p.HasExited) {
            $main = $script:MainHwnd[[uint32]$p.Id]
            if ($main) {
                [QcSyntaxE2e]::SendMessage($main, $WM_CLOSE_MSG, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
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
$fgAfter = [QcSyntaxE2e]::GetForegroundWindow()
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
