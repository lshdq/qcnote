#ifndef MULTICURSORENGINE_H
#define MULTICURSORENGINE_H

#include <QString>
#include <QStringList>
#include <QList>

namespace qcnote {

// 光标：anchor==position 表示空选区；position 是光标所在点
struct McCursor {
    int anchor = 0;
    int position = 0;
};

// 编辑指令：把原文档 [start, end) 替换为 text
struct McEdit {
    int start = 0;
    int end = 0;
    QString text;
};

// 一次操作的编辑批次：edits 基于操作前的原文坐标、互不重叠、按 start 降序；
// cursors 是应用全部 edits 后的新光标列表（已排序去重）
struct McEditBatch {
    QList<McEdit> edits;
    QList<McCursor> cursors;
};

class MultiCursorEngine
{
public:
    // 光标集合始终按 position 升序；(anchor, position) 完全相同的光标自动去重
    QList<McCursor> cursors() const;
    void setCursors(const QList<McCursor> &cursors); // 排序 + 去重
    int cursorCount() const;
    bool isMultiCursor() const; // >1

    // ---- 多光标生成 ----
    void clearToLast();                              // Esc：仅保留排序最末一个（保 anchor/position）
    void addCursorAt(int position);                  // Alt+Click：anchor=position，去重
    bool addCursorAbove(const QString &text);        // Ctrl+Alt+Up：每个光标上方同字符列各加一个（钳制行末）；已在首行的光标跳过；有实际新增返回 true
    bool addCursorBelow(const QString &text);        // Ctrl+Alt+Down：同理
    void startColumnSelect(const QString &text, int anchorPosition, int cursorPosition); // 进入列选模式，锚点+当前点确定矩形
    void updateColumnSelect(const QString &text, int cursorPosition); // 拖拽中更新
    void finishColumnSelect();                       // 结束拖拽，定格为普通多选区（之后可正常编辑）
    bool isColumnSelecting() const;                  // 是否处于列选拖拽中（控制器查询用）
    bool selectNextOccurrence(const QString &text);  // Ctrl+D：无选区→选中光标处单词（连续的字母/数字/下划线，中文按连续非空白非标点）；有选区→以最后一个有选区光标的选区（末位为空光标的混合状态回退；全部无选区走选词分支）为目标向后找完全相同文本（大小写敏感、不含换行），环绕查找，跳过与任一现有选区区间相交（含边界相接的内含/交叠）的出现；无可追加出现时返回 false 且无变化
    int selectAllOccurrences(const QString &text);   // Ctrl+Shift+L：以当前选区文本（无选区则光标处单词）为目标，全部出现各生成一个选区；返回选区数
    void endOfLines(const QString &text);            // Shift+Alt+I：所有光标选区覆盖到的每一行行末各生成空选区光标

    // ---- 编辑操作：返回编辑批次；纯移动类操作 edits 为空 ----
    McEditBatch insertText(const QString &inserted) const;      // 有选区替换选区，无选区插入
    McEditBatch backspace(const QString &text) const;           // 选区删除；否则删前一字符，行首并入上一行；文档 position 0 无操作
    McEditBatch deleteChar(const QString &text) const;          // 选区删除；否则删后一字符，行尾并下一行；文末无操作
    McEditBatch insertNewline() const;                          // 各光标插入 "\n"（LF）
    McEditBatch moveLeft(const QString &text, bool keepAnchor, bool wordMode) const;
    McEditBatch moveRight(const QString &text, bool keepAnchor, bool wordMode) const;
    McEditBatch moveUp(const QString &text, bool keepAnchor) const;   // 保持字符列（短行钳制）
    McEditBatch moveDown(const QString &text, bool keepAnchor) const;
    McEditBatch moveHome(const QString &text, bool keepAnchor) const; // 行首第 1 列（简单语义）
    McEditBatch moveEnd(const QString &text, bool keepAnchor) const;  // 行末

    // ---- 复制 / 剪切 / 粘贴 ----
    QStringList selectedTexts(const QString &text) const;       // 按光标顺序取各选区文本（无选区=空串）
    McEditBatch deleteSelections() const;                       // 删除所有选区（剪切第二步）
    McEditBatch pasteLines(const QStringList &lines) const;     // lines 数==光标数→逐行分发；否则每光标插入 lines 以 "\n" 连接的整份文本

    // ---- 列选区静态计算（startColumnSelect 内部也用它）----
    static QList<McCursor> columnCursors(const QString &text, int anchorPosition, int cursorPosition);

private:
    QList<McCursor> m_cursors;        // 始终按 position 升序、(anchor,position) 去重
    int m_columnAnchor = 0;           // 列选锚点（原文档坐标）
    bool m_columnSelecting = false;   // 是否处于列选拖拽中
};

} // namespace qcnote

#endif // MULTICURSORENGINE_H
