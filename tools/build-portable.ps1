# ============================================================
# qcnote Windows 便携版打包脚本
# 流程：提取版本 -> 全新 Release 构建 -> windeployqt 部署 ->
#       写入便携标记 -> offscreen 冒烟验证 -> 压缩 zip
# 用法：pwsh -NoProfile -ExecutionPolicy Bypass -File tools/build-portable.ps1
# ============================================================

# ---------- 输出编码（防中文乱码） ----------
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null

# ---------- 参数化常量（环境变化时只改这里） ----------
$CMakeBin    = 'D:\Program\Qt\Tools\CMake_64\bin'
$NinjaBin    = 'D:\Program\Qt\Tools\Ninja'
$QtBin       = 'D:\Program\Qt\6.11.2\mingw_64\bin'
$MinGwBin    = 'D:\Program\Qt\Tools\mingw1310_64\bin'  # windres 需要此目录在 PATH
$CCompiler   = 'D:/Program/Qt/Tools/mingw1310_64/bin/gcc.exe'
$CxxCompiler = 'D:/Program/Qt/Tools/mingw1310_64/bin/g++.exe'
$QtPrefix    = 'D:/Program/Qt/6.11.2/mingw_64'

$Root        = Split-Path -Parent $PSScriptRoot   # 项目根目录
$BuildDir    = Join-Path $Root 'build'
$DistDir     = Join-Path $Root 'dist'

# ---------- 工具函数 ----------
function Fail([string]$msg) {
    Write-Host "[错误] $msg" -ForegroundColor Red
    exit 1
}
function Step([string]$msg) {
    Write-Host "`n==> $msg" -ForegroundColor Cyan
}

# 按进程对象（PID）定向结束本次启动的进程，不误杀其他 qcnote 实例
function Stop-Qcnote([System.Diagnostics.Process]$proc) {
    if ($proc -and -not $proc.HasExited) {
        try {
            $proc.Kill()
            $proc.WaitForExit() | Out-Null
        } catch [System.InvalidOperationException] {
            # HasExited 检查与 Kill() 间隙进程恰好自行退出属正常情况，视为已结束
            if (-not $proc.HasExited) { throw }
        }
    }
}

# ---------- 1. 设置构建 PATH ----------
Step '设置构建 PATH'
$env:PATH = "$CMakeBin;$NinjaBin;$QtBin;$MinGwBin;" + $env:PATH
if (-not (Test-Path (Join-Path $QtBin 'windeployqt.exe'))) { Fail "未找到 windeployqt.exe：$QtBin" }

# ---------- 2. 提取版本号 ----------
Step '从 CMakeLists.txt 提取版本号'
$cmakeLists = Join-Path $Root 'CMakeLists.txt'
if (-not (Test-Path $cmakeLists)) { Fail "未找到 $cmakeLists" }
$m = Select-String -Path $cmakeLists -Pattern 'project\(\s*qcnote\s+VERSION\s+(\d+\.\d+\.\d+)'
if (-not $m) { Fail '未能从 CMakeLists.txt 提取版本号（project(qcnote VERSION x.y.z)）' }
$Version = $m.Matches[0].Groups[1].Value
Write-Host "版本号：$Version"

# ---------- 3. 清理并全新配置 + 构建 ----------
Step '清理旧 build 目录并全新配置 Release 构建'
if (Test-Path $BuildDir) { Remove-Item $BuildDir -Recurse -Force }
$cmakeArgs = @(
    "-B$BuildDir", '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_PREFIX_PATH=$QtPrefix",
    "-DCMAKE_C_COMPILER=$CCompiler",
    "-DCMAKE_CXX_COMPILER=$CxxCompiler"
)
cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { Fail 'cmake 配置失败' }

Step '完整构建'
cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { Fail 'cmake 构建失败' }

$ExePath = Join-Path $BuildDir 'qcnote.exe'
if (-not (Test-Path $ExePath)) { Fail "构建产物缺失：$ExePath" }

# ---------- 4. 组装便携目录 ----------
$PortableName = "qcnote-$Version-win64-portable"
$PortableDir  = Join-Path $DistDir $PortableName
Step "组装便携目录 $PortableName"
if (Test-Path $PortableDir) { Remove-Item $PortableDir -Recurse -Force }
New-Item -ItemType Directory -Path $PortableDir -Force | Out-Null
Copy-Item $ExePath $PortableDir

# ---------- 5. windeployqt 部署 ----------
Step 'windeployqt 部署 Qt 运行库'
$WinDeployQt = Join-Path $QtBin 'windeployqt.exe'
& $WinDeployQt --release --qmldir (Join-Path $Root 'src\qml') --compiler-runtime (Join-Path $PortableDir 'qcnote.exe')
if ($LASTEXITCODE -ne 0) { Fail 'windeployqt 部署失败' }

# ---------- 6. 写入便携标记文件 ----------
Step '写入便携标记文件 qcnote.portable'
New-Item -ItemType File -Path (Join-Path $PortableDir 'qcnote.portable') -Force | Out-Null

# ---------- 7. offscreen 冒烟验证 ----------
Step 'offscreen 冒烟验证（无可见窗口）'
# windeployqt 默认只部署 windows 平台插件；offscreen 冒烟需临时拷入 qoffscreen.dll，
# 冒烟结束即删除，保证产物干净
$PlatformsDir = Join-Path $PortableDir 'platforms'
$OffscreenDll = Join-Path $PlatformsDir 'qoffscreen.dll'
$OffscreenSrc = Join-Path $QtBin '..\plugins\platforms\qoffscreen.dll'
if (-not (Test-Path $OffscreenSrc)) { Fail "未找到 qoffscreen.dll：$OffscreenSrc" }
Copy-Item $OffscreenSrc $OffscreenDll -Force
# 预置一个含会话记录（documents/size=1，但条目无效）的 qcnote.ini：
# 首次启动若无会话记录，SessionManager 按设计不写盘（首启无副作用），
# 无法验证 ini 落点；预置 size=1 会走恢复路径并在末尾 saveNow() 重写为
# size=0——重写即证明应用确实以 exe 旁的便携 ini 作为会话文件（端到端验证）
$IniPath = Join-Path $PortableDir 'qcnote.ini'
Set-Content -Path $IniPath -Value "[documents]`nsize=1`n" -Encoding ascii

# 说明：本冒烟在开发机 PATH（含 Qt）下运行，便携产物 DLL 完整性的独立验证
# 由 tools/verify-portable.ps1 的最小 PATH 验证承担
$env:QT_QPA_PLATFORM = 'offscreen'
$proc = $null

# 冒烟整体包 try/finally：成功 / 崩溃 / 超时 / 异常任何退出路径，
# finally 都执行完整清理（结束进程 + 删 ini + 删 qoffscreen.dll + 清环境变量），
# 保证便携目录不残留冒烟临时件（与 DEC-008 一致）
try {
    $proc = Start-Process -FilePath (Join-Path $PortableDir 'qcnote.exe') -WorkingDirectory $PortableDir -PassThru

    # 冒烟等待采用条件轮询（冷启动 QML 冷缓存时 ini 重写可能略超固定 5 秒）：
    # 每 500ms 检查一次，总超时上限 20 秒；三分支：
    #   1) 进程退出      -> 立即失败（启动崩溃）
    #   2) ini 被重写    -> 成功，定向结束进程，进入后续清理与打包
    #   3) 超时仍未重写  -> 失败
    $SmokePollIntervalMs = 500   # 轮询间隔
    $SmokeTimeoutMs      = 20000 # 总超时上限
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $smokeOk = $false
    while ($sw.ElapsedMilliseconds -lt $SmokeTimeoutMs) {
        if ($proc.HasExited) {
            Fail "冒烟失败：进程已退出（退出码 $($proc.ExitCode)，启动后约 $([int]$sw.Elapsed.TotalMilliseconds) ms）"
        }
        $iniContent = Get-Content $IniPath -Raw -ErrorAction SilentlyContinue
        if ($iniContent -and ($iniContent -match 'size=0' -or $iniContent -match '\[window\]')) {
            $smokeOk = $true
            Write-Host ("便携数据模式验证通过：应用已重写 exe 旁 qcnote.ini（耗时 {0:N0} ms，$IniPath）" -f $sw.Elapsed.TotalMilliseconds)
            break
        }
        Start-Sleep -Milliseconds $SmokePollIntervalMs
    }
    if (-not $smokeOk) {
        Fail "冒烟失败：超过 $($SmokeTimeoutMs / 1000) 秒 qcnote.ini 仍未被应用重写（便携数据模式未生效）"
    }
} finally {
    # 结束进程（按 PID 定向，不误杀其他 qcnote 实例；已退出则跳过）
    # 并清理冒烟产生的 ini 与临时 offscreen 插件
    Stop-Qcnote $proc
    Start-Sleep -Milliseconds 500
    Remove-Item $IniPath -Force -ErrorAction SilentlyContinue
    Remove-Item $OffscreenDll -Force -ErrorAction SilentlyContinue
    Remove-Item Env:\QT_QPA_PLATFORM -ErrorAction SilentlyContinue
}

# ---------- 8. 压缩 zip ----------
$ZipPath = Join-Path $DistDir "$PortableName.zip"
Step '压缩便携目录为 zip'
if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
Compress-Archive -Path $PortableDir -DestinationPath $ZipPath
if (-not (Test-Path $ZipPath)) { Fail 'zip 压缩失败' }

# ---------- 9. 产物清单摘要 ----------
$ZipSize   = (Get-Item $ZipPath).Length
$EntryCount = (Get-ChildItem $PortableDir -Recurse -File).Count
Write-Host ''
Write-Host '================== 打包完成 ==================' -ForegroundColor Green
Write-Host ("zip 路径      ：{0}" -f $ZipPath)
Write-Host ("zip 大小      ：{0:N2} MB" -f ($ZipSize / 1MB))
Write-Host ("便携目录      ：{0}" -f $PortableDir)
Write-Host ("目录条目文件数：{0}" -f $EntryCount)
Write-Host '===============================================' -ForegroundColor Green
exit 0
