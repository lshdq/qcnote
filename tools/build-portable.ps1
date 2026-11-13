# ============================================================
# qcnote Windows 便携版打包脚本（纯 Win32 + Scintilla 静态链接单 exe）
# 流程：导入 MSVC 环境 -> 提取版本 -> 全新 Release 构建 ->
#       组装便携目录（qcnote.exe + qcnote.portable 标记）->
#       --headless 冒烟验证（不可见窗口 + 便携 ini 落盘断言）-> 压缩 zip
# 用法：pwsh -NoProfile -ExecutionPolicy Bypass -File tools/build-portable.ps1
# ============================================================

# ---------- 输出编码（防中文乱码） ----------
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null
$ProgressPreference       = 'SilentlyContinue'   # Compress-Archive 等不刷进度条
$ErrorActionPreference    = 'Stop'               # 意外错误立即终止，不静默继续

# ---------- 参数化常量（环境变化时只改这里） ----------
# CMake/Ninja 取自 Qt 官方工具链安装目录（仅路径复用，与 Qt 本身无关）。
# 本项目为纯 Win32 + MSVC /MT 静态 CRT：产物单 exe，无任何外部 DLL 依赖
$CMakeBin = 'D:\Program\Qt\Tools\CMake_64\bin'
$NinjaBin = 'D:\Program\Qt\Tools\Ninja'

$Root     = Split-Path -Parent $PSScriptRoot   # 项目根目录
$BuildDir = Join-Path $Root 'build-win'        # Win32 版构建目录
$DistDir  = Join-Path $Root 'dist'

# ---------- 工具函数 ----------
function Fail([string]$msg) {
    Write-Host "[错误] $msg" -ForegroundColor Red
    exit 1
}
function Step([string]$msg) {
    Write-Host "`n==> $msg" -ForegroundColor Cyan
}

# ---------- 1. 导入 MSVC 环境（脚本自行导入，不依赖外部会话） ----------
Step '导入 MSVC 环境（vcvars64）'
cmd /c '"d:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && set' | ForEach-Object {
    if ($_ -match '^(.*?)=(.*)$') { Set-Content -Path "env:\$($Matches[1])" -Value $Matches[2] }
}
$env:PATH = "$CMakeBin;$NinjaBin;" + $env:PATH
foreach ($tool in 'cl.exe', 'cmake', 'ninja') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Fail "构建工具不可用：$tool（MSVC 环境导入或 PATH 设置失败）"
    }
}
Write-Host 'MSVC 环境就绪（cl.exe / cmake / ninja 均可解析）'

# ---------- 2. 提取版本号 ----------
Step '从 CMakeLists.txt 提取版本号'
$cmakeLists = Join-Path $Root 'CMakeLists.txt'
if (-not (Test-Path $cmakeLists)) { Fail "未找到 $cmakeLists" }
$m = Select-String -Path $cmakeLists -Pattern 'project\(\s*qcnote\s+VERSION\s+(\d+\.\d+\.\d+)'
if (-not $m) { Fail '未能从 CMakeLists.txt 提取版本号（project(qcnote VERSION x.y.z)）' }
$Version = $m.Matches[0].Groups[1].Value
Write-Host "版本号：$Version"

# ---------- 3. 清理并全新配置 + 构建（Release） ----------
Step '清理旧 build-win 目录并全新配置 Release 构建'
if (Test-Path $BuildDir) { Remove-Item $BuildDir -Recurse -Force }
# 不传任何编译器参数：vcvars 导入后 cl 自动可用，由 CMake 自行探测 MSVC
cmake -S $Root -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release # 显式 -S：消除 CWD 依赖，支持从任意目录调用
if ($LASTEXITCODE -ne 0) { Fail 'cmake 配置失败' }

Step '完整构建'
cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { Fail 'cmake 构建失败' }

$ExePath = Join-Path $BuildDir 'qcnote.exe'
if (-not (Test-Path $ExePath)) { Fail "构建产物缺失：$ExePath" }

# ---------- 4. 组装便携目录（单 exe + 便携标记，无任何 DLL 依赖） ----------
$PortableName = "qcnote-$Version-win64-portable"
$PortableDir  = Join-Path $DistDir $PortableName
Step "组装便携目录 $PortableName"
if (Test-Path $PortableDir) { Remove-Item $PortableDir -Recurse -Force }
New-Item -ItemType Directory -Path $PortableDir -Force | Out-Null
Copy-Item $ExePath $PortableDir
# 便携标记：命中时应用的会话 ini/草稿落在 exe 旁（便携数据模式），不写 %LOCALAPPDATA%
New-Item -ItemType File -Path (Join-Path $PortableDir 'qcnote.portable') -Force | Out-Null
$entryCount = (Get-ChildItem $PortableDir -Recurse -File).Count
if ($entryCount -ne 2) {
    Fail "便携目录组装异常：应只有 qcnote.exe 与 qcnote.portable 共 2 个文件，实际 $entryCount 个"
}
Write-Host '便携目录就绪（qcnote.exe + qcnote.portable，共 2 个文件）'

# ---------- 5. --headless 冒烟验证（不可见窗口，不扰用户前台） ----------
Step '--headless 冒烟验证（存活 / 优雅关闭 / 退出码 / 便携 ini 落盘）'
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
$IniPath      = Join-Path $PortableDir 'qcnote.ini'
$proc         = $null

# 冒烟整体包 try/finally：成功 / 崩溃 / 超时 / 异常任何退出路径，finally 都执行
# 完整清理（按 PID 定向结束进程 + 删冒烟产生的 ini/drafts），保证便携目录不残留
# 冒烟临时件——zip 打包时目录只剩 qcnote.exe + qcnote.portable
try {
    $proc = Start-Process -FilePath (Join-Path $PortableDir 'qcnote.exe') -ArgumentList '--headless' -WorkingDirectory $PortableDir -PassThru

    # --- 存活断言：--headless 不显示窗口、无任务栏项、不激活；启动 3 秒后必须仍存活 ---
    Start-Sleep -Seconds 3
    if ($proc.HasExited) { Fail "冒烟失败：进程 3 秒内自行退出（退出码 $($proc.ExitCode)）" }
    Write-Host "[通过] 存活 >= 3s（HasExited=False, PID=$($proc.Id)）"

    # --- 优雅关闭：不可见窗口 MainWindowHandle==0，须枚举类名 QcnoteMainWindow 定位 HWND ---
    $main = [QcSmoke]::MainWindowOfPid([uint32]$proc.Id)
    if ($main -eq [IntPtr]::Zero) { Fail '冒烟失败：未找到主窗口（QcnoteMainWindow）' }
    [QcSmoke]::SendMessage($main, $WM_CLOSE_MSG, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    if (-not $proc.WaitForExit(5000)) {
        $proc.Kill()
        $proc.WaitForExit(3000) | Out-Null
        Fail '冒烟失败：WM_CLOSE 后 5 秒未退出，已强制结束'
    }
    Write-Host '[通过] WM_CLOSE 后 5s 内优雅退出'

    # --- 退出码断言 ---
    if ($proc.ExitCode -ne 0) { Fail "冒烟失败：退出码 $($proc.ExitCode)（期望 0）" }
    Write-Host '[通过] 退出码 = 0'

    # --- 便携 ini 落盘断言（本脚本特有）---
    # --headless 形态下持久化激活，WM_CLOSE 时同步把会话写到 exe 旁 qcnote.ini；
    # ini 存在且非空 = 便携数据模式端到端生效（不碰 %LOCALAPPDATA%）
    if (-not (Test-Path $IniPath)) { Fail "冒烟失败：便携 ini 未生成：$IniPath（便携数据模式未生效）" }
    $iniItem = Get-Item $IniPath
    if ($iniItem.Length -eq 0) { Fail "冒烟失败：便携 ini 为空文件：$IniPath" }
    Write-Host ("[通过] 便携 ini 已落盘且非空（{0} 字节：{1}）" -f $iniItem.Length, $IniPath)
}
finally {
    # 结束残留进程（按 PID 定向，不误杀其他 qcnote 实例；已退出则跳过）
    if ($proc -and -not $proc.HasExited) {
        $main = [QcSmoke]::MainWindowOfPid([uint32]$proc.Id)
        if ($main -ne [IntPtr]::Zero) {
            [QcSmoke]::SendMessage($main, $WM_CLOSE_MSG, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        }
        if (-not $proc.WaitForExit(2000)) { $proc.Kill() }
        $proc.WaitForExit(3000) | Out-Null
    }
    if ($proc) { $proc.Dispose() }
    # 删除冒烟产生的 ini 与 drafts 目录（便携目录不残留冒烟临时件）
    Remove-Item $IniPath -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $PortableDir 'drafts') -Recurse -Force -ErrorAction SilentlyContinue
}

# ---------- 6. 压缩 zip ----------
$ZipPath = Join-Path $DistDir "$PortableName.zip"
Step '压缩便携目录为 zip'
if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
Compress-Archive -Path $PortableDir -DestinationPath $ZipPath
if (-not (Test-Path $ZipPath)) { Fail 'zip 压缩失败' }

# ---------- 7. 产物清单摘要 ----------
$ZipSize    = (Get-Item $ZipPath).Length
$EntryCount = (Get-ChildItem $PortableDir -Recurse -File).Count
Write-Host ''
Write-Host '================== 打包完成 ==================' -ForegroundColor Green
Write-Host ("zip 路径       ：{0}" -f $ZipPath)
Write-Host ("zip 大小       ：{0:N2} MB" -f ($ZipSize / 1MB))
Write-Host ("便携目录       ：{0}" -f $PortableDir)
Write-Host ("目录条目文件数 ：{0}（应为 2：qcnote.exe + qcnote.portable）" -f $EntryCount)
Write-Host ("版本号         ：{0}" -f $Version)
Write-Host '==============================================' -ForegroundColor Green
exit 0
