#include "core/textutil.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>

namespace core {

std::string normalizeLineEndings(std::string_view text) {
    if (text.find('\r') == std::string_view::npos)
        return std::string(text); // 快路径：无 CR，内容本就是 LF 形态
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n')
                ++i; // CRLF → LF（吃掉 \n）
            out.push_back('\n'); // 孤立 CR → LF
        } else {
            out.push_back(c);
        }
    }
    return out;
}

LineEnding detectLineEnding(std::string_view text) {
    // 旧版语义：出现任一 CRLF 即 Crlf；否则 Lf（孤立 CR 不构成 Crlf）
    return text.find("\r\n") != std::string_view::npos ? LineEnding::Crlf : LineEnding::Lf;
}

namespace {

/// 无效首字节 → 0；否则该码点的字节长度（ASCII 为 1，多字节 2..4）。
/// 仅按首字节区间判定（不校验后续 continuation），供长度步进使用。
inline int utf8LeadLength(unsigned char b) {
    if (b < 0x80) return 1;
    if (b >= 0xC2 && b <= 0xDF) return 2;
    if (b >= 0xE0 && b <= 0xEF) return 3;
    if (b >= 0xF0 && b <= 0xF4) return 4;
    return 0;
}

inline bool isUtf8Continuation(unsigned char b) {
    return b >= 0x80 && b <= 0xBF;
}

/// 从 i 开始、按首字节声明长度构成的序列是否完整且 continuation 合法
inline bool utf8SequenceValidAt(std::string_view text, std::size_t i) {
    const int len = utf8LeadLength(static_cast<unsigned char>(text[i]));
    if (len <= 0) return false;
    if (i + static_cast<std::size_t>(len) > text.size()) return false; // 尾部截断
    for (int k = 1; k < len; ++k) {
        if (!isUtf8Continuation(static_cast<unsigned char>(text[i + static_cast<std::size_t>(k)])))
            return false;
    }
    return true;
}

} // namespace

std::size_t utf8CharCount(std::string_view text) {
    std::size_t chars = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        if (!utf8SequenceValidAt(text, i)) {
            // 无效首字节：按 1 字符计（对应一个 U+FFFD 呈现）
            ++chars;
            ++i;
            continue;
        }
        i += static_cast<std::size_t>(utf8LeadLength(static_cast<unsigned char>(text[i])));
        ++chars;
    }
    return chars;
}

std::size_t utf8CharIndexToByteOffset(std::string_view text, std::size_t charIndex) {
    std::size_t chars = 0;
    std::size_t i = 0;
    while (i < text.size() && chars < charIndex) {
        if (!utf8SequenceValidAt(text, i)) {
            ++i;
            ++chars;
            continue;
        }
        i += static_cast<std::size_t>(utf8LeadLength(static_cast<unsigned char>(text[i])));
        ++chars;
    }
    return i; // chars 到达 charIndex 或走到末尾（越界钳制）
}

std::size_t utf8ByteOffsetToCharIndex(std::string_view text, std::size_t byteOffset) {
    if (byteOffset > text.size()) byteOffset = text.size();
    std::size_t chars = 0;
    std::size_t i = 0;
    while (i < byteOffset) {
        std::size_t step;
        if (!utf8SequenceValidAt(text, i)) {
            step = 1; // 无效字节：1 字符
        } else {
            step = static_cast<std::size_t>(utf8LeadLength(static_cast<unsigned char>(text[i])));
            if (i + step > byteOffset)
                break; // 偏移落在本字符内部：按该字符起始计（chars 即其索引）
        }
        i += step;
        ++chars;
    }
    return chars;
}

std::string sha256Hex(std::string_view data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        return {};
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }

    // 分块喂入（避免巨文件一次性传参的 ULONG 截断风险；常规小文件单块完成）
    constexpr std::size_t kChunk = 1u << 20;
    bool ok = true;
    for (std::size_t off = 0; ok && off < data.size(); off += kChunk) {
        const ULONG n = static_cast<ULONG>(std::min(kChunk, data.size() - off));
        ok = BCRYPT_SUCCESS(BCryptHashData(
            hash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data() + off)), n, 0));
    }

    std::array<unsigned char, 32> digest{};
    if (ok)
        ok = BCRYPT_SUCCESS(
            BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0));

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok)
        return {};

    static const char* kHexDigits = "0123456789abcdef";
    std::string hex;
    hex.reserve(digest.size() * 2);
    for (const unsigned char b : digest) {
        hex.push_back(kHexDigits[b >> 4]);
        hex.push_back(kHexDigits[b & 0xF]);
    }
    return hex;
}

} // namespace core
