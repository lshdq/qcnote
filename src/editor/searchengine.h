#ifndef SEARCHENGINE_H
#define SEARCHENGINE_H

#include <QString>
#include <QStringList>
#include <QList>
#include <QRegularExpression>

namespace qcnote {

// 查找选项：默认值与 DEC-003 对齐（区分大小写默认关）。
// 引擎不持有“默认值”概念——QML 层决定选项初值，引擎只按传入选项行为。
struct SearchOptions {
    bool caseSensitive = false; // 区分大小写
    bool wholeWord = false;     // 全字匹配（仅正则关时生效；正则开时被忽略）
    bool useRegex = false;      // 正则模式（关 = 查找串按字面文本解释）
};

// 单个匹配：区间 [start, end) 按原文 Unicode 码位（UTF-16 单元）计数；
// captured 为该次命中的捕获组文本，captured[0] 是整体匹配，i >= 1 为第 i 组
//（未参与匹配的组为空串，与 PCRE 语义一致），供替换串 $n/\n 引用展开。
struct SearchMatch {
    int start = 0;
    int end = 0;
    QStringList captured;
};

// findAll 结果：matches 按 start 升序、互不重叠；
// overflowed 为真表示真实匹配数超过 10000，计算在上限处截断（DEC-004）
struct SearchResult {
    QList<SearchMatch> matches;
    bool overflowed = false;
};

// 单条替换编辑：把原文 [start, end) 替换为 text（替换串已按捕获组展开）
struct SearchEdit {
    int start = 0;
    int end = 0;
    QString text;
};

/**
 * @brief 查找替换纯逻辑引擎（仅 QtCore，无 GUI 依赖，可控制台全量单测）。
 *
 * 与 MultiCursorEngine 同层：只做纯文本坐标计算，不触碰 QTextDocument；
 * 编辑批次由上层（SearchController / 测试）按“从后往前 + 单 editBlock”契约应用。
 */
class SearchEngine
{
public:
    // 匹配数上限：达到后停止计算并置溢出标志（DEC-004，性能防护）
    static constexpr int kMaxMatchCount = 10000;

    SearchEngine() = default;

    // 设置查找模式并编译（正则关 → 元字符字面化；正则开 → 原样交给 PCRE）。
    // 编译失败（无效正则）不抛异常不崩溃：仅记录 errorMessage，isValid()
    // 为 false，findAll 返回空。空查找串合法但恒无匹配。
    void setPattern(const QString &pattern, const SearchOptions &options);

    bool isValid() const;            // 模式是否可正常匹配（空查找串除外恒真）
    QString errorMessage() const;    // 无效正则的错误信息（可直接显示给用户）
    QString pattern() const;
    SearchOptions options() const;

    // 全部匹配：升序、非重叠；零长度命中全部跳过（DEC-005，不计入不返回）；
    // 空查找串或无效模式返回空；达到 10000 上限后停止并置溢出标志。
    // 手动推进循环：零长度命中推进 1 个字符，防死循环。
    SearchResult findAll(const QString &text) const;

    // ---- 环绕导航纯函数 ----
    // 下一个/上一个目标索引（模环绕）；无匹配返回 -1；currentIndex < 0 表示
    // 无当前匹配（next 从 0、prev 从最后一个开始）
    static int nextIndex(int matchCount, int currentIndex);
    static int prevIndex(int matchCount, int currentIndex);

    // 从 fromPos 起第一个匹配（start >= fromPos）的下标；无则环绕到 0；无匹配
    // 返回 -1。replaceCurrent 替换第 i 处后以“替换文本之后的坐标”调用它定位
    // 下一个匹配（新文本坐标）。
    static int nextMatchIndexFrom(const QList<SearchMatch> &matches, int fromPos);

    // 外部内容变化后当前索引的钳制：索引越界（匹配数变少）收到最后一个合法
    // 下标；无匹配返回 -1；原为 -1（无当前匹配）保持不变——外部编辑（如用户
    // 正在输入）不应凭空制造当前选区。
    static int clampedIndex(int currentIndex, int matchCount);

    // 替换串展开（DEC-006，自实现解析器，两种查找模式行为一致）：
    // - $1..$99 与 \1..\9 等价的捕获组引用（$ 后贪婪取最多两位数字：
    //   $10 是第 10 组，不是“第 1 组 + 字面 0”）
    // - 引用不存在的组输出空串
    // - $$ 输出字面 $；$ 后跟非数字输出字面 $ 与该字符
    // - 替换串不做 \n/\t 等转义解释，一律按字面字符输出
    static QString expandReplacement(const QStringList &captured, const QString &replacement);

    // 一次性计算全部替换编辑：按原文本坐标计算（替换产物不参与本轮匹配，
    // 如把 a 替换为 aa 不会追加新编辑），结果按 start 降序排列，供上层从后
    // 往前应用保证偏移稳定。
    static QList<SearchEdit> computeReplaceEdits(const QString &text,
                                                 const QList<SearchMatch> &matches,
                                                 const QString &replacement);

private:
    QString m_pattern;
    SearchOptions m_options;
    QString m_errorMessage;   // 非空仅当正则编译失败
    QRegularExpression m_regex; // 纯文本模式 = 字面化后的等价正则（复用同一匹配循环）
};

} // namespace qcnote

#endif // SEARCHENGINE_H
