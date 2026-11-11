param(
    [string]$ExePath = 'build-win\qcnote.exe',
    [int]$TimeoutSec = 20
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null
$ProgressPreference = 'SilentlyContinue'

# =====================================================================
# qcnote Tab 条自动化验证（M1-3）
# 驱动 exe 的 --selftest-tabs 模式：mock Tab 数据（中英文长短名 + modified
# 混合 + 超量溢出），程序内同步驱动点击/键盘/右键菜单/滚轮，结果写入文件后
# 自动退出。脚本读取结果文件并输出 PASS/FAIL。
# =====================================================================

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

$result = Join-Path $root 'build-win\tabs-selftest-result.txt'
if (Test-Path $result) { Remove-Item $result -Force }

$fail = $false
$proc = $null
try {
    $proc = Start-Process -FilePath $ExePath -ArgumentList @('--headless', ('--selftest-tabs="{0}"' -f $result)) -PassThru
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        Write-Output "[FAIL] selftest 超时（${TimeoutSec}s 未退出），强制 Kill"
        if (-not $proc.HasExited) { $proc.Kill() }
        $fail = $true
    }
    else {
        if (-not (Test-Path $result)) {
            Write-Output ("[FAIL] 结果文件未生成（进程退出码 {0}）" -f $proc.ExitCode)
            $fail = $true
        }
        else {
            foreach ($line in Get-Content $result) { Write-Output $line }
            $last = (Get-Content $result | Select-Object -Last 1)
            if ($last -notmatch 'overall: PASS') { $fail = $true }
        }
    }
}
finally {
    if ($proc -and -not $proc.HasExited) {
        $proc.CloseMainWindow() | Out-Null
        if (-not $proc.WaitForExit(2000)) { $proc.Kill() }
        $proc.Dispose()
    }
}

Write-Output ""
if ($fail) { Write-Output '总结: FAIL'; exit 1 } else { Write-Output '总结: PASS'; exit 0 }
