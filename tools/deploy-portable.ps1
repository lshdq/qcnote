# ============================================================
# qcnote Windows 便携版部署脚本
# 流程：定位产物 -> 校验产物 -> 结束目标目录进程 -> robocopy 镜像部署 -> 摘要
# 用法：pwsh -NoProfile -ExecutionPolicy Bypass -File tools/deploy-portable.ps1
#       [-PortableDir <便携产物目录>] [-TargetDir <部署目标目录>]
# 示例：pwsh -NoProfile -ExecutionPolicy Bypass -File tools/deploy-portable.ps1
#       pwsh -NoProfile -ExecutionPolicy Bypass -File tools/deploy-portable.ps1 -TargetDir 'D:\Program\qcnote'
#       pwsh -NoProfile -ExecutionPolicy Bypass -File tools/deploy-portable.ps1 -PortableDir 'dist\qcnote-0.1.0-win64-portable'
# 安全：robocopy /MIR 镜像部署（自动清理旧版本残留 DLL 等多余文件），
#       /XF qcnote.ini 保留目标目录用户会话数据 qcnote.ini；
#       仅结束可执行文件位于目标目录内的 qcnote 进程（按 Path 前缀匹配 +
#       PID 定向结束），不误杀开发版（build/ 下）或其他目录实例；
#       不触碰 %APPDATA%
# ============================================================
param(
    [string]$PortableDir = '',                  # 便携产物目录；为空时自动选取 dist/ 下最新产物
    [string]$TargetDir   = 'D:\Program\qcnote'  # 部署目标目录
)

# ---------- 输出编码（防中文乱码） ----------
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null

$Root   = Split-Path -Parent $PSScriptRoot   # 项目根目录（脚本目录的父目录）
$DistDir = Join-Path $Root 'dist'

$KillWaitTimeoutMs = 10000   # 结束进程后等待退出的超时上限（ms）

# ---------- 工具函数 ----------
function Fail([string]$msg) {
    Write-Host "[错误] $msg" -ForegroundColor Red
    exit 1
}
function Step([string]$msg) {
    Write-Host "`n==> $msg" -ForegroundColor Cyan
}

# 按进程对象（PID）定向结束并限时等待退出，不误杀其他 qcnote 实例；
# 返回 $true 表示进程已退出，$false 表示等待超时仍存活
function Stop-Qcnote([System.Diagnostics.Process]$proc) {
    if (-not $proc) { return $true }
    try {
        if (-not $proc.HasExited) { $proc.Kill() }
    } catch [System.InvalidOperationException] {
        # HasExited 检查与 Kill() 间隙进程恰好自行退出属正常情况，视为已结束
        if (-not $proc.HasExited) { throw }
    }
    try {
        if (-not $proc.WaitForExit($KillWaitTimeoutMs)) { return $false }
    } catch {
        # WaitForExit 异常（句柄失效等）：刷新后按实际退出状态判定
        $proc.Refresh()
        return $proc.HasExited
    }
    return $true
}

# ---------- 1. 定位便携产物目录 ----------
Step '定位便携产物目录'
if (-not $PortableDir) {
    # 自动选取 dist/ 下 LastWriteTime 最新的 qcnote-*-win64-portable 目录
    $latest = Get-ChildItem $DistDir -Directory -Filter 'qcnote-*-win64-portable' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $latest) { Fail "未在 $DistDir 下找到 qcnote-*-win64-portable 产物目录（请先运行 tools/build-portable.ps1）" }
    $PortableDir = $latest.FullName
}
try {
    $PortableDir = (Resolve-Path -LiteralPath $PortableDir -ErrorAction Stop).Path.TrimEnd('\')
} catch {
    Fail "便携产物目录不存在：$PortableDir"
}
Write-Host "产物目录：$PortableDir"

# ---------- 2. 校验产物完整性 ----------
Step '校验产物完整性'
if (-not (Test-Path -LiteralPath (Join-Path $PortableDir 'qcnote.exe'))) {
    Fail "产物缺失 qcnote.exe：$PortableDir"
}
if (-not (Test-Path -LiteralPath (Join-Path $PortableDir 'qcnote.portable'))) {
    Fail "产物缺失 qcnote.portable：$PortableDir"
}
# 从目录名 qcnote-<版本>-win64-portable 提取版本号（自定义目录名提取不到则显示"未知"）
$Version  = '未知'
$leafName = Split-Path $PortableDir -Leaf
if ($leafName -match '^qcnote-(\d+\.\d+\.\d+)-win64-portable$') { $Version = $Matches[1] }
Write-Host "版本号：$Version"

# ---------- 3. 解析部署目标目录 ----------
Step '解析部署目标目录'
# 前置拒绝两类危险形态：空目标（会解析到当前 location，/MIR 误伤项目目录）与
# 裸盘符（如 D:，按该盘当前目录解析，实际指向不可控）
if ([string]::IsNullOrWhiteSpace($TargetDir)) { Fail "目标目录不能为空" }
if ($TargetDir -match '^[A-Za-z]:$') { Fail "拒绝裸盘符目标目录：$TargetDir（请写明完整路径，如 D:\target）" }
$targetExists = Test-Path -LiteralPath $TargetDir
if ($targetExists -and -not (Test-Path -LiteralPath $TargetDir -PathType Container)) {
    Fail "目标路径已存在且不是目录：$TargetDir"
}
# 存在/不存在两个分支统一按 PowerShell 当前 location 解析为绝对路径
# （GetUnresolvedProviderPathFromPSPath 对不存在的路径同样可解析，robocopy /MIR 会自动创建）；
# 不使用 [IO.Path]::GetFullPath：其基准是 .NET CurrentDirectory，与 PowerShell location 不一致，会导致解析错位
try {
    $TargetDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($TargetDir)
} catch {
    Fail "目标目录路径无效：$TargetDir"
}
# 尾随空格/点目标防御（DEC-005：检测即拒绝，不静默纠正）。
# PowerShell 路径解析保留尾随空格/点，而 robocopy/Win32 API 按剥离尾随空格/点的语义执行：
# 两套语义不一致会使上方裸盘符正则、下方卷根判定与嵌套/前缀比较全部失配（如 'D: '
# 解析为 'D:\works\qcnote\ ' 后 robocopy /MIR 实际作用于剥离尾空格的项目根），且部署后
# 校验按 PS 语义自洽通过、静默 exit 0 —— 无法安全部署，唯一处置是拒绝。
# 本校验须在卷根判定之前：点结尾形态同样会干扰 GetPathRoot 的卷根识别。
# 盘符段（形如 'D:'）不以空格/点结尾、解析后亦无空段残留，正常路径不受影响。
if ($TargetDir -match '[\s\.]$' -or @(($TargetDir -split '\\') -match '[\s\.]$').Count -gt 0) {
    Fail "目标目录路径含尾随空格/点或路径段以空格/点结尾，无法安全部署，请修正路径：$TargetDir"
}
# 卷根（如 D:\）作为 /MIR 目标会波及整盘，拒绝
if ([System.IO.Path]::GetPathRoot($TargetDir) -ieq $TargetDir) { Fail "拒绝部署到卷根：$TargetDir" }
# 归一化尾反斜杠：此时已排除卷根，去除安全；
# 不可对原始输入直接 TrimEnd——会把卷根 D:\ 折叠成裸盘符 D:，导致 robocopy 按盘符当前目录错位解析
if ($TargetDir.EndsWith('\')) { $TargetDir = $TargetDir.TrimEnd('\') }
if (-not $targetExists) {
    Write-Host "目标目录不存在，robocopy /MIR 将自动创建：$TargetDir"
}
if ($PortableDir -ieq $TargetDir) { Fail "产物目录与目标目录相同，拒绝部署：$TargetDir" }
# 产物目录与目标目录不可互为祖先/后代：目标在产物内部会自嵌套拷贝，
# 产物在目标内部则 /MIR 会把产物之外的其他内容（如 dist 下其他产物）当多余文件删除
if ($TargetDir.StartsWith($PortableDir + '\', [System.StringComparison]::OrdinalIgnoreCase) -or
    $PortableDir.StartsWith($TargetDir + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    Fail "产物目录与目标目录不可互为祖先/后代（$PortableDir <-> $TargetDir），拒绝部署"
}
Write-Host "目标目录：$TargetDir"

# 部署前目标目录 qcnote.ini 是否存在（供部署后比对保留状态）
$TargetIniPath = Join-Path $TargetDir 'qcnote.ini'
$TargetHadIni  = Test-Path -LiteralPath $TargetIniPath

# ---------- 4. 结束目标目录内运行中的 qcnote 进程 ----------
Step '结束目标目录内运行中的 qcnote 进程'
# 仅匹配 Path 以目标目录为前缀的实例（前缀拼接含目录分隔符，避免 D:\Program\qcnoteX 误匹配）；
# Path 为 null（权限不足读不到路径）的进程无法判定归属，安全跳过不动
$targetProcs    = @()
$skippedNoPath  = @()
foreach ($proc in @(Get-Process qcnote -ErrorAction SilentlyContinue)) {
    if (-not $proc.Path) { $skippedNoPath += $proc; continue }
    if ($proc.Path.StartsWith($TargetDir + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        $targetProcs += $proc
    }
}
if ($targetProcs.Count -eq 0) {
    Write-Host '目标目录内无运行中的 qcnote 进程，无需结束'
} else {
    foreach ($proc in $targetProcs) {
        Write-Host "结束进程 PID $($proc.Id)（$($proc.Path)）"
        try   { $stopped = Stop-Qcnote $proc }
        catch { Fail "结束进程 PID $($proc.Id) 失败（$($_.Exception.Message)）：$($proc.Path)" }
        if (-not $stopped) {
            Fail "进程结束失败（等待 $KillWaitTimeoutMs ms 仍存活）：PID $($proc.Id)（$($proc.Path)）"
        }
    }
    Start-Sleep -Milliseconds 500   # 等文件句柄释放，避免 robocopy 拷贝 exe 时遇占用
    Write-Host "已结束目标目录内全部 $($targetProcs.Count) 个 qcnote 进程"
}
if ($skippedNoPath.Count -gt 0) {
    Write-Host "提示：另有 $($skippedNoPath.Count) 个 qcnote 进程路径不可读（权限不足），已安全跳过" -ForegroundColor DarkGray
}

# ---------- 5. robocopy 镜像部署 ----------
Step 'robocopy 镜像部署（/MIR 同步清理残留 + /XF qcnote.ini 保留用户数据）'
# robocopy 退出码语义：0-7 均为成功（0=无文件拷贝，1=有文件拷贝），>=8 才判定失败；
# /R:2 /W:2 显式限制失败重试（robocopy 默认重试 1,000,000 次、每次等待 30 秒，
# 目标文件被占用时脚本会近乎挂死），快速失败交给下方退出码判定，失败语义不变
robocopy $PortableDir $TargetDir /MIR /XF qcnote.ini /R:2 /W:2
if ($LASTEXITCODE -ge 8) { Fail "robocopy 镜像部署失败（退出码 $LASTEXITCODE，>=8 判定失败；0-7 均为成功）" }
Write-Host "robocopy 成功（退出码 $LASTEXITCODE：1=有文件拷贝，0=目标已是最新无拷贝）"
Write-Host '已保留 qcnote.ini 用户数据（/XF 排除，不覆盖、不删除）'

# ---------- 6. 部署后校验 ----------
Step '部署后校验'
if (-not (Test-Path -LiteralPath (Join-Path $TargetDir 'qcnote.exe')) ) {
    Fail "部署后目标目录缺失 qcnote.exe：$TargetDir"
}
if (-not (Test-Path -LiteralPath (Join-Path $TargetDir 'qcnote.portable'))) {
    Fail "部署后目标目录缺失 qcnote.portable（便携标记未随产物部署）：$TargetDir"
}
$IniAfter = Test-Path -LiteralPath $TargetIniPath
if ($TargetHadIni -and -not $IniAfter) {
    Fail "部署后目标目录原有 qcnote.ini 丢失（/XF 保护失效，异常）：$TargetIniPath"
}

# ---------- 7. 部署摘要 ----------
$DeployedCount = @(Get-ChildItem -LiteralPath $TargetDir -Recurse -File -ErrorAction SilentlyContinue).Count
if ($TargetHadIni -and $IniAfter) {
    $IniState = '已保留（部署前已存在，用户会话数据未动）'
} elseif (-not $TargetHadIni -and -not $IniAfter) {
    $IniState = '不存在（首次部署，应用首次运行后自动生成）'
} else {
    $IniState = '存在（非本次部署写入，请人工确认来源）'   # /XF 排除下部署不会新增 ini
}
Write-Host ''
Write-Host '================== 部署完成 ==================' -ForegroundColor Green
Write-Host ("产物目录  ：{0}" -f $PortableDir)
Write-Host ("版本号    ：{0}" -f $Version)
Write-Host ("目标目录  ：{0}" -f $TargetDir)
Write-Host ("部署文件数：{0}" -f $DeployedCount)
Write-Host ("qcnote.ini：{0}" -f $IniState)
Write-Host '===============================================' -ForegroundColor Green
exit 0
