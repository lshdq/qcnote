param(
    [string]$ExePath = 'build-win\qcnote.exe',
    [int]$TimeoutSec = 60
)

# =====================================================================
# qcnote 外部文件改动检测自动化验证（TC-101..108）
# 驱动 exe 的 --selftest-filewatch 模式（--watch-interval-ms=50 注入短轮询
# 间隔）：场景临时文件与结果文件均落本脚本任务目录；exe 内阶段机由生产
# poll tick 计数推进，模态对话框经 onOpened 键盘注入自动应答。结果写入
# 文件后自动退出，脚本读取并输出 PASS/FAIL。
# 防扰约定：全程 --headless（窗口不显示、离屏停泊——零可见元素、不抢前台
#   焦点）+ --no-shell-prompt（跳过首启右键菜单提示）+ --userdir 重定向
#   （自测模式本身也屏蔽持久化，双保险不触碰真实用户目录）。
# 幂等：任务目录名带时间戳 + 随机段，可复跑；成功后清理临时文件（失败保留
#   供诊断，路径见输出）。
# 用法：pwsh.exe -NoProfile -ExecutionPolicy Bypass -File tests\verify-filewatch-selftest.ps1
# 退出码：0 = 通过；1 = 失败；2 = 前置缺失
# =====================================================================

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null
$ProgressPreference = 'SilentlyContinue'

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

# 唯一任务目录（临时产物约定：D:\temp\aiRunner\opencode 下按任务建立唯一目录）
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$rand = ([System.IO.Path]::GetRandomFileName() -replace '[^a-zA-Z0-9]', '')
$workDir = "D:\temp\aiRunner\opencode\qcnote-filewatch-selftest-$stamp-$rand"
New-Item -ItemType Directory -Force -Path $workDir | Out-Null
$userDir = Join-Path $workDir 'userdir'
New-Item -ItemType Directory -Force -Path $userDir | Out-Null

$result = Join-Path $workDir 'filewatch-selftest-result.txt'
if (Test-Path $result) { Remove-Item $result -Force }

$fail = $false
$proc = $null
try {
    $proc = Start-Process -FilePath $ExePath -ArgumentList @(
        '--headless',
        '--no-shell-prompt',
        ('--userdir="{0}"' -f $userDir),
        ('--selftest-filewatch="{0}"' -f $result),
        '--watch-interval-ms=50'
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
if ($fail) {
    Write-Output "诊断目录: $workDir（含 .detail.txt 采样明细）"
    Write-Output '总结: FAIL'
    exit 1
}
# 成功：清理本任务临时文件（结果已输出到 stdout；可重建产物不保留）
Remove-Item -Recurse -Force $workDir -ErrorAction SilentlyContinue
Write-Output '总结: PASS'
exit 0
