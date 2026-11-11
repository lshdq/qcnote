// qcnote 查找替换引擎实现（M3-A）：PCRE2 封装内聚在本文件（头文件不泄
// PCRE2 类型）。移植自 Qt 版 SearchEngine（QString/QRegularExpression →
// std::string/PCRE2-8）。
#include "core/searchengine.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include "pcre2.h"

#include <cstdint>
#include <string_view>

namespace core {

// vendored PCRE2 版本钉子（DEC-012）：版本变化时语义映射需复核
//（\b 词字符口径、CASELESS 折叠范围、默认换行约定等）。
static_assert(PCRE2_MAJOR == 10 && PCRE2_MINOR == 45,
              "vendored PCRE2 版本与 DEC-012 钉定版本（10.45）不一致，请复核语义映射");

namespace {

// PCRE2 \b 意义下的词字符（ASCII 字母/数字/下划线）。旧版 QRegularExpression
// 默认不开 UseUnicodePropertiesOption，\b 按 ASCII 词字符判定；全字模式附加
// \b 必须与此口径一致，否则「边界字符」与「断言语义」错位会错杀合法命中。
bool isAsciiWordChar(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') || u == '_';
}

bool isAsciiDigit(char c) {
    return c >= '0' && c <= '9';
}

// 无效首字节 → 1（按单字节字符推进，与内容契约的 U+FFFD 呈现一一对应）；
// 否则该码点的 UTF-8 字节长度（1..4）。仅按首字节区间判定。
int utf8LeadLength(unsigned char b) {
    if (b < 0x80) return 1;
    if (b >= 0xC2 && b <= 0xDF) return 2;
    if (b >= 0xE0 && b <= 0xEF) return 3;
    if (b >= 0xF0 && b <= 0xF4) return 4;
    return 1;
}

// 编译后的查找模式：code 为空表示空查找串（合法恒无匹配）或编译失败（invalid）。
struct CompiledPattern {
    pcre2_code* code = nullptr;
    bool invalid = false;
    int errorcode = 0;
    PCRE2_SIZE erroroffset = 0;
};

// 设置查找模式并编译：
// - 正则模式：查找串原样交给 PCRE2；wholeWord 被有意忽略（词边界语义由用户
//   在模式中自行表达，叠加自动 \b 会与量词/断言产生不可预期组合）
// - 纯文本模式：全部 PCRE2 元字符字面化（等价旧版 QRegularExpression::escape）
// - 全字模式：仅在首/尾是（ASCII）词字符的那一侧加 \b（参照 VS Code /
//   Notepad++ 做法："+foo"、中文查找串首尾非词字符时无脑包 \b 反而错杀
//   串首/串尾的合法命中）
CompiledPattern compilePattern(const std::string& pattern, const SearchOptions& options) {
    CompiledPattern cp;
    if (pattern.empty())
        return cp;  // 空查找串：合法但恒无匹配（调用方显示空计数，不算错误）

    std::string expr;
    if (options.regex) {
        expr = pattern;
    } else {
        expr.reserve(pattern.size() + 8);
        for (const char ch : pattern) {
            switch (ch) {
                case '\\': case '^': case '$': case '.': case '|': case '?':
                case '*': case '+': case '(': case ')': case '[': case ']':
                case '{': case '}':
                    expr.push_back('\\');
                    break;
                default:
                    break;
            }
            expr.push_back(ch);
        }
        if (options.wholeWord) {
            if (isAsciiWordChar(pattern.front()))
                expr.insert(expr.begin(), {'\\', 'b'});
            if (isAsciiWordChar(pattern.back()))
                expr.append("\\b");
        }
    }

    uint32_t flags = PCRE2_UTF;  // UTF-8 subject；不启用 PCRE2_UCP（\b/\w 保持
                                 // ASCII 词字符口径，与旧版 Qt 默认语义一致）
    if (!options.matchCase)
        flags |= PCRE2_CASELESS;  // ASCII 简单折叠（中文无大小写不受影响）
    cp.code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(expr.data()),
                            static_cast<PCRE2_SIZE>(expr.size()), flags, &cp.errorcode,
                            &cp.erroroffset, nullptr);
    cp.invalid = cp.code == nullptr;
    return cp;
}

// 无效正则的中文错误消息（旧版格式移植；PCRE2 原文为英文，直接附后）。
// erroroffset 为出错位置在查找串中的字节偏移（正则模式下与查找串一致）。
std::wstring formatPatternError(int errorcode, PCRE2_SIZE erroroffset) {
    char buf[256] = {};
    pcre2_get_error_message(errorcode, reinterpret_cast<PCRE2_UCHAR8*>(buf), sizeof buf);
    std::wstring msg = L"无效的正则表达式（位置 " + std::to_wstring(static_cast<unsigned long long>(erroroffset)) + L"）：";
    for (const char c : buf) {
        if (!c)
            break;
        msg.push_back(static_cast<unsigned char>(c));  // PCRE2 错误消息为 ASCII
    }
    return msg;
}

}  // namespace

// ==================== 全部匹配 ====================

SearchResult searchFindAll(const std::string& content, const std::string& pattern,
                           const SearchOptions& options) {
    SearchResult result;
    const CompiledPattern cp = compilePattern(pattern, options);
    if (pattern.empty())
        return result;  // 空查找串：恒无匹配，不是错误
    if (cp.invalid) {
        // 无效正则：不崩溃，调用方据 invalidPattern + errorMessage 显示错误
        result.invalidPattern = true;
        result.errorMessage = formatPatternError(cp.errorcode, cp.erroroffset);
        return result;
    }

    // 手动推进循环（推进策略自控且可测，不用 pcre2_substitute/globalMatch）：
    // - 非零长度命中 [start, end)：计入，从 end 继续（非重叠推进）
    // - 零长度命中：全部跳过（DEC-005：不计入不返回），推进 1 个字符（按
    //   UTF-8 序列长度步进，不落在多字节序列中间）防死循环
    // - 收集到第 10001 个非零长度命中即证明真实匹配数超上限（DEC-004），
    //   丢弃它并置溢出标志后停止；恰好 10000 个不误报
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(cp.code, nullptr);
    const PCRE2_SIZE n = static_cast<PCRE2_SIZE>(content.size());
    PCRE2_SIZE pos = 0;
    while (result.matches.size() <= static_cast<std::size_t>(kMaxMatches)) {
        const int rc = pcre2_match(cp.code, reinterpret_cast<PCRE2_SPTR>(content.data()), n,
                                   pos, 0, md, nullptr);
        if (rc < 0)
            break;  // PCRE2_ERROR_NOMATCH；其余负值为防御性终止（UTF 校验错误等），不崩溃
        const PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
        const PCRE2_SIZE start = ov[0];
        const PCRE2_SIZE end = ov[1];
        if (end > start) {
            result.matches.push_back(
                SearchMatch{static_cast<int>(start), static_cast<int>(end - start)});
            pos = end;
            if (static_cast<int>(result.matches.size()) > kMaxMatches)
                break;  // 第 10001 个只用于判定溢出，丢弃
        } else {
            const unsigned char lead =
                start < n ? static_cast<unsigned char>(content[start]) : 0;
            pos = start + static_cast<PCRE2_SIZE>(utf8LeadLength(lead));
        }
        if (pos > n)
            break;
    }
    if (static_cast<int>(result.matches.size()) > kMaxMatches) {
        result.matches.pop_back();
        result.overflowed = true;
    }
    pcre2_match_data_free(md);
    pcre2_code_free(cp.code);
    return result;
}

// ==================== 匹配数据重建（捕获组来源） ====================

const void* searchRebuildMatchData(const std::string& content, const std::string& pattern,
                                   const SearchOptions& options, const SearchMatch& match) {
    if (!options.regex || pattern.empty())
        return nullptr;  // 纯文本模式无捕获组（$0 = match 文本，expand 内处理）
    if (match.start < 0 || match.length <= 0)
        return nullptr;
    if (static_cast<std::size_t>(match.start) + static_cast<std::size_t>(match.length) >
        content.size())
        return nullptr;

    const CompiledPattern cp = compilePattern(pattern, options);
    if (cp.invalid) {
        if (cp.code)
            pcre2_code_free(cp.code);
        return nullptr;
    }
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(cp.code, nullptr);
    // 锚定在 match.start 重匹配：与 searchFindAll 从同一位置起的左most-first
    // 命中一致（同一引擎、同一位置），ovector 即该次命中的捕获组。
    const int rc = pcre2_match(cp.code, reinterpret_cast<PCRE2_SPTR>(content.data()),
                               static_cast<PCRE2_SIZE>(content.size()),
                               static_cast<PCRE2_SIZE>(match.start), PCRE2_ANCHORED, md, nullptr);
    pcre2_code_free(cp.code);
    if (rc < 0) {
        pcre2_match_data_free(md);
        return nullptr;
    }
    const PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
    if (ov[0] != static_cast<PCRE2_SIZE>(match.start) ||
        ov[1] - ov[0] != static_cast<PCRE2_SIZE>(match.length)) {
        pcre2_match_data_free(md);  // 内容/模式已变化导致失配：不返回过期数据
        return nullptr;
    }
    return md;
}

void searchFreeMatchData(const void* matchData) {
    if (matchData)
        pcre2_match_data_free(const_cast<pcre2_match_data*>(
            static_cast<const pcre2_match_data*>(matchData)));
}

// ==================== 替换串展开（DEC-006） ====================

std::string searchExpandReplacement(const std::string& replacement, const std::string& content,
                                    const SearchMatch& match, const void* matchData) {
    const pcre2_match_data* md =
        static_cast<const pcre2_match_data*>(matchData);
    const PCRE2_SIZE* ov = md ? pcre2_get_ovector_pointer(const_cast<pcre2_match_data*>(md))
                              : nullptr;
    const uint32_t groupCount = md ? pcre2_get_ovector_count(const_cast<pcre2_match_data*>(md))
                                   : 1;

    // 捕获组文本（string_view 借用 content，调用期间 content 不可变）：
    // - matchData 为空（纯文本模式）：组 0 = match 区间文本，其余组不存在
    // - 组未参与本次匹配（PCRE2_UNSET）→ 空串（与旧版「未参与匹配的组为
    //   空串」一致）
    auto groupText = [&](int num) -> std::string_view {
        if (num == 0 && !md) {
            const std::string_view sv(content);
            if (match.start < 0 || static_cast<std::size_t>(match.start) > sv.size())
                return {};
            return sv.substr(static_cast<std::size_t>(match.start),
                             static_cast<std::size_t>(match.length));
        }
        if (!md || num < 0 || num >= static_cast<int>(groupCount))
            return {};
        const PCRE2_SIZE s = ov[2 * static_cast<std::size_t>(num)];
        const PCRE2_SIZE e = ov[2 * static_cast<std::size_t>(num) + 1];
        if (s == PCRE2_UNSET || e < s || e > content.size())
            return {};
        return std::string_view(content).substr(static_cast<std::size_t>(s),
                                                static_cast<std::size_t>(e - s));
    };

    std::string out;
    out.reserve(replacement.size());  // 展开结果 >= 替换串长度，按下界预留
    const std::size_t n = replacement.size();
    std::size_t i = 0;
    while (i < n) {
        const char ch = replacement[i];
        if (ch == '$') {
            if (i + 1 < n && replacement[i + 1] == '$') {
                out.push_back('$');  // $$ → 字面 $
                i += 2;
                continue;
            }
            if (i + 1 < n && isAsciiDigit(replacement[i + 1])) {
                // $ 后贪婪取最多两位 ASCII 数字：$10 是第 10 组（DEC-006）；
                // 全角数字不是组引用（"$１" 输出字面 "$１"）
                int num = 0;
                int digits = 0;
                while (digits < 2 && i + 1 + static_cast<std::size_t>(digits) < n &&
                       isAsciiDigit(replacement[i + 1 + static_cast<std::size_t>(digits)])) {
                    num = num * 10 + (replacement[i + 1 + static_cast<std::size_t>(digits)] - '0');
                    ++digits;
                }
                const std::string_view t = groupText(num);  // 不存在的组 → 空串
                // 空 view 的 data()==nullptr：append(nullptr,0) 按标准是技术性
                // UB（[s,s+n) 须有效范围），显式跳过
                if (!t.empty())
                    out.append(t.data(), t.size());
                i += 1 + static_cast<std::size_t>(digits);
                continue;
            }
            // $ 后非数字（或 $ 位于串尾）：输出字面 $，后续字符照常处理
            out.push_back('$');
            ++i;
            continue;
        }
        if (ch == '\\') {
            if (i + 1 < n && isAsciiDigit(replacement[i + 1])) {
                // \0..\9：仅单个 ASCII 数字（与 $n 等价；\12 = 第 1 组 + 字面 "2"）
                const std::string_view t = groupText(replacement[i + 1] - '0');
                if (!t.empty())  // 同 $n：空 view 不进 append（技术性 UB 防御）
                    out.append(t.data(), t.size());
                i += 2;
                continue;
            }
            // 反斜杠后非数字：按字面输出（含 \n、\t 均不做转义解释——DEC-006）
            out.push_back('\\');
            ++i;
            continue;
        }
        out.push_back(ch);  // 多字节 UTF-8 字节透传（语法字符仅 ASCII）
        ++i;
    }
    return out;
}

// ==================== 全部替换 ====================

ReplaceResult searchReplaceAll(const std::string& content, const std::string& pattern,
                               const std::string& replacement, const SearchOptions& options) {
    ReplaceResult rr;
    rr.newContent = content;  // 无效模式 / 空查找串 / 无匹配时原样返回
    if (pattern.empty())
        return rr;

    const CompiledPattern cp = compilePattern(pattern, options);
    if (cp.invalid)
        return rr;  // 无效正则：内容不变（调用方经 searchFindAll 获知错误）

    // 编译一次，匹配与展开共用同一 match_data：命中即展开（ovector 正是
    // 当前命中的捕获组），升序缝合结果与「编辑按 start 降序从后往前应用」
    // 产物一致（matches 升序且互不重叠）。
    pcre2_match_data* md = pcre2_match_data_create_from_pattern(cp.code, nullptr);
    const PCRE2_SIZE n = static_cast<PCRE2_SIZE>(content.size());
    PCRE2_SIZE pos = 0;
    std::size_t prev = 0;
    std::string out;
    out.reserve(content.size());
    int replaced = 0;
    while (true) {
        const int rc = pcre2_match(cp.code, reinterpret_cast<PCRE2_SPTR>(content.data()), n,
                                   pos, 0, md, nullptr);
        if (rc < 0)
            break;
        const PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
        const PCRE2_SIZE start = ov[0];
        const PCRE2_SIZE end = ov[1];
        if (end > start) {
            if (replaced >= kMaxMatches)
                break;  // 溢出：只替换前 10000 处（DEC-004，与 findAll 截断一致）
            const SearchMatch m{static_cast<int>(start), static_cast<int>(end - start)};
            out.append(content, prev, static_cast<std::size_t>(start) - prev);
            out += searchExpandReplacement(replacement, content, m, md);
            prev = static_cast<std::size_t>(end);
            ++replaced;
            pos = end;
        } else {
            const unsigned char lead =
                start < n ? static_cast<unsigned char>(content[start]) : 0;
            pos = start + static_cast<PCRE2_SIZE>(utf8LeadLength(lead));  // 零长度跳过
        }
        if (pos > n)
            break;
    }
    out.append(content, prev, content.size() - prev);

    pcre2_match_data_free(md);
    pcre2_code_free(cp.code);
    rr.newContent = std::move(out);
    rr.replacedCount = replaced;
    return rr;
}

}  // namespace core
