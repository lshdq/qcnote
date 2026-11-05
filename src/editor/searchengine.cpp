#include "editor/searchengine.h"

#include <QChar>

#include <algorithm>

namespace qcnote {
namespace {

// PCRE \b 意义下的词字符（ASCII 字母/数字/下划线）。Qt 的 QRegularExpression
// 默认不开 UseUnicodePropertiesOption，\b 按 ASCII 词字符判定；全字模式是否
// 附加 \b 必须与此口径一致，否则“边界字符”与“断言语义”错位会错杀合法命中。
bool isAsciiWordChar(QChar ch)
{
    return (ch >= QLatin1Char('a') && ch <= QLatin1Char('z'))
        || (ch >= QLatin1Char('A') && ch <= QLatin1Char('Z'))
        || (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
        || ch == QLatin1Char('_');
}

// 显式 ASCII 数字判定：QChar::isDigit()/digitValue() 接受全部 Unicode Nd
// 类别（全角数字、阿拉伯-印度数字等），替换串的组引用语法只认 ASCII
// 0-9，"$１"（全角一）应输出字面 "$１" 而非当作组引用
bool isAsciiDigit(QChar ch)
{
    return ch >= QLatin1Char('0') && ch <= QLatin1Char('9');
}

} // namespace

// ==================== 模式编译 ====================

void SearchEngine::setPattern(const QString &pattern, const SearchOptions &options)
{
    m_pattern = pattern;
    m_options = options;
    m_errorMessage.clear();

    // 空查找串：合法但恒无匹配（调用方据此显示空计数，不算错误）
    if (pattern.isEmpty()) {
        m_regex = QRegularExpression();
        return;
    }

    QString expr;
    if (options.useRegex) {
        // 正则模式：查找串原样交给 PCRE。全字选项被有意忽略——正则下词边界
        // 语义（\b、Unicode 词类等）应由用户在模式中自行表达，叠加自动 \b
        // 会与用户模式中的量词/断言产生不可预期的组合
        expr = pattern;
    } else {
        // 纯文本模式：全部元字符字面化，查找串就是字面文本
        expr = QRegularExpression::escape(pattern);
        if (options.wholeWord) {
            // \b 只在词字符与非词字符交界成立。若查找串首/尾不是（ASCII）词
            // 字符（如 "+foo"、中文查找串），无脑包 \b 会要求交界另一侧为词
            // 字符，反而错杀串首/串尾的合法命中。参照主流编辑器（VS Code、
            // Notepad++）做法：仅在首/尾是词字符的那一侧加 \b。
            if (isAsciiWordChar(pattern.front()))
                expr.prepend(QStringLiteral("\\b"));
            if (isAsciiWordChar(pattern.back()))
                expr.append(QStringLiteral("\\b"));
        }
    }

    QRegularExpression::PatternOptions patternOptions;
    if (!options.caseSensitive)
        patternOptions |= QRegularExpression::CaseInsensitiveOption; // DEC-003：默认忽略大小写
    m_regex = QRegularExpression(expr, patternOptions);
    if (!m_regex.isValid()) {
        // 无效正则：记录错误信息（含出错位置，可直接显示）。保留编译失败的
        // regex 对象——默认构造的正则（空模式）反而是合法的，若用它覆盖，
        // isValid() 会误报为真；findAll 以 isValid() 为前置条件直接返回空，
        // 不会触碰无效对象——不抛异常不崩溃
        m_errorMessage = QStringLiteral("无效的正则表达式（位置 %1）：%2")
                .arg(m_regex.patternErrorOffset())
                .arg(m_regex.errorString());
    }
}

bool SearchEngine::isValid() const
{
    return m_regex.isValid();
}

QString SearchEngine::errorMessage() const
{
    return m_errorMessage;
}

QString SearchEngine::pattern() const
{
    return m_pattern;
}

SearchOptions SearchEngine::options() const
{
    return m_options;
}

// ==================== 全部匹配 ====================

SearchResult SearchEngine::findAll(const QString &text) const
{
    SearchResult result;
    if (m_pattern.isEmpty() || !isValid())
        return result;

    // 手动推进循环（不用 globalMatch 迭代器，推进策略自控且可测）：
    // - 非空命中 [s,e)：计入，从 e 继续（非重叠推进）
    // - 零长度命中：全部跳过（DEC-005：不计入不返回），推进 1 个字符防死循环
    // - 循环上界多留 1 个位置：收集到第 10001 个非空命中即证明真实匹配数
    //   超过上限，丢弃它并置溢出标志后停止（DEC-004），保证“恰好 10000 个
    //   匹配”不误报溢出
    const int n = text.length();
    qsizetype pos = 0;
    while (result.matches.size() < kMaxMatchCount + 1) {
        const QRegularExpressionMatch m = m_regex.match(text, pos);
        if (!m.hasMatch())
            break;
        const int start = int(m.capturedStart(0));
        const int end = int(m.capturedEnd(0));
        if (end > start) {
            SearchMatch match;
            match.start = start;
            match.end = end;
            const int groupCount = m.lastCapturedIndex();
            match.captured.reserve(groupCount + 1);
            for (int g = 0; g <= groupCount; ++g)
                match.captured.append(m.captured(g)); // captured[0] = 整体匹配
            result.matches.append(match);
            pos = end;
        } else {
            pos = start + 1; // 跳过零长度命中：推进 1 字符防死循环
        }
        if (pos > n)
            break;
    }
    if (result.matches.size() > kMaxMatchCount) {
        result.matches.removeLast(); // 第 10001 个只用于判定溢出，丢弃
        result.overflowed = true;
    }
    return result;
}

// ==================== 环绕导航与索引维护 ====================

int SearchEngine::nextIndex(int matchCount, int currentIndex)
{
    if (matchCount <= 0)
        return -1;
    if (currentIndex < 0)
        return 0;
    return (currentIndex + 1) % matchCount;
}

int SearchEngine::prevIndex(int matchCount, int currentIndex)
{
    if (matchCount <= 0)
        return -1;
    if (currentIndex < 0)
        return matchCount - 1;
    return (currentIndex - 1 + matchCount) % matchCount;
}

int SearchEngine::nextMatchIndexFrom(const QList<SearchMatch> &matches, int fromPos)
{
    if (matches.isEmpty())
        return -1;
    for (int i = 0; i < matches.size(); ++i) {
        if (matches.at(i).start >= fromPos)
            return i;
    }
    return 0; // fromPos 之后无匹配：环绕到第一个（模环绕导航语义一致）
}

int SearchEngine::clampedIndex(int currentIndex, int matchCount)
{
    if (matchCount <= 0)
        return -1;
    if (currentIndex < 0)
        return -1; // 原本无当前匹配：外部编辑不应凭空制造当前选区
    return qMin(currentIndex, matchCount - 1);
}

// ==================== 替换串展开（DEC-006） ====================

QString SearchEngine::expandReplacement(const QStringList &captured, const QString &replacement)
{
    QString out;
    out.reserve(replacement.size()); // 展开结果 >= 替换串长度，按下界预留
    const int n = replacement.size();
    int i = 0;
    while (i < n) {
        const QChar ch = replacement.at(i);
        if (ch == QLatin1Char('$')) {
            if (i + 1 < n && replacement.at(i + 1) == QLatin1Char('$')) {
                out += QLatin1Char('$'); // $$ → 字面 $
                i += 2;
                continue;
            }
            if (i + 1 < n && isAsciiDigit(replacement.at(i + 1))) {
                // $ 后贪婪取最多两位数字：$10 是第 10 组（DEC-006），
                // 而非“第 1 组 + 字面 0”；不足两位时取一位（仅 ASCII 数字）
                int num = 0;
                int digits = 0;
                while (digits < 2 && i + 1 + digits < n
                       && isAsciiDigit(replacement.at(i + 1 + digits))) {
                    num = num * 10 + (replacement.at(i + 1 + digits).unicode() - u'0');
                    ++digits;
                }
                // 引用不存在的组输出空串（$0 = 整体匹配，顺带支持）
                out += (num < captured.size()) ? captured.at(num) : QString();
                i += 1 + digits;
                continue;
            }
            // $ 后非数字（或 $ 位于串尾）：输出字面 $，后续字符照常处理，
            // 即 “$x” 输出字面 “$x”
            out += QLatin1Char('$');
            ++i;
            continue;
        }
        if (ch == QLatin1Char('\\')) {
            if (i + 1 < n && isAsciiDigit(replacement.at(i + 1))) {
                // \1..\9：仅单个 ASCII 数字（与 $n 等价；\12 = 第 1 组 + 字面 “2”）
                const int num = replacement.at(i + 1).unicode() - u'0';
                out += (num < captured.size()) ? captured.at(num) : QString();
                i += 2;
                continue;
            }
            // 反斜杠后非数字：按字面输出（含 \n、\t 均不做转义解释，
            // “\n” 是两个字符的字面文本——DEC-006）
            out += QLatin1Char('\\');
            ++i;
            continue;
        }
        out += ch;
        ++i;
    }
    return out;
}

// ==================== 全部替换编辑计算 ====================

QList<SearchEdit> SearchEngine::computeReplaceEdits(const QString &text,
                                                    const QList<SearchMatch> &matches,
                                                    const QString &replacement)
{
    // 按原文本坐标一次性计算：每处匹配独立展开替换串；替换产物不参与本轮
    //（如把 a 替换为 aa，新产生的 “aa” 不会再被扫描——这是显式设计，
    //“全部替换”语义为对当前已计算匹配集合的单轮改写）
    QList<SearchEdit> edits;
    edits.reserve(matches.size());
    const int n = text.length();
    for (const SearchMatch &m : matches) {
        const int start = qBound(0, m.start, n); // 防御性钳制：来源恒为合法，
        const int end = qBound(start, m.end, n); // 越界时截断保证不崩溃
        if (end <= start)
            continue; // 零长度区间不产生编辑（findAll 不会产出，双保险）
        SearchEdit e;
        e.start = start;
        e.end = end;
        e.text = expandReplacement(m.captured, replacement);
        edits.append(e);
    }
    // 降序排列：上层从后往前应用，前面的编辑偏移不受后面编辑影响
    std::sort(edits.begin(), edits.end(),
              [](const SearchEdit &a, const SearchEdit &b) { return a.start > b.start; });
    return edits;
}

} // namespace qcnote
