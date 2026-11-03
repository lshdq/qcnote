# ============================================================
# qcnote Windows 便携版产物验证脚本
# 对应测试用例（.localdoc/测试用例/测试用例-Windows便携版打包.md）：
#   TC-007      ：便携目录独立启动——最小系统 PATH（清除开发机 Qt PATH 干扰）
#                 + offscreen 冒烟，进程存活无崩溃
#   TC-008 前半 ：便携标记存在 → 会话 ini 落 exe 旁并被应用重写
#   TC-008 后半 ：删除便携标记 → 会话数据写回 %APPDATA%\qcnote\qcnote.ini
#
# 安全约定：
#   - 全程不修改打包产物，验证在临时副本目录进行，结束自动清理（-KeepTemp 保留现场）
#   - 真实 %APPDATA%\qcnote 如有数据，先改名到 %APPDATA% 同级的
#     qcnote.verify-backup-<时间戳>（与 qcnote 同卷同父目录，Rename 为纯元数据
#     原子改名、天然完整，不跨卷复制），验证后核验备份条目一致才还原；
#     核验不过则保留现场报错退出，绝不"先删原目录再从可能不完整的备份还原"
#   - 备份/还原等关键操作一律 -ErrorAction Stop，失败立即进 finally 安全处理
#   - 进程按 PID 定向结束，不误杀其他 qcnote 进程
#
# 用法：pwsh -NoProfile -ExecutionPolicy Bypass -File tools/verify-portable.ps1
#       [-PortableDir <便携产物目录>] [-KeepTemp]
# ============================================================
param(
    [string]$PortableDir = '',
    [switch]$KeepTemp
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8

# qoffscreen.dll 源：产物默认只带 qwindows.dll，offscreen 冒烟需临时拷入副本
$OffscreenDllSrc = 'D:\Program\Qt\6.11.2\mingw_64\plugins\platforms\qoffscreen.dll'
# 最小系统 PATH：验证产物不依赖开发机 Qt/MinGW PATH（TC-007 前置）
$CleanPath = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
$SmokeSeconds = 5

$script:TempRoot = $null
$script:AppDataQc = Join-Path $env:APPDATA 'qcnote'
$script:HadAppData = $false     # 运行前 %APPDATA%\qcnote 是否存在（仅备份步骤内使用）
$script:AppDataTestCreated = $false  # 验证流程是否已在 %APPDATA%\qcnote 创建过测试数据
$script:AppDataBackup = $null   # 同级备份目录（仅 Rename 成功后赋值，必为完整的改名后原目录）
$script:AppDataOrigFiles = @()  # 备份前原目录文件相对路径清单，供还原前后核验
$script:RestoreFailed = $false  # finally 中备份核验/还原失败标记（置位则退出码 1）
$script:OrigPath = $env:PATH

function Fail([string]$msg) { Write-Host "[失败] $msg" -ForegroundColor Red; exit 1 }
function Step([string]$msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }

function Stop-Qcnote([System.Diagnostics.Process]$proc) {
    if ($proc -and -not $proc.HasExited) {
        try {
            $proc.Kill()   # 按 PID 定向结束，不用 Get-Process 全杀
            $proc.WaitForExit() | Out-Null
        } catch {
            # HasExited 检查与 Kill() 间隙进程恰好自行退出会抛异常：已退出视为成功，仅其余异常上抛
            if (-not $proc.HasExited) { throw }
        }
    }
}

# 递归列出目录内全部文件的相对路径清单（小写、排序），供备份/还原核验比对
function Get-DirFileRelList([string]$dir) {
    if (-not (Test-Path -LiteralPath $dir)) { return @() }
    $base = (Resolve-Path -LiteralPath $dir).Path.TrimEnd('\')
    @(Get-ChildItem -LiteralPath $base -Recurse -File -Force -ErrorAction SilentlyContinue |
        ForEach-Object { $_.FullName.Substring($base.Length + 1).ToLowerInvariant() } |
        Sort-Object)
}

# 逐项比较两份相对路径清单是否完全一致（条目数与条目均相同）
function Test-FileListEqual([object[]]$left, [object[]]$right) {
    if ($null -eq $left)  { $left  = @() }
    if ($null -eq $right) { $right = @() }
    if ($left.Count -ne $right.Count) { return $false }
    for ($i = 0; $i -lt $left.Count; $i++) {
        if ($left[$i] -ne $right[$i]) { return $false }
    }
    return $true
}

# offscreen 启动副本 exe，存活 $SmokeSeconds 秒判定成功；进程由本函数内定向结束（无返回值）
function Start-AndAssertAlive([string]$dir, [string]$caseLabel) {
    Step "$caseLabel : offscreen 启动（最小 PATH）"
    $env:PATH = $CleanPath
    $env:QT_QPA_PLATFORM = 'offscreen'
    $proc = Start-Process -FilePath (Join-Path $dir 'qcnote.exe') -WorkingDirectory $dir -PassThru
    Start-Sleep -Seconds $SmokeSeconds
    if ($proc.HasExited) { Fail "$caseLabel 失败：进程已退出（退出码 $($proc.ExitCode)）" }
    Write-Host "进程存活 $SmokeSeconds 秒（PID $($proc.Id)）：启动成功"
    Stop-Qcnote $proc
    Start-Sleep -Milliseconds 500   # 等句柄与文件落盘释放
}

try {
    # ---------- 0. 定位产物并建临时副本 ----------
    if (-not $PortableDir) {
        $latest = Get-ChildItem (Join-Path $PSScriptRoot '..\dist') -Directory -Filter 'qcnote-*-win64-portable' -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if (-not $latest) { Fail '未在 dist/ 下找到 qcnote-*-win64-portable 产物目录' }
        $PortableDir = $latest.FullName
    }
    $PortableDir = (Resolve-Path $PortableDir).Path
    if (-not (Test-Path (Join-Path $PortableDir 'qcnote.exe'))) { Fail "产物缺失 qcnote.exe：$PortableDir" }
    if (-not (Test-Path (Join-Path $PortableDir 'qcnote.portable'))) { Fail "产物缺失 qcnote.portable：$PortableDir" }
    Write-Host "产物目录：$PortableDir"

    $tempBase = 'D:\temp\aiRunner\opencode'
    if (-not (Test-Path $tempBase)) { $tempBase = [System.IO.Path]::GetTempPath() }
    $script:TempRoot = Join-Path $tempBase ("qcnote-verify-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Path $script:TempRoot -Force | Out-Null
    $copyDir = Join-Path $script:TempRoot 'portable-copy'
    Copy-Item $PortableDir $copyDir -Recurse
    Write-Host "临时副本：$copyDir"

    # offscreen 平台插件：临时拷入副本（产物发布形态只带 qwindows.dll）
    if (-not (Test-Path $OffscreenDllSrc)) { Fail "未找到 qoffscreen.dll 源：$OffscreenDllSrc" }
    Copy-Item $OffscreenDllSrc (Join-Path $copyDir 'platforms\qoffscreen.dll') -Force

    # ---------- 1. TC-007 + TC-008 前半：带标记启动 ----------
    # 预置 size=1（无有效条目）的会话 ini：首启无会话记录时 SessionManager 设计为
    # 不写盘，无法观察落点；预置后走恢复路径并在末尾 saveNow() 重写为 size=0，
    # 重写即证明应用确实以 exe 旁 ini 作为会话文件（与 build-portable.ps1 冒烟同法）
    $copyIni = Join-Path $copyDir 'qcnote.ini'
    Set-Content -Path $copyIni -Value "[documents]`nsize=1`n" -Encoding ascii

    Start-AndAssertAlive $copyDir 'TC-007/TC-008前半'

    $iniContent = Get-Content $copyIni -Raw -ErrorAction SilentlyContinue
    if (-not $iniContent -or $iniContent -notmatch 'size=0') {
        Fail "TC-008 前半失败：exe 旁 qcnote.ini 未被应用重写为 size=0（便携数据模式未生效）"
    }
    Write-Host "TC-008 前半通过：会话 ini 落 exe 旁并被应用重写（size=1 → size=0）"

    # ---------- 2. TC-008 后半：删除标记 → 写回 %APPDATA% ----------
    Step 'TC-008 后半 : 备份真实 %APPDATA%\qcnote（如存在）'
    $script:HadAppData = Test-Path -LiteralPath $script:AppDataQc
    if ($script:HadAppData) {
        # 备份落 %APPDATA% 同级（与 qcnote 同卷同父目录）：Rename 为纯元数据原子改名，
        # 不跨卷复制，成功即天然完整；失败则原目录原样留在原地
        $backupLeaf = 'qcnote.verify-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
        $backupPath = Join-Path $env:APPDATA $backupLeaf
        $n = 1
        while (Test-Path -LiteralPath $backupPath) {   # 防与前次运行残留同名冲突
            $n++
            $backupPath = Join-Path $env:APPDATA "$backupLeaf-$n"
        }
        $script:AppDataOrigFiles = Get-DirFileRelList $script:AppDataQc
        try {
            Rename-Item -LiteralPath $script:AppDataQc -NewName (Split-Path $backupPath -Leaf) -ErrorAction Stop
        } catch {
            Fail "备份 %APPDATA%\qcnote 失败（$($_.Exception.Message)）；原目录保持原样未动"
        }
        $script:AppDataBackup = $backupPath   # 仅在改名成功后记录，finally 依此安全还原
        if (-not (Test-FileListEqual (Get-DirFileRelList $script:AppDataBackup) $script:AppDataOrigFiles)) {
            Fail "备份核验不通过（条目与备份前原目录不一致）：$script:AppDataBackup，请人工检查"
        }
        Write-Host "已备份（同卷原子改名）：$($script:AppDataQc) → $($script:AppDataBackup)"
    } else {
        Write-Host "$($script:AppDataQc) 不存在，无需备份（验证后删除创建物恢复原状）"
    }
    # 预置 %APPDATA% 会话（同法：size=1 无有效条目，驱动恢复路径重写）
    New-Item -ItemType Directory -Path $script:AppDataQc -Force -ErrorAction Stop | Out-Null
    $appDataIni = Join-Path $script:AppDataQc 'qcnote.ini'
    Set-Content -Path $appDataIni -Value "[documents]`nsize=1`n" -Encoding ascii -ErrorAction Stop
    $script:AppDataTestCreated = $true   # 此后 %APPDATA%\qcnote 内才是验证创建物，finally 方可删除

    # 副本删除便携标记与 exe 旁 ini（模拟用户删除标记的场景）
    Remove-Item (Join-Path $copyDir 'qcnote.portable') -Force
    Remove-Item $copyIni -Force

    Start-AndAssertAlive $copyDir 'TC-008后半'

    if (Test-Path $copyIni) {
        Fail "TC-008 后半失败：无标记时不应在 exe 旁生成 qcnote.ini（便携分支误触发）"
    }
    $appDataContent = Get-Content $appDataIni -Raw -ErrorAction SilentlyContinue
    if (-not $appDataContent -or $appDataContent -notmatch 'size=0') {
        Fail "TC-008 后半失败：%APPDATA% qcnote.ini 未被应用重写为 size=0（未写回用户目录）"
    }
    Write-Host "TC-008 后半通过：删除标记后数据写回 %APPDATA%\qcnote\qcnote.ini（size=1 → size=0），exe 旁无 ini"

    Write-Host ''
    Write-Host '================== 验证完成（TC-007 / TC-008 全部通过） ==================' -ForegroundColor Green
    exit 0
}
finally {
    # ---------- 还原 %APPDATA% 与清理现场（失败路径同样执行） ----------
    $env:PATH = $script:OrigPath
    Remove-Item Env:\QT_QPA_PLATFORM -ErrorAction SilentlyContinue

    if ($script:AppDataBackup -and (Test-Path -LiteralPath $script:AppDataBackup)) {
        # 用户数据整体在备份目录中（Rename 原子改名而来）。先核验备份条目与备份前
        # 一致，通过才允许删除当前目录（验证创建物）并原子改回原名；
        # 核验不过则两个目录全部保留现场并报错，绝不先删当前目录
        $backupList = Get-DirFileRelList $script:AppDataBackup
        if (-not (Test-FileListEqual $backupList $script:AppDataOrigFiles)) {
            Write-Host "[错误] 备份核验不通过（条目与备份前不一致）：保留 $($script:AppDataQc) 与备份 $($script:AppDataBackup)，请人工检查" -ForegroundColor Red
            $script:RestoreFailed = $true
        } else {
            $restored = $false
            try {
                if (Test-Path -LiteralPath $script:AppDataQc) {
                    Remove-Item -LiteralPath $script:AppDataQc -Recurse -Force -ErrorAction Stop   # 删除验证创建/改写的内容
                }
                Rename-Item -LiteralPath $script:AppDataBackup -NewName (Split-Path $script:AppDataQc -Leaf) -ErrorAction Stop   # 同卷原子改回原名
                $restored = $true
            } catch {
                Write-Host "[错误] 还原失败（$($_.Exception.Message)）：备份保留于 $($script:AppDataBackup)，请人工检查" -ForegroundColor Red
                $script:RestoreFailed = $true
            }
            if ($restored) {
                # 改回原名后再次核验条目一致（备份目录已随改名回到原名位置，无残留）
                if (Test-FileListEqual (Get-DirFileRelList $script:AppDataQc) $script:AppDataOrigFiles) {
                    Write-Host "已还原 %APPDATA%\qcnote 备份（条目核验一致，无备份残留）" -ForegroundColor DarkGray
                } else {
                    Write-Host "[错误] 还原后条目核验不一致，请人工检查 $($script:AppDataQc)" -ForegroundColor Red
                    $script:RestoreFailed = $true
                }
            }
        }
    } elseif ($script:AppDataBackup) {
        # 备份曾建立但目录缺失：异常状态，绝不改动现有 %APPDATA%\qcnote
        Write-Host "[错误] 备份目录意外缺失：$($script:AppDataBackup)；不改动 $($script:AppDataQc)，请人工检查" -ForegroundColor Red
        $script:RestoreFailed = $true
    } elseif ($script:AppDataTestCreated) {
        # 原目录本不存在（或已入备份）且验证创建过测试目录：当前目录为创建物，删除以恢复原状
        if (Test-Path -LiteralPath $script:AppDataQc) {
            Remove-Item -LiteralPath $script:AppDataQc -Recurse -Force
        }
    }
    # else：备份步骤尚未执行（含备份改名失败，原目录仍完整在原地）→ %APPDATA%\qcnote 原样未动

    if ($script:TempRoot -and (Test-Path $script:TempRoot) -and -not $KeepTemp) {
        Remove-Item $script:TempRoot -Recurse -Force -ErrorAction SilentlyContinue
    } elseif ($KeepTemp) {
        Write-Host "（-KeepTemp：临时现场保留于 $script:TempRoot）" -ForegroundColor DarkGray
    }

    if ($script:RestoreFailed) { exit 1 }
}
