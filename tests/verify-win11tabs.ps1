# verify-win11tabs.ps1 —— 「标签页Win11风格改造」验证编排（持久化回归脚本）
# 对应用例：docs/.localdoc/测试用例/测试用例-标签页Win11风格改造.md
#   TC-002/003 活动标签连通式视觉（浅/深）—— test_win11tabs 结构断言 +
#     e2e_tabshot_driver 截图取证（肉眼审阅连通观感）
#   TC-004 非活动透明与悬停高亮 —— test_win11tabs（合成悬停真实事件路径）
#     + 截图（light/dark-multi-hover）
#   TC-005 关闭按钮移除 —— test_win11tabs（无 Canvas/无多余 Text）+ 截图核对
#   TC-008 宽度自适应与收缩 —— test_win11tabs（公式/收缩/饱和）+ 截图
#     （light-shrink-1000 为 6 长名标签未饱和等比收缩；
#     light-overflow-800 / dark-overflow-800 为 8 标签饱和+溢出态）
#   TC-010 焦点环 —— test_win11tabs（radius 4 + 可见性）+ light-focusring 截图
#   DEC-002 深色悬停可辨性 —— dark-multi-hover 截图（肉眼/主流程判断）
#   DEC-004 菜单按钮与贴底标签错位观感 —— light-single / dark-single 截图
#     （供主流程判断，非脚本自动判定）
# 关联：TC-001 构建回归与 TC-006 右键菜单入口由全量 ctest（含
#   test_tabcontextmenu）承担，本脚本聚焦新增目标与截图。
# 验证方式：ctest（test_win11tabs 结构断言）+ e2e_tabshot_driver 截图
#   产物存在性/尺寸校验（观感结论以截图人工审阅为准），不改动生产代码。
# 前置：build 已配置（首次运行会执行 cmake 配置）；Qt 工具链 PATH 由脚本注入。
# 用法：pwsh.exe -NoProfile -ExecutionPolicy Bypass -File tests\verify-win11tabs.ps1
#       [-ProjectRoot <项目根>] [-ShotDir <截图输出目录>] [-SkipBuild]
# 退出码：0 = 通过；1 = 失败
# 安全：GUI 红线——一律 QT_QPA_PLATFORM=offscreen 启动，全程无可见窗口。
#Requires -Version 7

param(
    [string]$ProjectRoot = "",
    [string]$ShotDir = "",
    [switch]$SkipBuild
)

$origCodePage = [Console]::OutputEncoding
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null

if (-not $ProjectRoot) { $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path }
$BuildDir  = Join-Path $ProjectRoot "build"
$DriverExe = Join-Path $BuildDir "e2e_tabshot_driver.exe"
if (-not $ShotDir) {
    $ShotDir = Join-Path $ProjectRoot ".localdoc\测试附件\标签页Win11风格改造-20261110131553"
}

$script:FailCount   = 0
$script:CaseResults = @()
$script:OrigQpa     = $env:QT_QPA_PLATFORM
$tempRoot           = $null

function Add-Result([string]$case, [bool]$pass, [string]$detail) {
    $script:CaseResults += [pscustomobject]@{ Case = $case; Pass = $pass; Detail = $detail }
    $tag = if ($pass) { 'PASS' } else { 'FAIL' }
    Write-Host ("  [{0}] {1}：{2}" -f $tag, $case, $detail)
    if (-not $pass) { $script:FailCount++ }
}

# 结束路径为被测 exe 的进程（不波及本机其他路径同名进程）
function Stop-ByExePath([string]$exePath, [string]$procName) {
    Get-Process $procName -ErrorAction SilentlyContinue | Where-Object {
        $exe = $null
        try { $exe = $_.Path } catch { $exe = $null }
        $null -ne $exe -and $exe -ieq $exePath
    } | Stop-Process -Force -ErrorAction SilentlyContinue
}

try {
    # Qt/CMake 工具链 PATH（构建与 ctest 均需要，置于 SkipBuild 分支之外）
    $env:PATH = "D:\Program\Qt\Tools\CMake_64\bin;D:\Program\Qt\Tools\Ninja;" + $env:PATH

    # ---- 构建（新增测试目标与 harness）----
    if (-not $SkipBuild) {
        Write-Host "== 构建 test_win11tabs + e2e_tabshot_driver =="
        Push-Location $ProjectRoot
        try {
            cmake -B build -G Ninja `
                -DCMAKE_PREFIX_PATH=D:/Program/Qt/6.11.2/mingw_64 `
                -DCMAKE_C_COMPILER=D:/Program/Qt/Tools/mingw1310_64/bin/gcc.exe `
                -DCMAKE_CXX_COMPILER=D:/Program/Qt/Tools/mingw1310_64/bin/g++.exe
            if ($LASTEXITCODE -ne 0) { throw "cmake 配置失败（exit=$LASTEXITCODE）" }
            cmake --build build --target test_win11tabs e2e_tabshot_driver
            if ($LASTEXITCODE -ne 0) { throw "构建失败（exit=$LASTEXITCODE）" }
        } finally { Pop-Location }
        Add-Result '构建（test_win11tabs + e2e_tabshot_driver）' $true '0 error'
    } else {
        Add-Result '构建（跳过 -SkipBuild）' $true '按参数跳过'
    }

    # ---- ctest：新增结构断言测试 ----
    Write-Host "== ctest：test_win11tabs =="
    Push-Location $ProjectRoot
    try {
        $ctestOut = ctest --test-dir build -R "^test_win11tabs$" --output-on-failure 2>&1
        $ctestCode = $LASTEXITCODE
    } finally { Pop-Location }
    $ctestText = ($ctestOut | Out-String)
    Write-Host $ctestText
    Add-Result 'ctest test_win11tabs（结构断言）' ($ctestCode -eq 0) "exit=$ctestCode"

    # ---- 截图取证 ----
    Write-Host "== 截图取证：$ShotDir =="
    if (-not (Test-Path -LiteralPath $DriverExe)) {
        throw "未找到 $DriverExe，请先构建（去掉 -SkipBuild）"
    }
    New-Item -ItemType Directory -Path $ShotDir -Force | Out-Null

    $env:PATH = "D:\Program\Qt\6.11.2\mingw_64\bin;D:\Program\Qt\Tools\mingw1310_64\bin;" + $env:PATH
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:QCNOTE_TEST_SHELL_NOOP = '1'
    Stop-ByExePath $DriverExe 'e2e_tabshot_driver'

    # 临时日志放任务临时区（不写 C 盘）
    $tempBase = 'D:\temp\aiRunner\opencode'
    if (-not (Test-Path -LiteralPath $tempBase)) { throw "临时目录基址 $tempBase 不可用，按约定不回退 C 盘，中止" }
    $tempRoot = Join-Path $tempBase ("qcnote-verify-win11tabs-{0}" -f (Get-Date -Format 'yyyyMMddHHmmss'))
    New-Item -ItemType Directory -Path $tempRoot -Force | Out-Null
    $shotLog = Join-Path $tempRoot 'tabshot.log'
    $proc = Start-Process -FilePath $DriverExe -WorkingDirectory $BuildDir `
        -ArgumentList @($ShotDir) -RedirectStandardOutput $shotLog `
        -RedirectStandardError "$shotLog.err" -PassThru
    $proc.WaitForExit(120000) | Out-Null
    if (-not $proc.HasExited) {
        Stop-ByExePath $DriverExe 'e2e_tabshot_driver'
        Add-Result '截图 harness 运行' $false '120s 未退出（超时强杀）'
    } else {
        $logText = if (Test-Path -LiteralPath $shotLog) { Get-Content -LiteralPath $shotLog -Raw } else { '' }
        Write-Host $logText
        Add-Result '截图 harness 运行' ($proc.ExitCode -eq 0 -and $logText -match '\[shot\] ALL PASS') `
            "exit=$($proc.ExitCode)"
    }

    # 产物校验：预期 PNG 清单存在且非空（>2KB；标题栏裁剪图纯色占比高，
    # 2.6KB+ 属正常，阈值以排除空白帧为准）
    $expected = @(
        'light-single', 'light-single-titlebar',
        'light-multi-hover', 'light-multi-hover-titlebar',
        'light-shrink-1000', 'light-shrink-1000-titlebar',
        'light-overflow-800', 'light-overflow-800-titlebar',
        'light-focusring', 'light-focusring-titlebar',
        'dark-single', 'dark-single-titlebar',
        'dark-multi-hover', 'dark-multi-hover-titlebar',
        'dark-overflow-800', 'dark-overflow-800-titlebar'
    )
    $missing = @()
    foreach ($name in $expected) {
        $p = Join-Path $ShotDir "$name.png"
        $ok = (Test-Path -LiteralPath $p) -and ((Get-Item -LiteralPath $p).Length -gt 2KB)
        if (-not $ok) { $missing += $name }
    }
    Add-Result '截图产物齐全（16 张 PNG，浅/深 × 单标签/悬停/收缩/溢出/焦点环）' `
        ($missing.Count -eq 0) `
        $(if ($missing.Count -eq 0) { "目录：$ShotDir" } else { "缺失/过小：$($missing -join ', ')" })
}
finally {
    Stop-ByExePath $DriverExe 'e2e_tabshot_driver'
    if ($tempRoot -and (Test-Path -LiteralPath $tempRoot)) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
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
