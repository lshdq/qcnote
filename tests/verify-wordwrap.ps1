# verify-wordwrap.ps1 —— 自动换行：offscreen 冒烟（持久化回归脚本）
# 对应用例：.localdoc\测试用例\测试用例-自动换行.md
#   TC-010: offscreen 冒烟启动（禁止可见窗口；验证启动存活且无 QML 运行时错误输出）
# 用法：
#   pwsh.exe -NoProfile -ExecutionPolicy Bypass -File tests\verify-wordwrap.ps1 [-ProjectRoot <项目根>] [-WaitSeconds <秒>]
# 退出码：0 = 通过；1 = 失败
#Requires -Version 7
# 上行 #Requires：脚本头用法已限定 pwsh.exe（PowerShell 7+）；版本不满足时
# PowerShell 在执行前直接报错退出，错误前置。

param(
    [string]$ProjectRoot = "",
    [int]$WaitSeconds = 4
)

# 先记录原控制台代码页：后续 UTF-8 改写与 chcp 65001 均修改共享控制台状态，
# 必须在任何改写之前记录，否则记录到的是已改写的值，脚本退出前才能正确恢复
$origCodePage = [Console]::OutputEncoding
# 输出编码固定为 UTF-8，避免中文乱码（全局约定）
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null

if (-not $ProjectRoot) { $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path }

# 保存环境变量原值：脚本会临时改写 QT_QPA_PLATFORM 与 PATH，
# 所有退出路径（含异常提前退出）经 Restore-ScriptEnv 统一恢复，不污染调用方会话。
$script:OrigQpaPlatform = $env:QT_QPA_PLATFORM
$script:OrigPath        = $env:PATH
$script:OrigCodePage    = $origCodePage

function Restore-ScriptEnv {
    if ($null -ne $script:OrigQpaPlatform) { $env:QT_QPA_PLATFORM = $script:OrigQpaPlatform }
    else { Remove-Item Env:\QT_QPA_PLATFORM -ErrorAction SilentlyContinue }
    if ($null -ne $script:OrigPath) { $env:PATH = $script:OrigPath }
    else { Remove-Item Env:\PATH -ErrorAction SilentlyContinue }
    # 恢复原控制台代码页，与开头记录方式对应
    [Console]::OutputEncoding = $script:OrigCodePage
}

$exePath = Join-Path $ProjectRoot "build\qcnote.exe"
if (-not (Test-Path -LiteralPath $exePath)) {
    Write-Output "[FAIL] TC-010 offscreen 冒烟启动：exe 不存在: $exePath（先执行完整构建）"
    Restore-ScriptEnv  # 此 exit 不经 try/finally，需在此恢复代码页（env 尚未改写，幂等）
    exit 1
}

$failCount = 0
$details   = @()
$outFile = $errFile = ""

try {
    # GUI 红线：自动化验证一律 offscreen，禁止可见窗口、禁止抢占前台焦点
    $env:QT_QPA_PLATFORM = "offscreen"
    $env:PATH = "D:\Program\Qt\6.11.2\mingw_64\bin;D:\Program\Qt\Tools\mingw1310_64\bin;" + $env:PATH

    # stdout/stderr 重定向到临时文件：GUI 子系统无控制台，但句柄继承可捕获
    # qWarning 输出（QML 运行时错误经 qWarning 打到 stderr）
    $tag = [guid]::NewGuid().ToString("N").Substring(0, 8)
    $outFile = Join-Path ([System.IO.Path]::GetTempPath()) "qcnote-smoke-out-$tag.log"
    $errFile = Join-Path ([System.IO.Path]::GetTempPath()) "qcnote-smoke-err-$tag.log"

    $proc = Start-Process -FilePath $exePath -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    Start-Sleep -Seconds $WaitSeconds

    # 断言 1：进程存活（启动未崩溃）
    $alive = -not $proc.HasExited
    if ($alive) {
        # 仅结束本用例启动的进程（按 PID），避免误杀机器上运行中的其他 qcnote 实例；
        # -ErrorAction SilentlyContinue：进程恰好在此瞬间自行退出时忽略竞态错误
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        # TerminateProcess 为异步语义：等待句柄真正退出后再读取重定向输出文件，
        # 消除子进程仍持有文件句柄导致的共享冲突竞态
        # WaitForExit(ms) 带超时上限，避免进程未按预期退出时无限阻塞
        if (-not $proc.WaitForExit(5000)) {
            $failCount++
            $details += "Stop-Process 后进程未在 5 秒内退出（PID=$($proc.Id)）"
            # 超时后补一次强制结束收尾，防止残留进程
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }
    } else {
        $failCount++
        $details += "进程启动后 ${WaitSeconds}s 内退出（ExitCode=$($proc.ExitCode)）"
    }

    # 断言 2：无 QML 运行时错误输出（绑定失败 / 类型错误 / 未定义引用 / 绑定循环）
    $output = ""
    if (Test-Path -LiteralPath $outFile) { $output += [System.IO.File]::ReadAllText($outFile) }
    if (Test-Path -LiteralPath $errFile) { $output += [System.IO.File]::ReadAllText($errFile) }
    $errorLines = @()
    if ($output) {
        $errorLines = @($output -split "`r?`n" | Where-Object {
            ($_ -match 'TypeError|ReferenceError') -or
            ($_ -match 'QML' -and $_ -match 'Error|TypeError|ReferenceError|Cannot|not defined') -or
            ($_ -match 'Binding loop') })
    }
    if ($errorLines.Count -gt 0) {
        $failCount++
        $details += ("检测到 QML 运行时错误输出：" + ($errorLines -join " | "))
    }
}
catch {
    $failCount++
    $details += "未捕获异常: $($_.Exception.Message)"
}
finally {
    if ($outFile -or $errFile) {
        Remove-Item -LiteralPath @($outFile, $errFile) -Force -ErrorAction SilentlyContinue
    }
    Restore-ScriptEnv
}

Write-Output ""
Write-Output "==================== 自动换行冒烟结果（verify-wordwrap.ps1） ===================="
if ($failCount -eq 0) {
    Write-Output "[PASS] TC-010 offscreen 冒烟启动：存活 >= ${WaitSeconds}s，无 QML 错误输出，已结束进程"
} else {
    Write-Output "[FAIL] TC-010 offscreen 冒烟启动"
    foreach ($d in $details) { Write-Output ("       {0}" -f $d) }
}
Write-Output "========================================================================"

if ($failCount -gt 0) { exit 1 }
exit 0
