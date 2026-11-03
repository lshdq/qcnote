# verify-explorer-menu.ps1 —— 资源管理器右键菜单：跨进程单实例转发集成验证（持久化回归脚本）
# 对应用例：.localdoc/测试用例/测试用例-资源管理器右键菜单.md
#   TC-007（参数启动叠加打开 / 同 URL 仅激活不重复）、TC-008（单实例转发：
#   从实例退出码 0、主实例叠加打开且存活）、TC-014（多文件参数 + 连续两次
#   快速转发按序叠加、无遗漏）
# 说明：TC-001~TC-006/009/010/013/015 由 tests/test_core（ctest）覆盖；
#       TC-001/002/004/012 的真实资源管理器右键显示与点击为人工验证项；
#       TC-011 的 uiLocked 集成由代码核查 + test_window 回归覆盖，均不在本脚本范围。
# 隔离红线（与项目 AGENTS.md/test-workflow 约束一致）：
#   1. 全程 QT_QPA_PLATFORM=offscreen，不产生可见窗口；
#   2. QCNOTE_TEST_SHELL_NOOP=1：不弹首启提示、绝不写真实注册表
#      （脚本前后核验 HKCU\Software\qcnote\qcnote 与 Classes 菜单键不变）；
#   3. exe 复制到 D:\temp\aiRunner\opencode 下唯一临时目录 + qcnote.portable
#      便携标记（会话 qcnote.ini 落在临时目录，不碰用户 APPDATA）；
#      独立随机 USERNAME 隔离命名管道（qcnote-<USERNAME>），绝不触碰用户
#      运行中实例（进程清理仅按本脚本临时目录路径过滤，前后守护对照）。
# 用法：
#   pwsh.exe -NoProfile -ExecutionPolicy Bypass -File tests\verify-explorer-menu.ps1 [-ProjectRoot <项目根>] [-ExePath <qcnote.exe>] [-KeepTemp]
# 退出码：0 = 通过；1 = 失败
#Requires -Version 7

param(
    [string]$ProjectRoot = "",
    [string]$ExePath = "",
    [switch]$KeepTemp
)

$origCodePage = [Console]::OutputEncoding
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null

if (-not $ProjectRoot) { $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path }
if (-not $ExePath) { $ExePath = Join-Path $ProjectRoot "build\qcnote.exe" }

# 环境变量原值保存：所有退出路径经 Restore-ScriptEnv 统一恢复
$script:OrigQpaPlatform = $env:QT_QPA_PLATFORM
$script:OrigPath = $env:PATH
$script:OrigUsername = $env:USERNAME
$script:OrigShellNoop = $env:QCNOTE_TEST_SHELL_NOOP

function Restore-ScriptEnv {
    if ($null -ne $script:OrigQpaPlatform) { $env:QT_QPA_PLATFORM = $script:OrigQpaPlatform }
    else { Remove-Item Env:\QT_QPA_PLATFORM -ErrorAction SilentlyContinue }
    if ($null -ne $script:OrigPath) { $env:PATH = $script:OrigPath }
    else { Remove-Item Env:\PATH -ErrorAction SilentlyContinue }
    if ($null -ne $script:OrigUsername) { $env:USERNAME = $script:OrigUsername }
    else { Remove-Item Env:\USERNAME -ErrorAction SilentlyContinue }
    if ($null -ne $script:OrigShellNoop) { $env:QCNOTE_TEST_SHELL_NOOP = $script:OrigShellNoop }
    else { Remove-Item Env:\QCNOTE_TEST_SHELL_NOOP -ErrorAction SilentlyContinue }
    [Console]::OutputEncoding = $script:OrigCodePage
}

if (-not (Test-Path -LiteralPath $ExePath)) {
    Write-Output "[FAIL] 前置：qcnote.exe 不存在: $ExePath（先执行完整构建）"
    Restore-ScriptEnv
    exit 1
}

# 本地路径 → Document 落盘的 file URL 形态（与 qcnote.ini 中 documents/<n>/url
# 的实际存储形态精确比对）。实测（2026-11-03，Qt 6.11.2）DocumentManager 打开
# 后 doc->url().toString() 输出为“裸字符”形态：仅反斜杠→正斜杠，空格、中文
# 原样保留（QUrl::fromLocalFile 语义），不做 %20/%XX 百分号编码
function ConvertTo-FileUrl([string]$path) {
    'file:///' + [System.IO.Path]::GetFullPath($path).Replace('\', '/')
}

# 读取便携会话 ini 中已打开文档的 url 列表（1\url、2\url、… 顺序）与 active
function Read-SessionUrls([string]$iniPath) {
    if (-not (Test-Path -LiteralPath $iniPath)) { return @{ urls = @(); active = "" } }
    $urls = @()
    $active = ""
    foreach ($line in Get-Content -LiteralPath $iniPath -Encoding UTF8) {
        if ($line -match '^\d+\\url=(.*)$') { $urls += $Matches[1] }
        elseif ($line -match '^active=(.*)$') { $active = $Matches[1] }
    }
    return @{ urls = $urls; active = $active }
}

# 轮询等待会话 ini 达到预期状态（文档 url 列表全等 + active 匹配）。
# 主实例写盘经 500ms 防抖，固定 sleep 存在时序竞争；轮询消除 flaky
function Wait-SessionState([string]$iniPath, [string[]]$expectedUrls, [string]$expectedActive, [int]$timeoutSec = 15) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $s = Read-SessionUrls $iniPath
        $urlsOk = ($s.urls.Count -eq $expectedUrls.Count)
        if ($urlsOk) {
            for ($i = 0; $i -lt $expectedUrls.Count; $i++) {
                if ($s.urls[$i] -ne $expectedUrls[$i]) { $urlsOk = $false; break }
            }
        }
        if ($urlsOk -and $s.active -eq $expectedActive) { return $s }
        Start-Sleep -Milliseconds 500
    }
    return Read-SessionUrls $iniPath
}

$failCount = 0
$results = @()
function Add-Result([string]$id, [string]$name, [bool]$pass, [string]$note = "") {
    $script:results += [pscustomobject]@{ Id = $id; Name = $name; Result = $(if ($pass) { "PASS" } else { "FAIL" }); Note = $note }
    if (-not $pass) { $script:failCount++ }
    Write-Output ("[{0}] {1} {2} {3}" -f $(if ($pass) { "PASS" } else { "FAIL" }), $id, $name, $note)
}

# —— 注册表守护（约束 2：绝不写真实注册表，前后对照取证）——
# 键存在性经 reg.exe 退出码判定（PowerShell Registry provider 会把路径中
# 的字面量 “*” 当通配符展开，reg.exe 无此歧义）
$markerKey = "HKCU\Software\qcnote\qcnote"
$menuKey = "HKCU\Software\Classes\*\shell\qcnote"
$markerQuery = reg query $markerKey 2>&1 | Out-String
$markerBefore = ($LASTEXITCODE -eq 0) -and ($markerQuery -match 'contextMenuPrompted')
reg query $menuKey *> $null
$menuBefore = ($LASTEXITCODE -eq 0)

# —— 用户实例守护（约束 3：绝不触碰用户运行中实例）——
$procBefore = @(Get-Process qcnote -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Path)

# —— 临时目录：D:\temp\aiRunner\opencode 下按本次运行唯一命名 ——
$runId = "menu-it-{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss"), ([guid]::NewGuid().ToString('N').Substring(0, 8))
$tempRoot = "D:\temp\aiRunner\opencode"
$dir = Join-Path $tempRoot "qcnote-$runId"
New-Item -ItemType Directory -Force -Path $dir | Out-Null

$mainProc = $null
try {
    $env:QT_QPA_PLATFORM = "offscreen"
    $env:QCNOTE_TEST_SHELL_NOOP = "1"
    $env:PATH = "D:\Program\Qt\6.11.2\mingw_64\bin;D:\Program\Qt\Tools\mingw1310_64\bin;" + $env:PATH
    # 独立随机用户名：命名管道 qcnote-<USERNAME> 与真实用户实例（默认名）隔离
    $env:USERNAME = "qcnit-" + $runId

    # 复制 exe + 便携标记（会话/配置均落临时目录，不碰用户 APPDATA）
    Copy-Item $ExePath (Join-Path $dir "qcnote.exe") -Force
    Set-Content -Path (Join-Path $dir "qcnote.portable") -Value "" -Encoding ascii

    # 测试文件：含空格与中文路径（跨进程 IPC 编码无损验证，对应 TC-009 场景延伸）
    $docs1 = Join-Path $dir "docs1.txt"            # 预置会话文档
    $docs2 = Join-Path $dir "b c.txt"              # 启动参数：带空格
    $docs3 = Join-Path $dir "笔记.txt"             # 从实例转发：中文
    $docs4 = Join-Path $dir "d e.txt"              # 多参数 1：带空格
    $docs5 = Join-Path $dir "docs5.txt"            # 多参数 2
    $docs6 = Join-Path $dir "docs6.txt"            # 连续快速转发
    foreach ($f in @($docs1, $docs2, $docs3, $docs4, $docs5, $docs6)) {
        Set-Content -LiteralPath $f -Value ("content of " + [IO.Path]::GetFileName($f)) -Encoding utf8
    }
    $u1 = ConvertTo-FileUrl $docs1; $u2 = ConvertTo-FileUrl $docs2; $u3 = ConvertTo-FileUrl $docs3
    $u4 = ConvertTo-FileUrl $docs4; $u5 = ConvertTo-FileUrl $docs5; $u6 = ConvertTo-FileUrl $docs6

    # 预置便携会话（上次会话已打开 docs1）：与 SessionManager ini 格式一致
    $ini = Join-Path $dir "qcnote.ini"
    Set-Content -LiteralPath $ini -Encoding ascii -Value @(
        "[documents]",
        "1\url=$u1",
        "1\cursor=3",
        "size=1",
        "active=$u1",
        "",
        "[window]",
        "x=10",
        "y=20",
        "width=800",
        "height=600",
        "maximized=false"
    )

    # —— I1（TC-007）：恢复上次会话后叠加打开启动参数文件并激活 ——
    # 主实例带 docs2 参数启动：先恢复 docs1，再叠加打开 docs2（新标签被激活）
    $mainProc = Start-Process -FilePath (Join-Path $dir "qcnote.exe") -ArgumentList "`"$docs2`"" `
        -WorkingDirectory $dir -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput (Join-Path $dir "main-out.log") -RedirectStandardError (Join-Path $dir "main-err.log")
    $mainAlive = -not $mainProc.HasExited   # 启动即崩则后续轮询自然超时失败
    $s1 = Wait-SessionState $ini @($u1, $u2) $u2
    Add-Result "I1" "TC-007 参数启动叠加打开（会话恢复→叠加→激活）" `
        ($mainAlive -and -not $mainProc.HasExited -and $s1.urls.Count -eq 2 -and $s1.urls[0] -eq $u1 -and $s1.urls[1] -eq $u2 -and $s1.active -eq $u2) `
        ("urls=" + $s1.urls.Count + " 顺序1=docs1:$($s1.urls[0] -eq $u1) 2=带空格:$($s1.urls[1] -eq $u2) active=docs2:$($s1.active -eq $u2) 主实例存活=$(-not $mainProc.HasExited)")

    # —— I2（TC-007/008）：转发已在会话中的 URL → 仅激活不重复，从实例退出码 0 ——
    $p2 = Start-Process -FilePath (Join-Path $dir "qcnote.exe") -ArgumentList "`"$docs1`"" `
        -WorkingDirectory $dir -PassThru -WindowStyle Hidden
    $exited2 = $p2.WaitForExit(15000)
    $s2 = Wait-SessionState $ini @($u1, $u2) $u1
    Add-Result "I2" "TC-007 同 URL 仅激活不重复（经单实例转发路径）" `
        ($exited2 -and $p2.ExitCode -eq 0 -and $s2.urls.Count -eq 2 -and $s2.active -eq $u1) `
        ("从实例退出码=" + $(if ($exited2) { $p2.ExitCode } else { "超时" }) + " urls=" + $s2.urls.Count + "（仍 2=未重复）active=docs1:$($s2.active -eq $u1)")

    # —— I3（TC-008）：从实例转发新文件 → 退出码 0、主实例叠加打开且存活 ——
    $p3 = Start-Process -FilePath (Join-Path $dir "qcnote.exe") -ArgumentList "`"$docs3`"" `
        -WorkingDirectory $dir -PassThru -WindowStyle Hidden
    $exited3 = $p3.WaitForExit(15000)
    $s3 = Wait-SessionState $ini @($u1, $u2, $u3) $u3
    Add-Result "I3" "TC-008 单实例转发（退出码 0、主实例叠加中文文件并存活）" `
        ($exited3 -and $p3.ExitCode -eq 0 -and -not $mainProc.HasExited -and $s3.urls.Count -eq 3 -and $s3.urls[2] -eq $u3 -and $s3.active -eq $u3) `
        ("从实例退出码=" + $(if ($exited3) { $p3.ExitCode } else { "超时" }) + " urls=" + $s3.urls.Count + " 含中文文件=$($s3.urls -contains $u3) 主实例存活=$(-not $mainProc.HasExited)")

    # —— I4（TC-014）：一次多文件参数 + 连续两次快速转发，按序叠加无遗漏 ——
    $p4 = Start-Process -FilePath (Join-Path $dir "qcnote.exe") -ArgumentList "`"$docs4`"", "`"$docs5`"" `
        -WorkingDirectory $dir -PassThru -WindowStyle Hidden
    # 不等待 p4 退出，立即发起第二次（快速连续）
    $p5 = Start-Process -FilePath (Join-Path $dir "qcnote.exe") -ArgumentList "`"$docs6`"" `
        -WorkingDirectory $dir -PassThru -WindowStyle Hidden
    $exited4 = $p4.WaitForExit(15000)
    $exited5 = $p5.WaitForExit(15000)
    $s4 = Wait-SessionState $ini @($u1, $u2, $u3, $u4, $u5, $u6) $u6
    $ordered = ($s4.urls.Count -eq 6) -and ($s4.urls[3] -eq $u4) -and ($s4.urls[4] -eq $u5) -and ($s4.urls[5] -eq $u6)
    Add-Result "I4" "TC-014 多文件参数与连续快速转发按序叠加" `
        ($exited4 -and $p4.ExitCode -eq 0 -and $exited5 -and $p5.ExitCode -eq 0 -and -not $mainProc.HasExited -and $ordered) `
        ("退出码 p4=" + $(if ($exited4) { $p4.ExitCode } else { "超时" }) + " p5=" + $(if ($exited5) { $p5.ExitCode } else { "超时" }) + " urls=" + $s4.urls.Count + " 按序4/5/6=$ordered 主实例存活=$(-not $mainProc.HasExited)")

    # 主实例 QML 运行时错误检查（附带诊断：qWarning 会打到 stderr）
    $errLog = Get-Content (Join-Path $dir "main-err.log") -Raw -ErrorAction SilentlyContinue
    if ($errLog -and $errLog.Trim().Length -gt 0) {
        Write-Output "--- 主实例 stderr（诊断附带，不计成败）---"
        Write-Output $errLog.Trim()
    }
}
finally {
    # —— 清理：仅按本脚本临时目录路径过滤，绝不误杀用户实例 ——
    if ($mainProc -and -not $mainProc.HasExited) {
        Stop-Process -Id $mainProc.Id -Force -ErrorAction SilentlyContinue
    }
    Get-Process qcnote -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -eq (Join-Path $dir "qcnote.exe") } |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500

    # —— 注册表守护核验（须在删目录前做：与开头对照）——
    $markerQueryAfter = reg query $markerKey 2>&1 | Out-String
    $markerAfter = ($LASTEXITCODE -eq 0) -and ($markerQueryAfter -match 'contextMenuPrompted')
    reg query $menuKey *> $null
    $menuAfter = ($LASTEXITCODE -eq 0)
    if (($markerBefore -ne $markerAfter) -or ($menuBefore -ne $menuAfter)) {
        Add-Result "G1" "注册表守护（真实键前后不变）" $false "标记 前=$markerBefore 后=$markerAfter；菜单键 前=$menuBefore 后=$menuAfter"
    }
    else {
        Add-Result "G1" "注册表守护（真实键前后不变）" $true
    }

    # —— 用户实例守护核验：无新增残留 qcnote 进程、用户实例未受影响 ——
    $procAfter = @(Get-Process qcnote -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Path)
    $residue = @($procAfter | Where-Object { $_ -notin $procBefore })
    $userHurt = @($procBefore | Where-Object { $_ -notin $procAfter })
    if ($residue.Count -eq 0 -and $userHurt.Count -eq 0) {
        Add-Result "G2" "进程守护（无残留、用户实例无恙）" $true
    }
    else {
        Add-Result "G2" "进程守护（无残留、用户实例无恙）" $false ("残留=" + ($residue -join ';') + " 受影响=" + ($userHurt -join ';'))
    }

    if (-not $KeepTemp) {
        # 删除前核验目标确为本脚本临时目录边界内的 qcnote 目录（防误删）
        $full = [IO.Path]::GetFullPath($dir)
        if ($full.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $full)) {
            Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    else {
        Write-Output "临时目录保留（-KeepTemp）：$dir"
    }
    Restore-ScriptEnv
}

Write-Output ""
Write-Output "===== 集成验证汇总（verify-explorer-menu.ps1）====="
$results | Format-Table Id, Name, Result, Note -AutoSize | Out-String -Width 200 | Write-Output
$total = $results.Count; $passed = @($results | Where-Object { $_.Result -eq "PASS" }).Count
Write-Output ("通过率：{0}/{1}" -f $passed, $total)
exit $(if ($failCount -eq 0) { 0 } else { 1 })
