// 代码着色 T2 单测：语言注册表（core/syntax）。
//
// 语义基准：.localdoc/需求文档/需求-代码着色.md + 决策记录 DEC-002/003/
// 004/005/008/009。覆盖测试用例文档 TC-001（扩展名自动识别）、TC-002
//（大小写不敏感）、TC-014（样式映射表完整性）core 部分，以及
// Document 语言元数据/通知链/生效语言计算（16MB 阈值）。
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/document.h"
#include "core/syntax.h"

using namespace core;

namespace {

/// 15 个 Lexilla 词法器注册名白名单（editor::CreateLexerByName 支持集合；
/// 与 src/editor/lexercatalogue.cpp 的分发表一致——T1 已验证）
constexpr std::string_view kKnownLexerNames[] = {
    "cpp", "python", "json", "markdown", "hypertext", "xml", "css", "sql",
    "bash", "powershell", "yaml", "lua", "rust", "batch", "props",
};

/// 21 语言 id（与注册表条目一一对应；本断言集合独立维护——注册表丢条目
/// 或改 id 时本测试失败）
constexpr std::string_view kExpectedLanguageIds[] = {
    "c",         "cpp",    "javascript", "typescript", "python",
    "go",        "rust",   "lua",        "json",       "markdown",
    "html",      "xml",    "css",        "sql",        "bash",
    "powershell", "batch", "ini",        "yaml",       "csharp",
    "java",
};

} // namespace

// ===========================================================================
// TC-001：扩展名自动识别
// ===========================================================================
TEST_CASE("语言注册表：按扩展名识别 21 语言", "[core][syntax]") {
    SECTION("任务用例逐一命中") {
        // (扩展名, 期望语言 id) 对照（TC-001 操作步骤全集）
        const std::pair<std::wstring_view, std::string_view> cases[] = {
            {L"c", "c"},           {L"h", "c"},          {L"cpp", "cpp"},
            {L"cc", "cpp"},        {L"cxx", "cpp"},      {L"hpp", "cpp"},
            {L"hh", "cpp"},        {L"hxx", "cpp"},      {L"inl", "cpp"},
            {L"cs", "csharp"},     {L"java", "java"},    {L"js", "javascript"},
            {L"mjs", "javascript"}, {L"cjs", "javascript"}, {L"ts", "typescript"},
            {L"tsx", "typescript"}, {L"py", "python"},   {L"pyw", "python"},
            {L"go", "go"},         {L"rs", "rust"},      {L"lua", "lua"},
            {L"json", "json"},     {L"md", "markdown"},  {L"markdown", "markdown"},
            {L"html", "html"},     {L"htm", "html"},     {L"xml", "xml"},
            {L"xsl", "xml"},       {L"xsd", "xml"},      {L"svg", "xml"},
            {L"css", "css"},       {L"sql", "sql"},      {L"sh", "bash"},
            {L"bash", "bash"},     {L"ps1", "powershell"}, {L"psm1", "powershell"},
            {L"bat", "batch"},     {L"cmd", "batch"},    {L"ini", "ini"},
            {L"cfg", "ini"},       {L"conf", "ini"},     {L"properties", "ini"},
            {L"yaml", "yaml"},     {L"yml", "yaml"},
        };
        for (const auto& [ext, id] : cases) {
            const std::wstring fileName = std::wstring(L"a.") + std::wstring(ext);
            const LanguageEntry* entry = languageForFileName(fileName);
            REQUIRE(entry != nullptr);
            INFO("扩展名用例 " << std::string(id) << "：" << wideToUtf8(fileName));
            REQUIRE(entry->id == id);
        }
    }
    SECTION("纯文本回退") {
        REQUIRE(languageForFileName(L"a.txt") == nullptr);     // 已知非代码扩展
        REQUIRE(languageForFileName(L"a.xyzzy") == nullptr);   // 未知扩展名
        REQUIRE(languageForFileName(L"Makefile") == nullptr);  // 无扩展名
        REQUIRE(languageForFileName(L"a.") == nullptr);        // 点在末尾（空扩展）
        REQUIRE(languageForFileName(L"") == nullptr);          // 空文件名
        REQUIRE(languageForFileName(L".gitignore") == nullptr); // dotfile 非 .py
    }
    SECTION("带路径目录部分的完整文件名（取文件名尾部扩展）") {
        REQUIRE(languageForFileName(L"D:\\工作\\dir.py\\a.py")->id == "python");
        // 目录名带点不影响：扩展名取最后一个点
        REQUIRE(languageForFileName(L"D:\\v1.2\\a.json")->id == "json");
    }
}

// ===========================================================================
// TC-002：扩展名识别大小写不敏感
// ===========================================================================
TEST_CASE("语言注册表：扩展名大小写不敏感", "[core][syntax]") {
    SECTION("大写/混合形式与对应小写一致") {
        REQUIRE(languageForFileName(L"a.PY")->id == "python");
        REQUIRE(languageForFileName(L"a.Cpp")->id == "cpp");
        REQUIRE(languageForFileName(L"a.JSON")->id == "json");
        REQUIRE(languageForFileName(L"a.Md")->id == "markdown");
        REQUIRE(languageForFileName(L"a.HtMl")->id == "html");
        REQUIRE(languageForFileName(L"a.XmL")->id == "xml");
        REQUIRE(languageForFileName(L"a.CS")->id == "csharp");
        REQUIRE(languageForFileName(L"a.JS")->id == "javascript");
        REQUIRE(languageForFileName(L"a.TS")->id == "typescript");
        REQUIRE(languageForFileName(L"a.SQL")->id == "sql");
        REQUIRE(languageForFileName(L"a.PS1")->id == "powershell");
        REQUIRE(languageForFileName(L"a.BAT")->id == "batch");
        REQUIRE(languageForFileName(L"a.INI")->id == "ini");
        REQUIRE(languageForFileName(L"a.YAML")->id == "yaml");
        REQUIRE(languageForFileName(L"a.GO")->id == "go");
        REQUIRE(languageForFileName(L"a.RS")->id == "rust");
        REQUIRE(languageForFileName(L"a.LUA")->id == "lua");
        REQUIRE(languageForFileName(L"a.SH")->id == "bash");
        REQUIRE(languageForFileName(L"UPPER.PY")->id == "python"); // 文件名含大写
    }
    SECTION("未知扩展大写同样不命中") {
        REQUIRE(languageForFileName(L"a.TXT") == nullptr);
        REQUIRE(languageForFileName(L"a.XYZZY") == nullptr);
    }
}

// ===========================================================================
// TC-014：样式映射表完整性（21 语言全有条目、槽位值域合法、token 非 None）
// ===========================================================================
TEST_CASE("语言注册表：条目与样式映射表完整性", "[core][syntax]") {
    SECTION("21 语言全部有条目且 id 集合精确匹配") {
        REQUIRE(languageCount() == 21);
        // 注册表条目集合 == 期望 id 集合（丢条目/改 id 即红）
        for (std::string_view id : kExpectedLanguageIds)
            REQUIRE(languageById(id) != nullptr);
        // 反向：注册表每个 id 都在期望集合内（无多余条目）
        for (std::size_t i = 0; i < languageCount(); ++i) {
            const LanguageEntry* entry = languageAt(i);
            REQUIRE(entry != nullptr);
            const auto begin = std::begin(kExpectedLanguageIds);
            const auto end = std::end(kExpectedLanguageIds);
            REQUIRE(std::find(begin, end, entry->id) != end);
        }
        // id 唯一（重复 id 会造成查询/菜单歧义）
        for (std::size_t i = 0; i < languageCount(); ++i) {
            for (std::size_t j = i + 1; j < languageCount(); ++j)
                REQUIRE(languageAt(i)->id != languageAt(j)->id);
        }
    }
    SECTION("languageAt 越界返回 nullptr") {
        REQUIRE(languageAt(languageCount()) == nullptr);
        REQUIRE(languageAt(languageCount() + 100) == nullptr);
    }
    SECTION("每条目基础字段合法") {
        for (std::size_t i = 0; i < languageCount(); ++i) {
            const LanguageEntry& entry = *languageAt(i);
            INFO("语言[" << i << "] " << std::string(entry.id));
            // 显示名非空（菜单/状态栏用）
            REQUIRE(entry.displayName != nullptr);
            REQUIRE(*entry.displayName != L'\0');
            // 词法器名在 15 词法器白名单（editor::CreateLexerByName 支持集）
            REQUIRE(entry.lexerName != nullptr);
            const auto begin = std::begin(kKnownLexerNames);
            const auto end = std::end(kKnownLexerNames);
            INFO("词法器名 " << entry.lexerName);
            REQUIRE(std::find(begin, end, std::string_view(entry.lexerName)) != end);
            // 扩展名表非空、均为小写非空串（可反查命中）
            bool hasExtension = false;
            for (const wchar_t* const* ext = entry.extensions; *ext; ++ext) {
                REQUIRE(**ext != L'\0');
                for (const wchar_t* p = *ext; *p; ++p) {
                    // 小写 ASCII 约定（字母/数字；ps1、psm1 等含数字合法；
                    // Catch2 断言内不支持 && 链，布尔先行聚合）
                    const bool lowerOrDigit =
                        (*p >= L'a' && *p <= L'z') || (*p >= L'0' && *p <= L'9');
                    REQUIRE(lowerOrDigit);
                }
                hasExtension = true;
            }
            REQUIRE(hasExtension);
        }
    }
    SECTION("映射表完整性：非空、槽位值域合法、token 非 None、哨兵存在") {
        for (std::size_t i = 0; i < languageCount(); ++i) {
            const LanguageEntry& entry = *languageAt(i);
            INFO("语言 " << std::string(entry.id) << " 映射表");
            REQUIRE(entry.styleMap != nullptr);
            bool hasSentinel = false;
            bool hasEntry = false;
            for (const StyleMapEntry* m = entry.styleMap; /**/; ++m) {
                if (m->slot < 0) { // 表尾哨兵（唯一允许的负槽位）
                    hasSentinel = true;
                    REQUIRE(m->token == SyntaxToken::None); // 哨兵 token 约定
                    break;
                }
                hasEntry = true;
                // 槽位值域：Scintilla 词法样式槽 0..127（STYLE_DEFAULT 区间；
                // 128+ 是 indicator/marker 区，词法器不产出）
                REQUIRE(m->slot <= 127);
                // 有语义着色的映射条目 token 必非 None（None 槽位不入表约定）
                REQUIRE(m->token != SyntaxToken::None);
                // 映射表槽位不得重复（同槽重复映射后者覆盖前者，属数据错误）
                for (const StyleMapEntry* n = entry.styleMap; n->slot >= 0; ++n)
                    if (n != m)
                        REQUIRE(n->slot != m->slot);
            }
            REQUIRE(hasSentinel); // 无哨兵 = 越界读（UB），必须存在
            REQUIRE(hasEntry);    // 空映射表（无任何着色槽）= 语言条目无意义
        }
    }
    SECTION("cpp 系 7 语言共享同一词法器与映射语义") {
        // DEC-003/009：C/C++/C#/Java/JS/TS/Go 复用 cpp 词法器（不同关键词表）
        constexpr std::string_view kCppFamily[] = {"c",   "cpp",   "csharp", "java",
                                                   "javascript", "typescript", "go"};
        for (std::string_view id : kCppFamily) {
            const LanguageEntry* entry = languageById(id);
            REQUIRE(entry != nullptr);
            REQUIRE(std::string_view(entry->lexerName) == "cpp");
            // 主关键词组（组 0）非空——cpp 词法器关键字高亮完全依赖注入
            REQUIRE(entry->keywords[0] != nullptr);
            REQUIRE(*entry->keywords[0] != '\0');
        }
        // 共享同一映射表实例（同一词法器 → 同一槽位语义，避免 7 份拷贝）
        const StyleMapEntry* shared = languageById("c")->styleMap;
        for (std::string_view id : kCppFamily)
            REQUIRE(languageById(id)->styleMap == shared);
    }
    SECTION("DEC-008/009 特殊承载核对") {
        REQUIRE(std::string_view(languageById("ini")->lexerName) == "props");
        REQUIRE(std::string_view(languageById("go")->lexerName) == "cpp");
        REQUIRE(std::string_view(languageById("html")->lexerName) == "hypertext");
        REQUIRE(std::string_view(languageById("xml")->lexerName) == "xml");
    }
    SECTION("关键词分组约定：非空组只出现在词法器实际消费的组号上") {
        // 各词法器消费的关键词组（Lex*.cxx wordListDesc 核对）：
        //   cpp: 0/1（+2/3 未用）  python: 0/1  json: 0（1 为 JSON-LD 未用）
        //   rust: 0/1/2  lua: 0/1  sql: 0  bash: 0  powershell: 0/1/2
        //   batch: 0  yaml: 0  html: 1（嵌入 JS；组 0 有意留空）
        //   markdown/xml/css/ini(props): 全空
        struct Expect {
            std::string_view id;
            std::array<bool, 9> used;
        };
        const Expect expectations[] = {
            {"c", {true, true}},          {"cpp", {true, true}},
            {"csharp", {true, true}},     {"java", {true, true}},
            {"javascript", {true, true}}, {"typescript", {true, true}},
            {"python", {true, true}},     {"go", {true, true}},
            {"rust", {true, true, true}}, {"lua", {true, true}},
            {"json", {true}},             {"markdown", {}},
            {"html", {false, true}},      {"xml", {}},
            {"css", {}},                  {"sql", {true}},
            {"bash", {true}},             {"powershell", {true, true, true}},
            {"batch", {true}},            {"ini", {}},
            {"yaml", {true}},
        };
        for (const Expect& expect : expectations) {
            const LanguageEntry* entry = languageById(expect.id);
            REQUIRE(entry != nullptr);
            INFO("语言 " << std::string(expect.id) << " 关键词组");
            for (int group = 0; group < kKeywordGroupCount; ++group) {
                const bool nonEmpty = entry->keywords[group] != nullptr
                                      && *entry->keywords[group] != '\0';
                const bool expectUsed = group < static_cast<int>(expect.used.size())
                                        ? expect.used[group] : false;
                REQUIRE(nonEmpty == expectUsed); // 约定外组注入 = 词法器不消费（死数据）
            }
        }
    }
}

// ===========================================================================
// languageById / isValidLanguageId / languageAt
// ===========================================================================
TEST_CASE("语言注册表：按 id 查询与合法 id 校验", "[core][syntax]") {
    SECTION("languageById 精确匹配") {
        REQUIRE(languageById("python") != nullptr);
        REQUIRE(languageById("python")->id == "python");
        REQUIRE(languageById("Python") == nullptr); // id 为稳定小写约定（精确匹配）
        REQUIRE(languageById("") == nullptr);
        REQUIRE(languageById("no-such-language") == nullptr);
        REQUIRE(languageById("plaintext") == nullptr); // 保留值不在注册表（仅手动 id）
    }
    SECTION("isValidLanguageId：注册表 id 与 plaintext 保留值") {
        REQUIRE(isValidLanguageId("python"));
        REQUIRE(isValidLanguageId("c"));
        REQUIRE(isValidLanguageId(kPlainTextLanguageId));
        REQUIRE(kPlainTextLanguageId == "plaintext");
        REQUIRE_FALSE(isValidLanguageId("Python"));
        REQUIRE_FALSE(isValidLanguageId(""));
        REQUIRE_FALSE(isValidLanguageId("no-such-language"));
    }
}

// ===========================================================================
// Document：语言元数据（存取/通知链/同值短路/不置脏）
// ===========================================================================
TEST_CASE("Document：手动语言元数据与通知链", "[core][doc][syntax]") {
    Document doc(L"未命名-1");
    int metadataEvents = 0;
    doc.onMetadataChanged = [&] { ++metadataEvents; };

    SECTION("默认自动模式（空 optional）") {
        REQUIRE_FALSE(doc.manualLanguage().has_value());
        REQUIRE(metadataEvents == 0);
    }
    SECTION("setLanguage：赋值并通知") {
        doc.setLanguage("json");
        REQUIRE(doc.manualLanguage().has_value());
        REQUIRE(*doc.manualLanguage() == "json");
        REQUIRE(metadataEvents == 1);
    }
    SECTION("同值短路（不重复通知）") {
        doc.setLanguage("json");
        REQUIRE(metadataEvents == 1);
        doc.setLanguage("json"); // 同值
        REQUIRE(metadataEvents == 1);
        REQUIRE(*doc.manualLanguage() == "json");
    }
    SECTION("换值再通知") {
        doc.setLanguage("json");
        doc.setLanguage("python");
        REQUIRE(metadataEvents == 2);
        REQUIRE(*doc.manualLanguage() == "python");
    }
    SECTION("clearLanguage：回自动模式并通知；已是自动时短路") {
        doc.clearLanguage();
        REQUIRE(metadataEvents == 0); // 已是自动模式
        doc.setLanguage("json");
        REQUIRE(metadataEvents == 1);
        doc.clearLanguage();
        REQUIRE(metadataEvents == 2);
        REQUIRE_FALSE(doc.manualLanguage().has_value());
        doc.clearLanguage(); // 再清（已空）
        REQUIRE(metadataEvents == 2);
    }
    SECTION("纯元数据：不参与 modified 判定") {
        doc.setContent("let x = 1;\n");
        REQUIRE(doc.modified());
        doc.setLanguage("javascript");
        REQUIRE(doc.modified()); // 语言元数据不置脏也不清脏
        doc.clearLanguage();
        REQUIRE(doc.modified());
    }
    SECTION("手动纯文本保留值（语义在 effectiveLanguage 层解释）") {
        doc.setLanguage(kPlainTextLanguageId);
        REQUIRE(*doc.manualLanguage() == "plaintext");
        REQUIRE(metadataEvents == 1);
    }
}

// ===========================================================================
// effectiveLanguage：手动优先 / 自动识别 / 容错回退 / 16MB 阈值
// ===========================================================================
TEST_CASE("effectiveLanguage：生效语言计算", "[core][syntax]") {
    SECTION("自动模式：按 displayName（url 派生文件名）扩展名识别") {
        Document doc;
        doc.setUrl(std::filesystem::path(L"D:/工作/a.py"));
        const LanguageEntry* lang = effectiveLanguage(doc);
        REQUIRE(lang != nullptr);
        REQUIRE(lang->id == "python");
    }
    SECTION("自动模式：url 换扩展名即重识别（另存为/重命名语义）") {
        Document doc;
        doc.setUrl(std::filesystem::path(L"D:/t/a.txt"));
        REQUIRE(effectiveLanguage(doc) == nullptr); // 纯文本
        doc.setUrl(std::filesystem::path(L"D:/t/a.py"));
        REQUIRE(effectiveLanguage(doc)->id == "python");
        doc.setUrl(std::filesystem::path(L"D:/t/a.json"));
        REQUIRE(effectiveLanguage(doc)->id == "json");
    }
    SECTION("无 url 的未命名文档（无手动指定）→ 纯文本") {
        Document doc(L"未命名-3");
        REQUIRE(effectiveLanguage(doc) == nullptr);
    }
    SECTION("手动指定优先于自动识别") {
        Document doc;
        doc.setUrl(std::filesystem::path(L"D:/t/a.py"));
        doc.setLanguage("json");
        REQUIRE(effectiveLanguage(doc)->id == "json"); // 手动优先
    }
    SECTION("手动纯文本 → nullptr（即使扩展名可识别）") {
        Document doc;
        doc.setUrl(std::filesystem::path(L"D:/t/a.py"));
        doc.setLanguage(kPlainTextLanguageId);
        REQUIRE(effectiveLanguage(doc) == nullptr);
    }
    SECTION("手动未知 id（数据损坏）容错回退自动识别") {
        Document doc;
        doc.setUrl(std::filesystem::path(L"D:/t/a.py"));
        doc.setLanguage("corrupted-id"); // 不在注册表（会话文件被手改等）
        REQUIRE(effectiveLanguage(doc)->id == "python"); // 回退自动
    }
    SECTION("未命名文档手动语言（uid 条目会话恢复场景，TC-010 core 侧）") {
        Document doc(L"未命名-2");
        doc.setLanguage("python");
        REQUIRE(effectiveLanguage(doc) != nullptr);
        REQUIRE(effectiveLanguage(doc)->id == "python"); // 手动语言不依赖 url
    }
}

TEST_CASE("effectiveLanguageForColoring：16MB 阈值（DEC-005）", "[core][syntax]") {
    Document doc;
    doc.setUrl(std::filesystem::path(L"D:/t/a.py"));
    doc.setContent("x = 1\n");
    SECTION("阈值内：与 effectiveLanguage 一致") {
        REQUIRE(effectiveLanguageForColoring(doc) == effectiveLanguage(doc));
        REQUIRE(effectiveLanguageForColoring(doc)->id == "python");
    }
    SECTION("恰等于阈值（16 MiB 字节）：仍着色（上限含边界）") {
        Document big;
        big.setUrl(std::filesystem::path(L"D:/t/a.py"));
        big.setContent(std::string(kSyntaxColoringMaxBytes, 'x'));
        REQUIRE(big.content().size() == kSyntaxColoringMaxBytes);
        REQUIRE(effectiveLanguageForColoring(big) != nullptr); // == 阈值不跳过
    }
    SECTION("超过阈值：跳过着色（纯文本渲染）") {
        Document big;
        big.setUrl(std::filesystem::path(L"D:/t/a.py"));
        big.setContent(std::string(kSyntaxColoringMaxBytes + 1, 'x'));
        REQUIRE(effectiveLanguageForColoring(big) == nullptr); // 跳过
        // 语言身份判定不受阈值影响（菜单/状态栏语义分离）
        REQUIRE(effectiveLanguage(big) != nullptr);
        REQUIRE(effectiveLanguage(big)->id == "python");
    }
    SECTION("超阈值的手动语言同样跳过着色") {
        Document big;
        big.setUrl(std::filesystem::path(L"D:/t/a.txt"));
        big.setLanguage("python"); // 手动指定
        big.setContent(std::string(kSyntaxColoringMaxBytes + 1, 'x'));
        REQUIRE(effectiveLanguageForColoring(big) == nullptr);
        REQUIRE(effectiveLanguage(big)->id == "python"); // 身份仍是 Python
    }
}
