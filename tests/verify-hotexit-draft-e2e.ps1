# verify-hotexit-draft-e2e.ps1 —— 「退出免提示与草稿缓存单例修复」端到端回归（持久化回归脚本）
# 对应用例：docs/测试用例/测试用例-退出免提示与草稿缓存单例修复.md TC-010/TC-011/TC-012
#   TC-010 热退出全链路：新建标签真实输入链路键入 → 等真实防抖（1s）落盘 → QML 退出链路
#     热退出（doQuit → close → onClosing 放行，无确认框）→ 正常退出 → 重启真实 qcnote.exe
#     → 旧条目被消费 + 按新 uid 续写（content/name=未命名-1/url 空）→ 内容恢复为未保存状态
#   TC-011 强杀全链路（用户原始场景）：键入 → 等 2 秒（防抖已落盘）→ Stop-Process -Force
#     强杀（任务管理器等价）→ 重启真实 qcnote.exe 同 TC-010 断言；另以 harness restore
#     模式直接断言恢复后 activeDocument 指向恢复文档（DEC-002 恢复激活）
#   TC-012 强杀前失焦：键入后 200ms（防抖 1000ms 未到期）触发真实
#     applicationStateChanged(Inactive)（失焦即写）→ 失焦标记后 1 秒内强杀 →
#     条目仍落盘（失焦 flush 是防抖到期前唯一落盘路径，条目在即链路生效）
# 另含「关闭现场还原」TC-011（docs/测试用例/测试用例-关闭现场还原.md，场景名
#   与上一工作流 TC-011 同号但属不同用例文档）：
#   - mixexit 驱动构造 [文件A, 未命名1(键入内容), 文件B] 活动=未命名1 → 热退出；
#     断言退出会话记录完整标签序列（条目 2 为 uid、activeUid 互斥）；
#   - 重启真实 qcnote.exe → 断言会话对齐重写为 [A, 未命名(新uid)@2, B] 且
#     activeUid=新uid（真实进程还原顺序与活动标签的文件级证据）；
#   - restoremix 驱动直断言：documents() 顺序 [A, 未命名1, B]、activeDocument
#     =未命名1（DEC-002 保序 + DEC-003 一级激活）。
# 本轮测试扩展（关闭现场还原验收抽查，驱动 mixexit2/restoremix2/mixfile/
#   mixempty 模式）：
#   - 验收标准 2：[未命名1, 文件A, 未命名2] 活动=未命名2 → mixexit2 构造现场
#     （会话 [uid1, A, uid2] + activeUid）→ 真实 qcnote.exe 重启（会话对齐 +
#     草稿双续写）→ restoremix2 直断言顺序与尾部未命名激活；
#   - 验收标准 3：[文件A, 未命名1, 文件B] 活动=文件B → mixfile 构造现场
#     （会话记 active=URL 的现状格式）→ 真实重启断言现场还原且 active=文件B；
#   - 验收标准 4：[文件A, 未命名1(无内容), 文件B] 活动=文件B → mixempty 构造
#     现场（会话记空 uid 条目、无草稿）→ 真实重启断言空位消除（[A, B]）；
#   - 验收标准 5：手写旧格式会话（纯文件条目，无任何 uid 键）→ 真实 qcnote.exe
#     重启断言行为与现状一致（恢复 [A, B] active=B、无 QML 错误）。
# 写入侧说明：真实进程无法合成键盘输入（offscreen 无原生窗口句柄），由
#   tests\test_window\e2e_draft_driver.exe（真实 Main.qml + QML 动作入口
#   doNewDocument + 活动 TextArea append() 等价键入 → EditorArea onTextChanged
#   → doc.content 回写 → 真实防抖）完成；恢复侧用真实 build\qcnote.exe 重启，
#   以黑盒文件级断言（旧条目消费 + 新 uid 续写字段）证明标签带内容恢复。
# 验证方式：黑盒文件级断言（缓存条目 JSON 的产生/删除/字段）+ 进程存活/退出码/
#   stdout 标记轮询，不注入、不改动生产代码。
# 前置：build\qcnote.exe 与 build\e2e_draft_driver.exe 已构建；脚本清场只按可执行
#   文件路径过滤结束被测进程（Path 不可读的进程防御跳过），不波及本机其他路径
#   qcnote。缓存目录钉法：build\qcnote.portable 空标记（便携模式）→ 缓存
#   build\drafts\、会话 build\qcnote.ini；前置检查三件套均不存在（防误删他人
#   产物），finally 清理。
# 用法：pwsh.exe -NoProfile -ExecutionPolicy Bypass -File tests\verify-hotexit-draft-e2e.ps1
#       [-ProjectRoot <项目根>] [-KeepTemp]
# 退出码：0 = 通过；1 = 失败
# 安全：GUI 红线——一律 QT_QPA_PLATFORM=offscreen 启动，全程无可见窗口；测试数据与
#   日志仅建在 D:\temp\aiRunner\opencode 下自建子目录，不写 C 盘。
#Requires -Version 7

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
$BuildDir   = Join-Path $ProjectRoot "build"
$ExePath    = Join-Path $BuildDir "qcnote.exe"            # 被测主程序（恢复侧）
$DriverExe  = Join-Path $BuildDir "e2e_draft_driver.exe"  # 端到端驱动 harness（写入侧）
$MarkerPath = Join-Path $BuildDir "qcnote.portable"       # 便携标记 → 缓存钉在 build
$IniPath    = Join-Path $BuildDir "qcnote.ini"            # 便携会话文件
$DraftsDir  = Join-Path $BuildDir "drafts"                # 便携缓存目录

$script:FailCount   = 0
$script:CaseResults = @()
$script:TempRoot    = $null
$script:RunSeq      = 0
$script:OrigQpa     = $env:QT_QPA_PLATFORM

function Add-Result([string]$case, [bool]$pass, [string]$detail) {
    $script:CaseResults += [pscustomobject]@{ Case = $case; Pass = $pass; Detail = $detail }
    $tag = if ($pass) { 'PASS' } else { 'FAIL' }
    Write-Host ("  [{0}] {1}：{2}" -f $tag, $case, $detail)
    if (-not $pass) { $script:FailCount++ }
}

# 写 UTF-8 无 BOM 文本（QJsonDocument 对 BOM 会解析失败）
function Write-Utf8NoBom([string]$path, [string]$text) {
    [System.IO.File]::WriteAllText($path, $text, [System.Text.UTF8Encoding]::new($false))
}

# 按可执行文件路径过滤结束被测进程（不误杀本机其他路径运行的 qcnote；
# Path 不可读的进程防御跳过，宁漏勿误）
function Stop-ByExePath([string]$exePath, [string]$procName) {
    Get-Process $procName -ErrorAction SilentlyContinue | Where-Object {
        $exe = $null
        try { $exe = $_.Path } catch { $exe = $null }
        $null -ne $exe -and $exe -ieq $exePath
    } | Stop-Process -Force -ErrorAction SilentlyContinue
}

# stdout/stderr 是否含 QML 错误特征
$errorPatternRefs = '(?i)ReferenceError|TypeError|SyntaxError|is not defined|Cannot read|Cannot assign'
$errorPatternQmlFile = '(?i)file:///.+:\d+.*error'
function Test-LogHasQmlError([string]$outLog, [string]$errLog) {
    foreach ($log in @($outLog, $errLog)) {
        if (-not (Test-Path -LiteralPath $log)) { continue }
        $text = Get-Content -LiteralPath $log -Raw
        if (($text -match $errorPatternRefs) -or ($text -match $errorPatternQmlFile)) {
            return $true
        }
    }
    return $false
}

# drafts 目录下的条目 uid 清单（目录不存在视为空）
function Get-EntryUids {
    if (-not (Test-Path -LiteralPath $DraftsDir)) { return @() }
    @(Get-ChildItem -LiteralPath $DraftsDir -Filter '*.json' -File |
        ForEach-Object { $_.BaseName })
}

# 读 qcnote.ini [documents] 节键值表（QSettings IniFormat 落盘：节内键如
# "1\url"、"2\uid"、"activeUid"；文件不存在返回空表）——关闭现场还原的
# 文件级断言用（顺序 uid 条目与 activeUid 互斥）
function Get-DocumentsIniMap {
    $map = @{}
    if (-not (Test-Path -LiteralPath $IniPath)) { return $map }
    $inSection = $false
    foreach ($line in (Get-Content -LiteralPath $IniPath)) {
        if ($line -match '^\[(.+)\]\s*$') { $inSection = ($Matches[1] -eq 'documents'); continue }
        if ($inSection -and $line -match '^([^=]+)=(.*)$') { $map[$Matches[1]] = $Matches[2] }
    }
    return $map
}

# QUrl::fromLocalFile($path).toString() 的期望值：file:/// + 正斜杠绝对路径
function Convert-ToFileUrl([string]$path) {
    'file:///' + ($path -replace '\\', '/')
}

function Read-EntryJson([string]$uid) {
    Get-Content -LiteralPath (Join-Path $DraftsDir "$uid.json") -Raw | ConvertFrom-Json
}

# 场景间复位：清缓存与会话（保留便携标记），保证每场景首启无会话
function Reset-DraftState {
    if (Test-Path -LiteralPath $DraftsDir) { Remove-Item -LiteralPath $DraftsDir -Recurse -Force }
    if (Test-Path -LiteralPath $IniPath)   { Remove-Item -LiteralPath $IniPath -Force }
}

# 启动被测进程（offscreen），返回 进程/日志 句柄（不杀进程）
function Start-Tracked([string]$filePath, [string]$fileDir, [string[]]$argList = @()) {
    $script:RunSeq++
    $outLog = Join-Path $script:TempRoot ("stdout-{0}.log" -f $script:RunSeq)
    $errLog = Join-Path $script:TempRoot ("stderr-{0}.log" -f $script:RunSeq)
    $proc = Start-Process -FilePath $filePath -WorkingDirectory $fileDir `
        -ArgumentList $argList `
        -RedirectStandardOutput $outLog -RedirectStandardError $errLog -PassThru
    return @{ Proc = $proc; Out = $outLog; Err = $errLog }
}

# 等待进程退出（超时视为失败并强杀；返回 $true=自然退出）
function Wait-ProcExit([System.Diagnostics.Process]$proc, [int]$timeoutSec) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while (-not $proc.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }
    if (-not $proc.HasExited) {
        $proc | Stop-Process -Force
        $proc.WaitForExit(5000) | Out-Null
        return $false
    }
    return $true
}

# 轮询 stdout 日志等待标记行出现（进程提前退出且无标记则提前失败）
function Wait-StdoutMarker([System.Diagnostics.Process]$proc, [string]$outLog, [string]$pattern, [int]$timeoutMs) {
    $deadline = [System.Diagnostics.Stopwatch]::StartNew()
    while ($deadline.ElapsedMilliseconds -lt $timeoutMs) {
        if ($proc.HasExited) { return $false }
        if (Test-Path -LiteralPath $outLog) {
            $text = Get-Content -LiteralPath $outLog -Raw -ErrorAction SilentlyContinue
            if ($text -match $pattern) { return $true }
        }
        Start-Sleep -Milliseconds 50
    }
    return $false
}

# 结束本次启动的进程（定向；结束后等待句柄释放）
function Stop-Tracked([System.Diagnostics.Process]$proc) {
    if ($proc -and -not $proc.HasExited) {
        $proc | Stop-Process -Force
        $proc.WaitForExit(5000) | Out-Null
    }
}

# ---- 场景辅助断言 ----

# 断言 drafts 下恰一条目且字段正确（content/name=未命名-1/url 空）；返回条目 uid
function Assert-SingleDraftEntry([string]$casePrefix, [string]$expectedContent) {
    $uids = @(Get-EntryUids)
    if ($uids.Count -ne 1) {
        Add-Result "$casePrefix 落盘条目唯一" $false "条目数=$($uids.Count)（uids=$($uids -join ',')），期望 1"
        return $null
    }
    Add-Result "$casePrefix 落盘条目唯一" $true "uid=$($uids[0])"
    $e = Read-EntryJson $uids[0]
    $contentOk = ($e.content -eq $expectedContent)
    $nameOk    = ($e.name -eq '未命名-1')
    $urlOk     = ($e.url -eq '')
    Add-Result "$casePrefix 条目字段（content/name/url）" ($contentOk -and $nameOk -and $urlOk) `
        ("content 匹配=$contentOk name=未命名-1 匹配=$nameOk url 空=$urlOk")
    return $uids[0]
}

# 真实 qcnote.exe 重启恢复断言：进程存活 + 无 QML 错误 + 旧条目被消费 +
# 按新 uid 续写唯一条目且字段正确（未命名重建链路：恢复即删旧条目、重建
# 未命名文档（modified）→ 1s 防抖后按新 uid 续写——内容恢复为未保存状态）
function Assert-RealProcessRestore([string]$casePrefix, [string]$oldUid, [string]$expectedContent) {
    $run = Start-Tracked $ExePath $BuildDir
    # 轮询代替固定 4 秒睡眠（消除高负载机器时序敏感，DEC-008）：每约 50ms
    # 检查一次「旧条目被消费 + 新 uid 续写条目出现」（启动 + restoreDrafts +
    # 1s 防抖续写的完成信号），上限 10 秒，超时视为失败；成功后再留 300ms
    # 最小稳定等待（续写字段写入的余量）
    $newUid = $null
    $deadline = [System.Diagnostics.Stopwatch]::StartNew()
    while ($deadline.ElapsedMilliseconds -lt 10000) {
        if ($run.Proc.HasExited) { break }
        $uids = @(Get-EntryUids)
        if (($uids.Count -eq 1) -and ($uids[0] -ne $oldUid) -and `
                (-not (Test-Path -LiteralPath (Join-Path $DraftsDir "$oldUid.json")))) {
            $newUid = $uids[0]
            break
        }
        Start-Sleep -Milliseconds 50
    }
    if ($null -ne $newUid) { Start-Sleep -Milliseconds 300 }
    $elapsedMs = $deadline.ElapsedMilliseconds
    $alive = -not $run.Proc.HasExited
    Add-Result "$casePrefix 重启进程存活（真实 qcnote.exe）" $alive `
        $(if ($alive) { "offscreen 启动未退出（恢复链路无崩溃），恢复信号出现=$($null -ne $newUid)（${elapsedMs}ms）" } else { '启动后即退出（检查单实例/启动错误）' })
    Add-Result "$casePrefix 重启无 QML 错误" (-not (Test-LogHasQmlError $run.Out $run.Err)) 'stdout/stderr 无 QML 错误特征'
    Stop-Tracked $run.Proc

    $consumed = -not (Test-Path -LiteralPath (Join-Path $DraftsDir "$oldUid.json"))
    Add-Result "$casePrefix 旧条目已被恢复消费" $consumed `
        $(if ($consumed) { 'restoreDrafts 恢复即删（未命名重建）' } else { "条目 $oldUid.json 仍在（未被恢复）" })

    $uids = @(Get-EntryUids)
    $unique = ($uids.Count -eq 1) -and ($uids[0] -ne $oldUid)
    Add-Result "$casePrefix 续写唯一新条目" $unique "恢复后条目数=$($uids.Count)，新 uid=$($uids -join ',')"
    if ($unique) {
        $e = Read-EntryJson $uids[0]
        $contentOk = ($e.content -eq $expectedContent)
        $nameOk    = ($e.name -eq '未命名-1')
        $urlOk     = ($e.url -eq '')
        Add-Result "$casePrefix 新条目字段（content/name/url）" ($contentOk -and $nameOk -and $urlOk) `
            ("内容恢复为未保存状态：content 匹配=$contentOk name=未命名-1 匹配=$nameOk url 空=$urlOk")
    }
}

try {
    # ---- 前置检查 ----
    foreach ($p in @($ExePath, $DriverExe)) {
        if (-not (Test-Path -LiteralPath $p)) {
            throw "未找到 $p，请先构建（cmake --build build --target e2e_draft_driver）"
        }
    }
    # 便携三件套必须全部不存在：残留说明他人/上次异常退出产物，不误动、直接中止
    foreach ($p in @($MarkerPath, $IniPath, $DraftsDir)) {
        if (Test-Path -LiteralPath $p) {
            throw "build 下已存在 $p（异常残留），为避免误删他人产物，请先人工确认清理后重跑"
        }
    }
    # 临时数据目录：仅 D:\temp\aiRunner\opencode 下自建子目录（不回退 C 盘）
    $tempBase = 'D:\temp\aiRunner\opencode'
    if (-not (Test-Path -LiteralPath $tempBase)) {
        throw "临时目录基址 $tempBase 不可用，按约定不回退 C 盘，中止"
    }
    $script:TempRoot = Join-Path $tempBase ("qcnote-verify-hotexit-e2e-{0}-{1}" -f $PID, (Get-Date -Format 'yyyyMMddHHmmss'))
    New-Item -ItemType Directory -Path $script:TempRoot -Force | Out-Null

    # 环境就绪：Qt 运行库 PATH + offscreen 平台（GUI 红线：无可见窗口）
    # + 首启右键菜单提示隔离（不写注册表、不弹模态提示）
    $env:PATH = "D:\Program\Qt\6.11.2\mingw_64\bin;D:\Program\Qt\Tools\mingw1310_64\bin;" + $env:PATH
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:QCNOTE_TEST_SHELL_NOOP = '1'
    # 前置清场：仅结束路径为被测 exe 的进程（不波及本机其他路径的 qcnote）
    Stop-ByExePath $ExePath   'qcnote'
    Stop-ByExePath $DriverExe 'e2e_draft_driver'

    # 便携标记：缓存/会话钉在 build 内
    Write-Utf8NoBom $MarkerPath ''

    # ==================== TC-010：热退出全链路 ====================
    Write-Host "== TC-010：热退出（键入 → 防抖落盘 → QML 退出链路正常退出 → 重启恢复） =="
    $env:QCNOTE_E2E_CONTENT = 'hotexit-e2e-热退出未命名内容'
    $run = Start-Tracked $DriverExe $BuildDir @('hotexit')
    $exited = Wait-ProcExit $run.Proc 30
    Add-Result 'TC-010 驱动进程自然退出（热退出路径）' ($exited -and $run.Proc.ExitCode -eq 0) `
        $(if ($exited) { "退出码=$($run.Proc.ExitCode)（doQuit→close→onClosing 放行→exec 返回）" } else { '30s 未退出（超时强杀）' })
    Add-Result 'TC-010 无 QML 错误输出' (-not (Test-LogHasQmlError $run.Out $run.Err)) 'stdout/stderr 无 QML 错误特征'
    $uid = Assert-SingleDraftEntry 'TC-010' $env:QCNOTE_E2E_CONTENT
    if ($uid) { Assert-RealProcessRestore 'TC-010' $uid $env:QCNOTE_E2E_CONTENT }

    # ==================== TC-011：强杀全链路（用户原始场景） ====================
    Write-Host "== TC-011：强杀（键入 → 等 2 秒 → Stop-Process -Force → 重启恢复） =="
    Reset-DraftState
    $env:QCNOTE_E2E_CONTENT = 'kill-e2e-强杀未命名内容'
    $run = Start-Tracked $DriverExe $BuildDir @('kill')
    $wrote = Wait-StdoutMarker $run.Proc $run.Out '\[e2e\] WROTE' 15000
    Add-Result 'TC-011 键入完成（真实输入链路 WROTE 标记）' $wrote `
        $(if ($wrote) { 'doNewDocument + TextArea append → doc.content 回写链路 OK' } else { '15s 未见 WROTE 标记' })
    if ($wrote) {
        Start-Sleep -Seconds 2   # 防抖（1000ms）自然到期落盘
        $run.Proc | Stop-Process -Force   # 任务管理器强杀等价
        $run.Proc.WaitForExit(5000) | Out-Null
        Add-Result 'TC-011 进程被强杀终止（非自然退出）' ($run.Proc.ExitCode -ne 0) `
            "强杀退出码=$($run.Proc.ExitCode)（0 会与自然退出混淆）"
    }
    $uid = Assert-SingleDraftEntry 'TC-011' $env:QCNOTE_E2E_CONTENT
    if ($uid) { Assert-RealProcessRestore 'TC-011' $uid $env:QCNOTE_E2E_CONTENT }

    # TC-011 补充：恢复后激活的直接断言（harness restore 模式读 DocumentManager）
    Reset-DraftState
    $env:QCNOTE_E2E_CONTENT = 'kill-e2e-激活断言内容'
    $run = Start-Tracked $DriverExe $BuildDir @('kill')
    $wrote = Wait-StdoutMarker $run.Proc $run.Out '\[e2e\] WROTE' 15000
    if ($wrote) {
        Start-Sleep -Seconds 2
        $run.Proc | Stop-Process -Force
        $run.Proc.WaitForExit(5000) | Out-Null
        $runV = Start-Tracked $DriverExe $BuildDir @('restore')
        # restore 模式：加载即断言后返回（恢复文档防抖不落盘，drafts 留空）；
        # 期望内容沿用启动前的 QCNOTE_E2E_CONTENT（restore 断言 content 与其比对）
        $exited = Wait-ProcExit $runV.Proc 30
        $restoreOk = $exited -and $runV.Proc.ExitCode -eq 0
        $outText = if (Test-Path -LiteralPath $runV.Out) { Get-Content -LiteralPath $runV.Out -Raw } else { '' }
        Add-Result 'TC-011 恢复断言（含 activeDocument 激活，DEC-002）' `
            ($restoreOk -and $outText -match '\[e2e\] RESTORE PASS') `
            $(if ($restoreOk) { ($outText -split "`n" | Where-Object { $_ -match 'RESTORE' }) -join ' | ' } else { "restore 模式异常退出码=$($runV.Proc.ExitCode)" })
    } else {
        Add-Result 'TC-011 恢复断言（含 activeDocument 激活，DEC-002）' $false '前置 WROTE 未出现，场景未执行'
    }

    # ==================== TC-012：强杀前失焦场景 ====================
    Write-Host "== TC-012：失焦强杀（键入 → 200ms 失焦即写 → 1 秒内强杀 → 重启恢复） =="
    Reset-DraftState
    $env:QCNOTE_E2E_CONTENT = 'blur-e2e-失焦未命名内容'
    $run = Start-Tracked $DriverExe $BuildDir @('blurkill')
    $wrote = Wait-StdoutMarker $run.Proc $run.Out '\[e2e\] WROTE' 15000
    $blurred = Wait-StdoutMarker $run.Proc $run.Out '\[e2e\] BLURRED' 10000
    Add-Result 'TC-012 失焦触发（applicationStateChanged 等价驱动）' $blurred `
        $(if ($blurred) { '键入后 200ms 发真实失焦信号（防抖 1000ms 未到期）' } else { '未见 BLURRED 标记' })
    $tBlur = Get-Date
    $run.Proc | Stop-Process -Force   # 失焦后立即强杀（任务管理器场景：切窗后即杀）
    $run.Proc.WaitForExit(5000) | Out-Null
    $tKill = Get-Date
    if ($blurred) {
        $blurToKillMs = [int]($tKill - $tBlur).TotalMilliseconds
        Add-Result 'TC-012 强杀发生在失焦后 1 秒内' ($blurToKillMs -lt 1000) `
            "失焦标记→强杀 ${blurToKillMs}ms（防抖自键入起 1000ms，此窗口内失焦 flush 是唯一落盘路径）"
    }
    $uid = Assert-SingleDraftEntry 'TC-012' $env:QCNOTE_E2E_CONTENT
    if ($uid) { Assert-RealProcessRestore 'TC-012' $uid $env:QCNOTE_E2E_CONTENT }

    # ==================== TC-011：关闭现场还原（标签顺序与活动标签） ====================
    # 《测试用例-关闭现场还原》TC-011：[文件A, 未命名1(输入内容), 文件B] 且
    # 活动=未命名1 → 热退出 → 重启 → 断言标签顺序与激活（会话文件级 +
    # 驱动直断言双重证据）
    Write-Host "== TC-011：关闭现场还原（[文件A, 未命名1(内容), 文件B] 活动=未命名1 → 热退出 → 重启） =="
    Reset-DraftState
    $env:QCNOTE_E2E_CONTENT = 'scene-e2e-关闭现场还原未命名内容'
    $sceneFileA = Join-Path $script:TempRoot 'e2e-scene-a.txt'
    $sceneFileB = Join-Path $script:TempRoot 'e2e-scene-b.txt'
    Write-Utf8NoBom $sceneFileA 'scene-file-A'
    Write-Utf8NoBom $sceneFileB 'scene-file-B'
    $env:QCNOTE_E2E_FILE_A = $sceneFileA
    $env:QCNOTE_E2E_FILE_B = $sceneFileB
    $urlA = Convert-ToFileUrl $sceneFileA
    $urlB = Convert-ToFileUrl $sceneFileB

    $run = Start-Tracked $DriverExe $BuildDir @('mixexit')
    $exited = Wait-ProcExit $run.Proc 30
    Add-Result 'TC-011 驱动进程自然退出（混排热退出）' ($exited -and $run.Proc.ExitCode -eq 0) `
        $(if ($exited) { "退出码=$($run.Proc.ExitCode)（构造现场 → 防抖落盘 → doQuit 热退出）" } else { '30s 未退出（超时强杀）' })
    Add-Result 'TC-011 无 QML 错误输出' (-not (Test-LogHasQmlError $run.Out $run.Err)) 'stdout/stderr 无 QML 错误特征'

    # 退出时会话快照断言：完整标签序列 [A@1, 未命名1(uid)@2, B@3]、activeUid
    # 记录且与条目 2 uid 一致（uid 取自驱动 WROTE 标记）、无 active 键（互斥）
    $mixOut = if (Test-Path -LiteralPath $run.Out) { Get-Content -LiteralPath $run.Out -Raw } else { '' }
    $wroteUid = $null
    if ($mixOut -match '\[e2e\] WROTE uid=([0-9a-fA-F-]+)') { $wroteUid = $Matches[1] }
    $ini = Get-DocumentsIniMap
    $exitOrderOk = ($null -ne $wroteUid) -and ($ini['size'] -eq '3') `
        -and ($ini['1\url'] -eq $urlA) -and ($ini['2\uid'] -eq $wroteUid) `
        -and ($ini['3\url'] -eq $urlB)
    Add-Result 'TC-011 退出会话记录完整标签序列（A/uid/B）' $exitOrderOk `
        "size=$($ini['size']) 1\url=A=$($ini['1\url'] -eq $urlA) 2\uid=键入uid=$($ini['2\uid'] -eq $wroteUid) 3\url=B=$($ini['3\url'] -eq $urlB)"
    $exitActiveOk = ($null -ne $wroteUid) -and ($ini['activeUid'] -eq $wroteUid) `
        -and (-not $ini.ContainsKey('active'))
    Add-Result 'TC-011 退出会话 activeUid 互斥记录（活动=未命名）' $exitActiveOk `
        "activeUid=键入uid=$($ini['activeUid'] -eq $wroteUid) active 键不存在=$(-not $ini.ContainsKey('active'))"

    # 真实 qcnote.exe 重启：旧条目消费 + 新 uid 续写（复用既有断言），随后
    # 以会话对齐重写结果断言顺序与活动标签的还原（[A, 未命名(新uid)@2, B] +
    # activeUid=新uid——500ms 会话防抖在续写（1s）之前必然已落盘）
    $uid = Assert-SingleDraftEntry 'TC-011' $env:QCNOTE_E2E_CONTENT
    if ($uid) { Assert-RealProcessRestore 'TC-011' $uid $env:QCNOTE_E2E_CONTENT }
    $newUid = $null
    $newUids = @(Get-EntryUids)
    if ($newUids.Count -ge 1) { $newUid = $newUids[0] }
    $ini2 = Get-DocumentsIniMap
    $restoredOrderOk = ($null -ne $newUid) -and ($ini2['size'] -eq '3') `
        -and ($ini2['1\url'] -eq $urlA) -and ($ini2['2\uid'] -eq $newUid) `
        -and ($ini2['3\url'] -eq $urlB)
    Add-Result 'TC-011 重启后标签顺序还原（会话对齐：A/新uid/B）' $restoredOrderOk `
        "size=$($ini2['size']) 2\uid=新uid=$($ini2['2\uid'] -eq $newUid)"
    $restoredActiveOk = ($null -ne $newUid) -and ($ini2['activeUid'] -eq $newUid) `
        -and (-not $ini2.ContainsKey('active'))
    Add-Result 'TC-011 重启后活动标签还原（activeUid=新uid）' $restoredActiveOk `
        "activeUid=新uid=$($ini2['activeUid'] -eq $newUid) active 键不存在=$(-not $ini2.ContainsKey('active'))"

    # 驱动直断言（restoremix）：直接读 DocumentManager 顺序与 activeDocument
    $runV = Start-Tracked $DriverExe $BuildDir @('restoremix')
    $exitedV = Wait-ProcExit $runV.Proc 30
    $vText = if (Test-Path -LiteralPath $runV.Out) { Get-Content -LiteralPath $runV.Out -Raw } else { '' }
    Add-Result 'TC-011 恢复直断言（顺序 [A,未命名1,B] 与活动=未命名1）' `
        ($exitedV -and $runV.Proc.ExitCode -eq 0 -and $vText -match '\[e2e\] RESTOREMIX PASS') `
        $(if ($exitedV) { ($vText -split "`n" | Where-Object { $_ -match 'RESTOREMIX' }) -join ' | ' } else { "restoremix 异常退出码=$($runV.Proc.ExitCode)" })

    # ==================== 验收标准 2：[未命名1, 文件A, 未命名2] 活动=未命名2 ====================
    # 《需求-关闭现场还原》验收标准 2 变体（本轮测试扩展）：未命名分居头尾、
    # 文件居中——mixexit2 构造现场热退出 → 退出会话断言 [uid1, A, uid2] +
    # activeUid=uid2 → 真实 qcnote.exe 重启（双草稿消费续写 + 会话对齐重写）
    # → restoremix2 驱动直断言最终顺序与尾部未命名激活
    Write-Host "== 验收标准 2：[未命名1, 文件A, 未命名2] 活动=未命名2 → 热退出 → 重启 =="
    Reset-DraftState
    $env:QCNOTE_E2E_CONTENT  = 'scene2-e2e-双未命名内容一'
    $env:QCNOTE_E2E_CONTENT2 = 'scene2-e2e-双未命名内容二'
    $env:QCNOTE_E2E_FILE_A = $sceneFileA
    $env:QCNOTE_E2E_FILE_B = $sceneFileB

    $run = Start-Tracked $DriverExe $BuildDir @('mixexit2')
    $exited = Wait-ProcExit $run.Proc 30
    Add-Result '验收2 驱动进程自然退出（双未命名混排热退出）' ($exited -and $run.Proc.ExitCode -eq 0) `
        $(if ($exited) { "退出码=$($run.Proc.ExitCode)（构造现场 → 双防抖落盘 → doQuit 热退出）" } else { '30s 未退出（超时强杀）' })
    Add-Result '验收2 无 QML 错误输出' (-not (Test-LogHasQmlError $run.Out $run.Err)) 'stdout/stderr 无 QML 错误特征'

    # 退出时会话快照断言：[uid1@1, A@2, uid2@3] 且 activeUid=uid2（两键互斥）
    $mix2Out = if (Test-Path -LiteralPath $run.Out) { Get-Content -LiteralPath $run.Out -Raw } else { '' }
    $wroteUid1 = $null; $wroteUid2 = $null
    if ($mix2Out -match '\[e2e\] WROTE uid1=([0-9a-fA-F-]+) uid2=([0-9a-fA-F-]+)') {
        $wroteUid1 = $Matches[1]; $wroteUid2 = $Matches[2]
    }
    $ini = Get-DocumentsIniMap
    $exitOrder2Ok = ($null -ne $wroteUid1) -and ($null -ne $wroteUid2) -and ($ini['size'] -eq '3') `
        -and ($ini['1\uid'] -eq $wroteUid1) -and ($ini['2\url'] -eq $urlA) -and ($ini['3\uid'] -eq $wroteUid2)
    Add-Result '验收2 退出会话记录完整标签序列（uid1/A/uid2）' $exitOrder2Ok `
        "size=$($ini['size']) 1\uid=uid1=$($ini['1\uid'] -eq $wroteUid1) 2\url=A=$($ini['2\url'] -eq $urlA) 3\uid=uid2=$($ini['3\uid'] -eq $wroteUid2)"
    $exitActive2Ok = ($null -ne $wroteUid2) -and ($ini['activeUid'] -eq $wroteUid2) `
        -and (-not $ini.ContainsKey('active'))
    Add-Result '验收2 退出会话 activeUid=未命名2（互斥）' $exitActive2Ok `
        "activeUid=uid2=$($ini['activeUid'] -eq $wroteUid2) active 键不存在=$(-not $ini.ContainsKey('active'))"

    # 退出后草稿恰两条且两条键入内容各就位
    $uids = @(Get-EntryUids)
    $draft2CountOk = ($uids.Count -eq 2)
    $hasContent1 = $false; $hasContent2 = $false
    foreach ($u in $uids) {
        $e = Read-EntryJson $u
        if ($e.content -eq $env:QCNOTE_E2E_CONTENT)  { $hasContent1 = $true }
        if ($e.content -eq $env:QCNOTE_E2E_CONTENT2) { $hasContent2 = $true }
    }
    Add-Result '验收2 落盘条目恰两条且内容各就位' ($draft2CountOk -and $hasContent1 -and $hasContent2) `
        "条目数=$($uids.Count)（期望 2） 内容一=$hasContent1 内容二=$hasContent2"

    # 真实 qcnote.exe 重启：轮询「两条旧条目均被消费 + 两条新 uid 续写出现」
    # （恢复 + 1s 防抖续写的完成信号），上限 10 秒
    $run2 = Start-Tracked $ExePath $BuildDir
    $deadline = [System.Diagnostics.Stopwatch]::StartNew()
    $bothRenewed = $false
    while ($deadline.ElapsedMilliseconds -lt 10000) {
        if ($run2.Proc.HasExited) { break }
        $cur = @(Get-EntryUids)
        if (($cur.Count -eq 2) -and ($wroteUid1 -and $wroteUid2) `
                -and ($cur -notcontains $wroteUid1) -and ($cur -notcontains $wroteUid2)) {
            $bothRenewed = $true
            break
        }
        Start-Sleep -Milliseconds 50
    }
    if ($bothRenewed) { Start-Sleep -Milliseconds 300 }
    Add-Result '验收2 重启进程存活（真实 qcnote.exe）' (-not $run2.Proc.HasExited) `
        $(if (-not $run2.Proc.HasExited) { "双草稿消费续写完成=$bothRenewed（$($deadline.ElapsedMilliseconds)ms）" } else { '启动后即退出' })
    Add-Result '验收2 重启无 QML 错误' (-not (Test-LogHasQmlError $run2.Out $run2.Err)) 'stdout/stderr 无 QML 错误特征'
    Stop-Tracked $run2.Proc

    # 会话对齐重写断言：[新uid1@1, A@2, 新uid2@3] + activeUid=新uid2（文件级）
    $ini2 = Get-DocumentsIniMap
    $newUid1 = $ini2['1\uid']; $newUid2 = $ini2['3\uid']
    $restoredOrder2Ok = ($ini2['size'] -eq '3') -and ($newUid1 -and $newUid2) `
        -and ($newUid1 -ne $newUid2) -and ($ini2['2\url'] -eq $urlA) `
        -and ($newUid1 -notin @($wroteUid1, $wroteUid2)) -and ($newUid2 -notin @($wroteUid1, $wroteUid2))
    Add-Result '验收2 重启后标签顺序还原（新uid1/A/新uid2）' $restoredOrder2Ok `
        "size=$($ini2['size']) 1\uid 与 3\uid 均为新 uid 且互异=$restoredOrder2Ok 2\url=A=$($ini2['2\url'] -eq $urlA)"
    $restoredActive2Ok = ($newUid2) -and ($ini2['activeUid'] -eq $newUid2) -and (-not $ini2.ContainsKey('active'))
    Add-Result '验收2 重启后活动标签还原（activeUid=新uid2）' $restoredActive2Ok `
        "activeUid=尾部新uid=$($ini2['activeUid'] -eq $newUid2) active 键不存在=$(-not $ini2.ContainsKey('active'))"

    # 驱动直断言（restoremix2）：顺序 [未命名1, A, 未命名2] 与活动=未命名2
    $runV = Start-Tracked $DriverExe $BuildDir @('restoremix2')
    $exitedV = Wait-ProcExit $runV.Proc 30
    $vText = if (Test-Path -LiteralPath $runV.Out) { Get-Content -LiteralPath $runV.Out -Raw } else { '' }
    Add-Result '验收2 恢复直断言（顺序 [未命名1,A,未命名2] 与活动=未命名2）' `
        ($exitedV -and $runV.Proc.ExitCode -eq 0 -and $vText -match '\[e2e\] RESTOREMIX2 PASS') `
        $(if ($exitedV) { ($vText -split "`n" | Where-Object { $_ -match 'RESTOREMIX2' }) -join ' | ' } else { "restoremix2 异常退出码=$($runV.Proc.ExitCode)" })
    Remove-Item Env:\QCNOTE_E2E_CONTENT2 -ErrorAction SilentlyContinue
    Remove-Item Env:\QCNOTE_E2E_FILE_A -ErrorAction SilentlyContinue
    Remove-Item Env:\QCNOTE_E2E_FILE_B -ErrorAction SilentlyContinue

    # ==================== 验收标准 3：[文件A, 未命名1(内容), 文件B] 活动=文件B ====================
    # 活动为文件标签（任意位置）行为与现状一致：mixfile 构造现场 → 会话按现状
    # 记 active=URL（非 activeUid）→ 真实重启 → 断言现场还原且活动=文件B
    Write-Host "== 验收标准 3：[文件A, 未命名1, 文件B] 活动=文件B → 热退出 → 重启（现状不变） =="
    Reset-DraftState
    $env:QCNOTE_E2E_CONTENT = 'scene3-e2e-文件活动未命名内容'
    $env:QCNOTE_E2E_FILE_A = $sceneFileA
    $env:QCNOTE_E2E_FILE_B = $sceneFileB

    $run = Start-Tracked $DriverExe $BuildDir @('mixfile')
    $exited = Wait-ProcExit $run.Proc 30
    Add-Result '验收3 驱动进程自然退出（文件活动现场热退出）' ($exited -and $run.Proc.ExitCode -eq 0) `
        $(if ($exited) { "退出码=$($run.Proc.ExitCode)" } else { '30s 未退出（超时强杀）' })
    Add-Result '验收3 无 QML 错误输出' (-not (Test-LogHasQmlError $run.Out $run.Err)) 'stdout/stderr 无 QML 错误特征'

    $mix3Out = if (Test-Path -LiteralPath $run.Out) { Get-Content -LiteralPath $run.Out -Raw } else { '' }
    $wroteUid = $null
    if ($mix3Out -match '\[e2e\] WROTE uid=([0-9a-fA-F-]+)') { $wroteUid = $Matches[1] }
    $ini = Get-DocumentsIniMap
    $exitOrder3Ok = ($null -ne $wroteUid) -and ($ini['size'] -eq '3') `
        -and ($ini['1\url'] -eq $urlA) -and ($ini['2\uid'] -eq $wroteUid) -and ($ini['3\url'] -eq $urlB)
    Add-Result '验收3 退出会话记录完整标签序列（A/uid/B）' $exitOrder3Ok `
        "size=$($ini['size']) 2\uid=键入uid=$($ini['2\uid'] -eq $wroteUid)"
    $exitActive3Ok = ($ini['active'] -eq $urlB) -and (-not $ini.ContainsKey('activeUid'))
    Add-Result '验收3 退出会话活动=文件（active=URL，现状格式）' $exitActive3Ok `
        "active=B url=$($ini['active'] -eq $urlB) activeUid 键不存在=$(-not $ini.ContainsKey('activeUid'))"

    # 真实重启（复用单条草稿断言：消费 + 新 uid 续写 + 字段）
    $uid = Assert-SingleDraftEntry '验收3' $env:QCNOTE_E2E_CONTENT
    if ($uid) { Assert-RealProcessRestore '验收3' $uid $env:QCNOTE_E2E_CONTENT }

    # 会话对齐重写断言：[A, 新uid@2, B] + active=URL（活动还原为文件B）
    $newUids = @(Get-EntryUids)
    $newUid = if ($newUids.Count -ge 1) { $newUids[0] } else { $null }
    $ini3 = Get-DocumentsIniMap
    $restored3Ok = ($null -ne $newUid) -and ($ini3['size'] -eq '3') -and ($ini3['1\url'] -eq $urlA) `
        -and ($ini3['2\uid'] -eq $newUid) -and ($ini3['3\url'] -eq $urlB) -and ($ini3['active'] -eq $urlB) `
        -and (-not $ini3.ContainsKey('activeUid'))
    Add-Result '验收3 重启后现场还原（A/新uid/B，active=文件B）' $restored3Ok `
        "size=$($ini3['size']) 2\uid=新uid=$($ini3['2\uid'] -eq $newUid) active=B url=$($ini3['active'] -eq $urlB)"
    Remove-Item Env:\QCNOTE_E2E_FILE_A -ErrorAction SilentlyContinue
    Remove-Item Env:\QCNOTE_E2E_FILE_B -ErrorAction SilentlyContinue

    # ==================== 验收标准 4：[文件A, 未命名1(无内容), 文件B] 活动=文件B ====================
    # 无内容未命名标签不恢复：mixempty 构造现场（会话仍记录空 uid 条目，无草稿）
    # → 真实重启 → 空位自然消除（[A, B] 顺序不变），活动=文件B
    Write-Host "== 验收标准 4：[文件A, 空未命名, 文件B] 活动=文件B → 热退出 → 重启（空位消除） =="
    Reset-DraftState
    $env:QCNOTE_E2E_CONTENT = 'scene4-e2e-空未命名占位'   # mixempty 模式要求非空但不键入
    $env:QCNOTE_E2E_FILE_A = $sceneFileA
    $env:QCNOTE_E2E_FILE_B = $sceneFileB

    $run = Start-Tracked $DriverExe $BuildDir @('mixempty')
    $exited = Wait-ProcExit $run.Proc 30
    Add-Result '验收4 驱动进程自然退出（空未命名现场热退出）' ($exited -and $run.Proc.ExitCode -eq 0) `
        $(if ($exited) { "退出码=$($run.Proc.ExitCode)" } else { '30s 未退出（超时强杀）' })
    Add-Result '验收4 无 QML 错误输出' (-not (Test-LogHasQmlError $run.Out $run.Err)) 'stdout/stderr 无 QML 错误特征'

    $mix4Out = if (Test-Path -LiteralPath $run.Out) { Get-Content -LiteralPath $run.Out -Raw } else { '' }
    $wroteUid = $null
    if ($mix4Out -match '\[e2e\] WROTE uid=([0-9a-fA-F-]+)') { $wroteUid = $Matches[1] }
    $ini = Get-DocumentsIniMap
    $exitOrder4Ok = ($null -ne $wroteUid) -and ($ini['size'] -eq '3') `
        -and ($ini['1\url'] -eq $urlA) -and ($ini['2\uid'] -eq $wroteUid) -and ($ini['3\url'] -eq $urlB) `
        -and ($ini['active'] -eq $urlB)
    Add-Result '验收4 退出会话记录完整标签序列（含空未命名 uid）' $exitOrder4Ok `
        "size=$($ini['size']) 2\uid=空未命名uid=$($ini['2\uid'] -eq $wroteUid) active=B url=$($ini['active'] -eq $urlB)"
    $draftsEmpty4 = (@(Get-EntryUids).Count -eq 0)
    Add-Result '验收4 空未命名无草稿条目' $draftsEmpty4 "退出后 drafts 条目数=$(@(Get-EntryUids).Count)（期望 0）"

    # 真实重启：轮询会话对齐重写完成（size 由 3 收敛为 2 且 uid 条目消失）
    $run2 = Start-Tracked $ExePath $BuildDir
    $deadline = [System.Diagnostics.Stopwatch]::StartNew()
    $aligned = $false
    while ($deadline.ElapsedMilliseconds -lt 10000) {
        if ($run2.Proc.HasExited) { break }
        $m = Get-DocumentsIniMap
        if (($m['size'] -eq '2') -and (-not $m.ContainsKey('2\uid'))) {
            $aligned = $true
            break
        }
        Start-Sleep -Milliseconds 50
    }
    Add-Result '验收4 重启进程存活（真实 qcnote.exe）' (-not $run2.Proc.HasExited) `
        $(if (-not $run2.Proc.HasExited) { "会话对齐（size 3→2）完成=$aligned（$($deadline.ElapsedMilliseconds)ms）" } else { '启动后即退出' })
    Add-Result '验收4 重启无 QML 错误' (-not (Test-LogHasQmlError $run2.Out $run2.Err)) 'stdout/stderr 无 QML 错误特征'
    Stop-Tracked $run2.Proc

    $ini4 = Get-DocumentsIniMap
    $restored4Ok = ($ini4['size'] -eq '2') -and ($ini4['1\url'] -eq $urlA) -and ($ini4['2\url'] -eq $urlB) `
        -and ($ini4['active'] -eq $urlB) -and (-not $ini4.ContainsKey('2\uid')) -and (-not $ini4.ContainsKey('activeUid'))
    Add-Result '验收4 重启后空位消除（[A, B] 顺序不变，active=文件B）' $restored4Ok `
        "size=$($ini4['size'])（期望 2） 1\url=A=$($ini4['1\url'] -eq $urlA) 2\url=B=$($ini4['2\url'] -eq $urlB) active=B url=$($ini4['active'] -eq $urlB)"
    Add-Result '验收4 重启后无草稿残留' (@(Get-EntryUids).Count -eq 0) "drafts 条目数=$(@(Get-EntryUids).Count)"
    Remove-Item Env:\QCNOTE_E2E_FILE_A -ErrorAction SilentlyContinue
    Remove-Item Env:\QCNOTE_E2E_FILE_B -ErrorAction SilentlyContinue

    # ==================== 验收标准 5：旧会话格式兼容（无 uid 键） ====================
    # 手写旧格式会话（旧版本不记录未命名条目：纯文件条目 + active URL）→
    # 真实 qcnote.exe 重启 → 断言恢复 [A, B]、active=B、无 uid 键参与、无错误
    Write-Host "== 验收标准 5：旧格式会话（纯文件条目，无 uid）→ 真实 qcnote.exe 重启 =="
    Reset-DraftState
    $legacyIni = "[documents]`n"
    $legacyIni += "size=2`n"
    $legacyIni += "1\url=$urlA`n"
    $legacyIni += "1\cursor=0`n"
    $legacyIni += "2\url=$urlB`n"
    $legacyIni += "2\cursor=0`n"
    $legacyIni += "active=$urlB`n"
    Write-Utf8NoBom $IniPath $legacyIni

    $run2 = Start-Tracked $ExePath $BuildDir
    # 轮询恢复路径完成信号：恢复末尾对齐写盘补全 [editor] 节（wordWrap 键）
    $deadline = [System.Diagnostics.Stopwatch]::StartNew()
    $rewritten = $false
    while ($deadline.ElapsedMilliseconds -lt 10000) {
        if ($run2.Proc.HasExited) { break }
        if ((Test-Path -LiteralPath $IniPath) -and
                ((Get-Content -LiteralPath $IniPath -Raw) -match 'wordWrap=')) {
            $rewritten = $true
            break
        }
        Start-Sleep -Milliseconds 50
    }
    Add-Result '验收5 重启进程存活（真实 qcnote.exe）' (-not $run2.Proc.HasExited) `
        $(if (-not $run2.Proc.HasExited) { "恢复对齐重写完成=$rewritten（$($deadline.ElapsedMilliseconds)ms）" } else { '启动后即退出' })
    Add-Result '验收5 重启无 QML 错误' (-not (Test-LogHasQmlError $run2.Out $run2.Err)) 'stdout/stderr 无 QML 错误特征'
    Stop-Tracked $run2.Proc

    $ini5 = Get-DocumentsIniMap
    $restored5Ok = ($ini5['size'] -eq '2') -and ($ini5['1\url'] -eq $urlA) -and ($ini5['2\url'] -eq $urlB) `
        -and ($ini5['active'] -eq $urlB) -and (-not $ini5.ContainsKey('activeUid')) -and (-not $ini5.ContainsKey('1\uid'))
    Add-Result '验收5 旧格式会话恢复（[A, B] active=B，无 uid 参与）' $restored5Ok `
        "size=$($ini5['size']) 1\url=A=$($ini5['1\url'] -eq $urlA) 2\url=B=$($ini5['2\url'] -eq $urlB) active=B url=$($ini5['active'] -eq $urlB)"
    Add-Result '验收5 drafts 无条目' (@(Get-EntryUids).Count -eq 0) "drafts 条目数=$(@(Get-EntryUids).Count)"
}
finally {
    # 后置清场：仅结束路径为被测 exe 的进程；清理便携标记/会话/缓存临时物
    Stop-ByExePath $ExePath   'qcnote'
    Stop-ByExePath $DriverExe 'e2e_draft_driver'
    Reset-DraftState
    if (Test-Path -LiteralPath $MarkerPath) { Remove-Item -LiteralPath $MarkerPath -Force }
    # 临时数据目录：默认删除（可重建产物）；-KeepTemp 时保留供排查
    if ($script:TempRoot -and (Test-Path -LiteralPath $script:TempRoot) -and -not $KeepTemp) {
        Remove-Item -LiteralPath $script:TempRoot -Recurse -Force
    }
    Remove-Item Env:\QCNOTE_E2E_CONTENT -ErrorAction SilentlyContinue
    Remove-Item Env:\QCNOTE_E2E_CONTENT2 -ErrorAction SilentlyContinue
    Remove-Item Env:\QCNOTE_E2E_FILE_A -ErrorAction SilentlyContinue
    Remove-Item Env:\QCNOTE_E2E_FILE_B -ErrorAction SilentlyContinue
    Remove-Item Env:\QCNOTE_TEST_SHELL_NOOP -ErrorAction SilentlyContinue
    if ($null -ne $script:OrigQpa) { $env:QT_QPA_PLATFORM = $script:OrigQpa } else { Remove-Item Env:\QT_QPA_PLATFORM -ErrorAction SilentlyContinue }
    [Console]::OutputEncoding = $origCodePage
    $OutputEncoding = $origCodePage
}

Write-Host ""
Write-Host "==== 汇总 ===="
$script:CaseResults | Format-Table Case, Pass, Detail -AutoSize | Out-String | Write-Host
$passCount = @($script:CaseResults | Where-Object { $_.Pass }).Count
Write-Host ("通过 {0}/{1}，失败 {2}" -f $passCount, $script:CaseResults.Count, $script:FailCount)
if ($script:FailCount -gt 0 -or $passCount -lt $script:CaseResults.Count) { exit 1 }
exit 0
