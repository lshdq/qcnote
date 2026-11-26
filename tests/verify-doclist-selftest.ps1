#Requires -Version 7
param(
    [string]$ExePath = 'build-win\qcnote.exe',
    [int]$TimeoutSec = 60,
    [int]$E2eTimeoutMs = 15000
)

# =====================================================================
# qcnote 文档列表面板自动化验证（TC-001..008 selftest + TC-005 持久化 e2e）
#
# 第一阶段（进程内 selftest）：驱动 exe 的 --selftest-doclist 模式——面板
#   默认隐藏/命令翻转/编辑区收缩、条目同步、点击切换、分隔条拖拽（提交/Esc
#   回退/钳制）、溢出滚动、空面板/欢迎页联动、主题翻转。阶段机由
#   kMsgDocListSelftestStage（WM_APP+13）PostMessage 推进，全部断言落
#   结果文件后自动退出，脚本读取并判读。
# 第二阶段（TC-005 UI 侧持久化闭环，双 launch e2e）：非 selftest 模式
#（PersistenceActive = true，走真实会话）——
#   launch1 --headless --userdir=<临时目录>：经 WM_APP+7 命令钩子（code 7 =
#   ToggleDocList，与菜单同一 RouteCommand 路径）翻转面板 → 对主窗口合成
#   鼠标消息（WM_LBUTTONDOWN/MOUSEMOVE/LBUTTONUP）拖拽分隔条调宽 →
#   WM_CLOSE 热退出（PersistSessionNow 落盘）→ 断言 ini [editor]
#   docListVisible/docListWidth；
#   launch2 同 userdir：--selftest-dump（WM_APP+6）采样断言恢复可见/宽度；
#   场景 C：旧版 ini（无新键）→ 前向兼容回退默认（隐藏、200）；
#   场景 D：越界宽度（5000/10）→ 恢复侧钳制（上界 = 内容区宽/3 逻辑 ≈ 333；
#   下限 120）。
# 防扰约定：全程 --headless（窗口不显示、离屏停泊——零可见元素、不抢前台
#   焦点）+ --no-shell-prompt（第一阶段；第二阶段跳过首启提示同款）+
#   --userdir 重定向（不触碰真实用户目录）。
# 幂等：任务目录名带时间戳 + 随机段，可复跑；成功后清理临时文件（失败保留
#   供诊断，路径见输出）。
# 用法：pwsh.exe -NoProfile -ExecutionPolicy Bypass -File tests\verify-doclist-selftest.ps1
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

# 唯一任务目录（临时产物约定：D:\temp\aiRunner\opencode 下按任务建立唯一目录）
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$rand = ([System.IO.Path]::GetRandomFileName() -replace '[^a-zA-Z0-9]', '')
$workDir = "D:\temp\aiRunner\opencode\qcnote-doclist-selftest-$stamp-$rand"
New-Item -ItemType Directory -Force -Path $workDir | Out-Null
$userDir = Join-Path $workDir 'userdir'
New-Item -ItemType Directory -Force -Path $userDir | Out-Null

# ---------------- 断言收集 ----------------
$script:Results = @()
function Assert-Cond([string]$case, [bool]$cond, [string]$detail = '') {
    $script:Results += [pscustomobject]@{ Case = $case; Pass = $cond; Detail = $detail }
    $tag = if ($cond) { 'PASS' } else { 'FAIL' }
    Write-Host ("  [{0}] {1}{2}" -f $tag, $case, $(if ($detail) { "  ($detail)" } else { '' }))
}

# ---------------- Win32 P/Invoke（e2e 阶段） ----------------
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class QcDl {
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
                // 不过滤 IsWindowVisible：--headless 下主窗口永不显示（不可见窗口
                // 仍可收消息；自测阶段机经内部 PostMessage 推进不受影响）
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

# ---------------- e2e 常量与辅助 ----------------
$WM_CLOSE_MSG  = 0x0010
$WM_MOUSEMOVE  = 0x0200
$WM_LBUTTONDOWN = 0x0201
$WM_LBUTTONUP  = 0x0202
$MK_LBUTTON    = 0x0001
$MSG_DUMP      = 0x8006   # WM_APP + 6（exe 内 kMsgSelftestDump）
$MSG_COMMAND   = 0x8007   # WM_APP + 7（exe 内 kMsgSelftestCommand）
$CMD_TOGGLE_DOCLIST = 7   # ToggleDocList（与菜单同一 RouteCommand 路径）

function Make-LParam([int]$x, [int]$y) {
    # MAKELPARAM(x, y) = (y << 16) | x（客户区坐标，正值小坐标无符号风险）
    return [IntPtr]((([int64]$y -shl 16) -bor [int64]$x))
}

$script:Procs = @()
$script:MainHwnd = @{}
$script:DumpPath = Join-Path $workDir 'state-dump.json'

function Start-App([string]$userDir, [string[]]$extraArgs = @()) {
    $args = @('--headless', "--userdir=$userDir") + $extraArgs
    $p = Start-Process -FilePath $ExePath -ArgumentList $args -PassThru -WorkingDirectory $workDir
    $script:Procs += $p
    return $p
}

function Wait-MainWindow([System.Diagnostics.Process]$proc, [int]$timeoutMs = 10000) {
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $timeoutMs) {
        if ($proc.HasExited) { throw "进程提前退出（ExitCode=$($proc.ExitCode)）" }
        $wins = [QcDl]::WindowsOfPid([uint32]$proc.Id, 'QcnoteMainWindow')
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
    [QcDl]::SendMessageTimeout($main, $MSG_DUMP, [IntPtr]::Zero, [IntPtr]::Zero, 2, 5000, [ref]$r) | Out-Null
    if (Test-Path $script:DumpPath) {
        return Get-Content $script:DumpPath -Raw -Encoding utf8 | ConvertFrom-Json
    }
    return $null
}

function Send-Command([IntPtr]$main, [int]$code) {
    $r = [IntPtr]::Zero
    [QcDl]::SendMessageTimeout($main, $MSG_COMMAND, [IntPtr]$code, [IntPtr]::Zero, 2, 5000, [ref]$r) | Out-Null
    Start-Sleep -Milliseconds 150
}

# 合成分隔条拖拽（真实消息路径：MainWindowProc 的鼠标路由 → 面板拖拽状态机）
function Send-SplitterDrag([IntPtr]$main, [int]$panelRight, [int]$y, [int]$dxPx) {
    $x0 = $panelRight - 2   # 分隔条热区（右缘 4 物理像素内）
    [QcDl]::SendMessage($main, $WM_LBUTTONDOWN, [IntPtr]$MK_LBUTTON, (Make-LParam $x0 $y)) | Out-Null
    [QcDl]::SendMessage($main, $WM_MOUSEMOVE, [IntPtr]$MK_LBUTTON, (Make-LParam ($x0 + $dxPx) $y)) | Out-Null
    [QcDl]::SendMessage($main, $WM_LBUTTONUP, [IntPtr]::Zero, (Make-LParam ($x0 + $dxPx) $y)) | Out-Null
}

function Stop-AppForce([System.Diagnostics.Process]$proc) {
    if ($proc -and -not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit(3000) | Out-Null }
}

function Exit-AppNormal([System.Diagnostics.Process]$proc, [int]$timeoutMs = 5000) {
    $main = $script:MainHwnd[[uint32]$proc.Id]
    if ($main) {
        [QcDl]::SendMessage($main, $WM_CLOSE_MSG, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    } else {
        $proc.CloseMainWindow() | Out-Null
    }
    if (-not $proc.WaitForExit($timeoutMs)) {
        Stop-AppForce $proc
        return $false
    }
    return ($proc.ExitCode -eq 0)
}

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

# ---------------- 第一阶段：--selftest-doclist（进程内阶段机） ----------------
Write-Host "`n=== 第一阶段：--selftest-doclist（TC-001..009 进程内断言）==="
$result = Join-Path $workDir 'doclist-selftest-result.txt'
if (Test-Path $result) { Remove-Item $result -Force }
$selftestProc = $null
$fail = $false
try {
    $selftestProc = Start-Process -FilePath $ExePath -ArgumentList @(
        '--headless', '--no-shell-prompt',
        ('--userdir="{0}"' -f $userDir),
        ('--selftest-doclist="{0}"' -f $result)
    ) -PassThru -WorkingDirectory $workDir
    if (-not $selftestProc.WaitForExit($TimeoutSec * 1000)) {
        Write-Output "[FAIL] selftest 超时（${TimeoutSec}s 未退出），强制 Kill"
        Stop-AppForce $selftestProc
        $fail = $true
    }
    elseif (-not (Test-Path $result)) {
        Write-Output ("[FAIL] 结果文件未生成（进程退出码 {0}）" -f $selftestProc.ExitCode)
        $fail = $true
    }
    else {
        foreach ($line in Get-Content $result) { Write-Output $line }
        $last = (Get-Content $result | Select-Object -Last 1)
        if ($last -notmatch 'overall: PASS') { $fail = $true }
        $selftestProc.Dispose()
        $selftestProc = $null
    }
}
finally {
    if ($selftestProc -and -not $selftestProc.HasExited) { Stop-AppForce $selftestProc }
}

# ---------------- 第二阶段：TC-005 持久化 e2e（双 launch 真实会话） ----------------
try {
    if (-not $fail) {
        # ===== 场景 A+B：翻转 + 拖宽 → 热退出 → 重启恢复 =====
        Write-Host "`n=== 第二阶段（TC-005）：launch1 翻转面板 + 拖宽 → 热退出 ==="
        $uA = Join-Path $workDir 'tc005-user'
        New-Item -ItemType Directory -Force -Path $uA | Out-Null

        $p1 = Start-App $uA @("--selftest-dump=$script:DumpPath")
        $main1 = Wait-MainWindow $p1
        $d = Get-DumpJson $main1
        Assert-Cond 'I-A 启动后面板默认隐藏（新会话）' `
            ($d -and $d.docList.visible -eq $false -and $d.docList.width -eq 200) `
            ("visible={0} width={1}" -f $d.docList.visible, $d.docList.width)

        Send-Command $main1 $CMD_TOGGLE_DOCLIST
        $d = Get-DumpJson $main1
        Assert-Cond 'I-A 命令翻转后面板可见、宽度默认 200、面板矩形有效' `
            ($d.docList.visible -eq $true -and $d.docList.width -eq 200 -and
             $d.docList.right -gt $d.docList.left -and $d.docList.bottom -gt $d.docList.top) `
            ("right={0} bottom={1}" -f $d.docList.right, $d.docList.bottom)

        # 合成分隔条拖拽：+96 物理像素（= 96·96/DPI 逻辑，宽入 (200, 333] 区间）
        Send-SplitterDrag $main1 ([int]$d.docList.right) ([int](($d.docList.top + $d.docList.bottom) / 2)) 96
        $d = Get-DumpJson $main1
        $w1 = if ($d) { [int]$d.docList.width } else { -1 }
        Assert-Cond 'I-A 合成拖拽分隔条调宽生效（钳制区间内）' `
            ($w1 -gt 200 -and $w1 -le 333) `
            ("width {0} -> {1}" -f 200, $w1)

        $normalExit = Exit-AppNormal $p1
        Assert-Cond 'I-A 热退出（WM_CLOSE 免提示）退出码 0' $normalExit

        $ini = Get-IniMap (Join-Path $uA 'qcnote.ini')
        $edA = $ini['editor']
        Assert-Cond 'I-A ini [editor] 落盘 docListVisible=true + docListWidth=拖后宽' `
            ($edA -and $edA['docListVisible'] -eq 'true' -and [int]$edA['docListWidth'] -eq $w1) `
            ("docListVisible={0} docListWidth={1}（期望 {2}）" -f $edA['docListVisible'], $edA['docListWidth'], $w1)

        Write-Host "`n=== 第二阶段（TC-005）：launch2 重启恢复 ==="
        $p2 = Start-App $uA @("--selftest-dump=$script:DumpPath")
        $main2 = Wait-MainWindow $p2
        $d2 = Get-DumpJson $main2
        Assert-Cond 'I-B 重启恢复面板可见 + 宽度一致（DPI 同值换算）' `
            ($d2 -and $d2.docList.visible -eq $true -and [int]$d2.docList.width -eq $w1 -and
             $d2.docList.right -gt $d2.docList.left) `
            ("visible={0} width={1}（期望 {2}）" -f $d2.docList.visible, $d2.docList.width, $w1)
        $normalExit = Exit-AppNormal $p2
        Assert-Cond 'I-B 恢复后正常退出（重写不回退）' $normalExit

        # ===== 场景 C：旧版 ini（无新键）→ 前向兼容回退默认 =====
        Write-Host "`n=== 第二阶段（TC-005）：场景 C 旧版 ini 无新键 → 默认值 ==="
        $uC = Join-Path $workDir 'tc005c-user'
        New-Item -ItemType Directory -Force -Path $uC | Out-Null
        $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllText((Join-Path $uC 'qcnote.ini'),
            "[editor]`r`nwordWrap=false`r`nlineNumbers=true`r`ndarkTheme=false`r`n" +
            "[documents]`r`nsize=0`r`n", $utf8NoBom)
        $p3 = Start-App $uC @("--selftest-dump=$script:DumpPath")
        $main3 = Wait-MainWindow $p3
        $d3 = Get-DumpJson $main3
        Assert-Cond 'I-C 旧会话无新键 → 面板默认隐藏、宽度默认 200' `
            ($d3 -and $d3.docList.visible -eq $false -and $d3.docList.width -eq 200) `
            ("visible={0} width={1}" -f $d3.docList.visible, $d3.docList.width)
        Stop-AppForce $p3

        # ===== 场景 D：越界宽度 → 恢复侧钳制（TC-005 值越界钳制闭环） =====
        Write-Host "`n=== 第二阶段（TC-005）：场景 D 越界宽度 → 恢复钳制 ==="
        # D1：宽度 5000（超内容区宽/3 上界 ≈ 333）→ 首次 layout 钳到上界
        $uD1 = Join-Path $workDir 'tc005d1-user'
        New-Item -ItemType Directory -Force -Path $uD1 | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $uD1 'qcnote.ini'),
            "[editor]`r`ndocListVisible=true`r`ndocListWidth=5000`r`n" +
            "[documents]`r`nsize=0`r`n", $utf8NoBom)
        $p4 = Start-App $uD1 @("--selftest-dump=$script:DumpPath")
        $main4 = Wait-MainWindow $p4
        $d4 = Get-DumpJson $main4
        Assert-Cond 'I-D 超上界宽度 5000 → 恢复钳制到内容区宽/3（≈333）' `
            ($d4 -and $d4.docList.visible -eq $true -and
             [int]$d4.docList.width -ge 120 -and [int]$d4.docList.width -le 350) `
            ("width={0}" -f $d4.docList.width)
        Stop-AppForce $p4
        # D2：宽度 10（低于下限 120）→ 钳到下限
        $uD2 = Join-Path $workDir 'tc005d2-user'
        New-Item -ItemType Directory -Force -Path $uD2 | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $uD2 'qcnote.ini'),
            "[editor]`r`ndocListVisible=true`r`ndocListWidth=10`r`n" +
            "[documents]`r`nsize=0`r`n", $utf8NoBom)
        $p5 = Start-App $uD2 @("--selftest-dump=$script:DumpPath")
        $main5 = Wait-MainWindow $p5
        $d5 = Get-DumpJson $main5
        Assert-Cond 'I-D 低于下限宽度 10 → 恢复钳制到最小宽 120' `
            ($d5 -and $d5.docList.visible -eq $true -and [int]$d5.docList.width -eq 120) `
            ("width={0}" -f $d5.docList.width)
        Stop-AppForce $p5
    }
}
finally {
    foreach ($p in $script:Procs) {
        if ($p -and -not $p.HasExited) {
            $main = $script:MainHwnd[[uint32]$p.Id]
            if ($main) {
                [QcDl]::SendMessage($main, $WM_CLOSE_MSG, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
            } else {
                $p.CloseMainWindow() | Out-Null
            }
            if (-not $p.WaitForExit(2000)) { Stop-AppForce $p }
            $p.Dispose()
        }
    }
}

# ---------------- 汇总 ----------------
Write-Output ''
$failed = @($script:Results | Where-Object { -not $_.Pass })
foreach ($r in $script:Results) {
    Write-Output ("scenario={0}: {1}" -f $r.Case, $(if ($r.Pass) { 'PASS' } else { 'FAIL' }))
}
if ($fail) { Write-Output 'scenario=doclist selftest (stage 1 exe result): FAIL' }

$verdict = if (-not $fail -and $failed.Count -eq 0) { 'PASS' } else { 'FAIL' }
$totalCount = $script:Results.Count + 1
$passedCount = @($script:Results | Where-Object { $_.Pass }).Count + $(if (-not $fail) { 1 } else { 0 })
Write-Output ("overall: {0}（{1}/{2} 通过）" -f $verdict, $passedCount, $totalCount)
if ($verdict -ne 'PASS') {
    Write-Output "诊断目录: $workDir（含 .detail.txt 采样明细）"
    exit 1
}
# 成功：清理本任务临时文件（结果已输出到 stdout；可重建产物不保留）
Remove-Item -Recurse -Force $workDir -ErrorAction SilentlyContinue
Write-Output '总结: PASS'
exit 0
