# verify-icon.ps1 —— 应用图标资产与嵌入验证（持久化回归脚本）
# 对应用例：docs/测试用例/测试用例-添加应用图标.md
#   TC-001: ICO 文件格式与尺寸完整性（二进制解析 ICONDIR + ICONDIRENTRY）
#   TC-002: exe 嵌入图标（ExtractAssociatedIcon + 色彩分布统计）
#   TC-004: offscreen 冒烟运行（需 -WithSmoke；禁止可见窗口；仅验证启动不崩溃）
#   TC-006: 生成链重现（需 -WithRegen 与 build-icongen\icongen.exe；输出到临时目录，不覆盖 src/icons/qcnote.ico）
#   TC-007: 视觉验收 —— 不在本脚本范围（需用户肉眼确认）
#   TC-008: setWindowIcon 资源嵌入回归（双断言：UTF-16 LE 完整路径 + UTF-16 BE rcc 资源树段名）
# 用法：
#   pwsh.exe -NoProfile -ExecutionPolicy Bypass -File tests\icons\verify-icon.ps1 [-ProjectRoot <项目根>] [-WithSmoke] [-WithRegen] [-SkipExe]
# 退出码：0 = 全部通过；1 = 存在失败；2 = 存在跳过（无失败）
#Requires -Version 7
# 上行 #Requires：脚本依赖 [System.Text.Encoding]::Latin1（需 .NET 5+，Windows PowerShell 5.1 不支持），
# 且脚本头用法已限定 pwsh.exe（PowerShell 7+）；版本不满足时 PowerShell 在执行前直接报错退出，错误前置。

param(
    [string]$ProjectRoot = "",
    [switch]$WithSmoke,   # 执行 TC-004 offscreen 冒烟
    [switch]$WithRegen,   # 执行 TC-006 生成链重现
    [switch]$SkipExe      # 跳过 TC-002/TC-008（如未构建 exe 时）
)

# 输出编码固定为 UTF-8，避免中文乱码（全局约定）
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null

if (-not $ProjectRoot) { $ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path }

# 保存环境变量原值：脚本会临时改写 QT_QPA_PLATFORM 与 PATH，
# 所有退出路径（含异常提前退出）经 Restore-ScriptEnv 统一恢复，不污染调用方会话。
$script:OrigQpaPlatform = $env:QT_QPA_PLATFORM
$script:OrigPath        = $env:PATH

function Restore-ScriptEnv {
    if ($null -ne $script:OrigQpaPlatform) { $env:QT_QPA_PLATFORM = $script:OrigQpaPlatform }
    else { Remove-Item Env:\QT_QPA_PLATFORM -ErrorAction SilentlyContinue }
    if ($null -ne $script:OrigPath) { $env:PATH = $script:OrigPath }
    else { Remove-Item Env:\PATH -ErrorAction SilentlyContinue }
}

$script:Results = @()   # @{ Id; Name; Pass; Skip; Detail }
$script:FailCount = 0
$script:SkipCount = 0

function Add-Result {
    param([string]$Id, [string]$Name, [bool]$Pass, [bool]$Skip = $false, [string]$Detail = "")
    $script:Results += [pscustomobject]@{ Id = $Id; Name = $Name; Pass = $Pass; Skip = $Skip; Detail = $Detail }
    if ($Skip) { $script:SkipCount++ }
    elseif (-not $Pass) { $script:FailCount++ }
}

# ----------------------------------------------------------------------
# ICO 二进制解析：返回条目列表（TC-001 / TC-006 共用）
# ----------------------------------------------------------------------
function Parse-Ico {
    param([string]$Path)
    $bytes = [System.IO.File]::ReadAllBytes($Path)

    $reserved = [BitConverter]::ToUInt16($bytes, 0)
    $type     = [BitConverter]::ToUInt16($bytes, 2)
    $count    = [BitConverter]::ToUInt16($bytes, 4)

    $entries = @()
    $expectedOffset = 6 + 16 * $count
    for ($i = 0; $i -lt $count; $i++) {
        $base = 6 + 16 * $i
        $wByte     = $bytes[$base]
        $hByte     = $bytes[$base + 1]
        $colorCnt  = $bytes[$base + 2]
        $reservedB = $bytes[$base + 3]
        $planes    = [BitConverter]::ToUInt16($bytes, $base + 4)
        $bitCount  = [BitConverter]::ToUInt16($bytes, $base + 6)
        $dataLen   = [BitConverter]::ToUInt32($bytes, $base + 8)
        $offset    = [BitConverter]::ToUInt32($bytes, $base + 12)

        # 宽高字节 0 表示 256
        $dim = if ($wByte -eq 0) { 256 } else { [int]$wByte }

        # PNG 魔数与 IHDR 宽高（大端）
        $pngMagicOk = ($offset + 8 -le $bytes.Length) -and
            ($bytes[$offset] -eq 0x89) -and ($bytes[$offset + 1] -eq 0x50) -and
            ($bytes[$offset + 2] -eq 0x4E) -and ($bytes[$offset + 3] -eq 0x47) -and
            ($bytes[$offset + 4] -eq 0x0D) -and ($bytes[$offset + 5] -eq 0x0A) -and
            ($bytes[$offset + 6] -eq 0x1A) -and ($bytes[$offset + 7] -eq 0x0A)
        $ihdrW = $ihdrH = -1
        $ihdrTagOk = $false
        if ($offset + 24 -le $bytes.Length) {
            $ihdrTagOk = ($bytes[$offset +  12] -eq 0x49) -and ($bytes[$offset + 13] -eq 0x48) -and
                         ($bytes[$offset + 14] -eq 0x44) -and ($bytes[$offset + 15] -eq 0x52)  # "IHDR"
            $ihdrW = ([int]$bytes[$offset + 16] -shl 24) -bor ([int]$bytes[$offset + 17] -shl 16) -bor
                     ([int]$bytes[$offset + 18] -shl 8)  -bor  [int]$bytes[$offset + 19]
            $ihdrH = ([int]$bytes[$offset + 20] -shl 24) -bor ([int]$bytes[$offset + 21] -shl 16) -bor
                     ([int]$bytes[$offset + 22] -shl 8)  -bor  [int]$bytes[$offset + 23]
        }

        $entries += [pscustomobject]@{
            Index = $i; Dim = $dim; WByte = $wByte; HByte = $hByte
            ColorCount = $colorCnt; ReservedByte = $reservedB
            Planes = $planes; BitCount = $bitCount
            DataLen = $dataLen; Offset = $offset
            OffsetContiguous = ($offset -eq $expectedOffset)
            PngMagicOk = $pngMagicOk; IhdrTagOk = $ihdrTagOk
            IhdrW = $ihdrW; IhdrH = $ihdrH
            IhdrMatch = ($ihdrW -eq $dim) -and ($ihdrH -eq $dim)
        }
        $expectedOffset += $dataLen
    }

    [pscustomobject]@{
        Reserved = $reserved; Type = $type; Count = $count
        Entries = $entries
        FileLen = $bytes.Length
        FileLenOk = ($bytes.Length -eq $expectedOffset)  # 数据紧接目录区、无空洞无多余
    }
}

# ----------------------------------------------------------------------
# TC-001: ICO 文件格式与尺寸完整性
# ----------------------------------------------------------------------
function Test-TC001 {
    $icoPath = Join-Path $ProjectRoot "src\icons\qcnote.ico"
    if (-not (Test-Path -LiteralPath $icoPath)) {
        Add-Result "TC-001" "ICO 文件格式与尺寸完整性" $false $false "文件不存在: $icoPath"
        return
    }
    # ICO 严重损坏（count 与实际长度不符等）时 BitConverter 会抛异常，转用例 FAIL 而非异常栈
    try { $ico = Parse-Ico $icoPath }
    catch {
        Add-Result "TC-001" "ICO 文件格式与尺寸完整性" $false $false "ICO 解析异常: $($_.Exception.Message)"
        return
    }

    $detail = @()
    $ok = $true
    # 头部：reserved=0、type=1(icon)、条目数=6
    if ($ico.Reserved -ne 0) { $ok = $false; $detail += "reserved=$($ico.Reserved) 应为 0" }
    if ($ico.Type -ne 1)     { $ok = $false; $detail += "type=$($ico.Type) 应为 1" }
    if ($ico.Count -ne 6)    { $ok = $false; $detail += "条目数=$($ico.Count) 应为 6" }
    # 六档尺寸
    $dims = @($ico.Entries | ForEach-Object { $_.Dim } | Sort-Object)
    $expect = @(16, 24, 32, 48, 64, 256)
    if (($dims -join ",") -ne ($expect -join ",")) { $ok = $false; $detail += "尺寸集: $($dims -join ',') 应为 $($expect -join ',')" }
    foreach ($e in $ico.Entries) {
        $name = "$($e.Dim)px"
        if ($e.Dim -eq 256) {
            if ($e.WByte -ne 0 -or $e.HByte -ne 0) { $ok = $false; $detail += "$name 宽高字节应为 0（表示 256），实际 W=$($e.WByte) H=$($e.HByte)" }
        } else {
            if ($e.WByte -ne $e.Dim -or $e.HByte -ne $e.Dim) { $ok = $false; $detail += "$name 宽高字节 W=$($e.WByte) H=$($e.HByte) 与尺寸不符" }
        }
        if ($e.Planes -ne 1)          { $ok = $false; $detail += "$name planes=$($e.Planes) 应为 1" }
        if ($e.BitCount -ne 32)       { $ok = $false; $detail += "$name bitCount=$($e.BitCount) 应为 32" }
        if ($e.ColorCount -ne 0)      { $ok = $false; $detail += "$name colorCount=$($e.ColorCount) 应为 0" }
        if ($e.ReservedByte -ne 0)    { $ok = $false; $detail += "$name 保留字节=$($e.ReservedByte) 应为 0" }
        if (-not $e.OffsetContiguous) { $ok = $false; $detail += "$name 数据偏移 $($e.Offset) 不连续" }
        if (-not $e.PngMagicOk)       { $ok = $false; $detail += "$name 数据非合法 PNG 魔数" }
        if (-not $e.IhdrTagOk)        { $ok = $false; $detail += "$name 缺少 IHDR 块" }
        if (-not $e.IhdrMatch)        { $ok = $false; $detail += "$name IHDR $($e.IhdrW)x$($e.IhdrH) 与条目尺寸不符" }
    }
    if (-not $ico.FileLenOk) { $ok = $false; $detail += "文件长度 $($ico.FileLen) 与目录区+数据总长不符" }

    $sizeSummary = ($ico.Entries | ForEach-Object { "{0}({1}B)" -f $_.Dim, $_.DataLen }) -join " "
    if ($ok) { Add-Result "TC-001" "ICO 文件格式与尺寸完整性" $true $false "6 条目: $sizeSummary；PNG 合法、偏移连续" }
    else     { Add-Result "TC-001" "ICO 文件格式与尺寸完整性" $false $false ($detail -join "; ") }
}

# ----------------------------------------------------------------------
# TC-002: exe 嵌入图标（提取 + 色彩分布）
# ----------------------------------------------------------------------
function Test-TC002 {
    $exePath = Join-Path $ProjectRoot "build\qcnote.exe"
    if ($SkipExe) {
        Add-Result "TC-002" "exe 嵌入图标" $true $true "指定 -SkipExe 跳过"
        return
    }
    if (-not (Test-Path -LiteralPath $exePath)) {
        Add-Result "TC-002" "exe 嵌入图标" $false $false "exe 不存在: $exePath（先执行完整构建）"
        return
    }

    Add-Type -AssemblyName System.Drawing
    $icon = [System.Drawing.Icon]::ExtractAssociatedIcon($exePath)
    if ($null -eq $icon) {
        Add-Result "TC-002" "exe 嵌入图标" $false $false "ExtractAssociatedIcon 返回 null（无嵌入图标）"
        return
    }
    if ($icon.Width -le 0 -or $icon.Height -le 0) {
        Add-Result "TC-002" "exe 嵌入图标" $false $false "图标尺寸异常: $($icon.Width)x$($icon.Height)"
        $icon.Dispose(); return
    }

    # 色彩分布统计：品牌蓝 #3564d4 (53,100,212) 主导 + 白色文档字形，无其他显著色调
    $w = $icon.Width; $h = $icon.Height
    $bmp = $icon.ToBitmap()
    $n = $blue = $white = $transparent = $other = 0
    for ($x = 0; $x -lt $bmp.Width; $x++) {
        for ($y = 0; $y -lt $bmp.Height; $y++) {
            $c = $bmp.GetPixel($x, $y); $n++
            if ($c.A -lt 32) { $transparent++; continue }
            if ($c.R -ge 225 -and $c.G -ge 225 -and $c.B -ge 225) { $white++; continue }
            # 蓝（含抗锯齿深浅蓝）：B 明显大于 R、亮度足够
            if ($c.B -ge 140 -and $c.B -gt ($c.R + 40)) { $blue++; continue }
            $other++
        }
    }
    $bmp.Dispose(); $icon.Dispose()

    $pBlue = 100.0 * $blue / $n; $pWhite = 100.0 * $white / $n
    $pTrans = 100.0 * $transparent / $n; $pOther = 100.0 * $other / $n

    $detail = @()
    $ok = $true
    if ($pBlue -lt 20)  { $ok = $false; $detail += ("蓝色占比 {0:F1}% 低于 20%（未体现蓝底主导）" -f $pBlue) }
    if ($pWhite -lt 1)  { $ok = $false; $detail += ("白色占比 {0:F1}% 低于 1%（未见白色文档字形）" -f $pWhite) }
    if ($pOther -gt 30) { $ok = $false; $detail += ("杂色占比 {0:F1}% 高于 30%（存在非蓝白主色调）" -f $pOther) }

    $msg = ("提取 {0}x{1}；色彩分布 蓝 {2:F1}% / 白 {3:F1}% / 透明 {4:F1}% / 其他 {5:F1}%（含抗锯齿过渡）" -f `
        $w, $h, $pBlue, $pWhite, $pTrans, $pOther)
    if ($ok) { Add-Result "TC-002" "exe 嵌入图标" $true $false $msg }
    else     { Add-Result "TC-002" "exe 嵌入图标" $false $false ($detail -join "; ") }
}

# ----------------------------------------------------------------------
# TC-004: offscreen 冒烟运行（无可见窗口）
# ----------------------------------------------------------------------
function Test-TC004 {
    $exePath = Join-Path $ProjectRoot "build\qcnote.exe"
    if (-not (Test-Path -LiteralPath $exePath)) {
        Add-Result "TC-004" "offscreen 冒烟运行" $false $false "exe 不存在: $exePath"
        return
    }
    $env:QT_QPA_PLATFORM = "offscreen"
    $env:PATH = "D:\Program\Qt\6.11.2\mingw_64\bin;D:\Program\Qt\Tools\mingw1310_64\bin;" + $env:PATH
    $proc = Start-Process -FilePath $exePath -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 4
    $alive = -not $proc.HasExited
    $exitNote = ""
    if (-not $alive) { $exitNote = "（提前退出，ExitCode=$($proc.ExitCode)）" }
    # 仅结束本用例启动的进程（按 PID），禁止按进程名强杀——避免误杀机器上运行中的其他 qcnote 实例；
    # -ErrorAction SilentlyContinue：进程恰好在此瞬间自行退出时忽略 non-terminating 错误（竞态）
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    # 环境变量（QT_QPA_PLATFORM/PATH）不在此处恢复，统一由脚本级 finally 的 Restore-ScriptEnv 恢复

    if ($alive) { Add-Result "TC-004" "offscreen 冒烟运行" $true $false "offscreen 启动后存活 >= 4s（冒烟仅验证启动不崩溃；setWindowIcon 资源路径正确性由 TC-008 验证），已结束进程" }
    else        { Add-Result "TC-004" "offscreen 冒烟运行" $false $false "进程启动后 4s 内退出$exitNote" }
}

# ----------------------------------------------------------------------
# TC-006: 生成链重现（icongen + make-ico.ps1 → 临时目录，结构比对）
# ----------------------------------------------------------------------
function Test-TC006 {
    $icongen = Join-Path $ProjectRoot "build-icongen\icongen.exe"
    $makeIco = Join-Path $ProjectRoot "tools\make-ico.ps1"
    if (-not (Test-Path -LiteralPath $icongen)) {
        Add-Result "TC-006" "生成链可重现" $true $true "icongen.exe 不存在（$icongen），需先独立构建 tools/icongen"
        return
    }
    $tmpDir = Join-Path ([System.IO.Path]::GetTempPath()) ("qcnote-icon-regen-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
    New-Item -ItemType Directory -Path $tmpDir -Force | Out-Null
    # icongen.exe 依赖 Qt6 Svg/Gui/Core DLL
    $env:PATH = "D:\Program\Qt\6.11.2\mingw_64\bin;D:\Program\Qt\Tools\mingw1310_64\bin;" + $env:PATH
    try {
        # 1) SVG → PNG（六档）
        $pngDir = Join-Path $tmpDir "png"
        & $icongen (Join-Path $ProjectRoot "src\icons\qcnote.svg") $pngDir | Out-Null
        if ($LASTEXITCODE -ne 0) { Add-Result "TC-006" "生成链可重现" $false $false "icongen 退出码 $LASTEXITCODE"; return }
        # 2) PNG → ICO（临时输出，不覆盖 src/icons/qcnote.ico）
        $regenIco = Join-Path $tmpDir "qcnote.ico"
        & pwsh.exe -NoProfile -ExecutionPolicy Bypass -File $makeIco -PngDir $pngDir -Sizes "16,24,32,48,64,256" -Out $regenIco | Out-Null
        if ($LASTEXITCODE -ne 0) { Add-Result "TC-006" "生成链可重现" $false $false "make-ico.ps1 退出码 $LASTEXITCODE"; return }
        # 3) 结构比对（六档尺寸 + 条目参数 + PNG 尺寸）
        try {
            $a = Parse-Ico (Join-Path $ProjectRoot "src\icons\qcnote.ico")
            $b = Parse-Ico $regenIco
        }
        catch {
            Add-Result "TC-006" "生成链可重现" $false $false "ICO 解析异常: $($_.Exception.Message)"
            return
        }
        $detail = @(); $ok = $true
        if ($b.Count -ne 6) { $ok = $false; $detail += "重生成条目数 $($b.Count) 应为 6" }
        $dimsA = @($a.Entries | ForEach-Object { $_.Dim } | Sort-Object) -join ","
        $dimsB = @($b.Entries | ForEach-Object { $_.Dim } | Sort-Object) -join ","
        if ($dimsA -ne $dimsB) { $ok = $false; $detail += "尺寸集不一致: 入库=$dimsA 重生成=$dimsB" }
        foreach ($eb in $b.Entries) {
            if ($eb.Planes -ne 1 -or $eb.BitCount -ne 32 -or -not $eb.PngMagicOk -or -not $eb.IhdrMatch) {
                $ok = $false; $detail += "重生成 $($eb.Dim)px 条目参数异常"
            }
        }
        $hashA = (Get-FileHash (Join-Path $ProjectRoot "src\icons\qcnote.ico") -Algorithm SHA256).Hash
        $hashB = (Get-FileHash $regenIco -Algorithm SHA256).Hash
        # 输出哈希值本身，供测试报告直接引用对比（判定只依据是否相等，与哈希值展示无关）
        $byteNote = if ($hashA -eq $hashB) { "SHA256 字节级一致: $hashA" }
                    else { "SHA256 不同（光栅化允许差异），结构一致；入库=$hashA 重生成=$hashB" }
        if ($ok) { Add-Result "TC-006" "生成链可重现" $true $false "重生成 ICO 六档结构一致；$byteNote" }
        else     { Add-Result "TC-006" "生成链可重现" $false $false ($detail -join "; ") }
    }
    finally {
        Remove-Item -LiteralPath $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# ----------------------------------------------------------------------
# TC-008: setWindowIcon 资源嵌入回归（双断言合取）
#   QIcon 加载失败不崩溃不报错，TC-004 冒烟无法发现路径错误或资源漏配。
#   断言 A（UTF-16 LE 完整路径）：搜索 "qt/qml/qcnote/icons/qcnote.svg" 的
#     LE 序列——它只来自 main.cpp 的 QStringLiteral 字面量池，可检出
#     "main.cpp 路径写错/未编译进 exe"；但 CMake RESOURCES 漏配该 SVG 时
#     字面量仍在，单靠断言 A 会误 PASS（REVIEW-2 问题 1）。
#   断言 B（UTF-16 BE rcc 资源树段名）：Qt rcc 资源树不存完整路径，其
#     qt_resourceName 条目为「2 字节大端长度 + 4 字节哈希 + UTF-16 BE 单段名」；
#     搜索 BE 段名并校验命中前 6 字节的前 2 字节恰为段名字符数（UTF-16 码元数，
#     非字节数：qcnote.svg 10 字符 → 00 0A；qcnote 段 6 字符 → 00 06 而非 00 0C）
#     的大端编码，可证明 SVG 真实经 qt_add_qml_module RESOURCES
#     嵌入资源树（检出"资源漏配"）。注：LE 字面量区内会出现无前缀的 BE 伪命中，
#     前缀校验是鉴别器。辅助段名 icons（前缀 00 05）命中时计入 PASS 明细。
#   边界（REVIEW-3 可选-2，接受现状）：断言 B 仅校验单段名存在性，不含父路径
#     佐证；"SVG 移位且 CMake 与 main.cpp 字面量同时系统性改错"的复合场景不在
#     回归威胁模型内（防意外漏配，非防人为复合改错）。
#   两条断言都通过，TC-008 才 PASS。
# ----------------------------------------------------------------------
function Find-RccSegmentHits {
    # 在 Latin1 映射的 exe 全字节中搜索段名的 UTF-16 BE 序列的所有命中，
    # 仅保留前方第 6/5 字节恰为「段名字符数（UTF-16 码元数）大端编码」的命中
    # （rcc qt_resourceName 条目），返回有效命中的字节偏移数组（无则为空数组）
    param([string]$Hay, [byte[]]$ExeBytes, [string]$SegmentName)
    $needle = [System.Text.Encoding]::Latin1.GetString(
        [System.Text.Encoding]::BigEndianUnicode.GetBytes($SegmentName))
    $lenHi = [byte](($SegmentName.Length -shr 8) -band 0xFF)
    $lenLo = [byte]($SegmentName.Length -band 0xFF)
    $valid = @()
    $start = 0
    while ($true) {
        $i = $Hay.IndexOf($needle, $start, [System.StringComparison]::Ordinal)
        if ($i -lt 0) { break }
        if ($i -ge 6 -and $ExeBytes[$i - 6] -eq $lenHi -and $ExeBytes[$i - 5] -eq $lenLo) { $valid += $i }
        $start = $i + 1
    }
    return ,$valid
}

function Test-TC008 {
    $exePath = Join-Path $ProjectRoot "build\qcnote.exe"
    if ($SkipExe) {
        Add-Result "TC-008" "setWindowIcon 资源嵌入回归" $true $true "指定 -SkipExe 跳过"
        return
    }
    if (-not (Test-Path -LiteralPath $exePath)) {
        Add-Result "TC-008" "setWindowIcon 资源嵌入回归" $false $false "exe 不存在: $exePath（先执行完整构建）"
        return
    }

    # 借助 Latin-1 与字节的双射关系把字节序列映射为字符串后做 Ordinal 搜索（等价于字节序列搜索，且避免逐字节循环）
    $resourcePath = "qt/qml/qcnote/icons/qcnote.svg"
    $exeBytes = [System.IO.File]::ReadAllBytes($exePath)
    $latin1 = [System.Text.Encoding]::Latin1
    $hay    = $latin1.GetString($exeBytes)

    # 断言 A：UTF-16 LE 完整路径（来源为 main.cpp 字面量池）
    $leNeedle = $latin1.GetString([System.Text.Encoding]::Unicode.GetBytes($resourcePath))
    $leIndex  = $hay.IndexOf($leNeedle, [System.StringComparison]::Ordinal)

    # 断言 B：UTF-16 BE rcc 资源树段名（大端长度前缀校验）
    $svgHits   = Find-RccSegmentHits -Hay $hay -ExeBytes $exeBytes -SegmentName "qcnote.svg"
    $iconsHits = Find-RccSegmentHits -Hay $hay -ExeBytes $exeBytes -SegmentName "icons"

    $detail = @()
    if ($leIndex -lt 0) {
        $detail += ("断言A FAIL：未找到资源路径 {0} 的 UTF-16 LE 序列（main.cpp setWindowIcon 引用路径未编译进 exe 或路径写错；QIcon 加载失败不崩溃，冒烟无法发现）" -f $resourcePath)
    }
    if ($svgHits.Count -lt 1) {
        $detail += "断言B FAIL：未找到带 00 0A 大端长度前缀的 UTF-16 BE 段名 qcnote.svg（SVG 未经 CMake RESOURCES 嵌入 rcc 资源树，即资源漏配；资源树不存完整路径，LE 完整路径命中无法证明资源已嵌入）"
    }

    if ($detail.Count -eq 0) {
        $msg = ("双断言通过——断言A：资源路径 {0} 的 UTF-16 LE 序列命中 @偏移 {1}（main.cpp 引用路径已编译进 exe）；断言B：rcc 资源树段名 qcnote.svg 的 BE 条目命中 @偏移 {2}（前 6 字节含 00 0A 长度前缀，资源真实嵌入）" -f $resourcePath, $leIndex, ($svgHits -join ", "))
        if ($iconsHits.Count -ge 1) { $msg += ("；辅助段名 icons（前缀 00 05）命中 @偏移 {0}" -f ($iconsHits -join ", ")) }
        Add-Result "TC-008" "setWindowIcon 资源嵌入回归" $true $false $msg
    }
    else {
        Add-Result "TC-008" "setWindowIcon 资源嵌入回归" $false $false ($detail -join "; ")
    }
}

# ----------------------------------------------------------------------
# 执行（异常兜底转 FAIL；finally 统一恢复环境变量原值）
# ----------------------------------------------------------------------
try {
    Test-TC001
    Test-TC002
    if ($WithSmoke) { Test-TC004 } else { Add-Result "TC-004" "offscreen 冒烟运行" $true $true "未指定 -WithSmoke" }
    if ($WithRegen) { Test-TC006 } else { Add-Result "TC-006" "生成链可重现" $true $true "未指定 -WithRegen" }
    Test-TC008
}
catch {
    Add-Result "ERROR" "未捕获异常" $false $false $_.Exception.Message
}
finally {
    Restore-ScriptEnv
}

Write-Output ""
Write-Output "==================== 图标验证结果（verify-icon.ps1） ===================="
foreach ($r in $script:Results) {
    $state = if ($r.Skip) { "SKIP" } elseif ($r.Pass) { "PASS" } else { "FAIL" }
    Write-Output ("[{0}] {1} {2}" -f $state, $r.Id, $r.Name)
    if ($r.Detail) { Write-Output ("       {0}" -f $r.Detail) }
}
Write-Output "========================================================================"
Write-Output ("TC-007 视觉验收：不在自动化范围，留待用户肉眼确认（蓝底圆角方板 + 白色文档字形）")

if ($script:FailCount -gt 0) { exit 1 }
elseif ($script:SkipCount -gt 0) { exit 2 }
else { exit 0 }
