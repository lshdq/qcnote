# verify-draftcache.ps1 —— 「未保存修改临时缓存」QML 启动时序 offscreen 冒烟（持久化回归脚本）
# 对应用例：docs/测试用例/测试用例-未保存修改临时缓存.md TC-019
#   场景 A（静默恢复）：合法条目（url 指向磁盘现存文件且 baselineHash 与磁盘内容
#     相符）→ offscreen 启动 → 定向强杀：旧条目已被删除、且 drafts 下出现按新 uid
#     续写的唯一新条目（content=缓存修改内容、url 指向同一文件）——证明 Main.qml
#     启动时序在会话恢复后调用了 DraftCacheManager.restoreDrafts() 且静默应用成功
#     （无冲突弹窗；若未恢复则旧条目不会消失/不会有续写新条目）
#   场景 B（冲突弹窗）：条目 baselineHash 与磁盘内容不符（模拟外部修改）→
#     offscreen 启动：进程存活不崩溃、无 QML 错误输出、条目保留未删
#     （conflictsFound → draftConflictDialog 等待用户决策；对话框样式断言由
#     test_dialogstyle tc011 覆盖，恢复/丢弃行为由 test_core tst_draftcachemanager 覆盖）
#   场景 C（未命名恢复）：url 为空条目 → 强杀后旧条目删除、续写新条目 url 为空、
#     name 沿用「未命名-1」、content 即缓存内容（未命名重建链路）
# 验证方式：黑盒文件级断言（缓存条目 JSON 的产生/删除/字段），不注入、不改动生产代码。
# baselineHash 算法：与 Document::savedContentHash 同法——内容 LF 归一化后 UTF-8 字节
#   的 SHA-256 小写 hex。
# 前置：build\qcnote.exe 已构建；脚本清场只按可执行文件路径过滤结束被测
#   build\qcnote.exe 进程（Path 不可读的进程防御跳过），不波及本机其他路径
#   qcnote（如用户正在使用的真实编辑器）。注意：若本机其他 qcnote 进程与
#   被测实例同名用户会话，单实例转发仍会让新实例立即退出、无法判定被测
#   行为，此时需人工关闭后重跑。
# 缓存目录钉法：build\qcnote.portable 空标记（便携模式）→ 缓存 build\drafts\、
#   会话 build\qcnote.ini；脚本前置检查三者均不存在（防误删他人产物），finally 清理，
#   不留被测 qcnote 进程（按 exe 路径过滤结束）。
# 用法：pwsh.exe -NoProfile -ExecutionPolicy Bypass -File tests\verify-draftcache.ps1
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
$ExePath    = Join-Path $BuildDir "qcnote.exe"
$MarkerPath = Join-Path $BuildDir "qcnote.portable"   # 便携标记 → 缓存钉在 build
$IniPath    = Join-Path $BuildDir "qcnote.ini"        # 便携会话文件
$DraftsDir  = Join-Path $BuildDir "drafts"            # 便携缓存目录

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

# 字符串按 UTF-8 字节的 SHA-256 小写 hex（与 Document::savedContentHash 同法）
function Get-Sha256Hex([string]$text) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($text)
        (($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) -join '')
    } finally { $sha.Dispose() }
}

# 写 UTF-8 无 BOM 文本（QJsonDocument 对 BOM 会解析失败；正文一律 LF）
function Write-Utf8NoBom([string]$path, [string]$text) {
    [System.IO.File]::WriteAllText($path, $text, [System.Text.UTF8Encoding]::new($false))
}

# 写缓存条目 JSON（字段与 DraftCacheManager::writeEntry 一致；UTF-8 无 BOM）
function Write-EntryJson([string]$uid, [hashtable]$fields) {
    $fields['uid'] = $uid
    if (-not $fields.ContainsKey('version')) { $fields['version'] = 1 }
    $json = ConvertTo-Json ([pscustomobject]$fields) -Compress
    Write-Utf8NoBom (Join-Path $DraftsDir "$uid.json") $json
}

# 本地路径 → QUrl::fromLocalFile().toString() 形式（file:///D:/... 正斜杠）
function ConvertTo-FileUrl([string]$path) {
    "file:///" + ($path -replace '\\', '/')
}

# offscreen 启动 qcnote.exe，等待 $WaitSeconds 后返回进程与日志路径（不杀进程）
function Start-QcnoteOffscreen([int]$waitSeconds) {
    $script:RunSeq++
    $outLog = Join-Path $script:TempRoot ("stdout-{0}.log" -f $script:RunSeq)
    $errLog = Join-Path $script:TempRoot ("stderr-{0}.log" -f $script:RunSeq)
    $proc = Start-Process -FilePath $ExePath -WorkingDirectory $BuildDir `
        -RedirectStandardOutput $outLog -RedirectStandardError $errLog -PassThru
    Start-Sleep -Seconds $waitSeconds
    return @{ Proc = $proc; Out = $outLog; Err = $errLog }
}

# 结束本次启动的进程（定向，不波及他人；结束后等待句柄释放）
function Stop-Qcnote([System.Diagnostics.Process]$proc) {
    if ($proc -and -not $proc.HasExited) {
        $proc | Stop-Process -Force
        $proc.WaitForExit(5000) | Out-Null
    }
}

# 按可执行文件路径过滤结束被测 qcnote 进程（审查可选 #4）：只结束 Path 等于
# 被测 exe 的进程，不误杀本机其他路径运行的 qcnote（用户真实编辑器）。
# Path 属性可能为空/不可读（进程已退出、权限不足）——防御性跳过（宁漏勿误；
# 本脚本自己启动的进程持有 $run.Proc 引用，由 Stop-Qcnote 单独负责）。
function Stop-QcnoteByExePath {
    Get-Process qcnote -ErrorAction SilentlyContinue | Where-Object {
        $exe = $null
        try { $exe = $_.Path } catch { $exe = $null }
        $null -ne $exe -and $exe -eq $ExePath   # -eq 对路径大小写不敏感
    } | Stop-Process -Force
}

# stdout/stderr 是否含 QML 错误特征（QML 错误典型形如 file:///...: 行: Error: ...）
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

# drafts 目录下的条目文件名清单（不含 .json 后缀；目录不存在视为空）
function Get-EntryUids {
    if (-not (Test-Path -LiteralPath $DraftsDir)) { return @() }
    @(Get-ChildItem -LiteralPath $DraftsDir -Filter '*.json' -File |
        ForEach-Object { $_.BaseName })
}

# 场景间复位：清缓存与会话（保留便携标记），保证每场景首启无会话
function Reset-DraftState {
    if (Test-Path -LiteralPath $DraftsDir) { Remove-Item -LiteralPath $DraftsDir -Recurse -Force }
    if (Test-Path -LiteralPath $IniPath)   { Remove-Item -LiteralPath $IniPath -Force }
}

try {
    # ---- 前置检查 ----
    if (-not (Test-Path -LiteralPath $ExePath)) {
        throw "未找到 $ExePath，请先构建（cmake --build build）"
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
    $script:TempRoot = Join-Path $tempBase ("qcnote-verify-draftcache-{0}-{1}" -f $PID, (Get-Date -Format 'yyyyMMddHHmmss'))
    New-Item -ItemType Directory -Path $script:TempRoot -Force | Out-Null

    # 环境就绪：Qt 运行库 PATH + offscreen 平台（GUI 红线：无可见窗口）
    $env:PATH = "D:\Program\Qt\6.11.2\mingw_64\bin;D:\Program\Qt\Tools\mingw1310_64\bin;" + $env:PATH
    $env:QT_QPA_PLATFORM = 'offscreen'
    # 前置清场：仅结束路径为被测 exe 的 qcnote 进程（单实例转发会让被测实例立即退出；
    # 不波及本机其他路径的 qcnote 进程，Path 不可读的进程跳过）
    Stop-QcnoteByExePath

    # 便携标记：缓存/会话钉在 build 内
    Write-Utf8NoBom $MarkerPath ''
    Write-Host "== 场景 A：静默恢复（合法条目，磁盘未被外部修改） =="

    $fileA = Join-Path $script:TempRoot 'a-silent.txt'
    $diskContentA = "silent-base`nsecond line"                 # LF、无 BOM
    $dirtyContentA = "silent-base`nsecond line EDITED"         # 模拟未保存修改
    Write-Utf8NoBom $fileA $diskContentA

    New-Item -ItemType Directory -Path $DraftsDir -Force | Out-Null
    $uidA = 'aaaa0001-0000-0000-0000-000000000001'
    Write-EntryJson $uidA @{
        url          = ConvertTo-FileUrl $fileA
        name         = 'a-silent.txt'
        content      = $dirtyContentA
        cursor       = 5
        lineEnding   = 'lf'
        encoding     = 'utf8'
        hasBom       = $false
        baselineHash = Get-Sha256Hex $diskContentA            # 与磁盘相符 → 静默恢复
        savedAt      = 1
    }

    $run = Start-QcnoteOffscreen 4    # 启动 + restoreDrafts + 1s 防抖续写 + 余量
    $alive = -not $run.Proc.HasExited
    Add-Result 'A1 进程存活' $alive $(if ($alive) { 'offscreen 启动 4s 未退出' } else { '启动后即退出（检查单实例/启动错误）' })
    Add-Result 'A2 无 QML 错误输出' (-not (Test-LogHasQmlError $run.Out $run.Err)) 'stdout/stderr 无 QML 错误特征'
    Stop-Qcnote $run.Proc

    $uidsAfterA = @(Get-EntryUids)
    $oldGone = -not (Test-Path -LiteralPath (Join-Path $DraftsDir "$uidA.json"))
    Add-Result 'A3 旧条目已删除' $oldGone $(if ($oldGone) { '静默恢复成功后删除原条目' } else { "条目 $uidA.json 仍在（未被静默应用）" })

    $uidCountOk = ($uidsAfterA.Count -eq 1) -and ($uidsAfterA[0] -ne $uidA)
    Add-Result 'A4 按新 uid 续写唯一条目' $uidCountOk "恢复后条目数=$($uidsAfterA.Count)，新 uid=$($uidsAfterA -join ',')"
    if ($uidCountOk) {
        $entryA = Get-Content -LiteralPath (Join-Path $DraftsDir "$($uidsAfterA[0]).json") -Raw | ConvertFrom-Json
        $contentOk = ($entryA.content -eq $dirtyContentA)
        $urlOk = ($entryA.url -eq (ConvertTo-FileUrl $fileA))
        $nameOk = ($entryA.name -eq 'a-silent.txt')
        Add-Result 'A5 续写条目内容/url/name 正确' ($contentOk -and $urlOk -and $nameOk) `
            ("content 匹配=$contentOk url 匹配=$urlOk name 匹配=$nameOk（静默应用的未保存修改已再缓存）")
    } else {
        Add-Result 'A5 续写条目内容/url/name 正确' $false '无唯一新条目，无法校验字段'
    }

    Write-Host "== 场景 B：冲突弹窗（磁盘被外部修改，baselineHash 不符） =="
    Reset-DraftState

    $fileB = Join-Path $script:TempRoot 'b-conflict.txt'
    Write-Utf8NoBom $fileB 'v1'
    New-Item -ItemType Directory -Path $DraftsDir -Force | Out-Null
    $uidB = 'aaaa0002-0000-0000-0000-000000000002'
    $entryBPath = Join-Path $DraftsDir "$uidB.json"
    Write-EntryJson $uidB @{
        url          = ConvertTo-FileUrl $fileB
        name         = 'b-conflict.txt'
        content      = 'v1-dirty'
        cursor       = 2
        lineEnding   = 'lf'
        encoding     = 'utf8'
        hasBom       = $false
        baselineHash = Get-Sha256Hex 'deliberately-wrong'     # 与磁盘不符 → 冲突
        savedAt      = 1
    }

    $run = Start-QcnoteOffscreen 4
    $alive = -not $run.Proc.HasExited
    Add-Result 'B1 进程存活（冲突弹窗打开不崩溃）' $alive $(if ($alive) { 'conflictsFound → draftConflictDialog 打开后进程正常' } else { '启动后即退出' })
    Add-Result 'B2 无 QML 错误输出' (-not (Test-LogHasQmlError $run.Out $run.Err)) 'stdout/stderr 无 QML 错误特征'
    Stop-Qcnote $run.Proc

    $kept = Test-Path -LiteralPath $entryBPath
    Add-Result 'B3 冲突条目保留（等用户决策，未静默应用）' $kept `
        $(if ($kept) { '条目未被删除/未被静默覆盖，处于弹窗等待决策状态' } else { '条目消失：静默路径误处理冲突条目' })

    Write-Host "== 场景 C：未命名条目恢复（url 为空） =="
    Reset-DraftState

    New-Item -ItemType Directory -Path $DraftsDir -Force | Out-Null
    $uidC = 'aaaa0003-0000-0000-0000-000000000003'
    Write-EntryJson $uidC @{
        url          = ''
        name         = '未命名-1'
        content      = 'untitled-draft-content'
        cursor       = 3
        lineEnding   = 'lf'
        encoding     = 'utf8'
        hasBom       = $false
        baselineHash = Get-Sha256Hex ''
        savedAt      = 1
    }

    $run = Start-QcnoteOffscreen 4
    $alive = -not $run.Proc.HasExited
    Add-Result 'C1 进程存活' $alive $(if ($alive) { '未命名恢复后进程正常' } else { '启动后即退出' })
    Add-Result 'C2 无 QML 错误输出' (-not (Test-LogHasQmlError $run.Out $run.Err)) 'stdout/stderr 无 QML 错误特征'
    Stop-Qcnote $run.Proc

    $uidsAfterC = @(Get-EntryUids)
    $oldCGone = -not (Test-Path -LiteralPath (Join-Path $DraftsDir "$uidC.json"))
    Add-Result 'C3 旧条目已删除' $oldCGone $(if ($oldCGone) { '未命名重建后删除原条目' } else { "条目 $uidC.json 仍在" })

    $uidCountOk = ($uidsAfterC.Count -eq 1) -and ($uidsAfterC[0] -ne $uidC)
    Add-Result 'C4 续写唯一新条目' $uidCountOk "恢复后条目数=$($uidsAfterC.Count)，新 uid=$($uidsAfterC -join ',')"
    if ($uidCountOk) {
        $entryC = Get-Content -LiteralPath (Join-Path $DraftsDir "$($uidsAfterC[0]).json") -Raw | ConvertFrom-Json
        $urlEmpty = ($entryC.url -eq '')
        $nameOk = ($entryC.name -eq '未命名-1')
        $contentOk = ($entryC.content -eq 'untitled-draft-content')
        Add-Result 'C5 新条目 url 空/name 沿用/content 正确' ($urlEmpty -and $nameOk -and $contentOk) `
            ("url 空=$urlEmpty name 匹配=$nameOk content 匹配=$contentOk")
    } else {
        Add-Result 'C5 新条目 url 空/name 沿用/content 正确' $false '无唯一新条目，无法校验字段'
    }
}
finally {
    # 后置清场：仅结束路径为被测 exe 的 qcnote 进程（不波及本机其他路径的
    # qcnote 进程）；清理便携标记/会话/缓存临时物（脚本自己创建的）
    Stop-QcnoteByExePath
    Reset-DraftState
    if (Test-Path -LiteralPath $MarkerPath) { Remove-Item -LiteralPath $MarkerPath -Force }
    # 临时数据目录：默认删除（可重建产物）；-KeepTemp 时保留供排查
    if ($script:TempRoot -and (Test-Path -LiteralPath $script:TempRoot) -and -not $KeepTemp) {
        Remove-Item -LiteralPath $script:TempRoot -Recurse -Force
    }
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
