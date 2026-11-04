# verify-deploy-portable.ps1 —— 便携版部署脚本黑盒回归（持久化回归脚本）
# 对应用例：.localdoc/测试用例/测试用例-便携版构建部署命令.md
#   TC-101: 正常部署（目标目录不存在，自动创建并完整拷贝产物）
#   TC-102: 更新部署保留 qcnote.ini 用户数据并清理残留文件
#   TC-103: 目标目录内 exe 运行中被定向结束（offscreen，禁止可见窗口）；「不误杀」主断言
#           = 部署输出不含目标目录外实例 PID 的结束记录——qcnote 单实例：server 名
#           qcnote-<用户名> 用户级唯一（与 exe 目录无关），后启动实例被先启动实例转发退出，
#           对照实例存活按单实例语义分类记录；环境已有持管道主实例时本用例记 SKIP
#   TC-104: 产物目录不存在 / 缺 qcnote.exe / 缺 qcnote.portable 时明确报错且不触碰目标目录
#   TC-105: 默认自动选取 dist 下 LastWriteTime 最新产物（dist 下临时构造假产物双向验证，结束删除）
# 不在本脚本范围：TC-106（build.md 文档验证）、TC-107（端到端真实构建 + 真实目标部署）
# 前置：dist 下存在 qcnote-*-win64-portable 产物（tools/build-portable.ps1 产出）。
# 用法：pwsh.exe -NoProfile -ExecutionPolicy Bypass -File tests\verify-deploy-portable.ps1
#       [-ProjectRoot <项目根>] [-KeepTemp]
# 退出码：0 = 通过；1 = 失败
# 安全：临时目录仅建在 D:\temp\aiRunner\opencode 下；dist 下假产物目录 finally 删除，
#       不修改真实产物内容；进程一律 offscreen 启动、按 PID 定向结束，不误杀其他实例。
#Requires -Version 7
# 上行 #Requires：脚本头用法已限定 pwsh.exe（PowerShell 7+）；版本不满足时
# PowerShell 在执行前直接报错退出，错误前置。

param(
    [string]$ProjectRoot = "",
    [switch]$KeepTemp
)

# 先记录原控制台编码再改 UTF-8（全局约定三件套），退出前恢复
$origCodePage = [Console]::OutputEncoding
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null

if (-not $ProjectRoot) { $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path }
$DeployScript    = Join-Path $ProjectRoot "tools\deploy-portable.ps1"
$DistDir         = Join-Path $ProjectRoot "dist"
# qoffscreen.dll 源：产物默认只带 qwindows.dll，TC-103 offscreen 启动需临时拷入目标目录
$OffscreenDllSrc = 'D:\Program\Qt\6.11.2\mingw_64\plugins\platforms\qoffscreen.dll'

$script:FailCount    = 0
$script:CaseResults  = @()   # 每条断言一条记录：Case / Pass / Detail
$script:TempRoot     = $null
$script:FakeProduct  = $null # TC-105 在 dist 下构造的假产物目录（finally 删除）
$script:TestPids     = @()   # 本脚本启动的全部进程 PID（finally 兜底定向结束）
$script:OrigQpa      = $env:QT_QPA_PLATFORM

function Add-Result([string]$case, [bool]$pass, [string]$detail, [string]$failDetail = '') {
    # 通过时用 $detail；失败时优先用第 4 实参的失败专用文案（为空则回退 $detail）
    $shownDetail = if ($pass -or -not $failDetail) { $detail } else { $failDetail }
    # SKIP 记录：Pass=$true 且 Detail 以「SKIP：」开头（环境原因用例无法执行，不计
    # 失败、不影响退出码；汇总按 SKIP 展示）——TC-103 单实例语义跳过使用
    $isSkip = ($pass -and $detail.StartsWith('SKIP：', [System.StringComparison]::Ordinal))
    $script:CaseResults += [pscustomobject]@{ Case = $case; Pass = $pass; Skip = $isSkip; Detail = $shownDetail }
    if ($isSkip) {
        Write-Host ("  [SKIP] {0}：{1}" -f $case, $shownDetail) -ForegroundColor Yellow
        return
    }
    $tag = if ($pass) { 'PASS' } else { 'FAIL' }
    Write-Host ("  [{0}] {1}：{2}" -f $tag, $case, $shownDetail)
    if (-not $pass) { $script:FailCount++ }
}

# 子进程执行部署脚本（脚本内 exit 会退出进程，必须子进程调用取 $LASTEXITCODE）。
# $portableDir 传空字符串表示不带 -PortableDir（验证默认自动选择）。
# 返回 @{ ExitCode = int; Output = string }
function Invoke-Deploy([string]$portableDir, [string]$targetDir) {
    $deployArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $DeployScript)
    if ($portableDir) { $deployArgs += @('-PortableDir', $portableDir) }
    if ($targetDir)   { $deployArgs += @('-TargetDir', $targetDir) }
    $out = & pwsh.exe @deployArgs 2>&1 | ForEach-Object { $_.ToString() }
    return @{ ExitCode = $LASTEXITCODE; Output = ($out -join "`n") }
}

# 递归相对路径清单（小写、排序），供目录内容比对（与 verify-portable.ps1 同法）
function Get-FileRelList([string]$dir) {
    if (-not (Test-Path -LiteralPath $dir)) { return @() }
    $base = (Resolve-Path -LiteralPath $dir).Path.TrimEnd('\')
    @(Get-ChildItem -LiteralPath $base -Recurse -File -Force -ErrorAction SilentlyContinue |
        ForEach-Object { $_.FullName.Substring($base.Length + 1).ToLowerInvariant() } |
        Sort-Object)
}

function Test-ListEqual([object[]]$left, [object[]]$right) {
    if ($null -eq $left)  { $left  = @() }
    if ($null -eq $right) { $right = @() }
    if ($left.Count -ne $right.Count) { return $false }
    for ($i = 0; $i -lt $left.Count; $i++) {
        if ($left[$i] -ne $right[$i]) { return $false }
    }
    return $true
}

# ---------- 前置检查 ----------
if (-not (Test-Path -LiteralPath $DeployScript)) {
    Write-Host "[错误] 未找到被测脚本：$DeployScript"; exit 1
}
$realProduct = Get-ChildItem $DistDir -Directory -Filter 'qcnote-*-win64-portable' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $realProduct) {
    Write-Host "[错误] dist 下无 qcnote-*-win64-portable 产物，无法测试（先运行 tools/build-portable.ps1）"; exit 1
}
if (-not (Test-Path -LiteralPath $OffscreenDllSrc)) {
    Write-Host "[错误] 未找到 qoffscreen.dll 源：$OffscreenDllSrc（TC-103 前置）"; exit 1
}
$realVersion = if ($realProduct.Name -match '^qcnote-(\d+\.\d+\.\d+)-win64-portable$') { $Matches[1] } else { '' }

$TempBase = 'D:\temp\aiRunner\opencode'
if (-not (Test-Path $TempBase)) {
    Write-Host "[错误] 临时目录基址不可用：$TempBase（不回退 C 盘，请先创建该目录）"; exit 1
}
$script:TempRoot = Join-Path $TempBase ("qcnote-deploy-test-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $script:TempRoot -Force | Out-Null

Write-Host "==================== 便携版部署脚本黑盒回归 ===================="
Write-Host ("被测脚本 ：{0}" -f $DeployScript)
Write-Host ("真实产物 ：{0}（LastWriteTime {1:yyyy-MM-dd HH:mm:ss}，版本 {2}）" -f $realProduct.FullName, $realProduct.LastWriteTime, $realVersion)
Write-Host ("临时目录 ：{0}" -f $script:TempRoot)

try {
    # ========== TC-101: 正常部署（目标目录不存在） ==========
    Write-Host "`n==> TC-101 正常部署（目标目录不存在）"
    $t101    = Join-Path $script:TempRoot 'tc101\target'
    $srcList = Get-FileRelList $realProduct.FullName
    $r = Invoke-Deploy $realProduct.FullName $t101
    Add-Result 'TC-101' ($r.ExitCode -eq 0) "部署退出码 0（实际 $($r.ExitCode)）" "部署退出码应为 0，实际 $($r.ExitCode)"
    Add-Result 'TC-101' (Test-Path -LiteralPath (Join-Path $t101 'qcnote.portable')) "qcnote.portable 存在" "qcnote.portable 缺失"
    $t101List = Get-FileRelList $t101
    Add-Result 'TC-101' (Test-ListEqual $srcList $t101List) "文件清单与源产物完全一致（$($t101List.Count) 项，含 exe/Qt DLL/platforms/qml）" ("文件清单与源不一致（源 $($srcList.Count) 项 vs 目标 $($t101List.Count) 项）")
    Add-Result 'TC-101' ($r.Output -match '部署完成') "输出含部署完成摘要" "未输出部署完成摘要"

    # ========== TC-102: 更新部署保留用户数据并清理残留 ==========
    Write-Host "`n==> TC-102 更新部署保留 qcnote.ini 并清理残留（复用 TC-101 目标目录）"
    $iniPath = Join-Path $t101 'qcnote.ini'
    Set-Content -LiteralPath $iniPath -Value "[documents]`nsize=2`nuser=测试用户数据-TC102-勿动`n" -Encoding utf8
    $iniHashBefore = (Get-FileHash -LiteralPath $iniPath -Algorithm SHA256).Hash
    $stalePath = Join-Path $t101 'stale-old.dll'
    Set-Content -LiteralPath $stalePath -Value 'stale-file-must-be-removed' -Encoding ascii
    $r = Invoke-Deploy $realProduct.FullName $t101
    Add-Result 'TC-102' ($r.ExitCode -eq 0) "二次部署退出码 0（实际 $($r.ExitCode)）" "二次部署退出码应为 0，实际 $($r.ExitCode)"
    $iniHashAfter = (Get-FileHash -LiteralPath $iniPath -Algorithm SHA256).Hash
    Add-Result 'TC-102' ($iniHashAfter -eq $iniHashBefore) "qcnote.ini 内容原样保留（SHA256 一致）" "qcnote.ini 内容被改动（SHA256 不一致）"
    Add-Result 'TC-102' (-not (Test-Path -LiteralPath $stalePath)) "残留 stale-old.dll 已被清理" "残留 stale-old.dll 未被清理"
    $expected102 = @($srcList) + @('qcnote.ini') | Sort-Object
    $t102List = Get-FileRelList $t101
    Add-Result 'TC-102' (Test-ListEqual $expected102 $t102List) "目标清单 = 源产物 + qcnote.ini（$($t102List.Count) 项）" "目标清单与预期不一致（预期 $($expected102.Count) 项 vs 实际 $($t102List.Count) 项）"

    # ========== TC-103: 目标 exe 运行中被定向结束，不误杀其他目录实例 ==========
    # qcnote 单实例语义（src/platform/singleinstance.cpp）：server 名 qcnote-<用户名>
    # 用户级唯一，与 exe 目录无关；后启动实例 probe 连上先启动实例（含环境里已有的
    # qcnote 主实例）后以 Forwarded 角色自行退出（实测先实例事件循环就绪后约 3~5 秒）。
    # 因此本用例断言设计：
    # - 目标实例启动后退出且环境存在其他 qcnote 实例 → 被转发退出（产品语义），
    #   本用例无法执行，记 SKIP 并跳过剩余步骤；无环境实例佐证才按原语义 FAIL；
    # - 「不误杀」主断言 = 部署输出不含对照实例 PID 的结束记录（直接证据）；
    # - 对照实例存活情况按单实例语义分类记录（信息性/通过语义，转发退出非误杀）。
    Write-Host "`n==> TC-103 目标目录内 exe 运行中被定向结束（offscreen，禁止可见窗口）"
    $t103  = Join-Path $script:TempRoot 'tc103\target'
    $t103c = Join-Path $script:TempRoot 'tc103\ctrl'
    $preOk = $true
    foreach ($d in @($t103, $t103c)) {
        $pre = Invoke-Deploy $realProduct.FullName $d
        if ($pre.ExitCode -ne 0) { $preOk = $false; break }
        Copy-Item $OffscreenDllSrc (Join-Path $d 'platforms\qoffscreen.dll') -Force
    }
    if (-not $preOk) {
        Add-Result 'TC-103' $false "前置：两临时目录部署成功" "前置部署失败（退出码 $($pre.ExitCode)），用例无法执行"
    } else {
        # GUI 红线：一律 offscreen 启动，无可见窗口
        $env:QT_QPA_PLATFORM = 'offscreen'
        $targetProc = Start-Process -FilePath (Join-Path $t103 'qcnote.exe')  -WorkingDirectory $t103  -PassThru
        $ctrlProc   = Start-Process -FilePath (Join-Path $t103c 'qcnote.exe') -WorkingDirectory $t103c -PassThru
        $script:TestPids += @($targetProc.Id, $ctrlProc.Id)
        Start-Sleep -Seconds 3
        # 部署前对照存活快照（供部署后三分类记录；转发退出属产品语义，不作必过断言）
        $ctrlAliveBeforeDeploy = $null -ne (Get-Process -Id $ctrlProc.Id -ErrorAction SilentlyContinue)
        # 环境其他 qcnote 实例（本用例两 PID 之外）——存在即佐证有持单实例管道的主实例
        $envOther = @(Get-Process qcnote -ErrorAction SilentlyContinue |
            Where-Object { $_.Id -ne $targetProc.Id -and $_.Id -ne $ctrlProc.Id })
        $t103Skipped = $false
        if ($targetProc.HasExited) {
            if ($envOther.Count -gt 0) {
                # 目标实例被环境主实例转发退出（产品语义）：本用例无法执行，记 SKIP
                $envPids = (@($envOther | ForEach-Object { $_.Id }) -join '、')
                Add-Result 'TC-103' $true ("SKIP：环境存在 qcnote 主实例（PID {0}），目标实例被单实例转发退出，本用例无法执行" -f $envPids)
                $t103Skipped = $true
            } else {
                Add-Result 'TC-103' $false "目标实例启动存活（PID $($targetProc.Id)）" "目标实例启动后退出，无法验证进程结束（环境无其他 qcnote 实例，非单实例转发）"
            }
        } else {
            Add-Result 'TC-103' $true "目标实例启动存活（PID $($targetProc.Id)）" "目标实例启动后退出，无法验证进程结束"
        }
        if (-not $t103Skipped) {
            # 对照实例启动状态：信息性记录（不计失败）——单实例语义下对照实例可能在
            # 启动后 3~5 秒被先启动实例转发退出，该时点存活与否均属正常
            Add-Result 'TC-103' $true ("对照实例启动状态（信息性，不计失败）：{0}" -f $(if ($ctrlAliveBeforeDeploy) { "存活（PID $($ctrlProc.Id)）" } else { "已退出（单实例转发，产品语义）" }))
            Add-Result 'TC-103' ($targetProc.Path -and $targetProc.Path.StartsWith($t103, [System.StringComparison]::OrdinalIgnoreCase)) "目标实例 Path 位于目标目录内" "目标实例 Path 异常：$($targetProc.Path)"
            # 部署前复核：转发退出实测发生在 3~5 秒窗口，可能晚于上面 3 秒检查点；
            # 目标实例等不到被部署结束即消失，后续断言无法成立，按同一 SKIP 逻辑处理
            if ($targetProc.HasExited) {
                $envRe = @(Get-Process qcnote -ErrorAction SilentlyContinue |
                    Where-Object { $_.Id -ne $targetProc.Id -and $_.Id -ne $ctrlProc.Id })
                if ($envRe.Count -gt 0) {
                    $envPidsRe = (@($envRe | ForEach-Object { $_.Id }) -join '、')
                    Add-Result 'TC-103' $true ("SKIP：环境存在 qcnote 主实例（PID {0}），目标实例在部署前被单实例转发退出，本用例无法执行" -f $envPidsRe)
                    $t103Skipped = $true
                }
            }
        }
        if (-not $t103Skipped) {
            $r = Invoke-Deploy $realProduct.FullName $t103
            Add-Result 'TC-103' ($r.ExitCode -eq 0) "目标实例运行中部署退出码 0（实际 $($r.ExitCode)）" "目标实例运行中部署应成功，实际退出码 $($r.ExitCode)"
            Start-Sleep -Milliseconds 300
            $targetGone = $null -eq (Get-Process -Id $targetProc.Id -ErrorAction SilentlyContinue)
            Add-Result 'TC-103' $targetGone "目标实例已被定向结束（PID $($targetProc.Id) 不复存在）" "目标实例未被结束（PID $($targetProc.Id) 仍存活）"
            # 主断言（不误杀的直接证据）：部署输出不含对照实例 PID 的结束记录
            $ctrlKillRecorded = $r.Output -match ("结束进程 PID {0}" -f $ctrlProc.Id)
            Add-Result 'TC-103' (-not $ctrlKillRecorded) "部署未结束目标目录外实例（输出无对照 PID 结束记录）" "部署输出含对照实例 PID 的结束记录（误杀）"
            # 对照实例存活分类记录（信息性/通过语义，不因单实例转发退出而失败）
            $ctrlAliveAfterDeploy = $null -ne (Get-Process -Id $ctrlProc.Id -ErrorAction SilentlyContinue)
            if (-not $ctrlAliveBeforeDeploy) {
                Add-Result 'TC-103' $true "对照实例存活分类：部署前已被单实例转发退出（产品语义，非误杀；server 名用户级唯一）"
            } elseif ($ctrlAliveAfterDeploy) {
                Add-Result 'TC-103' $true "对照实例不受影响仍存活"
            } elseif (-not $ctrlKillRecorded) {
                Add-Result 'TC-103' $true "对照实例存活分类：部署期间被单实例转发退出（产品语义竞态，输出无结束记录，非误杀）"
            } else {
                Add-Result 'TC-103' $true "对照实例存活分类：部署期间退出且输出含结束记录（误杀，主断言已计失败）"
            }
            # 目标 PID 结束记录断言：部署扫描时目标实例仍在则输出必须含其结束记录；
            # 若部署扫描时已消失（复核点到扫描间被转发退出的窗口竞态），以部署输出
            # 「无运行中进程」提示 + 环境实例佐证判定为单实例转发（信息性，不计失败）
            if ($r.Output -match '目标目录内无运行中的 qcnote 进程') {
                $envLate = @(Get-Process qcnote -ErrorAction SilentlyContinue |
                    Where-Object { $_.Id -ne $targetProc.Id -and $_.Id -ne $ctrlProc.Id })
                if ($envLate.Count -gt 0) {
                    Add-Result 'TC-103' $true "目标 PID 结束记录（信息性）：部署扫描时目标实例已被单实例转发退出，部署未执行结束（输出含「无运行中进程」提示）"
                } else {
                    Add-Result 'TC-103' $false "输出含「结束进程 PID $($targetProc.Id)」" "部署扫描时目标实例已消失且无环境实例佐证转发（输出未含目标 PID 的结束记录）"
                }
            } else {
                Add-Result 'TC-103' ($r.Output -match ("结束进程 PID {0}" -f $targetProc.Id)) "输出含「结束进程 PID $($targetProc.Id)」" "输出未包含目标 PID 的结束记录"
            }
        }
        # 清理对照实例（按 PID 定向结束；已退出时静默，SKIP 路径同样清理）
        Stop-Process -Id $ctrlProc.Id -Force -ErrorAction SilentlyContinue
    }

    # ========== TC-104: 无产物明确报错且不触碰目标目录 ==========
    Write-Host "`n==> TC-104 无产物/产物不完整明确报错"
    $t104 = Join-Path $script:TempRoot 'tc104'
    # a) 产物目录不存在
    $tA = Join-Path $t104 'target-a'
    $r = Invoke-Deploy (Join-Path $t104 'no-such-product') $tA
    Add-Result 'TC-104' ($r.ExitCode -ne 0) "产物目录不存在时非零退出（实际 $($r.ExitCode)）" "产物目录不存在时应非零退出，实际 $($r.ExitCode)"
    Add-Result 'TC-104' ($r.Output -match '\[错误\]') "输出含 [错误] 标记" "输出未含 [错误] 标记"
    Add-Result 'TC-104' (-not (Test-Path -LiteralPath $tA)) "目标目录未被创建（不触碰）" "目标目录被创建，失败路径触碰了目标目录"
    # b) 缺 qcnote.exe
    $dirB = Join-Path $t104 'miss-exe'
    New-Item -ItemType Directory -Path $dirB -Force | Out-Null
    New-Item -ItemType File -Path (Join-Path $dirB 'qcnote.portable') -Force | Out-Null
    $tB = Join-Path $t104 'target-b'
    $r = Invoke-Deploy $dirB $tB
    Add-Result 'TC-104' ($r.ExitCode -ne 0 -and $r.Output -match '缺失 qcnote\.exe') "缺 qcnote.exe 时报「产物缺失 qcnote.exe」并退出 $($r.ExitCode)" "缺 exe 场景报错不符合预期（退出码 $($r.ExitCode)）"
    Add-Result 'TC-104' (-not (Test-Path -LiteralPath $tB)) "目标目录未被创建（不触碰）" "目标目录被创建，失败路径触碰了目标目录"
    # c) 缺 qcnote.portable
    $dirC = Join-Path $t104 'miss-marker'
    New-Item -ItemType Directory -Path $dirC -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $dirC 'qcnote.exe') -Value 'placeholder-not-a-real-exe' -Encoding ascii
    $tC = Join-Path $t104 'target-c'
    $r = Invoke-Deploy $dirC $tC
    Add-Result 'TC-104' ($r.ExitCode -ne 0 -and $r.Output -match '缺失 qcnote\.portable') "缺 qcnote.portable 时报「产物缺失 qcnote.portable」并退出 $($r.ExitCode)" "缺便携标记场景报错不符合预期（退出码 $($r.ExitCode)）"
    Add-Result 'TC-104' (-not (Test-Path -LiteralPath $tC)) "目标目录未被创建（不触碰）" "目标目录被创建，失败路径触碰了目标目录"

    # ========== TC-105: 默认自动选取 dist 下 LastWriteTime 最新产物 ==========
    Write-Host "`n==> TC-105 默认自动选择最新产物（dist 下临时构造假产物，双向验证）"
    # 假产物版本号 = 9.9.9 + 8 位随机数字（不可能真实的版本形态），目录名仍匹配
    # Filter 'qcnote-*-win64-portable'，且其版本号仍满足输出正则 9\.9\.9 前缀匹配。
    $fakeVersion = '9.9.9' + (-join ((1..8) | ForEach-Object { Get-Random -Minimum 0 -Maximum 10 }))
    $fake = Join-Path $DistDir ("qcnote-{0}-win64-portable" -f $fakeVersion)
    if (Test-Path -LiteralPath $fake) {
        # 命中即失败退出，绝不静默复用（避免覆盖他人目录/真实产物）
        Write-Host "[错误] 假产物目录已存在，拒绝复用：$fake" -ForegroundColor Red
        $script:FailCount++
        Add-Result 'TC-105' $false "假产物目录构造" "假产物目录已存在（$fake），未复用、未覆盖"
        throw "假产物目录冲突：$fake"
    }
    $script:FakeProduct = $fake
    New-Item -ItemType Directory -Path $fake | Out-Null
    Set-Content -LiteralPath (Join-Path $fake 'qcnote.exe') -Value 'fake-exe-for-selection-test' -Encoding ascii
    New-Item -ItemType File -Path (Join-Path $fake 'qcnote.portable') -Force | Out-Null
    $fakeList = @('qcnote.exe', 'qcnote.portable')
    # 方向 1：假产物 LastWriteTime 更旧 → 应选真实产物
    (Get-Item -LiteralPath $fake).LastWriteTime = Get-Date '2020-01-01 00:00:00'
    $t105a = Join-Path $script:TempRoot 'tc105\target-a'
    $r = Invoke-Deploy '' $t105a
    Add-Result 'TC-105' ($r.ExitCode -eq 0 -and $r.Output -match ('版本号\s*：\s*{0}' -f [regex]::Escape($realVersion))) "假产物较旧时选中真实产物 $realVersion（退出码 $($r.ExitCode)）" "假产物较旧时未选中真实产物（退出码 $($r.ExitCode)，输出版本行异常）"
    Add-Result 'TC-105' (Test-ListEqual (Get-FileRelList $t105a) $srcList) "部署内容 = 真实产物清单" "部署内容与真实产物清单不一致"
    # 方向 2：假产物 LastWriteTime 最新 → 应选假产物
    (Get-Item -LiteralPath $fake).LastWriteTime = (Get-Date).AddMinutes(2)
    $t105b = Join-Path $script:TempRoot 'tc105\target-b'
    $r = Invoke-Deploy '' $t105b
    Add-Result 'TC-105' ($r.ExitCode -eq 0 -and $r.Output -match ('版本号\s*：\s*{0}' -f [regex]::Escape($fakeVersion))) "假产物最新时选中假产物 $fakeVersion（退出码 $($r.ExitCode)）" "假产物最新时未选中假产物（退出码 $($r.ExitCode)）"
    Add-Result 'TC-105' (Test-ListEqual (Get-FileRelList $t105b) $fakeList) "部署内容 = 假产物清单（qcnote.exe + qcnote.portable）" "部署内容与假产物清单不一致"
    # 假产物目录随即删除（finally 兜底），dist 恢复原状
}
finally {
    # 恢复环境变量（QT_QPA_PLATFORM 仅 TC-103 改写）
    if ($null -ne $script:OrigQpa) { $env:QT_QPA_PLATFORM = $script:OrigQpa }
    else { Remove-Item Env:\QT_QPA_PLATFORM -ErrorAction SilentlyContinue }
    # 兜底结束本脚本启动的进程（正常路径已结束；按 PID 定向，不误杀其他 qcnote 实例）
    foreach ($testPid in $script:TestPids) {
        Stop-Process -Id $testPid -Force -ErrorAction SilentlyContinue
    }
    # 删除 dist 下假产物目录（TC-105 构造物，恢复 dist 原状）
    if ($script:FakeProduct -and (Test-Path -LiteralPath $script:FakeProduct)) {
        Remove-Item -LiteralPath $script:FakeProduct -Recurse -Force -ErrorAction SilentlyContinue
        # 复核：仍存在则以醒目红色错误输出并置失败标记（保证退出码非 0），
        # 防止 LastWriteTime 为“未来”的假产物残留、被下次默认部署误选为最新产物
        if (Test-Path -LiteralPath $script:FakeProduct) {
            Write-Host ("[错误] dist 假产物删除失败，残留：{0}（LastWriteTime 为未来时间，会被误选为最新产物，请手动删除）" -f $script:FakeProduct) -ForegroundColor Red
            $script:FailCount++
        }
    }
    # 清理临时目录（-KeepTemp 保留现场便于排查）
    if ($script:TempRoot -and (Test-Path -LiteralPath $script:TempRoot) -and -not $KeepTemp) {
        Remove-Item -LiteralPath $script:TempRoot -Recurse -Force -ErrorAction SilentlyContinue
    } elseif ($KeepTemp) {
        Write-Host "（-KeepTemp：临时现场保留于 $($script:TempRoot)）" -ForegroundColor DarkGray
    }
    [Console]::OutputEncoding = $origCodePage
}

# ---------- 汇总 ----------
Write-Host "`n==================== 用例汇总 ===================="
$grouped = $script:CaseResults | Group-Object Case | Sort-Object Name
$failedCases = @()
foreach ($g in $grouped) {
    $bad  = @($g.Group | Where-Object { -not $_.Pass })
    $skip = @($g.Group | Where-Object { $_.Skip })
    if ($bad.Count -gt 0) {
        $failedCases += $g.Name
        Write-Host ("{0}：FAIL（{1} 条断言未过）" -f $g.Name, $bad.Count) -ForegroundColor Red
        foreach ($b in $bad) { Write-Host ("    - {0}" -f $b.Detail) -ForegroundColor Red }
    } elseif ($skip.Count -gt 0) {
        # SKIP：环境原因用例无法执行（如存在持单实例管道的 qcnote 主实例），
        # 整体按通过计（不计失败、不影响退出码），黄色展示跳过原因
        Write-Host ("{0}：SKIP（{1} 条断言记录，整体按通过计）" -f $g.Name, $g.Group.Count) -ForegroundColor Yellow
        foreach ($s in $skip) { Write-Host ("    - {0}" -f $s.Detail) -ForegroundColor Yellow }
    } else {
        Write-Host ("{0}：PASS（{1} 条断言全部通过）" -f $g.Name, $g.Group.Count) -ForegroundColor Green
    }
}
Write-Host "================================================"
if ($script:FailCount -gt 0) {
    Write-Host ("结果：FAIL（失败用例 {0}）" -f ($failedCases -join ', ')) -ForegroundColor Red
    exit 1
}
Write-Host '结果：PASS（全部用例通过）' -ForegroundColor Green
exit 0
