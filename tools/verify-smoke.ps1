param(
    [string]$ExePath = 'build-win\qcnote.exe',
    # M4 起正常运行会读写用户目录会话/草稿（%LOCALAPPDATA%\qcnote）；
    # 传入临时目录经 --userdir 测试钩子隔离，防污染真实用户数据（可省略）
    [string]$UserDir = ''
)

[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null
$ProgressPreference = 'SilentlyContinue'

# =====================================================================
# qcnote 进程冒烟验证脚本
# 流程：启动（--headless：不显示窗口、停泊离屏，零可见元素不扰用户前台）
#       -> 存活 >=3s 断言（HasExited == false）
#       -> 对主窗口 HWND 直接 SendMessage(WM_CLOSE)（不可见窗口
#          MainWindowHandle==0，CloseMainWindow 不可用）-> 等待退出 <=5s
#          （未退则 Kill 并 FAIL）-> 退出码 0 断言 -> PASS/FAIL
# =====================================================================

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Collections.Generic;
public class QcSmoke {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    public static IntPtr MainWindowOfPid(uint target) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, lp) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid == target) {
                var sb = new StringBuilder(256);
                GetClassName(h, sb, 256);
                if (sb.ToString() == "QcnoteMainWindow") { found = h; return false; }
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@
$WM_CLOSE_MSG = 0x0010

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

$fail = $false
$proc = $null
try {
    Write-Output ("启动: {0}" -f $ExePath)
    $startArgs = @('--headless')
    if ($UserDir) { $startArgs += "--userdir=$UserDir" }
    $proc = [System.Diagnostics.Process]::Start($ExePath, ($startArgs -join ' '))

    # 存活 >= 3s 断言
    Start-Sleep -Seconds 3
    if ($proc.HasExited) {
        Write-Output ("[FAIL] 进程 3s 内自行退出，ExitCode={0}" -f $proc.ExitCode)
        $fail = $true
    }
    else {
        Write-Output "[PASS] 存活 >= 3s (HasExited=False, PID=$($proc.Id))"

        # 优雅关闭（--headless 窗口不可见：枚举类名找 HWND 后直接 WM_CLOSE）
        $main = [QcSmoke]::MainWindowOfPid([uint32]$proc.Id)
        if ($main -eq [IntPtr]::Zero) {
            Write-Output "[FAIL] 未找到主窗口（QcnoteMainWindow）"
            $fail = $true
        }
        else {
            [QcSmoke]::SendMessage($main, $WM_CLOSE_MSG, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        }
        if ($proc.HasExited -or $proc.WaitForExit(5000)) {
            if ($main -ne [IntPtr]::Zero) { Write-Output "[PASS] WM_CLOSE 后 5s 内退出" }
        }
        else {
            Write-Output "[FAIL] WM_CLOSE 后 5s 未退出，强制 Kill"
            $proc.Kill()
            $proc.WaitForExit(3000) | Out-Null
            $fail = $true
        }
    }

    # 退出码断言（3s 内退出的异常路径也检查）
    if ($proc.HasExited) {
        $code = $proc.ExitCode
        if ($code -eq 0) {
            Write-Output "[PASS] 退出码 = 0"
        }
        else {
            Write-Output ("[FAIL] 退出码 = {0}（期望 0）" -f $code)
            $fail = $true
        }
    }
}
finally {
    if ($proc -and -not $proc.HasExited) {
        $main = [QcSmoke]::MainWindowOfPid([uint32]$proc.Id)
        if ($main -ne [IntPtr]::Zero) {
            [QcSmoke]::SendMessage($main, $WM_CLOSE_MSG, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        }
        if (-not $proc.WaitForExit(2000)) { $proc.Kill() }
        $proc.Dispose()
    }
}

Write-Output ""
if ($fail) { Write-Output '总结: FAIL'; exit 1 } else { Write-Output '总结: PASS'; exit 0 }
