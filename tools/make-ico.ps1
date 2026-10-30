# make-ico.ps1 —— 把 qcnote-<size>.png 系列打包为多尺寸 ICO（PNG 压缩条目，Vista+ 标准做法）
# 用法：pwsh.exe -NoProfile -File tools\make-ico.ps1 -PngDir <png目录> -Sizes 16,24,32,48,64,256 -Out <输出.ico>
param(
    [Parameter(Mandatory = $true)][string]$PngDir,
    [Parameter(Mandatory = $true)][string[]]$Sizes,
    [Parameter(Mandatory = $true)][string]$Out
)

# 输出编码固定为 UTF-8，避免中文乱码（全局约定）
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 | Out-Null

# 兼容两种传参：-File 调用时 "16,24,32" 是单个字符串；pwsh 内调用时可能是数组
$sizeList = @($Sizes | ForEach-Object { $_ -split ',' } |
    ForEach-Object { [int]$_.Trim() } | Sort-Object)
if ($sizeList.Count -lt 1) { throw "尺寸列表为空" }

# 尺寸合法性：ICONDIRENTRY 宽高各占 1 字节（256 以 0 表示），仅支持 1..256。
# 传入超界值（如 512）会生成宽高字节 0 但实际数据不符的非规范 ICO，必须在源头拒绝。
$invalid = @($sizeList | Where-Object { $_ -lt 1 -or $_ -gt 256 })
if ($invalid.Count -gt 0) {
    throw ("非法尺寸值: {0}；每项必须为 1..256 的整数（ICO 条目宽高字段为单字节，256 以 0 表示）" -f ($invalid -join ", "))
}

# 重复尺寸会生成重复条目的非规范 ICO，同样在源头拒绝（与非法尺寸同一错误通道）
$dupes = @($sizeList | Group-Object | Where-Object { $_.Count -gt 1 } | ForEach-Object { [int]$_.Name })
if ($dupes.Count -gt 0) {
    throw ("重复尺寸值: {0}；每个尺寸只能出现一次（重复条目为非规范 ICO）" -f ($dupes -join ", "))
}

# 读取各尺寸 PNG 原始字节
$entries = @()
foreach ($size in $sizeList) {
    $pngPath = Join-Path $PngDir ("qcnote-{0}.png" -f $size)
    if (-not (Test-Path -LiteralPath $pngPath)) { throw "缺少 PNG: $pngPath" }
    $bytes = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $pngPath).Path)
    if ($bytes.Length -lt 8 -or $bytes[0] -ne 0x89 -or $bytes[1] -ne 0x50) {
        throw "不是有效 PNG: $pngPath"
    }
    $entries += [pscustomobject]@{ Size = $size; Bytes = $bytes }
}

$outFull = [System.IO.Path]::GetFullPath($Out)
$outParent = Split-Path -Parent $outFull
if ($outParent -and -not (Test-Path -LiteralPath $outParent)) {
    New-Item -ItemType Directory -Path $outParent -Force | Out-Null
}

$stream = $null
$writer = $null
try {
    $stream = [System.IO.File]::Create($outFull)
    $writer = New-Object System.IO.BinaryWriter($stream)

    # ICONDIR（6 字节）：reserved=0、type=1(icon)、count=条目数
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]1)
    $writer.Write([UInt16]$entries.Count)

    # ICONDIRENTRY（每条 16 字节）：宽高各 1 字节（256 写 0）、颜色数=0、保留=0、
    # planes=1、bitcount=32、PNG 字节长度、数据偏移
    $offset = 6 + 16 * $entries.Count
    foreach ($e in $entries) {
        $dimByte = if ($e.Size -ge 256) { [byte]0 } else { [byte]$e.Size }
        $writer.Write($dimByte)                    # 宽
        $writer.Write($dimByte)                    # 高（正方形图标，同宽）
        $writer.Write([byte]0)                     # 颜色数
        $writer.Write([byte]0)                     # 保留
        $writer.Write([UInt16]1)                   # 颜色平面数
        $writer.Write([UInt16]32)                  # 位深
        $writer.Write([UInt32]$e.Bytes.Length)     # PNG 字节长度
        $writer.Write([UInt32]$offset)             # PNG 数据偏移
        $offset += $e.Bytes.Length
    }

    # 依次拼接 PNG 原始字节（不做改写）
    foreach ($e in $entries) { $writer.Write($e.Bytes) }

    $writer.Flush()
}
finally {
    if ($writer) { $writer.Dispose() } elseif ($stream) { $stream.Dispose() }
}

$total = (Get-Item -LiteralPath $outFull).Length
Write-Output ("ICO 已生成: {0}" -f $outFull)
Write-Output ("条目数: {0}，文件总长: {1} 字节" -f $entries.Count, $total)
foreach ($e in $entries) {
    Write-Output ("  {0,3}x{0,-3}  {1,7} 字节" -f $e.Size, $e.Bytes.Length)
}
