// M2-A U1 单测：编码识别/编解码与文本工具（TC-U01~U07 + 补充）。
// 测试数据全部为内联十六进制字节流（GBK 样本等不依赖测试机生成，保证确定性）。
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include "core/encoding.h"
#include "core/textutil.h"

using namespace core;
using namespace std::string_view_literals;

// —— 确定性字节样本 ——
constexpr auto kAscii = "Hello, World! 123"sv;
constexpr auto kUtf8NiHao = "\xE4\xBD\xA0\xE5\xA5\xBD"sv;           // “你好” UTF-8
constexpr auto kGbkNiHao = "\xC4\xE3\xBA\xC3"sv;                     // “你好” CP936
constexpr auto kBom = "\xEF\xBB\xBF"sv;                              // UTF-8 BOM
constexpr auto kEmojiGrin = "\xF0\x9F\x98\x80"sv;                    // U+1F600 😀
constexpr auto kReplacement = "\xEF\xBF\xBD"sv;                      // U+FFFD
constexpr auto kUtf8Rare = "\xE3\x90\x80"sv;                         // U+3400 㐀（CJK 扩 A，GBK 无映射）
constexpr auto kGbkEuro = "\x80"sv;                                  // CP936 单字节欧元符 → U+20AC

static std::string cat(std::initializer_list<std::string_view> parts) {
    std::string out;
    for (const auto p : parts)
        out.append(p);
    return out;
}

// ===========================================================================
// TC-U01 / TC-U05：UTF-8 无 BOM 识别（纯 ASCII 不误判 GBK）
// ===========================================================================
TEST_CASE("TC-U01 UTF-8 无 BOM 识别（纯 ASCII / 中文 UTF-8）", "[core][encoding]") {
    SECTION("纯 ASCII → UTF-8 无 BOM") {
        const auto d = detectEncoding(kAscii);
        REQUIRE(d.encoding == TextEncoding::Utf8);
        REQUIRE_FALSE(d.hasBom);
    }
    SECTION("中文 UTF-8 → UTF-8 无 BOM，解码原样") {
        const auto d = detectEncoding(kUtf8NiHao);
        REQUIRE(d.encoding == TextEncoding::Utf8);
        REQUIRE_FALSE(d.hasBom);
        REQUIRE(sanitizeUtf8(kUtf8NiHao) == kUtf8NiHao);
    }
}

TEST_CASE("TC-U05 纯 ASCII 判 UTF-8（不误判 GBK）", "[core][encoding]") {
    const auto d = detectEncoding("plain ascii text 0123456789\r\nsecond line");
    REQUIRE(d.encoding == TextEncoding::Utf8);
    REQUIRE_FALSE(d.hasBom);
}

// ===========================================================================
// TC-U02：UTF-8 BOM 识别与剥离
// ===========================================================================
TEST_CASE("TC-U02 UTF-8 BOM 识别与剥离", "[core][encoding]") {
    SECTION("BOM 前缀 → UTF-8 + hasBom，BOM 不进内容") {
        const std::string raw = cat({kBom, kUtf8NiHao});
        const auto d = detectEncoding(raw);
        REQUIRE(d.encoding == TextEncoding::Utf8);
        REQUIRE(d.hasBom);
        // 剥离 BOM 后解码：内容不含 BOM
        REQUIRE(sanitizeUtf8(stripUtf8Bom(raw)) == kUtf8NiHao);
    }
    SECTION("BOM 是权威标记：后续字节非法也不回退 GBK") {
        const std::string raw = cat({kBom, "\xC0\x80"}); // 过长编码 C0 80
        const auto d = detectEncoding(raw);
        REQUIRE(d.encoding == TextEncoding::Utf8);
        REQUIRE(d.hasBom);
        // 非法序列以 U+FFFD 呈现（不崩溃、不失败）
        const std::string content = sanitizeUtf8(stripUtf8Bom(raw));
        REQUIRE(content == cat({kReplacement, kReplacement}));
    }
    SECTION("stripUtf8Bom 无 BOM 时原样") {
        REQUIRE(stripUtf8Bom(kAscii) == kAscii);
        REQUIRE(stripUtf8Bom("") == ""sv);
    }
}

// ===========================================================================
// TC-U03 / TC-U04：GBK 识别与无效字节容错
// ===========================================================================
TEST_CASE("TC-U03 GBK 识别与解码", "[core][encoding]") {
    SECTION("GBK 中文字节（非合法 UTF-8）→ 识别 GBK") {
        const auto d = detectEncoding(kGbkNiHao);
        REQUIRE(d.encoding == TextEncoding::Gbk);
        REQUIRE_FALSE(d.hasBom);
    }
    SECTION("按 CP936 解码正确") {
        const auto r = decodeGbk(cat({kGbkNiHao, " ok"}));
        REQUIRE(r.has_value());
        REQUIRE(*r == cat({kUtf8NiHao, " ok"}));
    }
    SECTION("GBK + ASCII 混合") {
        const auto d = detectEncoding(cat({"abc\xC4\xE3\xBA\xC3xyz"}));
        REQUIRE(d.encoding == TextEncoding::Gbk);
    }
}

TEST_CASE("TC-U04 GBK 无效字节容错（U+FFFD 呈现，不崩溃）", "[core][encoding]") {
    SECTION("0xFF 混入：非法字节 U+FFFD，前后内容正常") {
        const auto r = decodeGbk(cat({kGbkNiHao.substr(0, 2), "\xFF", kGbkNiHao.substr(2)}));
        REQUIRE(r.has_value());
        REQUIRE(*r == cat({kUtf8NiHao.substr(0, 3), kReplacement, kUtf8NiHao.substr(3)}));
    }
    SECTION("末尾孤立首字节 → U+FFFD") {
        const auto r = decodeGbk("\xC4");
        REQUIRE(r.has_value());
        REQUIRE(*r == kReplacement);
    }
    SECTION("次字节非法（0x7F 不在 GBK trail 区间）→ U+FFFD + 后续正常") {
        const auto r = decodeGbk("\xFE\x7F");
        REQUIRE(r.has_value());
        REQUIRE(*r == cat({kReplacement, "\x7F"})); // 0x7F 单字节照常解码为 DEL
    }
    SECTION("CP936 单字节欧元符 0x80 → U+20AC") {
        const auto r = decodeGbk(kGbkEuro);
        REQUIRE(r.has_value());
        REQUIRE(*r == "\xE2\x82\xAC"); // € 的 UTF-8
    }
    SECTION("空输入为合法空内容") {
        const auto r = decodeGbk("");
        REQUIRE(r.has_value());
        REQUIRE(r->empty());
    }
}

// ===========================================================================
// TC-U06：强制编码解码（同一字节流两种解释）
// ===========================================================================
TEST_CASE("TC-U06 强制编码解码（同一字节流两种解释）", "[core][encoding]") {
    // GBK “你好” 的字节 C4 E3：
    //  - 按 GBK 解 → “你”
    //  - 按 UTF-8 解 → C4 需 1 个 continuation、E3 不是 → 两个 U+FFFD
    SECTION("C4 E3：GBK → 你；UTF-8 → 两个 U+FFFD") {
        const auto asGbk = decodeGbk("\xC4\xE3");
        REQUIRE(asGbk.has_value());
        REQUIRE(*asGbk == kUtf8NiHao.substr(0, 3));

        const std::string asUtf8 = sanitizeUtf8("\xC4\xE3");
        REQUIRE(asUtf8 == cat({kReplacement, kReplacement}));
    }
    SECTION("UTF-8 字节强制按 GBK 解：可解但内容不同（乱码呈现，不失败）") {
        const std::string mojibake = *decodeGbk(kUtf8NiHao);
        REQUIRE(mojibake != kUtf8NiHao);
        REQUIRE_FALSE(mojibake.empty());
    }
    SECTION("严格 UTF-8 校验拒绝 GBK 字节流（识别为 GBK 的判据）") {
        REQUIRE_FALSE(isValidUtf8(kGbkNiHao));
        REQUIRE(isValidUtf8(kUtf8NiHao));
    }
}

// ===========================================================================
// TC-U07：GBK 编码校验（不可编码字符定位；纯中文通过）
// ===========================================================================
TEST_CASE("TC-U07 GBK 编码校验（emoji/生僻字失败 + 字符与行号定位）", "[core][encoding]") {
    SECTION("纯 GBK 可表示内容通过且可往返") {
        const auto r = encodeGbk(cat({kUtf8NiHao, " abc\n", kUtf8NiHao}));
        REQUIRE(r.has_value());
        REQUIRE(*r == cat({kGbkNiHao, " abc\n", kGbkNiHao}));
        // 解码回读一致
        const auto back = decodeGbk(*r);
        REQUIRE(back.has_value());
        REQUIRE(*back == cat({kUtf8NiHao, " abc\n", kUtf8NiHao}));
    }
    SECTION("emoji 失败：返回首个不可编码字符与行号") {
        const std::string content =
            cat({"第一行\n", "第二行\n", "第三行 ", kEmojiGrin, " 之后"});
        const auto r = encodeGbk(content);
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().line == 3);
        REQUIRE(r.error().character == L"😀");
    }
    SECTION("错误消息含行号与字符") {
        const auto r = encodeGbk(cat({"a\nb\n", kEmojiGrin}));
        REQUIRE_FALSE(r.has_value());
        const std::wstring msg = gbkEncodeFailureMessage(r.error());
        REQUIRE(msg.find(L"第 3 行") != std::wstring::npos);
        REQUIRE(msg.find(L"😀") != std::wstring::npos);
        REQUIRE(msg.find(L"GBK") != std::wstring::npos);
    }
    SECTION("生僻字（CJK 扩 A）同样失败") {
        const auto r = encodeGbk(cat({kUtf8NiHao, kUtf8Rare}));
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().line == 1);
        REQUIRE(r.error().character == L"㐀");
    }
    SECTION("空内容合法") {
        const auto r = encodeGbk("");
        REQUIRE(r.has_value());
        REQUIRE(r->empty());
    }
}

// ===========================================================================
// 补充：严格 UTF-8 校验状态机（识别判据的基础）
// ===========================================================================
TEST_CASE("补充：严格 UTF-8 校验（过长/代理/越界/截断）", "[core][encoding]") {
    REQUIRE(isValidUtf8(""));
    REQUIRE(isValidUtf8(kAscii));
    REQUIRE(isValidUtf8(kEmojiGrin));
    REQUIRE_FALSE(isValidUtf8("\xC0\x80"));     // 过长编码 NUL
    REQUIRE_FALSE(isValidUtf8("\xC1\xBF"));     // 过长编码
    REQUIRE_FALSE(isValidUtf8("\xE0\x80\x80")); // 3 字节过长
    REQUIRE_FALSE(isValidUtf8("\xED\xA0\x80")); // 代理区 U+D800
    REQUIRE_FALSE(isValidUtf8("\xF4\x90\x80\x80")); // > U+10FFFF
    REQUIRE_FALSE(isValidUtf8("\xE4\xBD"));     // 尾部截断（Stateless 语义：不静默吞）
    REQUIRE_FALSE(isValidUtf8("\xC4\xE3\xBA\xC3")); // GBK 字节非 UTF-8
    REQUIRE_FALSE(isValidUtf8("\xF5\x80\x80\x80")); // 非法首字节 F5
    REQUIRE_FALSE(isValidUtf8("\x80"));         // 孤立 continuation
    // 截断序列 → 识别回退 GBK
    REQUIRE(detectEncoding("\xE4\xBD").encoding == TextEncoding::Gbk);
}

// ===========================================================================
// 补充：编码显示名与 UTF-8 ↔ UTF-16 转换
// ===========================================================================
TEST_CASE("补充：编码显示名与 utf8ToWide/wideToUtf8", "[core][encoding]") {
    REQUIRE(encodingDisplayName(TextEncoding::Utf8) == L"UTF-8");
    REQUIRE(encodingDisplayName(TextEncoding::Gbk) == L"GBK");
    REQUIRE(utf8ToWide(kUtf8NiHao) == L"你好");
    REQUIRE(wideToUtf8(L"你好😀") == cat({kUtf8NiHao, kEmojiGrin}));
    REQUIRE(wideToUtf8(utf8ToWide(kUtf8NiHao)) == kUtf8NiHao);
    REQUIRE(utf8ToWide("").empty());
    REQUIRE(wideToUtf8(L"").empty());
}

// ===========================================================================
// 补充：换行工具（归一/探测；存储层往返见 TC-U08）
// ===========================================================================
TEST_CASE("补充：换行归一与探测（textutil）", "[core][text]") {
    REQUIRE(normalizeLineEndings("a\r\nb\rc\nd") == "a\nb\nc\nd");
    REQUIRE(normalizeLineEndings("") == "");
    REQUIRE(normalizeLineEndings("\r") == "\n");
    REQUIRE(normalizeLineEndings("\r\n\r\n") == "\n\n");
    REQUIRE(normalizeLineEndings("abc") == "abc"); // 无 CR 快路径

    REQUIRE(detectLineEnding("a\r\nb\rc\nd") == LineEnding::Crlf); // 任一 CRLF 即 Crlf
    REQUIRE(detectLineEnding("a\rb\r") == LineEnding::Lf);         // 孤立 CR 按 Lf 记录
    REQUIRE(detectLineEnding("") == LineEnding::Lf);
    REQUIRE(detectLineEnding("单行无换行") == LineEnding::Lf);

    REQUIRE(lineEndingDisplayName(LineEnding::Lf) == L"LF");
    REQUIRE(lineEndingDisplayName(LineEnding::Crlf) == L"CRLF");
}

// ===========================================================================
// 补充：UTF-8 字符换算（状态栏/光标换算基础）
// ===========================================================================
TEST_CASE("补充：UTF-8 字符计数与字节偏移换算", "[core][text]") {
    // h é l l o 😀 = 6 字符、10 字节
    const std::string text = cat({"h\xC3\xA9", "llo", kEmojiGrin});
    REQUIRE(text.size() == 10);
    REQUIRE(utf8CharCount(text) == 6);
    REQUIRE(utf8CharCount("") == 0);
    REQUIRE(utf8CharCount(kUtf8NiHao) == 2); // 你好

    REQUIRE(utf8CharIndexToByteOffset(text, 0) == 0);
    REQUIRE(utf8CharIndexToByteOffset(text, 1) == 1);
    REQUIRE(utf8CharIndexToByteOffset(text, 5) == 6); // emoji 起始
    REQUIRE(utf8CharIndexToByteOffset(text, 6) == 10);
    REQUIRE(utf8CharIndexToByteOffset(text, 99) == 10); // 越界钳制

    REQUIRE(utf8ByteOffsetToCharIndex(text, 0) == 0);
    REQUIRE(utf8ByteOffsetToCharIndex(text, 6) == 5);
    REQUIRE(utf8ByteOffsetToCharIndex(text, 8) == 5);  // emoji 中间：按起始字符计
    REQUIRE(utf8ByteOffsetToCharIndex(text, 10) == 6);
    REQUIRE(utf8ByteOffsetToCharIndex(text, 99) == 6); // 越界钳制
}

// ===========================================================================
// 补充：SHA-256 已知向量（基线哈希稳定性，TC-U26 基础）
// ===========================================================================
TEST_CASE("补充：SHA-256 指纹已知向量与稳定性", "[core][text]") {
    REQUIRE(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    REQUIRE(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 稳定：同内容同指纹；不同内容不同指纹；hex 小写 64 字符
    const std::string a = sha256Hex(kUtf8NiHao);
    REQUIRE(a.size() == 64);
    REQUIRE(a == sha256Hex(kUtf8NiHao));
    REQUIRE(a != sha256Hex(cat({kUtf8NiHao, "x"})));
    for (const char c : a)
        REQUIRE((c >= '0' && c <= '9' || c >= 'a' && c <= 'f'));
}
