param(
    [string]$ExePath = 'build-win\qcnote.exe',
    [int]$TimeoutSec = 30
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null
$ProgressPreference = 'SilentlyContinue'

# =====================================================================
# qcnote M3-B 查找替换 + 多光标自动化验证
# 驱动 exe 的 --selftest-search 模式（程序化消息直投：对话框预填/实时计数/
# indicator 采样/Enter 环绕/三开关/无效正则/替换当前/全部替换单撤销/DEC-009
# 关闭保留状态/F3 关闭态跳转/缓存失效；多光标：Alt+Click/Ctrl+D/Esc/
# Ctrl+Shift+L/Ctrl+Alt+方向/多选区复制粘贴分发/IME 提交分发路径/键盘列选/
# Shift+Alt+I）。结果写入文件后自动退出，脚本读取并输出 PASS/FAIL。
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

$workDir = 'D:\temp\aiRunner\opencode\qcnote-win32-rewrite-20261110183000\selftest-search'
New-Item -ItemType Directory -Force -Path $workDir | Out-Null

$result = Join-Path $workDir 'search-selftest-result.txt'
if (Test-Path $result) { Remove-Item $result -Force }

$fail = $false
$proc = $null
try {
    $proc = Start-Process -FilePath $ExePath -ArgumentList @(
        '--headless', ('--selftest-search="{0}"' -f $result)
    ) -PassThru -WorkingDirectory $workDir
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
