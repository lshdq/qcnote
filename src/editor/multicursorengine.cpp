#include "editor/multicursorengine.h"

#include <QChar>
#include <QHash>

#include <algorithm>
#include <utility>

namespace qcnote {
namespace {

// ==================== 字符类别与词边界 ====================

// 词字符判定：字母/数字/下划线；另将“可打印且非空白非标点”的字符
// （各类符号，如 $、￥ 等）也并入词字符，使中文按“连续非空白非标点”成词，
// 标点（含中文标点 ，。、）与空白（含换行）作为分隔符。
bool isWordChar(QChar ch)
{
    return ch.isLetterOrNumber()
        || ch == QLatin1Char('_')
        || (ch.isPrint() && !ch.isSpace() && !ch.isPunct());
}

// 词移动（wordMode）：跳过左侧连续分隔符，再跳过连续词字符，
// 行为接近 QTextCursor 的 StartOfWord/EndOfWord 词移动。
int wordLeft(const QString &text, int p)
{
    int q = p;
    while (q > 0 && !isWordChar(text.at(q - 1)))
        --q;
    while (q > 0 && isWordChar(text.at(q - 1)))
        --q;
    return q;
}

int wordRight(const QString &text, int p)
{
    const int n = text.length();
    int q = p;
    while (q < n && !isWordChar(text.at(q)))
        ++q;
    while (q < n && isWordChar(text.at(q)))
        ++q;
    return q;
}

// 光标处的词范围 [s, e)：光标在空白/标点/文末时返回空范围（s==e==p）
QPair<int, int> wordRangeAt(const QString &text, int p)
{
    const int n = text.length();
    if (p < 0 || p >= n)
        return qMakePair(p, p);
    if (!isWordChar(text.at(p)))
        return qMakePair(p, p);
    int s = p;
    while (s > 0 && isWordChar(text.at(s - 1)))
        --s;
    int e = p + 1;
    while (e < n && isWordChar(text.at(e)))
        ++e;
    return qMakePair(s, e);
}

// ==================== 行结构（仅 QtCore，自行计算行偏移表） ====================

struct LineTable
{
    QList<int> starts;  // 每行起始偏移，starts[0] == 0
    QList<int> ends;    // 每行末尾（不含换行符）：该行 '\n' 位置或文档末
    int length = 0;     // 文档长度

    explicit LineTable(const QString &text)
        : length(text.length())
    {
        starts.append(0);
        const int n = text.length();
        for (int i = 0; i < n; ++i) {
            if (text.at(i) == QLatin1Char('\n')) {
                ends.append(i);
                starts.append(i + 1);
            }
        }
        ends.append(n);
    }

    int lineCount() const { return starts.size(); }
    int lineStart(int line) const { return starts.at(line); }
    int lineEnd(int line) const { return ends.at(line); }

    // 位置 → 行号：最大的 line 使 starts[line] <= pos（pos 先钳制到 [0, length]）
    int lineForPosition(int pos) const
    {
        if (pos <= 0)
            return 0;
        if (pos >= length)
            return lineCount() - 1;
        int lo = 0;
        int hi = starts.size() - 1;
        while (lo < hi) {          // 二分：找最后一个 <= pos 的行起点
            const int mid = lo + (hi - lo + 1) / 2;
            if (starts.at(mid) <= pos)
                lo = mid;
            else
                hi = mid - 1;
        }
        return lo;
    }
};

// ==================== 光标列表整理 ====================

// 按（position 升序，anchor 升序）排序；(anchor, position) 完全相同者去重
void sortAndDedup(QList<McCursor> &list)
{
    std::sort(list.begin(), list.end(), [](const McCursor &a, const McCursor &b) {
        if (a.position != b.position)
            return a.position < b.position;
        return a.anchor < b.anchor;
    });
    QList<McCursor> unique;
    unique.reserve(list.size());
    for (const McCursor &c : std::as_const(list)) {
        if (unique.isEmpty()
            || unique.constLast().position != c.position
            || unique.constLast().anchor != c.anchor) {
            unique.append(c);
        }
    }
    list = unique;
}

// ==================== 编辑批次的核心：坐标映射 ====================

// 原坐标 p → 应用全部编辑后的新坐标（editsAsc：按 start 升序、互不重叠）。
// own >= 0 表示 p 所属光标的编辑在 editsAsc[own]：
//   落点 = （之前编辑的累计平移 out-p）+ (edit.start - p) + text.length()
//   即“该编辑替换后的末尾”，且不受其后编辑影响。
// own < 0 时按通用规则：
//   p 在编辑区间之后（p >= end，含 p 恰为插入点）→ 平移 delta；
//   p 落在编辑区间内（start <= p < end）→ 映射到该区间替换后末尾；
//   p 在编辑区间之前 → 不受影响（后续编辑更靠后，直接结束）。
int mapPosition(int p, const QList<McEdit> &editsAsc, int own)
{
    int out = p;
    for (int i = 0; i < editsAsc.size(); ++i) {
        const McEdit &e = editsAsc.at(i);
        if (i == own)
            return out + (e.start - p) + e.text.length();
        if (p >= e.end) {
            out += e.text.length() - (e.end - e.start);
        } else if (p >= e.start) {
            return out + (e.start - p) + e.text.length();
        } else {
            break;
        }
    }
    return out;
}

// ==================== 编辑批次装配 ====================

struct EditCandidate
{
    int cursorIndex = 0;  // 所属原光标下标
    McEdit edit;
};

// 把“每光标一条编辑”装配为合法批次：
// 1) 过滤无操作编辑（start==end 且 text 为空）；
// 2) 按（start 升序、end 降序）排序后过滤重叠（正常光标集合不会重叠，
//    此为 setCursors 传入重叠选区时的防御，重叠者后到先弃）；
// 3) 新光标经 mapPosition 映射到应用全部编辑后的坐标系，再排序去重；
// 4) edits 反转为 start 降序输出，供从后往前应用保持偏移有效。
McEditBatch assembleBatch(const QList<McCursor> &oldCursors, const QList<McEdit> &perCursorEdits)
{
    Q_ASSERT(oldCursors.size() == perCursorEdits.size());

    QList<EditCandidate> candidates;
    for (int i = 0; i < perCursorEdits.size(); ++i) {
        const McEdit &e = perCursorEdits.at(i);
        if (e.start == e.end && e.text.isEmpty())
            continue; // 无操作编辑
        EditCandidate c;
        c.cursorIndex = i;
        c.edit = e;
        candidates.append(c);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const EditCandidate &a, const EditCandidate &b) {
                  if (a.edit.start != b.edit.start)
                      return a.edit.start < b.edit.start;
                  return a.edit.end > b.edit.end; // 同起点：长区间优先
              });

    QList<McEdit> accepted;       // 升序、互不重叠
    QHash<int, int> ownOfCursor;  // 光标下标 -> accepted 下标（-1 表示被过滤/无编辑）
    int lastEnd = -1;
    for (const EditCandidate &c : std::as_const(candidates)) {
        if (!accepted.isEmpty() && c.edit.start < lastEnd)
            continue; // 与已接受编辑重叠：跳过
        ownOfCursor.insert(c.cursorIndex, accepted.size());
        accepted.append(c.edit);
        lastEnd = c.edit.end;
    }

    QList<McCursor> newCursors;
    newCursors.reserve(oldCursors.size());
    for (int i = 0; i < oldCursors.size(); ++i) {
        const int own = ownOfCursor.value(i, -1);
        McCursor nc;
        nc.anchor = mapPosition(oldCursors.at(i).anchor, accepted, own);
        nc.position = mapPosition(oldCursors.at(i).position, accepted, own);
        newCursors.append(nc);
    }
    sortAndDedup(newCursors);

    McEditBatch batch;
    for (int i = accepted.size() - 1; i >= 0; --i)
        batch.edits.append(accepted.at(i)); // start 降序
    batch.cursors = newCursors;
    return batch;
}

// ==================== 各操作的编辑计算 ====================

// 每光标把其选区 [min,max) 替换为 replacement（空选区即插入）
McEditBatch buildReplaceBatch(const QList<McCursor> &cursors, const QString &replacement)
{
    QList<McEdit> edits;
    edits.reserve(cursors.size());
    for (const McCursor &c : cursors) {
        const int s = qMin(c.anchor, c.position);
        const int e = qMax(c.anchor, c.position);
        edits.append(McEdit{s, e, replacement});
    }
    return assembleBatch(cursors, edits);
}

McEditBatch backspaceBatch(const QList<McCursor> &cursors, const QString &text)
{
    const int n = text.length();
    QList<McEdit> edits;
    edits.reserve(cursors.size());
    for (const McCursor &c : cursors) {
        const int s = qBound(0, qMin(c.anchor, c.position), n); // 越界防御：钳制
        const int e = qBound(0, qMax(c.anchor, c.position), n);
        if (s != e) {
            edits.append(McEdit{s, e, QString()});
            continue;
        }
        const int p = qBound(0, c.position, n);
        if (p == 0) {
            edits.append(McEdit{0, 0, QString()}); // 文档首：无操作（空编辑会被过滤）
            continue;
        }
        edits.append(McEdit{p - 1, p, QString()}); // 删前一字符；行首即删 '\n' 并入上一行
    }
    return assembleBatch(cursors, edits);
}

McEditBatch deleteCharBatch(const QList<McCursor> &cursors, const QString &text)
{
    const int n = text.length();
    QList<McEdit> edits;
    edits.reserve(cursors.size());
    for (const McCursor &c : cursors) {
        const int s = qBound(0, qMin(c.anchor, c.position), n);
        const int e = qBound(0, qMax(c.anchor, c.position), n);
        if (s != e) {
            edits.append(McEdit{s, e, QString()});
            continue;
        }
        const int p = qBound(0, c.position, n);
        if (p >= n) {
            edits.append(McEdit{p, p, QString()}); // 文末：无操作
            continue;
        }
        edits.append(McEdit{p, p + 1, QString()}); // 删后一字符；行尾即删 '\n' 并入下一行
    }
    return assembleBatch(cursors, edits);
}

// 纯移动类操作：无编辑，仅重算光标（keepAnchor 保持锚点以调整选区）
template <typename PositionFn>
McEditBatch moveBatch(const QList<McCursor> &cursors, bool keepAnchor, PositionFn positionFn)
{
    QList<McCursor> result;
    result.reserve(cursors.size());
    for (const McCursor &c : cursors) {
        McCursor nc;
        nc.position = positionFn(c.position);
        nc.anchor = keepAnchor ? c.anchor : nc.position;
        result.append(nc);
    }
    sortAndDedup(result);
    McEditBatch batch;
    batch.cursors = result;
    return batch;
}

// 上/下方加光标：direction 为 -1/+1；同字符列、钳制行末；首/末行光标跳过
bool addCursorVertical(QList<McCursor> &cursors, const QString &text, int direction)
{
    if (cursors.isEmpty())
        return false;
    LineTable lt(text);
    QList<McCursor> result = cursors;
    for (const McCursor &c : std::as_const(cursors)) {
        const int line = lt.lineForPosition(c.position);
        const int col = c.position - lt.lineStart(line);
        const int targetLine = line + direction;
        if (targetLine < 0 || targetLine >= lt.lineCount())
            continue; // 已在首/末行
        const int pos = qMin(lt.lineStart(targetLine) + col, lt.lineEnd(targetLine));
        result.append(McCursor{pos, pos});
    }
    const int before = cursors.size();
    sortAndDedup(result);
    const bool changed = result.size() > before; // 有实际新增（去重后仍变大）
    if (changed)
        cursors = result;
    return changed;
}

} // namespace

// ==================== 公共接口 ====================

QList<McCursor> MultiCursorEngine::cursors() const
{
    return m_cursors;
}

void MultiCursorEngine::setCursors(const QList<McCursor> &cursors)
{
    m_cursors = cursors;
    sortAndDedup(m_cursors);
}

int MultiCursorEngine::cursorCount() const
{
    return m_cursors.size();
}

bool MultiCursorEngine::isMultiCursor() const
{
    return m_cursors.size() > 1;
}

void MultiCursorEngine::clearToLast()
{
    if (m_cursors.isEmpty())
        return;
    McCursor last = m_cursors.last(); // 已排序：position 最末，保 anchor/position
    m_cursors.clear();
    m_cursors.append(last);
    m_columnSelecting = false; // Esc 退出多光标整体状态（含列选拖拽）
}

void MultiCursorEngine::addCursorAt(int position)
{
    m_cursors.append(McCursor{position, position});
    sortAndDedup(m_cursors);
}

bool MultiCursorEngine::addCursorAbove(const QString &text)
{
    return addCursorVertical(m_cursors, text, -1);
}

bool MultiCursorEngine::addCursorBelow(const QString &text)
{
    return addCursorVertical(m_cursors, text, +1);
}

QList<McCursor> MultiCursorEngine::columnCursors(const QString &text, int anchorPosition, int cursorPosition)
{
    LineTable lt(text);
    const int len = text.length();
    const int ap = qBound(0, anchorPosition, len);
    const int cp = qBound(0, cursorPosition, len);
    const int aLine = lt.lineForPosition(ap);
    const int cLine = lt.lineForPosition(cp);
    const int aCol = ap - lt.lineStart(aLine);
    const int cCol = cp - lt.lineStart(cLine);
    // 行列区间取 min/max：反向拖拽与正向等价
    const int rowFrom = qMin(aLine, cLine);
    const int rowTo = qMax(aLine, cLine);
    const int colFrom = qMin(aCol, cCol);
    const int colTo = qMax(aCol, cCol);
    QList<McCursor> result;
    for (int row = rowFrom; row <= rowTo; ++row) {
        const int ls = lt.lineStart(row);
        const int le = lt.lineEnd(row);
        const int s = qMin(ls + colFrom, le); // 短行钳制行末，不越界到下一行
        const int e = qMin(ls + colTo, le);
        result.append(McCursor{s, e}); // 每行选区统一正向（anchor=左端）
    }
    return result;
}

void MultiCursorEngine::startColumnSelect(const QString &text, int anchorPosition, int cursorPosition)
{
    m_columnSelecting = true;
    m_columnAnchor = qBound(0, anchorPosition, text.length());
    m_cursors = columnCursors(text, m_columnAnchor, cursorPosition);
}

void MultiCursorEngine::updateColumnSelect(const QString &text, int cursorPosition)
{
    if (!m_columnSelecting)
        return;
    m_cursors = columnCursors(text, m_columnAnchor, cursorPosition);
}

void MultiCursorEngine::updateColumnSelectPairs(const QList<QPair<int, int>> &pairs)
{
    if (!m_columnSelecting)
        return; // 仅列选拖拽/键盘列选会话中生效（与 updateColumnSelect 同守卫）
    QList<McCursor> cursors;
    cursors.reserve(pairs.size());
    for (const QPair<int, int> &p : pairs) {
        // 每行选区统一正向（anchor=左端）；契约保证 start<=end，此间不再归一
        cursors.append(McCursor{p.first, p.second});
    }
    sortAndDedup(cursors); // 与 setCursors 同构：按 position 升序去重
    m_cursors = cursors;
}

void MultiCursorEngine::finishColumnSelect()
{
    m_columnSelecting = false; // 光标集合定格为普通多选区
}

bool MultiCursorEngine::isColumnSelecting() const
{
    return m_columnSelecting;
}

bool MultiCursorEngine::selectNextOccurrence(const QString &text)
{
    if (m_cursors.isEmpty())
        return false;

    bool hasSelection = false;
    for (const McCursor &c : std::as_const(m_cursors)) {
        if (c.anchor != c.position) {
            hasSelection = true;
            break;
        }
    }

    if (!hasSelection) {
        // 无选区：每个光标选中其所在词（无词处保持原光标）
        QList<McCursor> result;
        result.reserve(m_cursors.size());
        bool changed = false;
        for (const McCursor &c : std::as_const(m_cursors)) {
            const QPair<int, int> wr = wordRangeAt(text, c.position);
            if (wr.first < wr.second) {
                result.append(McCursor{wr.first, wr.second});
                changed = true;
            } else {
                result.append(c);
            }
        }
        if (!changed)
            return false;
        m_cursors = result;
        sortAndDedup(m_cursors);
        return true;
    }

    // 有选区：以最后一个有选区光标的选区文本为目标，从其后向后查找，环绕整个文档
    //（混合状态——末位为空光标时回退取最后一个有选区的光标为目标，避免整体
    //  无响应；全部无选区的情形已在上方 !hasSelection 分支以末光标选词处理）
    McCursor targetCursor = m_cursors.last();
    for (int i = m_cursors.size() - 1; i >= 0; --i) {
        const McCursor &c = m_cursors.at(i);
        if (c.anchor != c.position) {
            targetCursor = c;
            break;
        }
    }
    const int selMin = qMin(targetCursor.anchor, targetCursor.position);
    const int selMax = qMax(targetCursor.anchor, targetCursor.position);
    const QString target = text.mid(selMin, selMax - selMin);
    if (target.isEmpty() || target.contains(QLatin1Char('\n')))
        return false; // 目标为空或跨行（含换行）不匹配

    // 某出现是否与任一现有选区区间严格重叠（端点相接不算重叠）：
    // 严格重叠即视为不可追加，跳过。此前仅“完全重合”才跳过，部分重叠的命中被
    // 加入后会在 assembleBatch 中因编辑重叠“后到先弃”——输入只替换一处
    //（编辑静默丢弃）且被丢光标映射出畸变假选区。端点相接的出现（如
    // "foofoo" 选 [0,3) 后的 [3,6)）与现有选区不共享字符，assembleBatch 的
    // 重叠过滤用严格比较不会丢弃相邻编辑，因此允许追加（VS Code 语义）。
    // 空光标无选区区间，不参与。
    auto intersectsSelection = [this](int start, int length) {
        for (const McCursor &c : std::as_const(m_cursors)) {
            if (c.anchor == c.position)
                continue;
            const int s = qMin(c.anchor, c.position);
            const int e = qMax(c.anchor, c.position);
            if (start < e && s < start + length)
                return true;
        }
        return false;
    };

    // 在 [from, limit) 起始范围内找第一个不与现有选区相交的出现（出现须整体落在 limit 内）
    const int targetLen = target.length();
    auto findHit = [&](int from, int limit) {
        int idx = from;
        while (true) {
            const int hit = int(text.indexOf(target, idx)); // indexOf 默认大小写敏感
            if (hit < 0 || hit > limit - targetLen)
                return -1;
            if (!intersectsSelection(hit, targetLen))
                return hit;
            idx = hit + 1; // 该出现与现有选区相交：从下一字符继续
        }
    };

    int hit = findHit(selMax, text.length()); // 后方
    if (hit < 0)
        hit = findHit(0, selMax);             // 环绕到前方
    if (hit < 0)
        return false; // 全部出现均与现有选区相交：无变化

    m_cursors.append(McCursor{hit, hit + targetLen});
    sortAndDedup(m_cursors);
    return true;
}

int MultiCursorEngine::selectAllOccurrences(const QString &text)
{
    if (m_cursors.isEmpty())
        return 0;

    // 目标：最后一个光标的选区文本；无选区则其光标处单词
    const McCursor last = m_cursors.last();
    const int selMin = qMin(last.anchor, last.position);
    const int selMax = qMax(last.anchor, last.position);
    QString target;
    if (selMin != selMax) {
        target = text.mid(selMin, selMax - selMin);
    } else {
        const QPair<int, int> wr = wordRangeAt(text, last.position);
        if (wr.first < wr.second)
            target = text.mid(wr.first, wr.second - wr.first);
    }
    if (target.isEmpty())
        return m_cursors.size(); // 无目标：无变化，返回当前选区数

    // 从左到右收集全部非重叠出现，各生成一个选区（替换整个光标集合）
    QList<McCursor> result;
    const int targetLen = target.length();
    qsizetype idx = 0;
    while (true) {
        idx = text.indexOf(target, idx); // indexOf 默认大小写敏感
        if (idx < 0)
            break;
        const int hit = int(idx);
        result.append(McCursor{hit, hit + targetLen});
        idx += targetLen; // 非重叠推进
    }
    m_cursors = result;
    return m_cursors.size();
}

void MultiCursorEngine::endOfLines(const QString &text)
{
    LineTable lt(text);
    QList<McCursor> result;
    for (const McCursor &c : std::as_const(m_cursors)) {
        const int s = qBound(0, qMin(c.anchor, c.position), lt.length);
        const int e = qBound(0, qMax(c.anchor, c.position), lt.length);
        const int l1 = lt.lineForPosition(s);
        const int l2 = lt.lineForPosition(e);
        for (int line = l1; line <= l2; ++line) // 选区覆盖到的每一行
            result.append(McCursor{lt.lineEnd(line), lt.lineEnd(line)}); // 行末空选区
    }
    sortAndDedup(result);
    m_cursors = result; // 原选区清除
}

// ==================== 编辑操作 ====================

McEditBatch MultiCursorEngine::insertText(const QString &inserted) const
{
    return buildReplaceBatch(m_cursors, inserted);
}

McEditBatch MultiCursorEngine::backspace(const QString &text) const
{
    return backspaceBatch(m_cursors, text);
}

McEditBatch MultiCursorEngine::deleteChar(const QString &text) const
{
    return deleteCharBatch(m_cursors, text);
}

McEditBatch MultiCursorEngine::insertNewline() const
{
    return buildReplaceBatch(m_cursors, QStringLiteral("\n"));
}

McEditBatch MultiCursorEngine::moveLeft(const QString &text, bool keepAnchor, bool wordMode) const
{
    const int n = text.length();
    return moveBatch(m_cursors, keepAnchor, [&](int p) -> int {
        p = qBound(0, p, n);
        if (wordMode)
            return wordLeft(text, p);
        return p > 0 ? p - 1 : 0;
    });
}

McEditBatch MultiCursorEngine::moveRight(const QString &text, bool keepAnchor, bool wordMode) const
{
    const int n = text.length();
    return moveBatch(m_cursors, keepAnchor, [&](int p) -> int {
        p = qBound(0, p, n);
        if (wordMode)
            return wordRight(text, p);
        return p < n ? p + 1 : n;
    });
}

McEditBatch MultiCursorEngine::moveUp(const QString &text, bool keepAnchor) const
{
    const int n = text.length();
    LineTable lt(text);
    return moveBatch(m_cursors, keepAnchor, [&](int p) -> int {
        p = qBound(0, p, n);
        const int line = lt.lineForPosition(p);
        if (line == 0)
            return p; // 首行：不动
        const int col = p - lt.lineStart(line); // 用当前实际列（无 desiredColumn 状态）
        return qMin(lt.lineStart(line - 1) + col, lt.lineEnd(line - 1)); // 短行钳制
    });
}

McEditBatch MultiCursorEngine::moveDown(const QString &text, bool keepAnchor) const
{
    const int n = text.length();
    LineTable lt(text);
    return moveBatch(m_cursors, keepAnchor, [&](int p) -> int {
        p = qBound(0, p, n);
        const int line = lt.lineForPosition(p);
        if (line == lt.lineCount() - 1)
            return p; // 末行：不动
        const int col = p - lt.lineStart(line);
        return qMin(lt.lineStart(line + 1) + col, lt.lineEnd(line + 1));
    });
}

McEditBatch MultiCursorEngine::moveHome(const QString &text, bool keepAnchor) const
{
    const int n = text.length();
    LineTable lt(text);
    return moveBatch(m_cursors, keepAnchor, [&](int p) -> int {
        p = qBound(0, p, n);
        return lt.lineStart(lt.lineForPosition(p)); // 简单语义：行首第 1 列
    });
}

McEditBatch MultiCursorEngine::moveEnd(const QString &text, bool keepAnchor) const
{
    const int n = text.length();
    LineTable lt(text);
    return moveBatch(m_cursors, keepAnchor, [&](int p) -> int {
        p = qBound(0, p, n);
        return lt.lineEnd(lt.lineForPosition(p)); // 行末（换行符之前）
    });
}

// ==================== 复制 / 剪切 / 粘贴 ====================

QStringList MultiCursorEngine::selectedTexts(const QString &text) const
{
    QStringList result;
    result.reserve(m_cursors.size());
    for (const McCursor &c : std::as_const(m_cursors)) {
        const int s = qMin(c.anchor, c.position);
        const int e = qMax(c.anchor, c.position);
        result.append(text.mid(s, e - s)); // 无选区即空串
    }
    return result;
}

McEditBatch MultiCursorEngine::deleteSelections() const
{
    QList<McEdit> edits;
    edits.reserve(m_cursors.size());
    for (const McCursor &c : m_cursors) {
        const int s = qMin(c.anchor, c.position);
        const int e = qMax(c.anchor, c.position);
        edits.append(McEdit{s, e, QString()}); // 空选区为无操作编辑（会被过滤）
    }
    return assembleBatch(m_cursors, edits);
}

McEditBatch MultiCursorEngine::pasteLines(const QStringList &lines) const
{
    if (lines.size() == m_cursors.size()) {
        // 行数与光标数相等：第 i 行分发到第 i 个光标（替换其选区）
        QList<McEdit> edits;
        edits.reserve(m_cursors.size());
        for (int i = 0; i < m_cursors.size(); ++i) {
            const McCursor &c = m_cursors.at(i);
            edits.append(McEdit{qMin(c.anchor, c.position), qMax(c.anchor, c.position), lines.at(i)});
        }
        return assembleBatch(m_cursors, edits);
    }
    // 不相等：每个光标插入整份文本（多行以 '\n' 连接）
    return buildReplaceBatch(m_cursors, lines.join(QLatin1Char('\n')));
}

} // namespace qcnote
