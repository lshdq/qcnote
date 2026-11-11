// qcnote 纯逻辑核心层：编码（M2-A，需求 B1 / 旧《需求-GBK与UTF8编码支持》全语义移植）。
//
// 内存表示 UTF-8 std::string（DEC-010）；GBK 仅在本层磁盘读写边界经 UTF-16
// 一次性中转（Win32 NLS：MultiByteToWideChar/WideCharToMultiByte，CP936）。
// 用户可见消息一律 std::wstring（Win32 界面原生编码；界面层零转换）。
#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace core {

/// 文档编码（仅两种，界面统一显示 "UTF-8"/"GBK"）
enum class TextEncoding {
    Utf8,
    Gbk,
};

/// 编码显示名（状态栏）；带 BOM 的 UTF-8 由调用方按 hasBom 追加 " BOM"
inline std::wstring encodingDisplayName(TextEncoding e) {
    return e == TextEncoding::Gbk ? L"GBK" : L"UTF-8";
}

/// UTF-8 BOM 字节（3 字节）
inline constexpr std::string_view kUtf8Bom{"\xEF\xBB\xBF", 3};

/// 剥离前导 UTF-8 BOM；无 BOM 时原样返回同一视图
inline std::string_view stripUtf8Bom(std::string_view bytes) {
    return bytes.starts_with(kUtf8Bom) ? bytes.substr(3) : bytes;
}

/// 自动识别结果：encoding + 是否带 UTF-8 BOM（仅 Utf8 有意义）
struct EncodingDetection {
    TextEncoding encoding = TextEncoding::Utf8;
    bool hasBom = false;
};

/// 编码自动识别（策略与旧 EncodingDetector 一致）：
/// ① 前 3 字节为 UTF-8 BOM → Utf8 + hasBom（BOM 为权威声明，后续字节非法也不回退）；
/// ② 严格 UTF-8 校验通过（含纯 ASCII）→ Utf8 无 BOM；
/// ③ 其余 → Gbk。空输入判 Utf8（空文件是合法 UTF-8）。
/// 已知误判（预期行为）：GBK 字节恰好构成合法 UTF-8 时判 Utf8，由「按编码重新加载」兜底。
EncodingDetection detectEncoding(std::string_view raw);

/// 严格 UTF-8 校验（单遍状态机）：拒绝 C0/C1 与 F5+ 首字节、过长编码、
/// UTF-16 代理区（U+D800..DFFF）、越界码点（>U+10FFFF）与尾部截断序列。
/// 尾部截断计为非法（对齐旧 Qt Stateless 语义：不得静默吞掉悬空字节）。
bool isValidUtf8(std::string_view text);

/// 宽松 UTF-8 解码：非法序列按「最大无效子序列」替换为一个 U+FFFD
/// （EF BF BD），合法序列原样保留。读路径不因非法字节失败（保证文件始终可打开）；
/// 用于 BOM 权威路径与强制 UTF-8 解码。
std::string sanitizeUtf8(std::string_view bytes);

/// UTF-8 ↔ UTF-16 转换（界面/消息边界；实现于 Win32 CP_UTF8，无效输入替换 U+FFFD）
std::wstring utf8ToWide(std::string_view utf8);
std::string wideToUtf8(std::wstring_view wide);

/// GBK 解码失败（仅系统缺少 CP936 支持等完全无法工作情形；错误消息为中文）
/// 供上层包装 StorageError 使用

/// GBK（CP936）解码：字节 → UTF-8 std::string（内部经 UTF-16 中转）。
/// 无法解码的字节以 U+FFFD 呈现（保证文件可打开、后续内容不丢）：
/// 快路径整块严格解码，失败则按 GBK 双字节结构分段回退（旧 gbkcodec 语义）。
std::expected<std::string, std::wstring> decodeGbk(std::string_view raw);

/// GBK 编码校验失败详情：首个不可编码字符 + 所在行号（从 1 起，按 '\n' 计数）
struct GbkEncodeFailure {
    std::wstring character; ///< 首个不可编码字符（代理对整体，如 emoji）
    int line = 1;           ///< 该字符所在行号；0 表示未能定位具体字符
};

/// GBK（CP936）编码：UTF-8 内容 → CP936 字节流。写盘前全量可编码校验：
/// 任一字符无直接 CP936 映射（含 best-fit 近似，WC_NO_BEST_FIT_CHARS 排除）
/// 即失败并返回首个不可编码字符与行号；此时不产出字节，调用方不得写盘。
/// 输入须为合法 UTF-8（内部先经 sanitizeUtf8 归一，无效字节按 U+FFFD 处理）。
std::expected<std::string, GbkEncodeFailure> encodeGbk(std::string_view utf8Content);

/// GBK 编码失败的完整用户可见错误（旧版文案移植）
std::wstring gbkEncodeFailureMessage(const GbkEncodeFailure& failure);

} // namespace core
