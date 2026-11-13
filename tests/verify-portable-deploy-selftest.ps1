# =====================================================================
# tests/verify-portable-deploy-selftest.ps1
# 便携版构建/部署脚本 selftest：覆盖测试用例 TC-005 ~ TC-009、TC-011
# 对应文档：.localdoc/测试用例/测试用例-便携版构建部署脚本Win32适配.md
#
# 用法：pwsh -NoProfile -ExecutionPolicy Bypass -File tests\verify-portable-deploy-selftest.ps1
# 前置：先运行 tools/build-portable.ps1（dist 产物 + build-win\qcnote.exe 就绪）
#
# 覆盖用例：
#   TC-005 deploy 镜像部署成功（临时目标：/MIR 清残留 + /XF 保留 qcnote.ini）
#   TC-006 deploy 结束目标目录内运行中的进程（目标实例被结束、build-win 开发实例存活）
#   TC-007 deploy 危险目标防御（空/裸盘符/卷根/尾随空格/产物自身/产物子目录/产物父目录）
#   TC-008 deploy 产物缺失防御（缺 qcnote.portable）
#   TC-009 deploy 自动选取 dist 下 LastWriteTime 最新产物
#   TC-011 build-portable 失败即停（CMakeLists.txt 注入语法错误 → exit 1 → 字节级还原）
#
# 安全约定（项目测试防扰约定）：
#   - 全部使用临时目标目录，绝不触碰 D:\Program\qcnote
#   - 被测 qcnote 实例一律 --headless 启动（不可见窗口、不抢前台焦点）
#   - 开发实例经 --userdir 临时目录隔离，WM_CLOSE 落盘不污染 %LOCALAPPDATA%
#   - 不修改任何生产脚本与 C++ 源码（TC-011 注入后字节级还原并经 git 校验）
# =====================================================================
param(
    [string]$TempBase = 'D:\temp\aiRunner\opencode\qcnote-portable-deploy-selftest'
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding        = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null
$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'

$Root        = Split-Path -Parent $PSScriptRoot
$BuildScript = Join-Path $Root 'tools\build-portable.ps1'
$DeployScript = Join-Path $Root 'tools\deploy-portable.ps1'
$DistDir     = Join-Path $Root 'dist'
$DevExe      = Join-Path $Root 'build-win\qcnote.exe'
$CMakeLists  = Join-Path $Root 'CMakeLists.txt'

# ---------- 窗口枚举/消息（清理开发实例用，手法同 tools\verify-smoke.ps1） ----------
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class QcSmokeTest {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    public static IntPtr MainWindowOfPid(uint target) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, lp) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid == target) {
                var sb = new StringBuilder(256);
                GetClassName(h, sb, 256);
                if (sb.ToString() == "QcnoteMainWindow") { found = h; return false; }
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@
$WM_CLOSE_MSG = 0x0010

# ---------- 结果收集 ----------
$script:Results = [System.Collections.Generic.List[object]]::new()
function Record([string]$tc, [string]$name, [bool]$ok, [string]$note) {
    $script:Results.Add([pscustomobject]@{ TC = $tc; Name = $name; Result = $(if ($ok) { 'PASS' } else { 'FAIL' }); Note = $note })
    Write-Host ("[{0}] {1} {2}{3}" -f $(if ($ok) { '通过' } else { '失败' }), $tc, $name, $(if ($note) { " —— $note" } else { '' })) -ForegroundColor $(if ($ok) { 'Green' } else { 'Red' })
}
function Assert([bool]$cond, [string]$msg) { if (-not $cond) { throw $msg } }

# ---------- 子进程执行被测脚本（-Command 包装确保空串参数可靠传递） ----------
function Invoke-Deploy([string]$targetDir, [string]$portableDir = '') {
    $cmd = "& '$DeployScript'"
    if ($portableDir) { $cmd += " -PortableDir '$portableDir'" }
    if ($null -ne $targetDir) { $cmd += " -TargetDir '$targetDir'" }
    $out = & pwsh -NoProfile -ExecutionPolicy Bypass -Command $cmd 2>&1 | Out-String
    return @{ Output = $out; Code = $LASTEXITCODE }
}

# =====================================================================
# 预检：dist 最新产物 + 开发 exe
# =====================================================================
Write-Host '==> 预检：dist 产物与 build-win\qcnote.exe' -ForegroundColor Cyan
$latestDir = Get-ChildItem $DistDir -Directory -Filter 'qcnote-*-win64-portable' |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
Assert ($null -ne $latestDir) 'dist 下无 qcnote-*-win64-portable 产物（请先运行 build-portable.ps1）'
Assert (Test-Path (Join-Path $latestDir.FullName 'qcnote.exe')) "最新产物缺 qcnote.exe：$($latestDir.FullName)"
Assert (Test-Path (Join-Path $latestDir.FullName 'qcnote.portable')) "最新产物缺 qcnote.portable：$($latestDir.FullName)"
Write-Host ("最新产物：{0}（LastWriteTime {1}）" -f $latestDir.FullName, $latestDir.LastWriteTime)
$devExeReady = Test-Path $DevExe
Write-Host ("build-win\\qcnote.exe：{0}" -f $(if ($devExeReady) { "存在（$DevExe）" } else { '不存在（TC-006 开发实例部分将跳过）' }))

# ---------- 临时目录 ----------
if (Test-Path $TempBase) { Remove-Item $TempBase -Recurse -Force }
New-Item -ItemType Directory -Path $TempBase -Force | Out-Null
$TargetMain  = Join-Path $TempBase 'target-main'     # TC-005/006 目标
$TargetAuto  = Join-Path $TempBase 'target-auto'     # TC-009 目标
$NoPortable  = Join-Path $TempBase 'no-portable-dir' # TC-008 假产物
$DevUserDir  = Join-Path $TempBase 'dev-userdir'     # TC-006 开发实例 --userdir 隔离

# =====================================================================
# TC-005: deploy-portable 镜像部署成功（临时目标目录替身）
# =====================================================================
Write-Host "`n==> TC-005 deploy 镜像部署成功（临时目标目录）" -ForegroundColor Cyan
New-Item -ItemType Directory -Path $TargetMain -Force | Out-Null
# 预置用户会话数据（qcnote.ini 带特征内容）与旧版本残留（文件 + 子目录）
$iniMarker = "; TC-005 user session marker 20261113"
Set-Content -Path (Join-Path $TargetMain 'qcnote.ini') -Value $iniMarker -Encoding utf8
New-Item -ItemType Directory -Path (Join-Path $TargetMain 'stale-qt-dir') -Force | Out-Null
Set-Content -Path (Join-Path $TargetMain 'stale-qt-dir\Qt6Core.dll') -Value 'stale' -Encoding utf8
Set-Content -Path (Join-Path $TargetMain 'old-residue.txt') -Value 'stale' -Encoding utf8
$iniBefore = (Get-FileHash (Join-Path $TargetMain 'qcnote.ini') -Algorithm SHA256).Hash

$r = Invoke-Deploy $TargetMain
$ok = ($r.Code -eq 0) -and
      (Test-Path (Join-Path $TargetMain 'qcnote.exe')) -and
      (Test-Path (Join-Path $TargetMain 'qcnote.portable')) -and
      (-not (Test-Path (Join-Path $TargetMain 'old-residue.txt'))) -and
      (-not (Test-Path (Join-Path $TargetMain 'stale-qt-dir'))) -and
      ((Get-FileHash (Join-Path $TargetMain 'qcnote.ini') -Algorithm SHA256).Hash -eq $iniBefore) -and
      ($r.Output -match '部署完成') -and ($r.Output -match '已保留')
$note = ("exit={0}; exe/portable 就位; /MIR 清理 old-residue.txt+stale-qt-dir; qcnote.ini 内容保留; 摘要含版本号/目标/文件数/ini 状态" -f $r.Code)
if (-not $ok) { $note += "; 异常输出片段: " + (($r.Output -split "`n" | Select-String '错误|FAIL' | Select-Object -First 3) -join ' / ') }
Record 'TC-005' 'deploy 镜像部署成功' $ok $note

# =====================================================================
# TC-006: deploy 结束目标目录内运行中的进程（--headless 双实例互不干扰）
# =====================================================================
Write-Host "`n==> TC-006 结束目标目录内运行中的进程" -ForegroundColor Cyan
$devProc = $null
try {
    # 目标实例：部署目标目录内（portable 标记在）；开发实例：build-win（--userdir 隔离）
    $tgtProc = Start-Process -FilePath (Join-Path $TargetMain 'qcnote.exe') -ArgumentList '--headless' -WorkingDirectory $TargetMain -PassThru
    if ($devExeReady) {
        New-Item -ItemType Directory -Path $DevUserDir -Force | Out-Null
        $devProc = Start-Process -FilePath $DevExe -ArgumentList "--headless", "--userdir=$DevUserDir" -WorkingDirectory (Split-Path $DevExe) -PassThru
    }
    Start-Sleep -Seconds 2
    Assert (-not $tgtProc.HasExited) 'TC-006 前置失败：目标实例启动后 2s 内退出'
    if ($devProc) { Assert (-not $devProc.HasExited) 'TC-006 前置失败：开发实例启动后 2s 内退出' }

    $r = Invoke-Deploy $TargetMain
    $tgtKilled = $tgtProc.HasExited
    $devAlive  = ($null -eq $devProc) -or (-not $devProc.HasExited)
    $ok = ($r.Code -eq 0) -and $tgtKilled -and $devAlive -and ($r.Output -match '结束进程 PID')
    $note = ("exit={0}; 目标实例(PID {1})被结束; 开发实例{2}; 输出含『结束进程 PID』" -f $r.Code, $tgtProc.Id,
             $(if ($devProc) { "(PID $($devProc.Id)) 存活未受影响" } else { '部分跳过（build-win 无 exe）' }))
    Record 'TC-006' '结束目标目录内运行中的进程' $ok $note
} finally {
    # 清理开发实例：WM_CLOSE 优雅退出（--userdir 隔离，落盘不污染 LOCALAPPDATA），超时 Kill 兜底
    if ($devProc -and -not $devProc.HasExited) {
        $main = [QcSmokeTest]::MainWindowOfPid([uint32]$devProc.Id)
        if ($main -ne [IntPtr]::Zero) { [QcSmokeTest]::SendMessage($main, $WM_CLOSE_MSG, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }
        if (-not $devProc.WaitForExit(5000)) { $devProc.Kill(); $devProc.WaitForExit(3000) | Out-Null }
    }
    if ($devProc) { $devProc.Dispose() }
    # 兜底：目标实例若仍存活（deploy 失败路径）按 PID Kill
    if ($tgtProc -and -not $tgtProc.HasExited) { $tgtProc.Kill(); $tgtProc.WaitForExit(3000) | Out-Null }
}

# =====================================================================
# TC-007: deploy 危险目标防御（7 个变体全部拒绝且不执行 robocopy）
# =====================================================================
Write-Host "`n==> TC-007 危险目标防御" -ForegroundColor Cyan
$dangerVariants = @(
    @{ Label = '空目标';        Target = '' }
    @{ Label = '裸盘符 D:';     Target = 'D:' }
    @{ Label = '卷根 D:\';      Target = 'D:\' }
    @{ Label = '尾随空格';      Target = 'D:\path ' }
    @{ Label = '产物目录自身';  Target = $latestDir.FullName }
    @{ Label = '产物子目录';    Target = (Join-Path $latestDir.FullName 'sub') }
    @{ Label = '产物父目录dist'; Target = $DistDir }
)
$tc7AllOk = $true
foreach ($v in $dangerVariants) {
    $r = Invoke-Deploy $v.Target
    $ok = ($r.Code -eq 1) -and ($r.Output -match '错误|拒绝|不能为空') -and ($r.Output -notmatch 'robocopy 成功')
    if (-not $ok) { $tc7AllOk = $false }
    Record 'TC-007' ("危险目标防御 - {0}（{1}）" -f $v.Label, $v.Target) $ok ("exit={0}" -f $r.Code)
}

# =====================================================================
# TC-008: deploy 产物缺失防御（缺 qcnote.portable）
# =====================================================================
Write-Host "`n==> TC-008 产物缺失防御" -ForegroundColor Cyan
New-Item -ItemType Directory -Path $NoPortable -Force | Out-Null
Copy-Item (Join-Path $latestDir.FullName 'qcnote.exe') $NoPortable   # 只有 exe，无 qcnote.portable
$r = Invoke-Deploy (Join-Path $TempBase 'tc008-target') $NoPortable
$ok = ($r.Code -eq 1) -and ($r.Output -match '产物缺失 qcnote\.portable')
Record 'TC-008' '产物缺失防御（缺 qcnote.portable）' $ok ("exit={0}" -f $r.Code)

# =====================================================================
# TC-009: deploy 自动选取最新产物（dist 下 0.1.0 与 1.0.0 并存）
# =====================================================================
Write-Host "`n==> TC-009 自动选取最新产物" -ForegroundColor Cyan
$r = Invoke-Deploy $TargetAuto
$ok = ($r.Code -eq 0) -and ($r.Output -match [regex]::Escape($latestDir.FullName))
Record 'TC-009' ("自动选取最新产物（期望 {0}）" -f $latestDir.Name) $ok ("exit={0}; 摘要产物目录={1}" -f $r.Code,
    $(if ($r.Output -match '产物目录\s*[:：]\s*(\S+)') { $Matches[1] } else { '未解析' }))

# =====================================================================
# TC-011: build-portable 失败即停（CMakeLists.txt 注入语法错误，字节级还原）
# =====================================================================
Write-Host "`n==> TC-011 build 失败即停（注入语法错误 → exit 1 → 还原）" -ForegroundColor Cyan
$cmakeBackup = Join-Path $TempBase 'CMakeLists.txt.tc011-bak'
$zipPath = Join-Path $DistDir "$($latestDir.Name).zip"
$zipBefore = if (Test-Path $zipPath) { Get-Item $zipPath } else { $null }
try {
    Copy-Item $CMakeLists $cmakeBackup -Force                       # 字节级备份
    Add-Content -Path $CMakeLists -Value '__TC011_INJECTED_INVALID_CMAKE_SYNTAX__ ((( !!!' -Encoding ascii
    $out = & pwsh -NoProfile -ExecutionPolicy Bypass -File $BuildScript 2>&1 | Out-String
    $code = $LASTEXITCODE
    $zipAfter = if (Test-Path $zipPath) { Get-Item $zipPath } else { $null }
    $zipUntouched = ($null -eq $zipBefore) -or ($null -eq $zipAfter) -or
                    ($zipAfter.LastWriteTime -eq $zipBefore.LastWriteTime -and $zipAfter.Length -eq $zipBefore.Length)
    $ok = ($code -eq 1) -and ($out -match 'cmake 配置失败') -and
          ($out -notmatch '组装便携目录') -and ($out -notmatch '打包完成') -and $zipUntouched
    Record 'TC-011' 'build 失败即停（exit 1、不组装、不打包）' $ok ("exit={0}; 报『cmake 配置失败』; 无组装/打包段; zip 未变动" -f $code)
} finally {
    # 字节级还原（Move 覆盖）并经 git 校验
    if (Test-Path $cmakeBackup) { Move-Item $cmakeBackup $CMakeLists -Force }
    $gitState = & git -C $Root status --porcelain -- CMakeLists.txt 2>&1 | Out-String
    if ([string]::IsNullOrWhiteSpace($gitState)) {
        Write-Host 'TC-011 还原校验：CMakeLists.txt 与 git 基线一致（无改动）' -ForegroundColor Green
    } else {
        Write-Host "TC-011 还原校验：异常！CMakeLists.txt 残留改动：$gitState" -ForegroundColor Red
        Record 'TC-011' 'CMakeLists.txt 还原' $false "git 状态异常：$gitState"
    }
}

# =====================================================================
# 汇总
# =====================================================================
Write-Host ''
Write-Host '================== 便携脚本 selftest 结果 ==================' -ForegroundColor Cyan
$pass = @($script:Results | Where-Object { $_.Result -eq 'PASS' }).Count
$fail = @($script:Results | Where-Object { $_.Result -eq 'FAIL' }).Count
$script:Results | Format-Table TC, Name, Result, Note -AutoSize -Wrap | Out-String | Write-Host
Write-Host ("总计：{0} 项断言，通过 {1}，失败 {2}" -f $script:Results.Count, $pass, $fail)
Write-Host ("临时目录（可清理）：{0}" -f $TempBase)
if ($fail -gt 0) { Write-Host '总结: FAIL'; exit 1 } else { Write-Host '总结: PASS'; exit 0 }
