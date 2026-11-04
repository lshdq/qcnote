# verify-deploy-portable-boundary.ps1 —— 便携版部署脚本边界回归（持久化回归脚本）
# 对应审查修复：目标目录路径解析加固（卷根/裸盘符/空目标/文件目标/相对路径/嵌套目标）
#               + robocopy /R:2 /W:2 有界重试（目标文件被占用时快速失败而非挂死）
# 关联需求：.localdoc/需求文档/需求-便携版构建部署命令.md「边界与约束」「非功能需求（安全）」
#   TC-BND-01: -TargetDir 为卷根（D:\）→ exit 1 拒绝且不执行 robocopy，D:\ 根条目不变
#   TC-BND-02: -TargetDir 为裸盘符（D:）→ exit 1 拒绝且不执行 robocopy
#   TC-BND-03: -TargetDir 相对路径（cwd=项目根运行）→ 解析为项目根内绝对路径且部署成功；
#              含尾反斜杠形态（.\xxx\）验证归一化
#   TC-BND-04: -TargetDir 为 dist（产物父目录）→ exit 1 嵌套拒绝，dist 条目不变
#   TC-BND-05: -TargetDir 为产物目录内子路径 → exit 1 嵌套拒绝，子目录不创建，产物不变
#   TC-BND-06: 目标 qcnote.exe 被独占句柄占用 → robocopy 有界重试快速失败
#              （exit 1 且限时返回）；释放句柄后重部署可恢复
#   TC-BND-07: -TargetDir 为空字符串 → exit 1 拒绝（路径解析加固附赠项）
#   TC-BND-08: -TargetDir 指向已存在文件（非目录）→ exit 1 拒绝且文件内容不变（加固附赠项）
#   TC-BND-09: -TargetDir 尾随空格/点形态（'D: '、绝对路径尾空格、中段点结尾三形态）→
#              exit 1 拒绝且不执行 robocopy（DEC-005：robocopy/Win32 剥离尾空格/点与
#              PS 解析语义不一致，检测即拒绝；中段点结尾如 ...\dir.\x 段级 `dir.` 点形态）
# 不在本脚本范围：TC-101~TC-105（tests/verify-deploy-portable.ps1）、TC-106/TC-107（文档/端到端）
# 前置：dist 下存在 qcnote-*-win64-portable 产物（tools/build-portable.ps1 产出）。
# 用法：pwsh.exe -NoProfile -ExecutionPolicy Bypass -File tests\verify-deploy-portable-boundary.ps1
#       [-ProjectRoot <项目根>] [-KeepTemp]
# 退出码：0 = 通过；1 = 失败
# 安全：临时目录仅建在 D:\temp\aiRunner\opencode 下；相对路径目标仅建在项目根下、测试后删除；
#       不修改真实产物与 dist 现有内容；不触碰 D:\Program\qcnote。
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
$DeployScript = Join-Path $ProjectRoot "tools\deploy-portable.ps1"
$DistDir      = Join-Path $ProjectRoot "dist"

$script:FailCount   = 0
$script:CaseResults = @()   # 每条断言一条记录：Case / Pass / Detail
$script:TempRoot    = $null
$script:RelTargets  = @()   # TC-BND-03 在项目根下创建的相对路径目标（finally 删除）

function Add-Result([string]$case, [bool]$pass, [string]$detail, [string]$failDetail = '') {
    # 通过时用 $detail；失败时优先用第 4 实参的失败专用文案（为空则回退 $detail）
    $shownDetail = if ($pass -or -not $failDetail) { $detail } else { $failDetail }
    $script:CaseResults += [pscustomobject]@{ Case = $case; Pass = $pass; Detail = $shownDetail }
    $tag = if ($pass) { 'PASS' } else { 'FAIL' }
    Write-Host ("  [{0}] {1}：{2}" -f $tag, $case, $shownDetail)
    if (-not $pass) { $script:FailCount++ }
}

# 子进程执行部署脚本（脚本内 exit 会退出进程，必须子进程调用取 $LASTEXITCODE）。
# 返回 @{ ExitCode = int; Output = string }
function Invoke-Deploy([string]$portableDir, [string]$targetDir) {
    $deployArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $DeployScript)
    if ($portableDir) { $deployArgs += @('-PortableDir', $portableDir) }
    if ($targetDir)   { $deployArgs += @('-TargetDir', $targetDir) }
    $out = & pwsh.exe @deployArgs 2>&1 | ForEach-Object { $_.ToString() }
    return @{ ExitCode = $LASTEXITCODE; Output = ($out -join "`n") }
}

# 部署输出是否已到达 robocopy 步骤（Step 标题「robocopy 镜像部署」）——
# 拒绝类断言要求 false，证明失败发生在 robocopy 之前、未做任何拷贝
function Test-ReachedRobocopy([string]$output) { $output -match 'robocopy 镜像部署' }

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

$TempBase = 'D:\temp\aiRunner\opencode'
if (-not (Test-Path $TempBase)) {
    Write-Host "[错误] 临时目录基址不可用：$TempBase（不回退 C 盘，请先创建该目录）"; exit 1
}
$script:TempRoot = Join-Path $TempBase ("qcnote-deploy-boundary-" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $script:TempRoot -Force | Out-Null

Write-Host "==================== 便携版部署脚本边界回归 ===================="
Write-Host ("被测脚本 ：{0}" -f $DeployScript)
Write-Host ("真实产物 ：{0}（LastWriteTime {1:yyyy-MM-dd HH:mm:ss}）" -f $realProduct.FullName, $realProduct.LastWriteTime)
Write-Host ("临时目录 ：{0}" -f $script:TempRoot)

try {
    # ========== TC-BND-01: 卷根目标拒绝（D:\） ==========
    Write-Host "`n==> TC-BND-01 卷根目标拒绝（-TargetDir 'D:\'）"
    $driveRootBefore = @(Get-ChildItem 'D:\' -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.Name } | Sort-Object)
    $r = Invoke-Deploy $realProduct.FullName 'D:\'
    Add-Result 'TC-BND-01' ($r.ExitCode -eq 1) "卷根目标退出码 1（实际 $($r.ExitCode)）" "卷根目标应退出码 1 拒绝，实际 $($r.ExitCode)"
    Add-Result 'TC-BND-01' ($r.Output -match '拒绝部署到卷根') "输出含「拒绝部署到卷根」" "输出未含「拒绝部署到卷根」拒绝文案"
    Add-Result 'TC-BND-01' (-not (Test-ReachedRobocopy $r.Output)) "未执行 robocopy（输出无 robocopy 步骤）" "输出出现 robocopy 步骤——卷根未被前置拦截，危险！"
    $driveRootAfter = @(Get-ChildItem 'D:\' -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.Name } | Sort-Object)
    Add-Result 'TC-BND-01' (Test-ListEqual $driveRootBefore $driveRootAfter) "D:\ 根目录条目未变化（$($driveRootAfter.Count) 项）" "D:\ 根目录条目发生变化（部署前 $($driveRootBefore.Count) 项 vs 部署后 $($driveRootAfter.Count) 项）"

    # ========== TC-BND-02: 裸盘符目标拒绝（D:） ==========
    Write-Host "`n==> TC-BND-02 裸盘符目标拒绝（-TargetDir 'D:'）"
    $r = Invoke-Deploy $realProduct.FullName 'D:'
    Add-Result 'TC-BND-02' ($r.ExitCode -eq 1) "裸盘符目标退出码 1（实际 $($r.ExitCode)）" "裸盘符目标应退出码 1 拒绝，实际 $($r.ExitCode)"
    Add-Result 'TC-BND-02' ($r.Output -match '拒绝裸盘符目标目录') "输出含「拒绝裸盘符目标目录」" "输出未含「拒绝裸盘符目标目录」拒绝文案"
    Add-Result 'TC-BND-02' (-not (Test-ReachedRobocopy $r.Output)) "未执行 robocopy（输出无 robocopy 步骤）" "输出出现 robocopy 步骤——裸盘符未被前置拦截"

    # ========== TC-BND-03: 相对路径目标解析为项目根内绝对路径且部署成功 ==========
    Write-Host "`n==> TC-BND-03 相对路径目标解析（cwd=项目根）"
    Push-Location $ProjectRoot
    try {
        # a) 纯相对名
        $relA = 'rettest-rel-target-a'
        $absA = Join-Path $ProjectRoot $relA
        $script:RelTargets += $absA
        $r = Invoke-Deploy $realProduct.FullName $relA
        # 行级提取「目标目录：...」行（$ 不加 Multiline 不匹配多行字符串中间行尾，故按行比对）
        $targetLineA = @($r.Output -split "`n") | Where-Object { $_ -match '^\s*目标目录\s*：' } | Select-Object -First 1
        Add-Result 'TC-BND-03' ($r.ExitCode -eq 0) "相对路径部署退出码 0（实际 $($r.ExitCode)）" "相对路径部署应成功，实际退出码 $($r.ExitCode)"
        Add-Result 'TC-BND-03' ($targetLineA -and $targetLineA -match ('^\s*目标目录\s*：\s*' + [regex]::Escape($absA) + '\s*$')) "输出目标目录已解析为绝对路径：$absA" "输出目标目录行未解析为预期绝对路径 $absA（实际输出：$targetLineA）"
        Add-Result 'TC-BND-03' (Test-ListEqual (Get-FileRelList $absA) (Get-FileRelList $realProduct.FullName)) "目标清单与源产物一致" "目标清单与源产物不一致"
        # b) .\ 前缀 + 尾反斜杠形态（验证归一化后仍为无尾反斜杠绝对路径）
        $relB = '.\rettest-rel-target-b\'
        $absB = Join-Path $ProjectRoot 'rettest-rel-target-b'
        $script:RelTargets += $absB
        $r = Invoke-Deploy $realProduct.FullName $relB
        $targetLineB = @($r.Output -split "`n") | Where-Object { $_ -match '^\s*目标目录\s*：' } | Select-Object -First 1
        Add-Result 'TC-BND-03' ($r.ExitCode -eq 0) "「.\name\」形态部署退出码 0（实际 $($r.ExitCode)）" "「.\name\」形态部署应成功，实际退出码 $($r.ExitCode)"
        Add-Result 'TC-BND-03' ($targetLineB -and $targetLineB -match ('^\s*目标目录\s*：\s*' + [regex]::Escape($absB) + '\s*$')) "尾反斜杠已归一为：$absB" "尾反斜杠形态未归一为预期绝对路径 $absB（实际输出：$targetLineB）"
    } finally { Pop-Location }

    # ========== TC-BND-04: 目标 = dist（产物父目录）嵌套拒绝 ==========
    Write-Host "`n==> TC-BND-04 dist 嵌套拒绝（-TargetDir = dist）"
    $distBefore = @(Get-ChildItem $DistDir -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.Name } | Sort-Object)
    $r = Invoke-Deploy $realProduct.FullName $DistDir
    Add-Result 'TC-BND-04' ($r.ExitCode -eq 1) "dist 目标退出码 1（实际 $($r.ExitCode)）" "dist 目标应退出码 1 拒绝，实际 $($r.ExitCode)"
    Add-Result 'TC-BND-04' ($r.Output -match '不可互为祖先/后代') "输出含「不可互为祖先/后代」" "输出未含「不可互为祖先/后代」拒绝文案"
    Add-Result 'TC-BND-04' (-not (Test-ReachedRobocopy $r.Output)) "未执行 robocopy（输出无 robocopy 步骤）" "输出出现 robocopy 步骤——嵌套目标未被拦截"
    $distAfter = @(Get-ChildItem $DistDir -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.Name } | Sort-Object)
    Add-Result 'TC-BND-04' (Test-ListEqual $distBefore $distAfter) "dist 条目未变化（$($distAfter.Count) 项）" "dist 条目发生变化（前 $($distBefore.Count) 项 vs 后 $($distAfter.Count) 项）"

    # ========== TC-BND-05: 目标 = 产物目录内子路径 嵌套拒绝 ==========
    Write-Host "`n==> TC-BND-05 产物目录内子路径嵌套拒绝"
    $nested = Join-Path $realProduct.FullName 'nested-inside'
    $productListBefore = Get-FileRelList $realProduct.FullName
    $r = Invoke-Deploy $realProduct.FullName $nested
    Add-Result 'TC-BND-05' ($r.ExitCode -eq 1) "产物内子路径目标退出码 1（实际 $($r.ExitCode)）" "产物内子路径目标应退出码 1 拒绝，实际 $($r.ExitCode)"
    Add-Result 'TC-BND-05' ($r.Output -match '不可互为祖先/后代') "输出含「不可互为祖先/后代」" "输出未含「不可互为祖先/后代」拒绝文案"
    Add-Result 'TC-BND-05' (-not (Test-ReachedRobocopy $r.Output)) "未执行 robocopy（输出无 robocopy 步骤）" "输出出现 robocopy 步骤——嵌套目标未被拦截"
    Add-Result 'TC-BND-05' (-not (Test-Path -LiteralPath $nested)) "子路径未被创建（不触碰产物目录）" "子路径被创建——失败路径触碰了产物目录"
    Add-Result 'TC-BND-05' (Test-ListEqual $productListBefore (Get-FileRelList $realProduct.FullName)) "真实产物清单未变化（$($productListBefore.Count) 项）" "真实产物清单发生变化"

    # ========== TC-BND-06: 目标 exe 被独占占用 → robocopy 有界重试快速失败 ==========
    Write-Host "`n==> TC-BND-06 目标 qcnote.exe 独占占用 → robocopy /R:2 /W:2 有界重试快速失败"
    $t6   = Join-Path $script:TempRoot 'bnd06\target'
    $pre  = Invoke-Deploy $realProduct.FullName $t6
    $exeT = Join-Path $t6 'qcnote.exe'
    $lock = $null
    if ($pre.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $exeT)) {
        Add-Result 'TC-BND-06' $false "前置：临时目标部署成功" "前置部署失败（退出码 $($pre.ExitCode)），用例无法执行"
    } else {
        # 目标 exe 时间戳改旧 → 强制 robocopy 判定需覆盖拷贝（否则时间戳相同会跳过、占用场景不触发）
        (Get-Item -LiteralPath $exeT).LastWriteTime = (Get-Date).AddDays(-1)
        # 独占打开目标 exe（FileShare::None 模拟运行中/被占用），保持句柄执行部署
        $lock = [System.IO.File]::Open($exeT, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::None)
        try {
            $sw = [System.Diagnostics.Stopwatch]::StartNew()
            $r = Invoke-Deploy $realProduct.FullName $t6
            $sw.Stop()
            $elapsedSec = [math]::Round($sw.Elapsed.TotalSeconds, 1)
            Add-Result 'TC-BND-06' ($r.ExitCode -eq 1) "占用时部署退出码 1（实际 $($r.ExitCode)）" "占用时部署应失败退出码 1，实际 $($r.ExitCode)"
            Add-Result 'TC-BND-06' ($r.Output -match 'robocopy 镜像部署失败') "输出含「robocopy 镜像部署失败」" "输出未含「robocopy 镜像部署失败」（失败语义异常）"
            # /R:2 /W:2 期望单文件重试约 2×2s 后放弃；60s 上界区分有界重试与默认重试（100 万次×30s 近乎挂死）
            Add-Result 'TC-BND-06' ($sw.Elapsed.TotalSeconds -lt 60) "有界重试限时返回（实际耗时 $elapsedSec s < 60 s 上界）" "部署耗时 $elapsedSec s ≥ 60 s——疑似未生效有界重试（/R:2 /W:2）"
        } finally {
            if ($lock) { $lock.Close() }
        }
        # 释放占用后重部署可恢复
        $r2 = Invoke-Deploy $realProduct.FullName $t6
        Add-Result 'TC-BND-06' ($r2.ExitCode -eq 0) "释放占用后重部署退出码 0（可恢复）" "释放占用后重部署仍失败（退出码 $($r2.ExitCode)）"
        Add-Result 'TC-BND-06' ((Get-FileHash -LiteralPath $exeT -Algorithm SHA256).Hash -eq (Get-FileHash -LiteralPath (Join-Path $realProduct.FullName 'qcnote.exe') -Algorithm SHA256).Hash) "目标 exe 已恢复为源产物版本（SHA256 一致）" "目标 exe 与源产物 SHA256 不一致"
    }

    # ========== TC-BND-07: 空目标目录拒绝（加固附赠项） ==========
    Write-Host "`n==> TC-BND-07 空目标目录拒绝（-TargetDir ''）"
    # 空串经 -Command 内部调用传递（避免外层命令行空串参数被吞导致回退默认值、误部署真实目标）
    $cmd = "& '$DeployScript' -PortableDir '$($realProduct.FullName)' -TargetDir ''"
    $out = & pwsh.exe -NoProfile -ExecutionPolicy Bypass -Command $cmd 2>&1 | ForEach-Object { $_.ToString() }
    $code = $LASTEXITCODE
    $outText = $out -join "`n"
    Add-Result 'TC-BND-07' ($code -eq 1) "空目标退出码 1（实际 $code）" "空目标应退出码 1 拒绝，实际 $code"
    Add-Result 'TC-BND-07' ($outText -match '目标目录不能为空') "输出含「目标目录不能为空」" "输出未含「目标目录不能为空」拒绝文案"
    Add-Result 'TC-BND-07' (-not (Test-ReachedRobocopy $outText)) "未执行 robocopy（输出无 robocopy 步骤）" "输出出现 robocopy 步骤——空目标未被拦截"

    # ========== TC-BND-08: 文件目标拒绝（加固附赠项） ==========
    Write-Host "`n==> TC-BND-08 文件目标拒绝（-TargetDir 指向已存在文件）"
    $t8dir  = Join-Path $script:TempRoot 'bnd08'
    New-Item -ItemType Directory -Path $t8dir -Force | Out-Null
    $t8file = Join-Path $t8dir 'target-is-file.txt'
    Set-Content -LiteralPath $t8file -Value 'bnd08-target-is-file-content-must-stay' -Encoding ascii
    $t8hashBefore = (Get-FileHash -LiteralPath $t8file -Algorithm SHA256).Hash
    $r = Invoke-Deploy $realProduct.FullName $t8file
    Add-Result 'TC-BND-08' ($r.ExitCode -eq 1) "文件目标退出码 1（实际 $($r.ExitCode)）" "文件目标应退出码 1 拒绝，实际 $($r.ExitCode)"
    Add-Result 'TC-BND-08' ($r.Output -match '目标路径已存在且不是目录') "输出含「目标路径已存在且不是目录」" "输出未含「目标路径已存在且不是目录」拒绝文案"
    Add-Result 'TC-BND-08' (-not (Test-ReachedRobocopy $r.Output)) "未执行 robocopy（输出无 robocopy 步骤）" "输出出现 robocopy 步骤——文件目标未被拦截"
    Add-Result 'TC-BND-08' ((Get-FileHash -LiteralPath $t8file -Algorithm SHA256).Hash -eq $t8hashBefore) "目标文件内容不变（SHA256 一致）" "目标文件内容被改动（SHA256 不一致）"

    # ========== TC-BND-09: 尾空格目标拒绝（DEC-005 尾随空格/点防御） ==========
    Write-Host "`n==> TC-BND-09 尾空格/点目标拒绝（'D: '、绝对路径尾空格、中段点结尾三形态）"
    $t9dir = Join-Path $script:TempRoot 'bnd09'
    New-Item -ItemType Directory -Path $t9dir -Force | Out-Null
    # a) 裸盘符 + 尾空格 'D: '：PS 解析保留尾空格（cwd 下追加空格段），而 robocopy/Win32
    #    剥离尾空格后实际作用于剥离目录（cwd 场景即灾难）——必须前置拒绝。
    #    含尾空格参数经 -Command 内嵌传递（同 TC-BND-07 做法，确保尾空格不被外层命令行
    #    解析吞掉）；调用前切到临时目录 cwd —— 纵使防御失效，剥离后的 /MIR 目标也只落在
    #    临时目录内（脚本自身防御性兜底，不波及项目根）
    Push-Location $t9dir
    try {
        $cmdA = "& '$DeployScript' -PortableDir '$($realProduct.FullName)' -TargetDir 'D: '"
        $outA = & pwsh.exe -NoProfile -ExecutionPolicy Bypass -Command $cmdA 2>&1 | ForEach-Object { $_.ToString() }
        $codeA = $LASTEXITCODE
        $textA = $outA -join "`n"
    } finally { Pop-Location }
    Add-Result 'TC-BND-09' ($codeA -eq 1) "「D: 」目标退出码 1（实际 $codeA）" "「D: 」目标应退出码 1 拒绝，实际 $codeA"
    Add-Result 'TC-BND-09' ($textA -match '尾随空格/点') "「D: 」输出含尾随空格/点拒绝文案" "「D: 」输出未含尾随空格/点拒绝文案"
    Add-Result 'TC-BND-09' (-not (Test-ReachedRobocopy $textA)) "「D: 」未执行 robocopy" "「D: 」输出出现 robocopy 步骤——尾空格目标未被拦截"
    # b) 临时绝对路径 + 尾空格（目录名 dst 带尾空格）
    $t9b = Join-Path $t9dir 'dst '
    $cmdB = "& '$DeployScript' -PortableDir '$($realProduct.FullName)' -TargetDir '$t9b'"
    $outB = & pwsh.exe -NoProfile -ExecutionPolicy Bypass -Command $cmdB 2>&1 | ForEach-Object { $_.ToString() }
    $codeB = $LASTEXITCODE
    $textB = $outB -join "`n"
    Add-Result 'TC-BND-09' ($codeB -eq 1) "绝对路径尾空格目标退出码 1（实际 $codeB）" "绝对路径尾空格目标应退出码 1 拒绝，实际 $codeB"
    Add-Result 'TC-BND-09' ($textB -match '尾随空格/点') "绝对路径尾空格输出含尾随空格/点拒绝文案" "绝对路径尾空格输出未含尾随空格/点拒绝文案"
    Add-Result 'TC-BND-09' (-not (Test-ReachedRobocopy $textB)) "绝对路径尾空格未执行 robocopy" "绝对路径尾空格输出出现 robocopy 步骤——尾空格目标未被拦截"
    # c) 中段点结尾（临时目录内 ...\dir.\x）：PS 的 GetUnresolvedProviderPathFromPSPath 只解析
    #    消除 `.`/`..` 段，段级 `dir.` 以点结尾会被保留——正是要测的段级 `[\s\.]$` 点形态分支
    $t9c = Join-Path (Join-Path $t9dir 'dir.') 'x'
    $cmdC = "& '$DeployScript' -PortableDir '$($realProduct.FullName)' -TargetDir '$t9c'"
    $outC = & pwsh.exe -NoProfile -ExecutionPolicy Bypass -Command $cmdC 2>&1 | ForEach-Object { $_.ToString() }
    $codeC = $LASTEXITCODE
    $textC = $outC -join "`n"
    Add-Result 'TC-BND-09' ($codeC -eq 1) "中段点结尾目标退出码 1（实际 $codeC）" "中段点结尾目标（$t9c）应退出码 1 拒绝，实际 $codeC"
    Add-Result 'TC-BND-09' ($textC -match '尾随空格/点') "中段点结尾输出含尾随空格/点拒绝文案" "中段点结尾输出未含尾随空格/点拒绝文案"
    Add-Result 'TC-BND-09' (-not (Test-ReachedRobocopy $textC)) "中段点结尾未执行 robocopy" "中段点结尾输出出现 robocopy 步骤——段级点形态未被拦截"
} finally {
    # 删除项目根下相对路径目标（恢复项目根原状）
    foreach ($rel in $script:RelTargets) {
        if ($rel -and (Test-Path -LiteralPath $rel)) {
            Remove-Item -LiteralPath $rel -Recurse -Force -ErrorAction SilentlyContinue
            if (Test-Path -LiteralPath $rel) {
                Write-Host ("[错误] 项目根下测试目标删除失败，残留：{0}（请手动删除）" -f $rel) -ForegroundColor Red
                $script:FailCount++
            }
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
    $bad = @($g.Group | Where-Object { -not $_.Pass })
    if ($bad.Count -gt 0) {
        $failedCases += $g.Name
        Write-Host ("{0}：FAIL（{1} 条断言未过）" -f $g.Name, $bad.Count) -ForegroundColor Red
        foreach ($b in $bad) { Write-Host ("    - {0}" -f $b.Detail) -ForegroundColor Red }
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
