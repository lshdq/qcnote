param(
    [string]$ExePath = 'build-win\qcnote.exe',
    [int]$TimeoutSec = 30
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null
$ProgressPreference = 'SilentlyContinue'

# =====================================================================
# qcnote 列模式填充编号自动化验证（任务C：editor 接线 + main.cpp 集成）
# 驱动 exe 的 --selftest-numbering 模式，覆盖测试用例
# （.localdoc\测试用例\测试用例-列模式填充编号.md）：
#   TC-011 菜单入口置灰：无文档/单光标置灰、多光标（选区 ≥2）可用
#         （RefreshMenuState 状态源 + BuildMainMenu 直调断言）
#   TC-001 多光标基础填充：对话框驱动确定（预填 1/1 + Enter），从上到下
#         插入 1/2/3，光标在编号末尾
#   TC-010 单批撤销：一次 Ctrl+Z 整体回滚；重做恢复全部编号
#   TC-005 负数补零：-15/-05/05（补零自动位数，负号在宽度之外；
#         含 TC-003 补零自动位数语义的等价小规模覆盖——宽度按全体编号
#         最大绝对值位数）
#   TC-008 非空选区替换：选区内容替换为编号、选区外文本不动
#   TC-019 e2e 全链路：本脚本自身（headless 全链路不扰用户）
# 另覆盖路由防御（单光标/无文档 no-op）与对话框参数预填（DEC-005）。
#
# 防扰依据（项目 AGENTS.md 测试防扰约定）：exe 以 --headless 启动——窗口
# 「可见但离屏」（TOOLWINDOW 无任务栏按钮）+ SW_SHOWNOACTIVATE 不激活不抢
# 前台，弹出层锚点随之离屏，全程零可见元素零前台抢占；结果写入文件后进程
# 自动退出，脚本读取并输出 PASS/FAIL。
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

$workDir = 'D:\temp\aiRunner\opencode\qcnote-numbering-20261118181824\selftest'
New-Item -ItemType Directory -Force -Path $workDir | Out-Null

$result = Join-Path $workDir 'numbering-selftest-result.txt'
if (Test-Path $result) { Remove-Item $result -Force }

$fail = $false
$proc = $null
try {
    $proc = Start-Process -FilePath $ExePath -ArgumentList @(
        '--headless', ('--selftest-numbering="{0}"' -f $result)
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
