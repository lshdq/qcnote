#include "core/encoding.h"

#include <windows.h>

#include <climits>
#include <string_view>

namespace core {

// ---------------------------------------------------------------------------
// UTF-8 识别与宽松解码
// ---------------------------------------------------------------------------

namespace {

/// 首字节合法时返回所需 continuation 数量（0 = 单字节 ASCII），非法返回 -1。
/// 严格规则：C0/C1（过长 2 字节）、F5..FF 越界；E0/ED/F0/F4 的首 continuation
/// 各有子区间约束（防过长编码 / 代理区 / 超 U+10FFFF）。
inline int utf8ContinuationsNeeded(unsigned char lead, unsigned char& lo, unsigned char& hi) {
    if (lead < 0x80) return 0;
    if (lead >= 0xC2 && lead <= 0xDF) { lo = 0x80; hi = 0xBF; return 1; }
    if (lead == 0xE0) { lo = 0xA0; hi = 0xBF; return 2; }  // 防 E0 80..9F 过长
    if (lead >= 0xE1 && lead <= 0xEC) { lo = 0x80; hi = 0xBF; return 2; }
    if (lead == 0xED) { lo = 0x80; hi = 0x9F; return 2; }  // 防代理区 ED A0..BF
    if (lead >= 0xEE && lead <= 0xEF) { lo = 0x80; hi = 0xBF; return 2; }
    if (lead == 0xF0) { lo = 0x90; hi = 0xBF; return 3; }  // 防 F0 80..8F 过长
    if (lead >= 0xF1 && lead <= 0xF3) { lo = 0x80; hi = 0xBF; return 3; }
    if (lead == 0xF4) { lo = 0x80; hi = 0x8F; return 3; }  // 防超 U+10FFFF
    return -1; // C0、C1、F5..FF、独立 continuation 字节
}

inline bool inRange(unsigned char b, unsigned char lo, unsigned char hi) {
    return b >= lo && b <= hi;
}

/// 从 i 开始的序列是否为完整合法 UTF-8 码点（不越界、continuation 全部合法）
inline bool strictSequenceAt(std::string_view t, std::size_t i) {
    unsigned char lo = 0, hi = 0;
    const int need = utf8ContinuationsNeeded(static_cast<unsigned char>(t[i]), lo, hi);
    if (need < 0) return false;
    if (i + 1 + static_cast<std::size_t>(need) > t.size()) return false; // 尾部截断
    if (need > 0 && !inRange(static_cast<unsigned char>(t[i + 1]), lo, hi)) return false;
    for (int k = 2; k <= need; ++k) {
        if (!inRange(static_cast<unsigned char>(t[i + static_cast<std::size_t>(k)]), 0x80, 0xBF))
            return false;
    }
    return true;
}

} // namespace

bool isValidUtf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        if (!strictSequenceAt(text, i))
            return false;
        unsigned char lo = 0, hi = 0;
        const int need =
            utf8ContinuationsNeeded(static_cast<unsigned char>(text[i]), lo, hi);
        i += 1 + static_cast<std::size_t>(need);
    }
    return true;
}

EncodingDetection detectEncoding(std::string_view raw) {
    // ① BOM 优先：显式字节比对；带 BOM 即权威判定为 UTF-8，即使后续字节
    //    含非法序列也不回退 GBK（非法序列在解码时以 U+FFFD 呈现）
    if (raw.starts_with(kUtf8Bom))
        return {TextEncoding::Utf8, true};
    // ② 严格 UTF-8 校验（含纯 ASCII——ASCII 是 UTF-8 子集，不误判 GBK）
    if (isValidUtf8(raw))
        return {TextEncoding::Utf8, false};
    // ③ 其余按 GBK（GBK 双字节结构几乎总能容纳高位字节序列，回退判定即最终判定）
    return {TextEncoding::Gbk, false};
}

std::string sanitizeUtf8(std::string_view bytes) {
    if (isValidUtf8(bytes))
        return std::string(bytes); // 快路径：合法内容原样
    std::string out;
    out.reserve(bytes.size());
    static constexpr std::string_view kReplacementChar = "\xEF\xBF\xBD"; // U+FFFD
    std::size_t i = 0;
    while (i < bytes.size()) {
        unsigned char lo = 0x80, hi = 0xBF;
        const int need =
            utf8ContinuationsNeeded(static_cast<unsigned char>(bytes[i]), lo, hi);
        if (strictSequenceAt(bytes, i)) {
            const std::size_t len = 1 + static_cast<std::size_t>(need);
            out.append(bytes.substr(i, len));
            i += len;
            continue;
        }
        // 非法：按「最大无效子序列」消费——首字节 + 已接受的合法 continuation
        // 前缀，替换为一个 U+FFFD（对齐 WHATWG；尾部截断同样合并为一个替换符）
        if (need < 0) {
            out.append(kReplacementChar);
            ++i;
            continue;
        }
        std::size_t j = i + 1;
        if (j < bytes.size() && inRange(static_cast<unsigned char>(bytes[j]), lo, hi)) {
            ++j;
            while (j < i + 1 + static_cast<std::size_t>(need) && j < bytes.size()
                   && inRange(static_cast<unsigned char>(bytes[j]), 0x80, 0xBF))
                ++j;
        }
        out.append(kReplacementChar);
        i = j;
    }
    return out;
}

// ---------------------------------------------------------------------------
// UTF-8 ↔ UTF-16（界面/消息边界转换；GBK 编解码的 UTF-16 中转也走这里）
// ---------------------------------------------------------------------------

std::wstring utf8ToWide(std::string_view utf8) {
    if (utf8.empty())
        return {};
    if (utf8.size() > static_cast<std::size_t>(INT_MAX))
        return {}; // 防御：Win32 API 长度为 int；超限内容 unrealistic，安全失败
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                        static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0)
        return {};
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        wide.data(), len);
    return wide; // 无 MB_ERR_INVALID_CHARS：无效序列由系统替换为 U+FFFD
}

std::string wideToUtf8(std::wstring_view wide) {
    if (wide.empty())
        return {};
    if (wide.size() > static_cast<std::size_t>(INT_MAX))
        return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                        static_cast<int>(wide.size()), nullptr, 0,
                                        nullptr, nullptr);
    if (len <= 0)
        return {};
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------------------
// GBK（CP936）编解码 —— 旧 src/platform/gbkcodec.cpp 语义移植（DEC-009）
// ---------------------------------------------------------------------------

namespace {

constexpr UINT kCp936 = 936; // GBK 的 Windows 官方实现（兼容 GBK/GB2312）

/// CP936 是否可用（进程内一次探测）：代码页 936 为 Windows 内置 NLS 表，
/// 防御极端裁剪系统；用 GBK 标准样本字节（“你”= C4 E3）严格解码探测。
bool cp936Available() {
    static const bool available = [] {
        const char probe[] = "\xC4\xE3";
        wchar_t buf[2] = {};
        return MultiByteToWideChar(kCp936, MB_ERR_INVALID_CHARS, probe, 2, buf, 2) == 1
               && buf[0] == 0x4F60;
    }();
    return available;
}

/// 整块严格解码（MB_ERR_INVALID_CHARS：遇非法或未指派序列整体失败）。
bool decodeCp936Strict(const char* data, int size, std::wstring& out) {
    const int len = MultiByteToWideChar(kCp936, MB_ERR_INVALID_CHARS, data, size, nullptr, 0);
    if (len <= 0)
        return false;
    out.resize(static_cast<std::size_t>(len));
    const int written =
        MultiByteToWideChar(kCp936, MB_ERR_INVALID_CHARS, data, size, out.data(), len);
    return written == len;
}

/// GBK 双字节结构的次字节判定：0x40–0xFE 且非 0x7F（GBK 规范区间）
inline bool isGbkTrailByte(unsigned char b) {
    return b >= 0x40 && b <= 0xFE && b != 0x7F;
}

/// 单个字符单元（1 个 BMP 字符或 1 对代理项）能否直接编码为 CP936（不含 best-fit 近似）
bool unitEncodable(const wchar_t* unit, int unitLen) {
    char buf[4] = {};
    BOOL usedDefault = FALSE;
    const char defChar = '?';
    const int n = WideCharToMultiByte(kCp936, WC_NO_BEST_FIT_CHARS, unit, unitLen, buf,
                                      sizeof(buf), &defChar, &usedDefault);
    return n > 0 && !usedDefault;
}

} // namespace

std::expected<std::string, std::wstring> decodeGbk(std::string_view raw) {
    if (raw.size() > static_cast<std::size_t>(INT_MAX))
        return std::unexpected<std::wstring>(L"文件过大，无法以 GBK 解码");
    const int size = static_cast<int>(raw.size());
    if (size == 0)
        return std::string{}; // 空输入即合法空内容

    // 快路径：绝大多数真实 GBK 文件整块严格解码即成功
    std::wstring whole;
    if (decodeCp936Strict(raw.data(), size, whole))
        return wideToUtf8(whole);

    if (!cp936Available())
        return std::unexpected<std::wstring>(L"当前系统缺少 Windows 代码页 936（GBK）支持，无法解码");

    // 回退：按 GBK 双字节结构扫描分段，段内整块严格解码、失败逐单元回退为 U+FFFD，
    // 结构非法字节（孤立首字节/非法次字节/0xFF）单个 U+FFFD 且仅前进 1 字节——
    // 后续内容仍正常解码（保证文件始终可打开，不丢尾部）
    std::wstring result;
    result.reserve(static_cast<std::size_t>(size)); // 上界估计
    int i = 0;
    while (i < size) {
        const unsigned char lead = static_cast<unsigned char>(raw[static_cast<std::size_t>(i)]);
        if (lead == 0xFF) {
            // 0xFF 不在 GBK 首字节区间（0x81–0xFE）：非法单字节
            result.push_back(0xFFFD);
            ++i;
            continue;
        }

        // 收集从 i 开始连续“结构合法”的最长区段 [i, j)：
        // 单字节（< 0x80 的 ASCII、0x80 欧元符）与结构合法的双字节对
        int j = i;
        bool structurallyValid = true;
        while (j < size) {
            const unsigned char b = static_cast<unsigned char>(raw[static_cast<std::size_t>(j)]);
            if (b <= 0x80) {
                ++j; // 单字节单元（ASCII 或 0x80 欧元符 U+20AC）
            } else if (b <= 0xFE) {
                if (j + 1 < size
                    && isGbkTrailByte(static_cast<unsigned char>(
                          raw[static_cast<std::size_t>(j + 1)]))) {
                    j += 2; // 结构合法双字节对
                } else {
                    structurallyValid = false; // 次字节非法或末尾孤立首字节
                    break;
                }
            } else {
                structurallyValid = false; // 0xFF
                break;
            }
        }
        if (j > i) {
            // 区段整块严格解码；失败（区段含 CP936 未指派对）时逐单元回退
            std::wstring chunk;
            if (decodeCp936Strict(raw.data() + i, j - i, chunk)) {
                result.append(chunk);
            } else {
                for (int k = i; k < j;) {
                    const unsigned char b =
                        static_cast<unsigned char>(raw[static_cast<std::size_t>(k)]);
                    const int unitLen = (b >= 0x81 && b <= 0xFE) ? 2 : 1;
                    std::wstring unit;
                    if (decodeCp936Strict(raw.data() + k, unitLen, unit))
                        result.append(unit);
                    else
                        result.push_back(0xFFFD); // 结构合法但 CP936 未指派
                    k += unitLen;
                }
            }
            i = j;
        }
        if (!structurallyValid) {
            // 位置 j 结构非法：单个 U+FFFD，仅前进 1 字节——
            // 若非法首字节后跟合法 ASCII，后续内容仍正常解码
            result.push_back(0xFFFD);
            ++i;
        }
    }

    return wideToUtf8(result);
}

std::expected<std::string, GbkEncodeFailure> encodeGbk(std::string_view utf8Content) {
    // 归一为合法 UTF-8 再转 UTF-16（无效字节按 U+FFFD；内容契约本应合法，防御路径）
    const std::wstring wide = utf8ToWide(sanitizeUtf8(utf8Content));
    if (wide.size() > static_cast<std::size_t>(INT_MAX))
        return std::unexpected(GbkEncodeFailure{L"", 0}); // 未能定位（内容超长）
    if (wide.empty())
        return std::string{}; // 空内容合法

    const int len = static_cast<int>(wide.size());

    // 快路径：整串一次编码，default-char 机制做全量可编码校验。
    // WC_NO_BEST_FIT_CHARS：近似映射也计为不可直接映射（若系统存在近似表，
    // 不加该标志会静默替换破坏数据）；WC_ERR_INVALID_CHARS 对 CP936 无效
    // （返回 ERROR_INVALID_FLAGS），不可编码检测必须走 default-char 机制。
    const int need = WideCharToMultiByte(kCp936, WC_NO_BEST_FIT_CHARS, wide.data(), len,
                                         nullptr, 0, nullptr, nullptr);
    if (need > 0) {
        std::string bytes(static_cast<std::size_t>(need), '\0');
        BOOL usedDefault = FALSE;
        const char defChar = '?';
        const int written =
            WideCharToMultiByte(kCp936, WC_NO_BEST_FIT_CHARS, wide.data(), len, bytes.data(),
                                need, &defChar, &usedDefault);
        if (written == need && !usedDefault)
            return bytes; // 全部字符均有直接 CP936 映射
    }

    // 失败路径：定位首个不可编码字符及行号（从 1 起，按 '\n' 计数）。
    // 逐“字符单元”探测：代理项对（如 emoji）作为完整码点整体编码，
    // 拆半探测必失败，会导致行号/字符定位错误
    int line = 1;
    std::wstring badChar;
    for (std::size_t i = 0; i < wide.size();) {
        const wchar_t c = wide[i];
        wchar_t unit[2] = {};
        int unitLen = 1;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < wide.size() && wide[i + 1] >= 0xDC00
            && wide[i + 1] <= 0xDFFF) {
            unit[0] = c;
            unit[1] = wide[i + 1];
            unitLen = 2;
        } else {
            unit[0] = c;
        }
        if (!unitEncodable(unit, unitLen)) {
            badChar = wide.substr(i, static_cast<std::size_t>(unitLen));
            break;
        }
        if (c == L'\n')
            ++line;
        i += static_cast<std::size_t>(unitLen);
    }

    if (badChar.empty())
        return std::unexpected(GbkEncodeFailure{L"", 0}); // 极端：整串失败但逐单元可编码
    return std::unexpected(GbkEncodeFailure{std::move(badChar), line});
}

std::wstring gbkEncodeFailureMessage(const GbkEncodeFailure& failure) {
    if (failure.character.empty() || failure.line <= 0) {
        // 未定位到具体字符（极端情况）
        return L"无法以 GBK 编码保存该内容，请改用 UTF-8";
    }
    return L"无法以 GBK 编码保存：第 " + std::to_wstring(failure.line)
           + L" 行的字符“" + failure.character
           + L"”在 GBK 中没有对应编码，请改用 UTF-8 或删除该字符";
}

} // namespace core
