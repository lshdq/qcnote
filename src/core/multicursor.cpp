// qcnote 多光标纯逻辑实现（M3-A）。移植自 Qt 版 MultiCursorEngine：
// 词边界（QChar 类别语义 → Win32 GetStringTypeW 判定）、行表（UTF-16 偏移
// → UTF-8 字节偏移 + 字符列）、Ctrl+D 严格重叠判定（端点相接允许追加，
// DEC-010）、Shift+Alt+I 行尾集合、Esc 收缩、粘贴分发判定。
#include "core/multicursor.h"

#include "core/textutil.h"

#include <windows.h>

#include <algorithm>
#include <string_view>
#include <utility>

namespace core {
namespace {

// ==================== UTF-8 步进（内容契约：合法 UTF-8；防御无效字节按 1） ====================

int utf8LeadLength(unsigned char b) {
    if (b < 0x80) return 1;
    if (b >= 0xC2 && b <= 0xDF) return 2;
    if (b >= 0xE0 && b <= 0xEF) return 3;
    if (b >= 0xF0 && b <= 0xF4) return 4;
    return 1;
}

bool isUtf8Continuation(unsigned char b) {
    return (b & 0xC0) == 0x80;
}

// 从 i 起解码一个码点（供词字符分类；无效序列返回 0xFFFD 对应「分隔符」）
uint32_t decodeCodePoint(const std::string& s, std::size_t i) {
    const unsigned char b0 = static_cast<unsigned char>(s[i]);
    const int len = utf8LeadLength(b0);
    if (i + static_cast<std::size_t>(len) > s.size())
        return 0xFFFD;
    auto cont = [&](std::size_t k) {
        return isUtf8Continuation(static_cast<unsigned char>(s[k]));
    };
    uint32_t cp = 0;
    switch (len) {
        case 1: return b0;
        case 2:
            if (!cont(i + 1)) return 0xFFFD;
            cp = (b0 & 0x1Fu) << 6 | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            return cp >= 0x80 ? cp : 0xFFFD;
        case 3:
            if (!cont(i + 1) || !cont(i + 2)) return 0xFFFD;
            cp = (uint32_t(b0 & 0x0Fu) << 12) | (uint32_t(static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6)
               | (uint32_t(static_cast<unsigned char>(s[i + 2]) & 0x3Fu));
            return (cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF)) ? cp : 0xFFFD;
        case 4:
            if (!cont(i + 1) || !cont(i + 2) || !cont(i + 3)) return 0xFFFD;
            cp = (uint32_t(b0 & 0x07u) << 18) | (uint32_t(static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12)
               | (uint32_t(static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6)
               | (uint32_t(static_cast<unsigned char>(s[i + 3]) & 0x3Fu));
            return (cp >= 0x10000 && cp <= 0x10FFFF) ? cp : 0xFFFD;
        default:
            return 0xFFFD;
    }
}

// ==================== 词字符判定（旧版 QChar 类别语义的 Win32 移植） ====================
//
// 旧版 isWordChar = isLetterOrNumber || '_' || (isPrint && !isSpace && !isPunct)
// 按 Unicode 类别等价于：L*（字母，含 CJK）/ N*（数字）/ M*（记号）/
// S*（符号，如 $、￥、±）/ '_' 为词字符；P*（标点，含中文标点 ，。、）、
// Z*（空白，含换行）、C*（控制/格式/代理/未赋值）为分隔符。
//
// Win32 GetStringTypeW(CT_CTYPE1) 的 C1_PUNCT 混装 P* 与 S*，用
// CT_CTYPE3 的 C3_SYMBOL 修正符号类。增补平面字符（> 0xFFFF）旧版按
// UTF-16 代理半区判定为非可打印（Cs）→ 分隔符，此处保持一致。
bool isAsciiWordCodePoint(uint32_t cp) {
    // ASCII 精确表（Unicode 类别：L/N/M/S = 词；P/Z/C = 分隔）：
    // 字母数字下划线 + 符号 $ + < = > ^ ` | ~（Sc/Sm/Sk）
    if (cp >= '0' && cp <= '9') return true;
    if (cp >= 'A' && cp <= 'Z') return true;
    if (cp >= 'a' && cp <= 'z') return true;
    switch (cp) {
        case '_': case '$': case '+': case '<': case '=': case '>':
        case '^': case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

bool classifyWordChar(const wchar_t* wch) {
    WORD t1 = 0;
    WORD t3 = 0;
    if (!GetStringTypeW(CT_CTYPE1, wch, 1, &t1))
        return false;
    if (!GetStringTypeW(CT_CTYPE3, wch, 1, &t3))
        t3 = 0;
    if (t1 & (C1_ALPHA | C1_DIGIT | C1_UPPER | C1_LOWER))
        return true;                                   // 字母/数字（含 CJK 表意文字）
    if (t1 & C1_PUNCT)
        return (t3 & C3_SYMBOL) != 0;                  // 符号（S*）→ 词；标点（P*）→ 分隔
    if (t3 & (C3_NONSPACING | C3_DIACRITIC | C3_KASHIDA))
        return true;                                   // 组合记号（M*）
    if (t1 & (C1_SPACE | C1_CNTRL | C1_BLANK))
        return false;                                  // 空白（Z*）/控制（Cc）
    return (t1 & C1_DEFINED) != 0;                     // 其余已定义字符按可打印归词
}

// GetStringTypeW 与 Unicode 通用类别的已知偏差修正（旧版语义对齐）：
// - U+00B7（· 间隔号，Po）：Windows 标记 C3_SYMBOL 会被误判为词字符；中文
//   排版（外国人名间隔）按旧版应为分隔符
// - U+00BC..U+00BE（¼ ½ ¾ 简分数，No）：旧版为数字（词字符）；Windows 仅
//   标记 C1_PUNCT 会被误判为分隔符
// 返回 true 表示有覆盖（isWord 写入结果）；false 走通用分类。
bool curatedWordOverride(uint32_t cp, bool& isWord) {
    if (cp == 0x00B7) {
        isWord = false;  // Po：分隔
        return true;
    }
    if (cp >= 0x00BC && cp <= 0x00BE) {
        isWord = true;  // No：词
        return true;
    }
    return false;
}

// content[i] 起的字符是否词字符（i 为序列首字节）
bool isWordCharAt(const std::string& content, std::size_t i) {
    const uint32_t cp = decodeCodePoint(content, i);
    if (cp < 0x80)
        return isAsciiWordCodePoint(cp);
    if (cp > 0xFFFF)
        return false;  // 增补平面：与旧版按代理半区判定的结果一致（分隔符）
    bool curated = false;
    if (curatedWordOverride(cp, curated))
        return curated;
    const wchar_t wch = static_cast<wchar_t>(cp);
    return classifyWordChar(&wch);
}

// ==================== 行表（字节偏移；行 = '\n' 分隔，行尾不含 '\n'） ====================

struct LineTable {
    std::vector<std::pair<int, int>> lines;  // 每行 [start, end)，end 不含 '\n'
    int length = 0;

    explicit LineTable(const std::string& text)
        : length(static_cast<int>(text.size())) {
        int start = 0;
        for (int i = 0; i < length; ++i) {
            if (text[static_cast<std::size_t>(i)] == '\n') {
                lines.emplace_back(start, i);
                start = i + 1;
            }
        }
        lines.emplace_back(start, length);
    }

    int lineCount() const { return static_cast<int>(lines.size()); }
    int lineStart(int line) const { return lines[static_cast<std::size_t>(line)].first; }
    int lineEnd(int line) const { return lines[static_cast<std::size_t>(line)].second; }

    std::string_view lineView(const std::string& text, int line) const {
        return std::string_view(text).substr(static_cast<std::size_t>(lineStart(line)),
                                             static_cast<std::size_t>(lineEnd(line) - lineStart(line)));
    }

    // 位置 → 行号：最大的 line 使 lineStart(line) <= pos（pos 钳制到 [0, length]）
    int lineForPosition(int pos) const {
        if (pos <= 0)
            return 0;
        if (pos >= length)
            return lineCount() - 1;
        int lo = 0;
        int hi = lineCount() - 1;
        while (lo < hi) {  // 二分：找最后一个 <= pos 的行起点
            const int mid = lo + (hi - lo + 1) / 2;
            if (lineStart(mid) <= pos)
                lo = mid;
            else
                hi = mid - 1;
        }
        return lo;
    }
};

int clampInt(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// 行内字节偏移 → 字符列（码点列）；复用 textutil 换算（自动钳制）
int charColumnInLine(const std::string& content, const LineTable& lt, int line, int bytePos) {
    const int rel = clampInt(bytePos - lt.lineStart(line), 0, lt.lineEnd(line) - lt.lineStart(line));
    return static_cast<int>(
        utf8ByteOffsetToCharIndex(lt.lineView(content, line), static_cast<std::size_t>(rel)));
}

// 行内字符列 → 字节偏移（钳制行末）；utf8CharIndexToByteOffset 越界钳制到行末
int byteForColumn(const std::string& content, const LineTable& lt, int line, int charCol) {
    return lt.lineStart(line) + static_cast<int>(
        utf8CharIndexToByteOffset(lt.lineView(content, line), static_cast<std::size_t>(charCol)));
}

// 按（caret 升序，anchor 升序）排序并去重 (anchor, caret) 完全相同者
void sortAndDedup(std::vector<SelRange>& list) {
    std::sort(list.begin(), list.end(), [](const SelRange& a, const SelRange& b) {
        if (a.caret != b.caret)
            return a.caret < b.caret;
        return a.anchor < b.anchor;
    });
    list.erase(std::unique(list.begin(), list.end(), [](const SelRange& a, const SelRange& b) {
                   return a.caret == b.caret && a.anchor == b.anchor;
               }),
               list.end());
}

// 按（caret, anchor）最大的元素（= 排序后最末）
const SelRange& lastByPosition(const std::vector<SelRange>& selections) {
    return *std::max_element(selections.begin(), selections.end(),
                             [](const SelRange& a, const SelRange& b) {
                                 if (a.caret != b.caret)
                                     return a.caret < b.caret;
                                 return a.anchor < b.anchor;
                             });
}

}  // namespace

// ==================== Ctrl+Alt+Up/Down：上下行同字符列加光标 ====================

std::vector<SelRange> multicursorAddLine(const std::string& content,
                                         const std::vector<SelRange>& current, bool up) {
    const LineTable lt(content);
    std::vector<SelRange> result = current;
    for (const SelRange& c : current) {
        const int pos = clampInt(c.caret, 0, lt.length);
        const int line = lt.lineForPosition(pos);
        const int targetLine = line + (up ? -1 : 1);
        if (targetLine < 0 || targetLine >= lt.lineCount())
            continue;  // 已在首/末行：跳过（不产生新增）
        // 保持字符列（码点列）：按旧行内 [lineStart, caret) 的字符数在目标行取位
        const int charCol = charColumnInLine(content, lt, line, pos);
        int newPos = byteForColumn(content, lt, targetLine, charCol);
        newPos = (std::min)(newPos, lt.lineEnd(targetLine));  // 短行钳制行末，不越界到下一行
        result.push_back(SelRange{newPos, newPos});
    }
    sortAndDedup(result);
    return result;
}

// ==================== Shift+Alt+I：选中行行尾插光标 ====================

std::vector<SelRange> multicursorLineEnds(const std::string& content, int anchor, int caret) {
    const LineTable lt(content);
    const int lo = clampInt((std::min)(anchor, caret), 0, lt.length);
    const int hi = clampInt((std::max)(anchor, caret), 0, lt.length);
    const int l1 = lt.lineForPosition(lo);
    const int l2 = lt.lineForPosition(hi);
    std::vector<SelRange> result;
    result.reserve(static_cast<std::size_t>(l2 - l1 + 1));
    for (int line = l1; line <= l2; ++line) {  // 含最后一行（hi == length 时即文档末行）
        const int e = lt.lineEnd(line);        // 行尾 = 换行符之前
        result.push_back(SelRange{e, e});
    }
    return result;  // 行末偏移随行号严格递增 → 天然有序且互异
}

// ==================== Ctrl+D：下一个相同词 ====================

std::optional<SelRange> multicursorNextOccurrence(const std::string& content,
                                                  const std::vector<SelRange>& selections) {
    if (selections.empty())
        return std::nullopt;

    // 目标：按位置最末的非空选区；全空 → 最末光标处的单词——首次 Ctrl+D
    // 直接返回该词本身（旧版语义：无选区先选中当前词，词即第一个选区）
    std::optional<SelRange> target;
    for (auto it = selections.rbegin(); it != selections.rend(); ++it) {
        if (it->anchor != it->caret) {
            target = *it;
            break;
        }
    }
    if (!target)
        return wordRangeAt(content, lastByPosition(selections).caret);

    const int selMin = (std::min)(target->anchor, target->caret);
    const int selMax = (std::max)(target->anchor, target->caret);
    const int len = selMax - selMin;
    const std::string_view sv(content);
    const std::string_view targetText = sv.substr(static_cast<std::size_t>(selMin),
                                                  static_cast<std::size_t>(len));
    if (targetText.empty() || targetText.find('\n') != std::string_view::npos)
        return std::nullopt;  // 目标为空或跨行（含换行）不匹配

    // 与任一现有选区**严格重叠**（共享至少一个字符）即跳过；端点相接不算
    // 重叠、允许追加（DEC-010：如 "foofoo" 选 [0,3) 后可追加 [3,6)——相邻
    // 编辑不共享字符，同步替换不会互相吞掉）。空光标无区间，不参与。
    //
    // 判定结构（防平方复杂度）：非空选区区间 [s,e) 按 s 排序 + 前缀 max e。
    // 出现 [q, q+len) 与某选区重叠 ⇔ {s < q+len} 前缀中存在 e > q
    // ⇔ 该前缀 max e > q。命中重叠时直接跳到该 max e——(q, maxE) 内的后续
    // 出现均与同一选区重叠（跳过安全，结果不变），避免逐字符推进造成的
    // 出现数×选区数平方扫描（Ctrl+Shift+L 全部选中后继续 Ctrl+D 的极端场景）。
    std::vector<std::pair<int, int>> selIv;  // 非空选区 [s,e)，按 s 升序
    selIv.reserve(selections.size());
    for (const SelRange& c : selections) {
        if (c.anchor != c.caret)
            selIv.emplace_back((std::min)(c.anchor, c.caret), (std::max)(c.anchor, c.caret));
    }
    std::sort(selIv.begin(), selIv.end());
    std::vector<int> selPrefixMaxEnd(selIv.size() + 1, -1);
    for (std::size_t i = 0; i < selIv.size(); ++i)
        selPrefixMaxEnd[i + 1] = (std::max)(selPrefixMaxEnd[i], selIv[i].second);

    // 出现起点 q 与现有选区重叠时返回可安全跳到的位置（重叠前缀的最大右
    // 端，恒 > q）；不重叠返回 -1
    auto overlapSkip = [&](int q) -> int {
        const auto it = std::lower_bound(
            selIv.begin(), selIv.end(), q + len,
            [](const std::pair<int, int>& p, int value) { return p.first < value; });
        const int maxEnd = selPrefixMaxEnd[static_cast<std::size_t>(it - selIv.begin())];
        return maxEnd > q ? maxEnd : -1;
    };

    // 在 [from, limit) 起始范围内找第一个不与现有选区重叠的出现（须整体落在 limit 内）
    auto findHit = [&](int from, int limit) -> int {
        int idx = from;
        while (true) {
            const std::size_t hit = sv.find(targetText, static_cast<std::size_t>(idx));
            if (hit == std::string_view::npos || static_cast<int>(hit) > limit - len)
                return -1;
            const int skip = overlapSkip(static_cast<int>(hit));
            if (skip < 0)
                return static_cast<int>(hit);
            idx = skip;  // 与现有选区重叠：跳到重叠选区最大右端继续
        }
    };

    int hit = findHit(selMax, static_cast<int>(content.size()));  // 后方
    if (hit < 0)
        hit = findHit(0, selMin);                                 // 环绕到前方
    if (hit < 0)
        return std::nullopt;  // 全部出现均与现有选区重叠：无变化
    return SelRange{hit, hit + len};
}

// ==================== Ctrl+Shift+L：全部相同实例 ====================

std::vector<SelRange> multicursorAllOccurrences(const std::string& content,
                                                const std::vector<SelRange>& selections) {
    if (selections.empty())
        return {};

    // 目标：最末选区文本；空选区则最末光标处单词
    const SelRange& last = lastByPosition(selections);
    const int s = (std::min)(last.anchor, last.caret);
    const int e = (std::max)(last.anchor, last.caret);
    const std::string_view sv(content);
    std::string_view targetText;
    if (s != e) {
        targetText = sv.substr(static_cast<std::size_t>(s), static_cast<std::size_t>(e - s));
    } else if (const auto word = wordRangeAt(content, last.caret)) {
        targetText = sv.substr(static_cast<std::size_t>(word->anchor),
                               static_cast<std::size_t>(word->caret - word->anchor));
    }
    if (targetText.empty())
        return selections;  // 无目标：原样返回（旧版语义：无变化）

    // 从左到右收集全部非重叠出现（大小写敏感精确字节匹配；目标含换行时
    // 跨行匹配——旧版 Ctrl+Shift+L 不排除跨行目标）
    std::vector<SelRange> result;
    const int len = static_cast<int>(targetText.size());
    std::size_t idx = 0;
    while (true) {
        idx = sv.find(targetText, idx);
        if (idx == std::string_view::npos)
            break;
        result.push_back(SelRange{static_cast<int>(idx), static_cast<int>(idx) + len});
        idx += static_cast<std::size_t>(len);  // 非重叠推进
    }
    return result;
}

// ==================== Esc 收缩 / 粘贴分发判定 ====================

SelRange multicursorCollapse(const std::vector<SelRange>& selections) {
    if (selections.empty())
        return SelRange{0, 0};
    return lastByPosition(selections);
}

bool multicursorPasteDistributes(int clipboardLines, int caretCount) {
    return clipboardLines > 0 && clipboardLines == caretCount;
}

// ==================== 光标处单词 ====================

std::optional<SelRange> wordRangeAt(const std::string& content, int position) {
    const std::size_t n = content.size();
    if (position < 0)
        return std::nullopt;
    std::size_t i = static_cast<std::size_t>(position);
    if (i > n)
        i = n;  // 越界钳制到文末
    if (i == n)
        return std::nullopt;  // 文末无词

    // 防御：落在多字节序列中间时回退到该字符的序列首字节
    while (i > 0 && isUtf8Continuation(static_cast<unsigned char>(content[i])))
        --i;

    if (!isWordCharAt(content, i))
        return std::nullopt;  // 空白/标点/控制符上无词

    // 向左扩展到词首（按整字符步进）
    std::size_t start = i;
    while (start > 0) {
        std::size_t prev = start - 1;
        while (prev > 0 && isUtf8Continuation(static_cast<unsigned char>(content[prev])))
            --prev;  // 回退到前一个字符的序列首
        if (!isWordCharAt(content, prev))
            break;
        start = prev;
    }
    // 向右扩展到词尾
    std::size_t end = i + static_cast<std::size_t>(utf8LeadLength(static_cast<unsigned char>(content[i])));
    while (end < n && isWordCharAt(content, end))
        end += static_cast<std::size_t>(utf8LeadLength(static_cast<unsigned char>(content[end])));

    return SelRange{static_cast<int>(start), static_cast<int>(end)};
}

}  // namespace core
