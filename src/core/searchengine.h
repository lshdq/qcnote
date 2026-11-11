// qcnote 纯逻辑核心层：查找替换引擎（M3-A）。
//
// 内容契约（DEC-010）：内容一律 UTF-8 std::string、LF 换行、无 BOM；
// 所有偏移为字节偏移（int，文档 < 2GB 假设）。无 UI / Scintilla 依赖，
// 可全量单测（M3-B 做对话框 / indicator / 交互映射接线）。
//
// 语义移植自 Qt 版 SearchEngine（QString/QRegularExpression → std::string/
// PCRE2，vendored 10.45，DEC-012）：
// - 三开关默认全关（DEC-003）
// - 零长度命中全部跳过（DEC-005，不计入不高亮不替换）
// - 匹配数 > 10000 截断并置溢出标志（DEC-004）
// - 替换串 $n/\n 语法自实现解析器（DEC-006）
// - 全字匹配仅在查找串首/尾为 ASCII 词字符的一侧附加 \b（\b 为 ASCII 词
//   字符口径，与 PCRE2 默认一致；正则模式忽略 wholeWord）
// - 不区分大小写 = PCRE2_CASELESS（ASCII 简单折叠，不启用 PCRE2_UCP），
//   与旧版 QRegularExpression 默认语义一致（中文无大小写不受影响）
#pragma once

#include <string>
#include <vector>

namespace core {

/// 查找选项（默认值对应 DEC-003：三个开关默认全部关闭）。
struct SearchOptions {
    bool matchCase = false;  // 区分大小写（关 = 忽略大小写，ASCII 简单折叠）
    bool wholeWord = false;  // 全字匹配（仅 regex == false 时生效）
    bool regex = false;      // 正则模式（PCRE 语义；关 = 查找串按字面文本解释）
};

/// 单个匹配：区间 [start, start + length)，UTF-8 字节偏移；升序、互不重叠。
struct SearchMatch {
    int start = 0;
    int length = 0;
};

/// 全部匹配结果。
struct SearchResult {
    std::vector<SearchMatch> matches;  // 按 start 升序；>10000 截断为 10000 个
    bool overflowed = false;           // 真实匹配数超过 10000（计数显示「>10000」）
    bool invalidPattern = false;       // 无效正则（matches 为空，不崩溃，调用方显示错误）
    std::wstring errorMessage;         // invalidPattern 时的中文错误消息（含出错位置）
};

/// 匹配数上限（DEC-004 性能防护）：收集到第 10001 个非零长度命中即证明溢出，
/// 丢弃它并停止（恰好 10000 个不误报）。
inline constexpr int kMaxMatches = 10000;

/// 全部匹配：纯文本模式按字面匹配（PCRE2 元字符全部转义）；正则模式按 PCRE2
/// 编译（PCRE2_UTF 恒开：`.` 不匹配 \n、^/$ 默认全文首尾、`(?m)` 内联生效，
/// 与旧版 QRegularExpression 默认选项一致）。零长度命中全部跳过。空 pattern
/// （合法，恒无匹配）与无效正则返回空 matches。
SearchResult searchFindAll(const std::string& content, const std::string& pattern,
                           const SearchOptions& options);

/// 跳转导航（环绕）：forward 时 currentIndex+1、否则 -1，模 matches.size()。
/// matches 为空返回 -1；currentIndex < 0（无当前匹配）时 forward 从 0、
/// backward 从最后一个开始。
/// 计数重载：调用方持有 matches 容器时免构造 SearchResult（避免整向量深拷贝）。
inline int searchNextIndex(int matchCount, int currentIndex, bool forward) {
    const int n = matchCount;
    if (n <= 0)
        return -1;
    if (currentIndex < 0)
        return forward ? 0 : n - 1;
    return forward ? (currentIndex + 1) % n : (currentIndex - 1 + n) % n;
}

inline int searchNextIndex(const SearchResult& result, int currentIndex, bool forward) {
    return searchNextIndex(static_cast<int>(result.matches.size()), currentIndex, forward);
}

/// 在 match 处重建正则匹配数据（供替换串展开引用捕获组）：按同一 pattern/
/// options 在 match.start 处锚定（PCRE2_ANCHORED）重匹配并校验与 match 区间
/// 一致。正则模式且成功时返回不透明指针（PCRE2 match_data，生命周期归调用
/// 方，用毕 searchFreeMatchData 释放）；纯文本模式 / 失配返回 nullptr。
const void* searchRebuildMatchData(const std::string& content, const std::string& pattern,
                                   const SearchOptions& options, const SearchMatch& match);

/// 释放 searchRebuildMatchData 返回的匹配数据（nullptr 安全）。
void searchFreeMatchData(const void* matchData);

/// 展开替换串（DEC-006，自实现解析器，两种查找模式行为一致）：
/// - `$1..$99` 与 `\1..\9`（含 `\0`）等价的捕获组引用；`$` 后贪婪取最多
///   两位数字（`$10` 是第 10 组，不是「第 1 组 + 字面 0」）
/// - 引用不存在的组输出空串；`$0` = 整体匹配
/// - `$$` 输出字面 `$`；`$` 后跟非数字输出字面 `$` 与该字符（`$x` → `$x`）
/// - `\` 后跟非数字按字面输出（`\n`/`\t` 不做转义解释，一律字面字符）
/// - 替换串中的多字节 UTF-8 字节序列原样透传（语法字符仅 ASCII）
///
/// matchData：searchRebuildMatchData 返回的 PCRE2 匹配数据（捕获组来源）；
/// 纯文本模式传 nullptr —— 此时 `$0` = match 区间文本，其余组引用为空串。
std::string searchExpandReplacement(const std::string& replacement, const std::string& content,
                                    const SearchMatch& match, const void* matchData);

/// 全部替换结果。
struct ReplaceResult {
    std::string newContent;   // 替换后全文（无效模式 / 空查找串 / 无匹配时原样返回）
    int replacedCount = 0;    // 实际替换处数（overflowed 时为 kMaxMatches）
};

/// 全部替换（单批）：按原文本坐标一次性计算，替换产物不再参与本轮匹配
///（如 "a"→"aa" 不会追加新替换）；每处匹配独立展开替换串。matches 升序且
/// 互不重叠，升序缝合与旧版「编辑按 start 降序从后往前应用」契约产物一致。
ReplaceResult searchReplaceAll(const std::string& content, const std::string& pattern,
                               const std::string& replacement, const SearchOptions& options);

}  // namespace core
