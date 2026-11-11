// M3-A U2/U4 单测：查找替换引擎（TC-U10~U17）+ 多光标纯逻辑（TC-U27/U28 纯化部分）。
//
// 语义基准：docs/需求文档/需求-Win32全量重写.md B4/B5 + 旧版 Qt 实现语义
//（QString/QRegularExpression → std::string/PCRE2-8 移植；UTF-16 单元偏移 →
// UTF-8 字节偏移）。测试数据全部内联 UTF-8 字符串（源码 /utf-8 编译），
// 关键字节偏移以注释标明，不依赖外部文件。
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/multicursor.h"
#include "core/searchengine.h"

using namespace core;

// ===== 测试辅助 =====
namespace {

SearchOptions opts(bool matchCase, bool wholeWord, bool regex) {
    SearchOptions o;
    o.matchCase = matchCase;
    o.wholeWord = wholeWord;
    o.regex = regex;
    return o;
}

// 匹配列表 → 可读字符串 "[start,start+len)" 空格分隔，失败输出可对比
std::string fmt(const std::vector<SearchMatch>& matches) {
    std::string out;
    for (const SearchMatch& m : matches) {
        if (!out.empty())
            out.push_back(' ');
        out += "[" + std::to_string(m.start) + "," + std::to_string(m.start + m.length) + ")";
    }
    return out;
}

std::string fmt(const SearchResult& r) { return fmt(r.matches); }

// 选区集合 → "anchor:caret" 空格分隔（旧版测试同款格式）
std::string fmtSel(const std::vector<SelRange>& sels) {
    std::string out;
    for (const SelRange& s : sels) {
        if (!out.empty())
            out.push_back(' ');
        out += std::to_string(s.anchor) + ":" + std::to_string(s.caret);
    }
    return out;
}

std::vector<SelRange> sel(std::initializer_list<std::pair<int, int>> list) {
    std::vector<SelRange> out;
    for (const auto& p : list)
        out.push_back(SelRange{p.first, p.second});
    return out;
}

bool sameSel(const std::vector<SelRange>& a, const std::vector<SelRange>& b) {
    return a == b;
}

}  // namespace

// ===========================================================================
// TC-U10：纯文本查找（大小写敏感/不敏感、全字匹配、元字符字面化）
// ===========================================================================
TEST_CASE("TC-U10 纯文本查找：基本/大小写/中文", "[search][plain]") {
    SECTION("基本：默认选项返回全部出现的精确区间") {
        const SearchResult r = searchFindAll("foo bar foo baz", "foo", SearchOptions{});
        REQUIRE(fmt(r) == "[0,3) [8,11)");
        REQUIRE_FALSE(r.overflowed);
        REQUIRE_FALSE(r.invalidPattern);
    }
    SECTION("大小写开关：不区分全部命中，区分仅精确大小写") {
        const std::string text = "Foo foo FOO";
        REQUIRE(fmt(searchFindAll(text, "foo", opts(false, false, false))) == "[0,3) [4,7) [8,11)");
        REQUIRE(fmt(searchFindAll(text, "foo", opts(true, false, false))) == "[4,7)");
    }
    SECTION("中文无大小写：开关不影响命中（字节偏移正确）") {
        // 你(0-2) 好(3-5) ' '(6) 世(7-9) 界(10-12) ' '(13) 你(14-16) 好(17-19)
        const std::string text = "你好 世界 你好";
        REQUIRE(fmt(searchFindAll(text, "你好", SearchOptions{})) == "[0,6) [14,20)");
        REQUIRE(fmt(searchFindAll(text, "你好", opts(true, false, false))) == "[0,6) [14,20)");
    }
}

TEST_CASE("TC-U10 纯文本查找：全字匹配（英文词边界 + 中文场景）", "[search][plain]") {
    SECTION("英文词边界：foobar 不命中 foo.bar/foo! 命中") {
        const std::string text = "foo foobar foo.bar foo!";
        REQUIRE(fmt(searchFindAll(text, "foo", opts(false, true, false))) == "[0,3) [11,14) [19,22)");
        // 全字关：全部 4 处
        REQUIRE(fmt(searchFindAll(text, "foo", SearchOptions{})) == "[0,3) [4,7) [11,14) [19,22)");
    }
    SECTION("查找串首尾非 ASCII 词字符时不加 \\b：全字开关不影响") {
        REQUIRE(fmt(searchFindAll("a++ b++", "++", opts(false, true, false))) == "[1,3) [5,7)");
        // 中文查找串同理（世界 的首尾是汉字，非 ASCII 词字符——旧版语义：
        // 中文全字匹配与关闭等价，不做 Unicode 分词）
        // 你(0-2) 好(3-5) 世(6-8) 界(9-11) ' '(12) 世(13-15) 界(16-18)
        const std::string zh = "你好世界 世界";
        REQUIRE(fmt(searchFindAll(zh, "世界", opts(false, true, false))) == "[6,12) [13,19)");
        REQUIRE(fmt(searchFindAll(zh, "世界", SearchOptions{})) == "[6,12) [13,19)");
    }
    SECTION("元字符字面化：. + ( ) [ ] \\ 按字面匹配") {
        const std::string text = "a.c abc a+c (x) [y] a\\d";
        REQUIRE(fmt(searchFindAll(text, "a.c", SearchOptions{})) == "[0,3)");   // abc 不被 . 命中
        REQUIRE(fmt(searchFindAll(text, "a+c", SearchOptions{})) == "[8,11)");
        REQUIRE(fmt(searchFindAll(text, "(x)", SearchOptions{})) == "[12,15)");
        REQUIRE(fmt(searchFindAll(text, "[y]", SearchOptions{})) == "[16,19)");
        REQUIRE(fmt(searchFindAll(text, "a\\d", SearchOptions{})) == "[20,23)");
        REQUIRE(fmt(searchFindAll(text, "\\", SearchOptions{})) == "[21,22)");  // 单反斜杠字面
    }
}

// ===========================================================================
// TC-U11：正则模式（PCRE 语义）
// ===========================================================================
TEST_CASE("TC-U11 正则模式：`.` 不匹配换行、^/$ 全文首尾、(?m) 生效", "[search][regex]") {
    SECTION("`.` 不匹配换行") {
        const SearchResult none = searchFindAll("a\nb", "a.b", opts(false, false, true));
        REQUIRE(fmt(none).empty());
        REQUIRE(fmt(searchFindAll("acb", "a.b", opts(false, false, true))) == "[0,3)");
    }
    SECTION("^/$ 默认仅全文首尾（非多行）") {
        // x(0) \n(1) x(2)
        REQUIRE(fmt(searchFindAll("x\nx", "^x", opts(false, false, true))) == "[0,1)");
        REQUIRE(fmt(searchFindAll("x\nx", "x$", opts(false, false, true))) == "[2,3)");
    }
    SECTION("(?m) 内联开启多行") {
        REQUIRE(fmt(searchFindAll("x\nx", "(?m)^x", opts(false, false, true))) == "[0,1) [2,3)");
        REQUIRE(fmt(searchFindAll("a\nb", "(?m)^b", opts(false, false, true))) == "[2,3)");
    }
    SECTION("字符类与量词") {
        REQUIRE(fmt(searchFindAll("ab12cd345", "[0-9]+", opts(false, false, true))) == "[2,4) [6,9)");
        // 贪婪 a{2,3} 在 aaaa 仅一处 [0,3)（剩 1 个 a 不足量词）
        REQUIRE(fmt(searchFindAll("aaaa", "a{2,3}", opts(false, false, true))) == "[0,3)");
    }
    SECTION("中文正则（码点区间写法）") {
        // 你(0-2) 好(3-5) ' '(6) w(7)…d(11) ' '(12) 世(13-15) 界(16-18)
        const std::string text = "你好 world 世界";
        REQUIRE(fmt(searchFindAll(text, "[\\x{4e00}-\\x{9fa5}]+", opts(false, false, true))) ==
                "[0,6) [13,19)");
    }
    SECTION("捕获组参与匹配（供替换展开）") {
        const SearchResult r = searchFindAll("user@host", "(\\w+)@(\\w+)", opts(false, false, true));
        REQUIRE(fmt(r) == "[0,9)");
    }
}

// ===========================================================================
// TC-U12：无效正则（invalidPattern + 不崩溃）
// ===========================================================================
TEST_CASE("TC-U12 无效正则：错误标志与消息，不崩溃", "[search][regex][invalid]") {
    const std::vector<std::string> invalid{"(", "[unclosed", "*x", "a)", "(?<name"};
    for (const std::string& pattern : invalid) {
        const SearchResult r = searchFindAll("abc", pattern, opts(false, false, true));
        INFO("pattern: " << pattern);
        REQUIRE(r.invalidPattern);
        REQUIRE(r.matches.empty());
        REQUIRE_FALSE(r.overflowed);
        REQUIRE_FALSE(r.errorMessage.empty());
        REQUIRE(r.errorMessage.find(L"无效") != std::wstring::npos);
    }
    SECTION("纯文本模式下任意“看起来像坏正则”的串经字面化恒合法") {
        const SearchResult r = searchFindAll("a(b(c", "(", SearchOptions{});
        REQUIRE_FALSE(r.invalidPattern);
        REQUIRE(r.errorMessage.empty());
        REQUIRE(fmt(r) == "[1,2) [3,4)");
    }
    SECTION("空查找串：合法但恒无匹配（不是错误）") {
        const SearchResult r = searchFindAll("abc", "", SearchOptions{});
        REQUIRE_FALSE(r.invalidPattern);
        REQUIRE(r.matches.empty());
    }
}

// ===========================================================================
// TC-U13：零长度匹配跳过（DEC-005）
// ===========================================================================
TEST_CASE("TC-U13 零长度匹配跳过", "[search][regex]") {
    SECTION("a* 仅命中实际 a，空匹配不计入无死循环") {
        REQUIRE(fmt(searchFindAll("abc", "a*", opts(false, false, true))) == "[0,1)");
        REQUIRE(fmt(searchFindAll("abc", "b*", opts(false, false, true))) == "[1,2)");
        REQUIRE(fmt(searchFindAll("xxy", "x*", opts(false, false, true))) == "[0,2)");
    }
    SECTION("全零长度模式：匹配数为 0") {
        const SearchResult r = searchFindAll("xyz", "q*", opts(false, false, true));
        REQUIRE(r.matches.empty());
        REQUIRE_FALSE(r.overflowed);
        // 空捕获组 () 同理：恒零长度 → 无匹配
        REQUIRE(searchFindAll("ab", "()", opts(false, false, true)).matches.empty());
    }
}

// ===========================================================================
// TC-U14：>10000 截断（DEC-004）
// ===========================================================================
TEST_CASE("TC-U14 匹配数 >10000 截断", "[search][cap]") {
    SECTION("10001 个匹配：截断为 10000 + 溢出标志") {
        const std::string text(10001, 'a');
        const SearchResult r = searchFindAll(text, "a", SearchOptions{});
        REQUIRE(r.matches.size() == static_cast<std::size_t>(kMaxMatches));
        REQUIRE(r.matches.size() == 10000);
        REQUIRE(r.overflowed);
        REQUIRE(r.matches.front().start == 0);
        REQUIRE(r.matches.back().start == 9999);
    }
    SECTION("恰好 10000：不误报溢出") {
        const std::string text(kMaxMatches, 'a');
        const SearchResult r = searchFindAll(text, "a", SearchOptions{});
        REQUIRE(r.matches.size() == 10000);
        REQUIRE_FALSE(r.overflowed);
    }
}

// ===========================================================================
// TC-U15：替换串展开（DEC-006）
// ===========================================================================
TEST_CASE("TC-U15 替换展开：$n/\\n 捕获组引用", "[search][replace]") {
    // 两组：user@host
    const SearchResult two = searchFindAll("user@host", "(\\w+)@(\\w+)", opts(false, false, true));
    const SearchMatch m2 = two.matches.front();
    const void* md2 = searchRebuildMatchData("user@host", "(\\w+)@(\\w+)",
                                             opts(false, false, true), m2);
    REQUIRE(md2 != nullptr);
    SECTION("$2.$1 与 \\2.\\1 等价") {
        REQUIRE(searchExpandReplacement("$2.$1", "user@host", m2, md2) == "host.user");
        REQUIRE(searchExpandReplacement("\\2.\\1", "user@host", m2, md2) == "host.user");
    }
    searchFreeMatchData(md2);

    // 十组：$10 贪婪取两位
    const std::string tenPattern = "(1)(2)(3)(4)(5)(6)(7)(8)(9)(x)";
    const SearchResult ten = searchFindAll("123456789x", tenPattern, opts(false, false, true));
    REQUIRE(ten.matches.size() == 1);
    const void* md10 =
        searchRebuildMatchData("123456789x", tenPattern, opts(false, false, true), ten.matches.front());
    REQUIRE(md10 != nullptr);
    REQUIRE(searchExpandReplacement("$10", "123456789x", ten.matches.front(), md10) == "x");
    REQUIRE(searchExpandReplacement("$9", "123456789x", ten.matches.front(), md10) == "9");
    REQUIRE(searchExpandReplacement("$1", "123456789x", ten.matches.front(), md10) == "1");
    searchFreeMatchData(md10);

    // 单组：不存在组 / $$ / $0 / 字面
    const SearchResult one = searchFindAll("user", "(\\w+)", opts(false, false, true));
    const SearchMatch m1 = one.matches.front();
    const void* md1 = searchRebuildMatchData("user", "(\\w+)", opts(false, false, true), m1);
    REQUIRE(md1 != nullptr);
    SECTION("不存在组输出空串") {
        REQUIRE(searchExpandReplacement("$10", "user", m1, md1).empty());  // 第 10 组不存在
        REQUIRE(searchExpandReplacement("$2", "user", m1, md1).empty());
        REQUIRE(searchExpandReplacement("\\5", "user", m1, md1).empty());
    }
    SECTION("$$ 字面 $；$0 整体匹配；$ 后非数字字面") {
        REQUIRE(searchExpandReplacement("$$", "user", m1, md1) == "$");
        REQUIRE(searchExpandReplacement("$0", "user", m1, md1) == "user");
        REQUIRE(searchExpandReplacement("$x", "user", m1, md1) == "$x");
        REQUIRE(searchExpandReplacement("$1", "user", m1, md1) == "user");
    }
    SECTION("全角数字不是组引用：$１ 输出字面 $１") {
        REQUIRE(searchExpandReplacement("$１x", "user", m1, md1) == "$１x");
    }
    SECTION("反斜杠序列不做转义解释；串尾 $ / \\ 字面") {
        REQUIRE(searchExpandReplacement("$1\\n", "user", m1, md1) == "user\\n");  // \ + n 两个 literal
        REQUIRE(searchExpandReplacement("\\t", "user", m1, md1) == "\\t");
        REQUIRE(searchExpandReplacement("$1\n", "user", m1, md1) == "user\n");   // 真实换行原样
        REQUIRE(searchExpandReplacement("abc$", "user", m1, md1) == "abc$");
        REQUIRE(searchExpandReplacement("abc\\", "user", m1, md1) == "abc\\");
    }
    searchFreeMatchData(md1);

    SECTION("纯文本模式（matchData=nullptr）：$0 = 匹配文本，组引用为空") {
        const SearchResult plain = searchFindAll("xx ab yy", "ab", SearchOptions{});
        REQUIRE(fmt(plain) == "[3,5)");
        const SearchMatch pm = plain.matches.front();
        REQUIRE(searchExpandReplacement("[$0]", "xx ab yy", pm, nullptr) == "[ab]");
        REQUIRE(searchExpandReplacement("\\1", "xx ab yy", pm, nullptr).empty());
        REQUIRE(searchExpandReplacement("a\\b", "xx ab yy", pm, nullptr) == "a\\b");
    }
    SECTION("重建数据失配（match 区间与重匹配不一致）返回 nullptr 不崩溃") {
        const SearchMatch bogus{100, 5};  // 越界
        REQUIRE(searchRebuildMatchData("user", "(\\w+)", opts(false, false, true), bogus) == nullptr);
        // nullptr 展开退化为纯文本语义
        REQUIRE(searchExpandReplacement("$0", "user", SearchMatch{0, 4}, nullptr) == "user");
    }
}

// ===========================================================================
// TC-U16：全部替换（原文本坐标一次性计算）
// ===========================================================================
TEST_CASE("TC-U16 全部替换：坐标计算与产物不重匹配", "[search][replace]") {
    SECTION("中文多行") {
        // 苹(0-2) 果(3-5) \n(6) 香(7-9) 蕉(10-12) \n(13) 苹(14-16) 果(17-19)
        const ReplaceResult r =
            searchReplaceAll("苹果\n香蕉\n苹果", "苹果", "橘子", SearchOptions{});
        REQUIRE(r.replacedCount == 2);
        REQUIRE(r.newContent == "橘子\n香蕉\n橘子");
    }
    SECTION("替换产物不参与本轮：a→aa 不死循环") {
        const ReplaceResult r = searchReplaceAll("aaa", "a", "aa", SearchOptions{});
        REQUIRE(r.replacedCount == 3);
        REQUIRE(r.newContent == "aaaaaa");
    }
    SECTION("正则捕获组替换：原坐标计算、前段偏移不受后段影响") {
        const ReplaceResult r = searchReplaceAll("k1=1 k2=22 k3=333", "k(\\d+)=(\\d+)",
                                                 "K[$1]=$2", opts(false, false, true));
        REQUIRE(r.replacedCount == 3);
        REQUIRE(r.newContent == "K[1]=1 K[2]=22 K[3]=333");
    }
    SECTION("从后往前应用等价性：findAll + 每处展开 + 降序应用 == searchReplaceAll") {
        const std::string text = "foo a foo b foo";
        const SearchResult found = searchFindAll(text, "foo", SearchOptions{});
        REQUIRE(fmt(found) == "[0,3) [6,9) [12,15)");
        // 计算编辑（升序）后按 start 降序应用到副本——旧版控制器契约
        struct Edit { int start; int length; std::string text; };
        std::vector<Edit> edits;
        for (const SearchMatch& m : found.matches) {
            const void* md =
                searchRebuildMatchData(text, "foo", SearchOptions{}, m);  // 纯文本 → nullptr
            edits.push_back({m.start, m.length, searchExpandReplacement("XY", text, m, md)});
            searchFreeMatchData(md);
        }
        std::string mutated = text;
        for (auto it = edits.rbegin(); it != edits.rend(); ++it)
            mutated.replace(static_cast<std::size_t>(it->start), static_cast<std::size_t>(it->length),
                            it->text);
        const ReplaceResult r = searchReplaceAll(text, "foo", "XY", SearchOptions{});
        REQUIRE(mutated == "XY a XY b XY");     // 中间状态：降序应用正确
        REQUIRE(r.newContent == mutated);       // 与单批结果一致
        REQUIRE(r.replacedCount == 3);
    }
    SECTION("零长度模式不替换；无效正则/空查找串内容不变") {
        REQUIRE(searchReplaceAll("xyz", "q*", "-", opts(false, false, true)).replacedCount == 0);
        REQUIRE(searchReplaceAll("xyz", "q*", "-", opts(false, false, true)).newContent == "xyz");
        REQUIRE(searchReplaceAll("abc", "((", "X", opts(false, false, true)).replacedCount == 0);
        REQUIRE(searchReplaceAll("abc", "((", "X", opts(false, false, true)).newContent == "abc");
        REQUIRE(searchReplaceAll("abc", "", "X", SearchOptions{}).newContent == "abc");
    }
    SECTION("溢出时只替换前 10000 处") {
        const std::string text(10001, 'a');
        const ReplaceResult r = searchReplaceAll(text, "a", "b", SearchOptions{});
        REQUIRE(r.replacedCount == 10000);
        REQUIRE(r.newContent.size() == 10001);
        REQUIRE(r.newContent.substr(0, 10000) == std::string(10000, 'b'));
        REQUIRE(r.newContent.back() == 'a');
    }
}

// ===========================================================================
// TC-U17：环绕导航
// ===========================================================================
TEST_CASE("TC-U17 环绕导航", "[search][nav]") {
    SearchResult r;
    r.matches = {{0, 3}, {8, 11}, {16, 19}};  // 3 个匹配
    SECTION("末尾 findNext 回到首个；头部 findPrev 回到末个") {
        REQUIRE(searchNextIndex(r, 2, true) == 0);
        REQUIRE(searchNextIndex(r, 0, false) == 2);
        REQUIRE(searchNextIndex(r, 0, true) == 1);
        REQUIRE(searchNextIndex(r, 2, false) == 1);
    }
    SECTION("单匹配环绕回自身") {
        SearchResult single;
        single.matches = {{0, 3}};
        REQUIRE(searchNextIndex(single, 0, true) == 0);
        REQUIRE(searchNextIndex(single, 0, false) == 0);
    }
    SECTION("空匹配列表返回 -1") {
        SearchResult empty;
        REQUIRE(searchNextIndex(empty, 0, true) == -1);
        REQUIRE(searchNextIndex(empty, -1, false) == -1);
    }
    SECTION("无当前索引：next 从 0、prev 从最后一个开始") {
        REQUIRE(searchNextIndex(r, -1, true) == 0);
        REQUIRE(searchNextIndex(r, -1, false) == 2);
    }
    SECTION("与 findAll 集成：Enter/Shift+Enter 序列") {
        const SearchResult found = searchFindAll("foo bar foo baz foo", "foo", SearchOptions{});
        REQUIRE(fmt(found) == "[0,3) [8,11) [16,19)");
        int cur = -1;
        cur = searchNextIndex(found, cur, true);   // 打开后 Enter → 第 1 个
        REQUIRE(found.matches[static_cast<std::size_t>(cur)].start == 0);
        cur = searchNextIndex(found, cur, true);
        cur = searchNextIndex(found, cur, true);
        REQUIRE(found.matches[static_cast<std::size_t>(cur)].start == 16);
        cur = searchNextIndex(found, cur, true);   // 末尾环绕回首
        REQUIRE(found.matches[static_cast<std::size_t>(cur)].start == 0);
        cur = searchNextIndex(found, cur, false);  // 回退
        REQUIRE(found.matches[static_cast<std::size_t>(cur)].start == 16);
    }
    SECTION("计数重载与结果重载逐点一致（免 SearchResult 深拷贝路径）") {
        const int n = static_cast<int>(r.matches.size());
        const int cursors[] = {-1, 0, 1, 2, n, n + 5};
        for (const int cur : cursors) {
            for (const bool fwd : {true, false}) {
                REQUIRE(searchNextIndex(n, cur, fwd) == searchNextIndex(r, cur, fwd));
            }
        }
        REQUIRE(searchNextIndex(0, 0, true) == -1);   // 空计数
        REQUIRE(searchNextIndex(0, -1, false) == -1); // 空计数
        REQUIRE(searchNextIndex(1, 0, true) == 0);    // 单计数环绕回自身
    }
}

// ===========================================================================
// TC-U27：Ctrl+D 下一个相同词（纯化部分）
// ===========================================================================
TEST_CASE("TC-U27 Ctrl+D：无选区选词 / 依次追加 / 环绕", "[mc][ctrlD]") {
    const std::string text = "foo bar foo baz foo";  // 出现：0、8、16
    SECTION("无选区：选中光标处单词") {
        REQUIRE(multicursorNextOccurrence(text, sel({{1, 1}})) == std::optional<SelRange>{SelRange{0, 3}});
    }
    SECTION("依次追加到全部选中，之后无变化") {
        REQUIRE(multicursorNextOccurrence(text, sel({{0, 3}})) == std::optional<SelRange>{SelRange{8, 11}});
        REQUIRE(multicursorNextOccurrence(text, sel({{0, 3}, {8, 11}})) ==
                std::optional<SelRange>{SelRange{16, 19}});
        REQUIRE(multicursorNextOccurrence(text, sel({{0, 3}, {8, 11}, {16, 19}})) == std::nullopt);
    }
    SECTION("环绕：从文末选区环绕找到文首出现") {
        REQUIRE(multicursorNextOccurrence(text, sel({{9, 9}})) == std::optional<SelRange>{SelRange{8, 11}});
        REQUIRE(multicursorNextOccurrence(text, sel({{8, 11}})) ==
                std::optional<SelRange>{SelRange{16, 19}});
        REQUIRE(multicursorNextOccurrence(text, sel({{8, 11}, {16, 19}})) ==
                std::optional<SelRange>{SelRange{0, 3}});
    }
}

TEST_CASE("TC-U27 Ctrl+D：大小写敏感 / 不跨行 / 无词处", "[mc][ctrlD]") {
    SECTION("大小写敏感：目标 Foo 无其他精确出现") {
        REQUIRE(multicursorNextOccurrence("Foo foo fOO", sel({{1, 1}})) ==
                std::optional<SelRange>{SelRange{0, 3}});
        REQUIRE(multicursorNextOccurrence("Foo foo fOO", sel({{0, 3}})) == std::nullopt);
    }
    SECTION("选区含换行不匹配；单词不跨行") {
        REQUIRE(multicursorNextOccurrence("ab\ncd ab", sel({{0, 3}})) == std::nullopt);  // "ab\n"
        // 词 "fo" 止于换行
        REQUIRE(multicursorNextOccurrence("fo\no", sel({{1, 1}})) ==
                std::optional<SelRange>{SelRange{0, 2}});
    }
    SECTION("空白处无词：无操作") {
        REQUIRE(multicursorNextOccurrence("foo bar", sel({{3, 3}})) == std::nullopt);
        REQUIRE(multicursorNextOccurrence("foo bar", sel({})).has_value() == false);
    }
}

TEST_CASE("TC-U27 Ctrl+D：严格重叠判定（端点相接允许追加，DEC-010）", "[mc][ctrlD][overlap]") {
    SECTION("部分重叠：出现均与现有选区共享字符 → 全部跳过") {
        // "aaaa" 出现 [0,2)/[1,3)/[2,4) 均与 [1,3) 严格重叠
        REQUIRE(multicursorNextOccurrence("aaaa", sel({{1, 3}})) == std::nullopt);
    }
    SECTION("端点相接不算重叠：[0,2) 与现有 [2,4) 仅共享端点 → 允许追加") {
        REQUIRE(multicursorNextOccurrence("aaaa", sel({{2, 4}})) ==
                std::optional<SelRange>{SelRange{0, 2}});
    }
    SECTION("连续重复词：foofoo 选 [0,3) 后追加紧邻 [3,6)") {
        REQUIRE(multicursorNextOccurrence("foofoo", sel({{0, 3}})) ==
                std::optional<SelRange>{SelRange{3, 6}});
    }
    SECTION("完全不相交的出现正常追加") {
        REQUIRE(multicursorNextOccurrence("aa aa aa", sel({{3, 5}})) ==
                std::optional<SelRange>{SelRange{6, 8}});
    }
    SECTION("混合状态：末位空光标回退取最后一个有选区光标为目标") {
        REQUIRE(multicursorNextOccurrence("aa aa aa", sel({{0, 2}, {7, 7}})) ==
                std::optional<SelRange>{SelRange{3, 5}});
    }
    SECTION("中文词匹配与中文标点边界") {
        // 代(0-2) 码(3-5) ' '(6) 注(7-9) 释(10-12) ' '(13) 代(14-16) 码(17-19)
        REQUIRE(multicursorNextOccurrence("代码 注释 代码", sel({{3, 3}})) ==
                std::optional<SelRange>{SelRange{0, 6}});
        REQUIRE(multicursorNextOccurrence("代码 注释 代码", sel({{0, 6}})) ==
                std::optional<SelRange>{SelRange{14, 20}});
        // 你(0-2) 好(3-5) ，(6-8) 世(9-11) 界(12-14)：，为词边界
        REQUIRE(multicursorNextOccurrence("你好，世界", sel({{0, 0}})) ==
                std::optional<SelRange>{SelRange{0, 6}});
    }
}

TEST_CASE("补充：Ctrl+D 重叠命中最坏场景不平方卡顿（跳到选区末尾）", "[mc][ctrlD][overlap]") {
    SECTION("大量单字符选区全重叠：结果 nullopt 且快速返回") {
        // 全同字符 + 每个字符都已选中：所有出现均重叠，须 nullopt。
        // 逐字符推进的旧实现为 出现数×选区数（60000² ≈ 3.6e9）量级比较；
        // 跳到重叠选区最大右端后为线性 + 二分。
        const int n = 60000;
        const std::string text(static_cast<std::size_t>(n), 'a');
        std::vector<SelRange> sels;
        sels.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) sels.push_back(SelRange{i, i + 1});
        const auto t0 = std::chrono::steady_clock::now();
        REQUIRE(multicursorNextOccurrence(text, sels) == std::nullopt);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        REQUIRE(ms < 5000);  // 回归防护：新实现毫秒级；平方实现远超此界
    }
    SECTION("非重叠全选中（Ctrl+Shift+L 后继续 Ctrl+D 语义不变）") {
        // "ab"*K 全部按词选中后，目标 "ab" 的所有出现均重叠 → nullopt；
        // 半数选中时其余出现按序追加（结果语义与逐字符推进实现一致）
        const std::string text = "abababab";
        std::vector<SelRange> all;
        for (int i = 0; i + 1 < 8; i += 2) all.push_back(SelRange{i, i + 2});
        REQUIRE(multicursorNextOccurrence(text, all) == std::nullopt);
        // 奇数位起点出现（[1,3)/[3,5)...）与偶数位选区 [0,2)/[2,4)... 重叠，
        // 仅剩下标 0 起点的 [0,2) 环绕判定（已选中）→ 无可追加
        std::vector<SelRange> half;
        for (int i = 2; i + 1 < 8; i += 2) half.push_back(SelRange{i, i + 2});
        REQUIRE(multicursorNextOccurrence(text, half) ==
                std::optional<SelRange>{SelRange{0, 2}});
    }
    SECTION("无序选区输入（乱序注册）判定不依赖列表顺序") {
        // 列表乱序 + 嵌套选区：与有序输入同判（结构按 start 排序后判定）
        REQUIRE(multicursorNextOccurrence("aaaa", sel({{3, 4}, {0, 2}, {1, 3}})) == std::nullopt);
        REQUIRE(multicursorNextOccurrence("aa aa aa", sel({{6, 8}, {0, 2}})) ==
                std::optional<SelRange>{SelRange{3, 5}});
    }
}

// ===========================================================================
// TC-U27：Ctrl+Shift+L 全部相同实例（纯化部分）
// ===========================================================================
TEST_CASE("TC-U27 Ctrl+Shift+L：全部相同文本实例选区集合", "[mc][ctrlShiftL]") {
    SECTION("从光标处单词出发：全部出现、从左到右") {
        REQUIRE(fmtSel(multicursorAllOccurrences("cat dog cat cat", sel({{1, 1}}))) ==
                "0:3 8:11 12:15");
    }
    SECTION("从既有选区出发") {
        REQUIRE(fmtSel(multicursorAllOccurrences("cat dog cat cat", sel({{4, 7}}))) == "4:7");
    }
    SECTION("重叠出现按非重叠从左到右只取一个") {
        REQUIRE(fmtSel(multicursorAllOccurrences("aaaa", sel({{1, 1}}))) == "0:4");
    }
    SECTION("无目标：原样返回当前集合") {
        const auto unchanged = sel({{1, 1}});
        REQUIRE(sameSel(multicursorAllOccurrences("   ", unchanged), unchanged));
    }
    SECTION("中文") {
        REQUIRE(fmtSel(multicursorAllOccurrences("代码 注释 代码", sel({{0, 0}}))) == "0:6 14:20");
    }
}

// ===========================================================================
// TC-U28：交互语义判定（纯化部分）
// ===========================================================================
TEST_CASE("TC-U28 Ctrl+Alt+Up/Down：字符列保持（含中文）", "[mc][addline]") {
    SECTION("ASCII：上下行同列") {
        // a(0) b(1) \n(2) c(3) d(4) e(5) f(6) \n(7) g(8) h(9)
        const std::string text = "ab\ncdef\ngh";
        REQUIRE(fmtSel(multicursorAddLine(text, sel({{5, 5}}), true)) == "2:2 5:5");    // 上：行末恰为列 2
        REQUIRE(fmtSel(multicursorAddLine(text, sel({{5, 5}}), false)) == "5:5 10:10");  // 下：同列
    }
    SECTION("中文：字符列（非字节列）保持") {
        // a(0) b(1) \n(2) 中(3-5) 文(6-8) x(9) \n(10) c(11)
        const std::string text = "ab\n中文x\nc";
        // 光标 6 = 「中」之后（字符列 1）：上行列 1 → 字节 1；下行列 1 → 短行钳制行末 12
        REQUIRE(fmtSel(multicursorAddLine(text, sel({{6, 6}}), true)) == "1:1 6:6");
        REQUIRE(fmtSel(multicursorAddLine(text, sel({{6, 6}}), false)) == "6:6 12:12");
        // 光标 9 = 「x」之前（字符列 2）：上行列 2 → 行末 2（字节钳制）
        REQUIRE(fmtSel(multicursorAddLine(text, sel({{9, 9}}), true)) == "2:2 9:9");
    }
    SECTION("首行上方/末行下方：无新增（返回与输入等价）") {
        const std::string text = "ab\ncd";
        const auto one = sel({{0, 0}});
        REQUIRE(sameSel(multicursorAddLine(text, one, true), one));
        const auto last = sel({{5, 5}});
        REQUIRE(sameSel(multicursorAddLine(text, last, false), last));
    }
    SECTION("短行钳制行末；重复添加去重；多光标整体平移") {
        // abcdef(0-5) \n(6) x(7) \n(8) abcdef(9-14)
        const std::string text = "abcdef\nx\nabcdef";
        REQUIRE(fmtSel(multicursorAddLine(text, sel({{5, 5}}), false)) == "5:5 8:8");  // 列 5 钳制行末 8
        // 上/下目标已存在：集合不变
        const auto both = sel({{0, 0}, {3, 3}});
        REQUIRE(sameSel(multicursorAddLine("ab\nab", both, true), both));   // 3 的上方 0 已存在
        REQUIRE(sameSel(multicursorAddLine("ab\nab", both, false), both));  // 0 的下方 3 已存在
        // 多光标整体下移一行
        REQUIRE(fmtSel(multicursorAddLine("ab\ncd\nef", sel({{1, 1}, {4, 4}}), false)) ==
                "1:1 4:4 7:7");
    }
}

TEST_CASE("TC-U28 Shift+Alt+I：选中行行尾插光标", "[mc][lineends]") {
    // a(0) b(1) \n(2) c(3) d(4) e(5) f(6) \n(7) g(8) h(9)：行尾 2、7、10
    const std::string text = "ab\ncdef\ngh";
    SECTION("跨第 1~2 行部分选中 → 两行行尾") {
        REQUIRE(fmtSel(multicursorLineEnds(text, 0, 5)) == "2:2 7:7");
    }
    SECTION("覆盖全部 3 行（含最后一行；反向选区等价）") {
        REQUIRE(fmtSel(multicursorLineEnds(text, 0, 10)) == "2:2 7:7 10:10");
        REQUIRE(fmtSel(multicursorLineEnds(text, 10, 0)) == "2:2 7:7 10:10");
        // 选区终点 == 文档末：末行同样包含
        REQUIRE(fmtSel(multicursorLineEnds(text, 0, 100)) == "2:2 7:7 10:10");
    }
    SECTION("单行选区只出一行") {
        REQUIRE(fmtSel(multicursorLineEnds(text, 8, 10)) == "10:10");
    }
    SECTION("空行（行尾 == 行首）正常产出空光标") {
        // "a\n\nb"：行尾 1、2、4
        REQUIRE(fmtSel(multicursorLineEnds("a\n\nb", 0, 4)) == "1:1 2:2 4:4");
    }
}

TEST_CASE("TC-U28 Esc 收缩 / 粘贴分发判定", "[mc][collapse][paste]") {
    SECTION("Esc：保留按位置最末光标（保 anchor/caret）") {
        REQUIRE(fmtSel({multicursorCollapse(sel({{1, 1}, {4, 7}, {9, 9}}))}) == "9:9");
        REQUIRE(fmtSel({multicursorCollapse(sel({{1, 1}, {5, 9}}))}) == "5:9");
        REQUIRE(fmtSel({multicursorCollapse(sel({{3, 0}, {8, 8}}))}) == "8:8");  // 无序输入取 caret 最大
    }
    SECTION("粘贴分发：行数 == 光标数 → 逐行分发；否则每光标整份") {
        REQUIRE(multicursorPasteDistributes(3, 3));
        REQUIRE(multicursorPasteDistributes(1, 1));
        REQUIRE_FALSE(multicursorPasteDistributes(5, 3));
        REQUIRE_FALSE(multicursorPasteDistributes(2, 3));
        REQUIRE_FALSE(multicursorPasteDistributes(0, 3));
        REQUIRE_FALSE(multicursorPasteDistributes(0, 0));
    }
}

TEST_CASE("TC-U28 wordRangeAt：中英文词边界", "[mc][word]") {
    // 你(0-2) 好(3-5) ，(6-8) 世(9-11) 界(12-14)
    const std::string zh = "你好，世界";
    SECTION("中文按连续非空白非标点成词；中文标点为边界") {
        REQUIRE(wordRangeAt(zh, 0) == std::optional<SelRange>{SelRange{0, 6}});
        REQUIRE(wordRangeAt(zh, 3) == std::optional<SelRange>{SelRange{0, 6}});
        REQUIRE(wordRangeAt(zh, 6) == std::nullopt);  // ，上无词
        REQUIRE(wordRangeAt(zh, 9) == std::optional<SelRange>{SelRange{9, 15}});
        REQUIRE(wordRangeAt(zh, 12) == std::optional<SelRange>{SelRange{9, 15}});
    }
    SECTION("英文：下划线并入词，空白/标点为边界；ASCII 符号按词字符") {
        REQUIRE(wordRangeAt("foo_bar", 3) == std::optional<SelRange>{SelRange{0, 7}});
        REQUIRE(wordRangeAt("foo bar", 3) == std::nullopt);
        REQUIRE(wordRangeAt("foo bar", 5) == std::optional<SelRange>{SelRange{4, 7}});
        // 旧版语义：可打印符号（Unicode S* 类）并入词字符（$ + = | 等成词），
        // 标点（P* 类，如 , . !）为分隔
        REQUIRE(wordRangeAt("a+b", 1) == std::optional<SelRange>{SelRange{0, 3}});
        REQUIRE(wordRangeAt("a$b", 1) == std::optional<SelRange>{SelRange{0, 3}});
        REQUIRE(wordRangeAt("a,b", 1) == std::nullopt);
    }
    SECTION("中英混排连续成词；多字节序列中间解析为该字符") {
        // a(0) b(1) c(2) 中(3-5) 文(6-8) 1(9) 2(10) 3(11)
        const std::string mix = "abc中文123";
        REQUIRE(wordRangeAt(mix, 0) == std::optional<SelRange>{SelRange{0, 12}});
        REQUIRE(wordRangeAt(mix, 10) == std::optional<SelRange>{SelRange{0, 12}});
        REQUIRE(wordRangeAt(mix, 7) == std::optional<SelRange>{SelRange{0, 12}});  // 「文」序列中间
    }
    SECTION("边界防御：文末/负值/越界/增补平面（旧版按代理半区判非词）") {
        REQUIRE(wordRangeAt("foo", 3) == std::nullopt);  // 文末
        REQUIRE(wordRangeAt("foo", -1) == std::nullopt);
        REQUIRE(wordRangeAt("foo", 99) == std::nullopt);  // 越界钳制到文末 → 无词
        // 😀（U+1F600，代理对）：旧版按 UTF-16 半区判定为分隔符，保持一致
        REQUIRE(wordRangeAt("\xF0\x9F\x98\x80", 0) == std::nullopt);
    }
    SECTION("分类修正锚点：· 间隔号为分隔（中文人名），½ 简分数为词字符") {
        // GetStringTypeW 与 Unicode 类别的已知偏差经 curated 修正对齐旧版：
        // 玛(0-2) 丽(3-5) ·(6-7) 居(8-10) 里(11-13)
        REQUIRE(wordRangeAt("玛丽·居里", 0) == std::optional<SelRange>{SelRange{0, 6}});
        REQUIRE(wordRangeAt("玛丽·居里", 6) == std::nullopt);  // · 分隔
        REQUIRE(wordRangeAt("玛丽·居里", 8) == std::optional<SelRange>{SelRange{8, 14}});
        REQUIRE(wordRangeAt("a\xC2\xBD" "b", 1) == std::optional<SelRange>{SelRange{0, 4}});  // ½ 词字符
    }
}
