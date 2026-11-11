// qcnote 纯逻辑核心层：文本工具（M2-A）。
//
// 内容契约（DEC-010）：文档内容在内存中一律 UTF-8 std::string、LF 换行、无 BOM；
// 本文件提供磁盘读写边界使用的换行归一/探测、UTF-8 字符换算（状态栏/光标）与
// 内容 SHA-256 指纹（草稿基线哈希）。无 UI / Scintilla / Qt 依赖，全部可单测。
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace core {

/// 换行风格：原文件事实记录（保存时写回）；内存内容恒为 LF。
/// 新文档默认 Lf（需求 B1）。
enum class LineEnding {
    Lf,
    Crlf,
};

/// 换行显示名（状态栏）："LF" / "CRLF"
inline std::wstring_view lineEndingDisplayName(LineEnding e) {
    return e == LineEnding::Crlf ? L"CRLF" : L"LF";
}

/// CRLF/CR → LF 单遍归一（读取路径用；孤立 CR 亦归一为 LF，与旧版语义一致）。
/// 无 CR 时原样返回（拷贝）。
std::string normalizeLineEndings(std::string_view text);

/// 探测换行风格（旧版 localstorage 语义移植）：任一 "\r\n" → Crlf；否则 Lf。
/// 孤立 CR 归一为 LF 后按 Lf 记录；空文件/单行无换行 → Lf。
LineEnding detectLineEnding(std::string_view text);

// —— UTF-8 字符换算（供状态栏字符数与光标换算；M2-B Scintilla 接线用）——
// 约定：内容为合法 UTF-8（读路径已归一）；遇无效字节按每字节 1 字符计，
// 与解码边界的 U+FFFD 替换呈现一一对应，计数稳定。

/// 字符（Unicode 码点）计数
std::size_t utf8CharCount(std::string_view text);

/// 字符索引 → 字节偏移；charIndex 越界钳制到 [0, size]
std::size_t utf8CharIndexToByteOffset(std::string_view text, std::size_t charIndex);

/// 字节偏移 → 字符索引；byteOffset 越界钳制到 [0, size]，
/// 落在多字节序列中间时按该序列起始字符计数（即该字符的索引）
std::size_t utf8ByteOffsetToCharIndex(std::string_view text, std::size_t byteOffset);

/// 内容 SHA-256 指纹（64 个 hex 小写字符；BCrypt 实现，链 bcrypt.lib）。
/// 草稿基线哈希用（检测磁盘是否被外部修改）；失败返回空串（理论不应发生）
std::string sha256Hex(std::string_view data);

} // namespace core
