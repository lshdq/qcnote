param(
    [string]$ExePath = 'build-win\qcnote.exe',
    [int]$TimeoutSec = 30
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null
$ProgressPreference = 'SilentlyContinue'

# =====================================================================
# qcnote M2-B 文档全链路自动化验证
# 驱动 exe 的 --selftest-doc 模式（携带一个启动位置参数文件，验证 D2 打开 +
# A1 全链路：新建/输入/切换保留/另存为/GBK 转存/关闭重开/状态栏/对话框/IME 注入/
# 换行切换/清空欢迎页）。结果写入文件后自动退出，脚本读取并输出 PASS/FAIL。
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

$workDir = 'D:\temp\aiRunner\opencode\qcnote-win32-rewrite-20261110183000\selftest-doc'
New-Item -ItemType Directory -Force -Path $workDir | Out-Null

# 启动参数测试文件（UTF-8 无 BOM、LF；内容与 exe 内 --selftest-doc 阶段 0 断言一致）
$argFile = Join-Path $workDir '打开测试-中文 文件.txt'
'你好 qcnote' + "`n" + 'second line' | Out-File -FilePath $argFile -Encoding utf8NoBOM -NoNewline

$result = Join-Path $workDir 'doc-selftest-result.txt'
if (Test-Path $result) { Remove-Item $result -Force }

$fail = $false
$proc = $null
try {
    $proc = Start-Process -FilePath $ExePath -ArgumentList @(
        '--headless', ('--selftest-doc="{0}"' -f $result), ('"{0}"' -f $argFile)
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
