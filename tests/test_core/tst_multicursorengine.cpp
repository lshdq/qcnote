#include <QTest>
#include <QTextCursor>
#include <QTextDocument>

#include "editor/multicursorengine.h"

#include <initializer_list>
#include <utility>

using qcnote::McCursor;
using qcnote::McEdit;
using qcnote::McEditBatch;
using qcnote::MultiCursorEngine;

// ===== 测试辅助 =====
namespace {

// 构造光标列表：每项 {anchor, position}
QList<McCursor> cs(std::initializer_list<std::pair<int, int>> pairs)
{
    QList<McCursor> out;
    for (const auto &pr : pairs)
        out.append(McCursor{pr.first, pr.second});
    return out;
}

// 光标列表转为可读字符串（anchor:position，空格分隔），便于 QCOMPARE 输出差异
QString fmt(const QList<McCursor> &cursors)
{
    QStringList parts;
    for (const McCursor &c : cursors)
        parts << QStringLiteral("%1:%2").arg(c.anchor).arg(c.position);
    return parts.join(QLatin1Char(' '));
}

// 按契约从后往前应用编辑批次（edits 已按 start 降序，原坐标保持有效）
QString applyEdits(QString text, const QList<McEdit> &edits)
{
    for (const McEdit &e : edits)
        text.replace(e.start, e.end - e.start, e.text);
    return text;
}

// 校验批次不变量：edits 按降序且互不重叠；cursors 升序且去重
void verifyBatchInvariants(const McEditBatch &batch)
{
    for (int i = 0; i + 1 < batch.edits.size(); ++i)
        QVERIFY(batch.edits.at(i).start >= batch.edits.at(i + 1).end);
    for (int i = 0; i + 1 < batch.cursors.size(); ++i) {
        const McCursor &a = batch.cursors.at(i);
        const McCursor &b = batch.cursors.at(i + 1);
        QVERIFY(a.position < b.position
                || (a.position == b.position && a.anchor < b.anchor));
    }
}

} // namespace

// 列式编辑 T1：多光标纯逻辑引擎（TC-001 至 TC-023）
class TestMultiCursorEngine : public QObject
{
    Q_OBJECT

private slots:
    void tc001_columnSelect_equalLines();
    void tc002_columnSelect_shortLineClamp();
    void tc003_columnSelect_reverseDrag();
    void tc004_addCursorAboveBelow_keepColumn();
    void tc005_addCursorAboveBelow_clampAndBoundary();
    void tc006_cursorDedup();
    void tc007_insertText();
    void tc008_backspace();
    void tc009_deleteChar();
    void tc010_insertNewline();
    void tc011_homeEnd();
    void tc012_arrowMoves();
    void tc013_selectNextOccurrence();
    void tc014_selectAllOccurrences();
    void tc015_endOfLines();
    void tc016_selectedTexts();
    void tc017_pasteLines_distribute();
    void tc018_pasteLines_wholeText();
    void tc019_clearToLast();
    void tc020_mergeAfterEdit();
    void tc021_undoAtomicity();
    void tc022_emptyAndSingleLineDocs();
    void tc023_selectNextOccurrence_overlapSkip();
    void tc024_columnSelectPairsUpdate();
};

// TC-001：等长行列选区：3 行各覆盖 0 基列 [1,3)，并验证列选交互流程与后续编辑
void TestMultiCursorEngine::tc001_columnSelect_equalLines()
{
    const QString text = QStringLiteral("aaaa\nbbbb\ncccc");
    // 锚点第 1 行列 2（0 基列 1，位置 1），游标第 3 行列 4（0 基列 3，位置 13）
    QCOMPARE(fmt(MultiCursorEngine::columnCursors(text, 1, 13)), QStringLiteral("1:3 6:8 11:13"));

    MultiCursorEngine engine;
    engine.startColumnSelect(text, 1, 13);
    QVERIFY(engine.isColumnSelecting());
    QCOMPARE(fmt(engine.cursors()), QStringLiteral("1:3 6:8 11:13"));

    // 拖拽中更新：游标回到第 2 行列 2（位置 6，列 1）→ 前 2 行空选区
    engine.updateColumnSelect(text, 6);
    QCOMPARE(fmt(engine.cursors()), QStringLiteral("1:1 6:6"));
    engine.updateColumnSelect(text, 8); // 游标到第 2 行列 4（位置 8，列 3）
    QCOMPARE(fmt(engine.cursors()), QStringLiteral("1:3 6:8"));

    // 结束拖拽：定格为普通多选区，之后可正常编辑
    engine.finishColumnSelect();
    QVERIFY(!engine.isColumnSelecting());
    McEditBatch batch = engine.insertText(QStringLiteral("="));
    verifyBatchInvariants(batch);
    QCOMPARE(applyEdits(text, batch.edits), QStringLiteral("a=a\nb=b\ncccc"));
    QCOMPARE(fmt(batch.cursors), QStringLiteral("2:2 6:6"));

    // 未进入列选模式时 updateColumnSelect 为无操作
    MultiCursorEngine idle;
    idle.setCursors(cs({{0, 0}}));
    idle.updateColumnSelect(text, 13);
    QCOMPARE(fmt(idle.cursors()), QStringLiteral("0:0"));
}

// TC-002：短行截断：中间行短于选区起始列时生成行末空选区，不越界到下一行
void TestMultiCursorEngine::tc002_columnSelect_shortLineClamp()
{
    const QString text = QStringLiteral("aaaa\nb\ncccc");
    // 锚点位置 2（第 1 行列 3，0 基列 2），游标位置 10（第 3 行列 4，0 基列 3）
    const QList<McCursor> result = MultiCursorEngine::columnCursors(text, 2, 10);
    QCOMPARE(fmt(result), QStringLiteral("2:3 6:6 9:10"));
    // 中间行 "b"（位置 5，行末 6）：空选区钳制在行末 6，未越界到下一行起点 7
    const McCursor mid = result.at(1);
    QCOMPARE(mid.anchor, 6);
    QCOMPARE(mid.position, 6);
}

// TC-003：反向拖拽（锚点右下、游标左上）与正向等价，每行选区 start<=end
void TestMultiCursorEngine::tc003_columnSelect_reverseDrag()
{
    const QString text = QStringLiteral("aaaa\nbbbb\ncccc");
    // 锚点第 3 行列 4（位置 13），游标第 1 行列 2（位置 1）
    const QList<McCursor> result = MultiCursorEngine::columnCursors(text, 13, 1);
    QCOMPARE(fmt(result), QStringLiteral("1:3 6:8 11:13"));
    for (const McCursor &c : result)
        QVERIFY(c.anchor <= c.position);
}

// TC-004：上/下方加光标保持字符列
void TestMultiCursorEngine::tc004_addCursorAboveBelow_keepColumn()
{
    const QString text = QStringLiteral("ab\ncdef\ngh");
    MultiCursorEngine up;
    up.setCursors(cs({{5, 5}})); // 第 2 行列 3（0 基列 2）
    QVERIFY(up.addCursorAbove(text));
    QCOMPARE(fmt(up.cursors()), QStringLiteral("2:2 5:5")); // 第 1 行同列（行末恰为列 2）

    MultiCursorEngine down;
    down.setCursors(cs({{5, 5}}));
    QVERIFY(down.addCursorBelow(text));
    QCOMPARE(fmt(down.cursors()), QStringLiteral("5:5 10:10")); // 第 3 行同列
}

// TC-005：短行钳制行末；首行上方/末行下方为无操作；重复添加不产生实际新增
void TestMultiCursorEngine::tc005_addCursorAboveBelow_clampAndBoundary()
{
    const QString text = QStringLiteral("abcdef\nx\nabcdef");

    MultiCursorEngine clamp;
    clamp.setCursors(cs({{3, 3}})); // 第 1 行列 4（0 基列 3）
    QVERIFY(clamp.addCursorBelow(text)); // 第 2 行 "x" 仅 1 字符：钳制行末 8
    QCOMPARE(fmt(clamp.cursors()), QStringLiteral("3:3 8:8"));

    MultiCursorEngine top;
    top.setCursors(cs({{0, 0}})); // 首行首列
    QVERIFY(!top.addCursorAbove(text)); // 首行上方：无操作
    QCOMPARE(top.cursorCount(), 1);

    MultiCursorEngine bottom;
    bottom.setCursors(cs({{9, 9}})); // 末行
    QVERIFY(!bottom.addCursorBelow(text));
    QCOMPARE(bottom.cursorCount(), 1);

    // 第 1、2 行各有光标：上方加只对第 2 行光标生效，但列 0 与既有光标重复 → 无实际新增
    MultiCursorEngine dup;
    dup.setCursors(cs({{0, 0}, {7, 7}}));
    QVERIFY(!dup.addCursorAbove(text));
    QCOMPARE(fmt(dup.cursors()), QStringLiteral("0:0 7:7"));
    // 下方加：第 1 行光标加到第 2 行列 0（重复），第 2 行光标加到第 3 行列 0（新增）
    QVERIFY(dup.addCursorBelow(text));
    QCOMPARE(fmt(dup.cursors()), QStringLiteral("0:0 7:7 9:9"));
}

// TC-006：光标去重：完全相同 (anchor, position) 合并；仅同 position 不同 anchor 不合并
void TestMultiCursorEngine::tc006_cursorDedup()
{
    MultiCursorEngine engine;
    engine.setCursors(cs({{3, 3}, {7, 7}}));
    engine.addCursorAt(3); // 与现有光标完全相同
    engine.addCursorAt(7);
    QCOMPARE(engine.cursorCount(), 2);
    engine.addCursorAt(5);
    QCOMPARE(fmt(engine.cursors()), QStringLiteral("3:3 5:5 7:7"));
    QVERIFY(engine.isMultiCursor());

    MultiCursorEngine dups;
    dups.setCursors(cs({{2, 2}, {2, 2}, {9, 9}, {2, 2}}));
    QCOMPARE(fmt(dups.cursors()), QStringLiteral("2:2 9:9"));

    // 同 position 不同 anchor：不是重复，不合并
    MultiCursorEngine partial;
    partial.setCursors(cs({{2, 5}, {5, 5}}));
    QCOMPARE(partial.cursorCount(), 2);
}

// TC-007：多光标插入：带选区光标替换选区，空光标处插入；新光标位于插入文本之后
void TestMultiCursorEngine::tc007_insertText()
{
    const QString text = QStringLiteral("one two three");
    MultiCursorEngine engine;
    engine.setCursors(cs({{0, 3}, {4, 7}, {13, 13}})); // 2 个选区 + 1 个空光标
    McEditBatch batch = engine.insertText(QStringLiteral("X"));
    verifyBatchInvariants(batch);
    QCOMPARE(batch.edits.size(), 3);
    QCOMPARE(applyEdits(text, batch.edits), QStringLiteral("X X threeX"));
    QCOMPARE(fmt(batch.cursors), QStringLiteral("1:1 3:3 10:10"));
}

// TC-008：Backspace：选区删除 / 行中删前字符 / 行首并入上一行；文档首无操作
void TestMultiCursorEngine::tc008_backspace()
{
    const QString text = QStringLiteral("hello\nworld");
    MultiCursorEngine engine;
    engine.setCursors(cs({{2, 4}, {6, 6}, {8, 8}})); // 选区 "ll" / 第 2 行行首 / 行中
    McEditBatch batch = engine.backspace(text);
    verifyBatchInvariants(batch);
    QCOMPARE(applyEdits(text, batch.edits), QStringLiteral("heowrld"));
    QCOMPARE(fmt(batch.cursors), QStringLiteral("2:2 3:3 4:4"));

    MultiCursorEngine docStart;
    docStart.setCursors(cs({{0, 0}}));
    McEditBatch noop = docStart.backspace(text);
    QVERIFY(noop.edits.isEmpty());
    QCOMPARE(fmt(noop.cursors), QStringLiteral("0:0"));
}

// TC-009：Delete：选区删除 / 行中删后字符 / 行尾并下一行；文末无操作
void TestMultiCursorEngine::tc009_deleteChar()
{
    const QString text = QStringLiteral("hello\nworld");
    MultiCursorEngine engine;
    engine.setCursors(cs({{2, 4}, {5, 5}, {8, 8}, {11, 11}})); // 选区 / 第 1 行行尾 / 行中 / 文末
    McEditBatch batch = engine.deleteChar(text);
    verifyBatchInvariants(batch);
    QCOMPARE(applyEdits(text, batch.edits), QStringLiteral("heowold"));
    QCOMPARE(fmt(batch.cursors), QStringLiteral("2:2 3:3 5:5 7:7"));

    MultiCursorEngine docEnd;
    docEnd.setCursors(cs({{11, 11}}));
    McEditBatch noop = docEnd.deleteChar(text);
    QVERIFY(noop.edits.isEmpty());
    QCOMPARE(fmt(noop.cursors), QStringLiteral("11:11"));
}

// TC-010：Enter：各光标处插入 LF，光标位于新行首；有选区先替换选区
void TestMultiCursorEngine::tc010_insertNewline()
{
    const QString text = QStringLiteral("ab\ncd");
    MultiCursorEngine engine;
    engine.setCursors(cs({{1, 1}, {5, 5}}));
    McEditBatch batch = engine.insertNewline();
    verifyBatchInvariants(batch);
    QCOMPARE(applyEdits(text, batch.edits), QStringLiteral("a\nb\ncd\n"));
    QCOMPARE(fmt(batch.cursors), QStringLiteral("2:2 7:7")); // 均在新行行首

    // 空光标在行中：光标位于插入换行之后（新行首）
    MultiCursorEngine mid;
    mid.setCursors(cs({{4, 4}})); // "cd" 中间
    McEditBatch midBatch = mid.insertNewline();
    QCOMPARE(applyEdits(text, midBatch.edits), QStringLiteral("ab\nc\nd"));
    QCOMPARE(fmt(midBatch.cursors), QStringLiteral("5:5"));

    // 有选区：替换选区为换行
    MultiCursorEngine sel;
    sel.setCursors(cs({{0, 2}}));
    McEditBatch selBatch = sel.insertNewline();
    QCOMPARE(applyEdits(text, selBatch.edits), QStringLiteral("\n\ncd"));
    QCOMPARE(fmt(selBatch.cursors), QStringLiteral("1:1"));
}

// TC-011：Home/End（含 Shift）：行首第 1 列/行末；keepAnchor 扩展或收缩选区
void TestMultiCursorEngine::tc011_homeEnd()
{
    const QString text = QStringLiteral("ab\ncdef\ngh"); // 行范围 [0,2) [3,7) [8,10)
    MultiCursorEngine engine;
    engine.setCursors(cs({{4, 4}, {9, 9}}));
    QCOMPARE(fmt(engine.moveHome(text, false).cursors), QStringLiteral("3:3 8:8"));
    QCOMPARE(fmt(engine.moveEnd(text, false).cursors), QStringLiteral("7:7 10:10"));

    MultiCursorEngine keep;
    keep.setCursors(cs({{4, 4}}));
    QCOMPARE(fmt(keep.moveHome(text, true).cursors), QStringLiteral("4:3")); // 反向选区
    keep.setCursors(cs({{5, 5}}));
    QCOMPARE(fmt(keep.moveEnd(text, true).cursors), QStringLiteral("5:7"));

    // 同行多光标 Home 汇聚并去重
    MultiCursorEngine merge;
    merge.setCursors(cs({{3, 3}, {5, 5}}));
    QCOMPARE(fmt(merge.moveHome(text, false).cursors), QStringLiteral("3:3"));

    // 选区上 Home（keepAnchor）：锚点不动，光标移其所在行行首（position 4 在第 2 行）
    MultiCursorEngine sel;
    sel.setCursors(cs({{2, 4}}));
    QCOMPARE(fmt(sel.moveHome(text, true).cursors), QStringLiteral("2:3"));
}

// TC-012：方向键移动与 Shift 选择：左右（含词移动）、上下保持列（短行钳制）
void TestMultiCursorEngine::tc012_arrowMoves()
{
    // ---- 左右（含 keepAnchor；移动类操作为 const，每次独立构造光标状态）----
    const QString text = QStringLiteral("abcd");
    MultiCursorEngine engine;
    engine.setCursors(cs({{2, 2}}));
    QCOMPARE(fmt(engine.moveLeft(text, false, false).cursors), QStringLiteral("1:1"));
    engine.setCursors(cs({{2, 2}}));
    QCOMPARE(fmt(engine.moveLeft(text, true, false).cursors), QStringLiteral("2:1"));
    engine.setCursors(cs({{1, 1}}));
    QCOMPARE(fmt(engine.moveRight(text, true, false).cursors), QStringLiteral("1:2"));
    engine.setCursors(cs({{2, 2}}));
    QCOMPARE(fmt(engine.moveRight(text, false, false).cursors), QStringLiteral("3:3"));
    // 边界：文首左移、文末右移均不动
    engine.setCursors(cs({{0, 0}}));
    QCOMPARE(fmt(engine.moveLeft(text, false, false).cursors), QStringLiteral("0:0"));
    engine.setCursors(cs({{4, 4}}));
    QCOMPARE(fmt(engine.moveRight(text, false, false).cursors), QStringLiteral("4:4"));

    // ---- 词移动（wordMode）：跳过分隔符到词边界 ----
    const QString words = QStringLiteral("foo bar baz");
    MultiCursorEngine wordEngine;
    wordEngine.setCursors(cs({{4, 4}})); // 空格处 → 前一词首
    QCOMPARE(fmt(wordEngine.moveLeft(words, false, true).cursors), QStringLiteral("0:0"));
    wordEngine.setCursors(cs({{11, 11}})); // 词尾 → 该词首
    QCOMPARE(fmt(wordEngine.moveLeft(words, false, true).cursors), QStringLiteral("8:8"));
    wordEngine.setCursors(cs({{1, 1}}));
    QCOMPARE(fmt(wordEngine.moveLeft(words, false, true).cursors), QStringLiteral("0:0"));
    wordEngine.setCursors(cs({{0, 0}})); // 词首 → 前一词首
    QCOMPARE(fmt(wordEngine.moveLeft(words, false, true).cursors), QStringLiteral("0:0"));
    wordEngine.setCursors(cs({{0, 0}})); // 词首右移 → 词尾
    QCOMPARE(fmt(wordEngine.moveRight(words, false, true).cursors), QStringLiteral("3:3"));
    wordEngine.setCursors(cs({{3, 3}})); // 空格处 → 下一词尾
    QCOMPARE(fmt(wordEngine.moveRight(words, false, true).cursors), QStringLiteral("7:7"));
    wordEngine.setCursors(cs({{11, 11}})); // 文末不动
    QCOMPARE(fmt(wordEngine.moveRight(words, false, true).cursors), QStringLiteral("11:11"));

    // ---- 上下保持字符列（短行钳制；钳制后以实际列为准）----
    const QString lines = QStringLiteral("abcd\nxy\nlonger"); // 行范围 [0,4) [5,7) [8,15)
    MultiCursorEngine vengine;
    vengine.setCursors(cs({{2, 2}}));
    QCOMPARE(fmt(vengine.moveDown(lines, false).cursors), QStringLiteral("7:7")); // 列 2 恰为行末
    vengine.setCursors(cs({{3, 3}}));
    QCOMPARE(fmt(vengine.moveDown(lines, false).cursors), QStringLiteral("7:7")); // 列 3 钳制行末
    vengine.setCursors(cs({{7, 7}})); // 实际列 2（钳制后）
    QCOMPARE(fmt(vengine.moveDown(lines, false).cursors), QStringLiteral("10:10")); // 用实际列 2
    vengine.setCursors(cs({{10, 10}}));
    QCOMPARE(fmt(vengine.moveUp(lines, false).cursors), QStringLiteral("7:7"));
    vengine.setCursors(cs({{7, 7}}));
    QCOMPARE(fmt(vengine.moveUp(lines, false).cursors), QStringLiteral("2:2"));
    // 首行上移 / 末行下移不动
    vengine.setCursors(cs({{1, 1}}));
    QCOMPARE(fmt(vengine.moveUp(lines, false).cursors), QStringLiteral("1:1"));
    vengine.setCursors(cs({{9, 9}}));
    QCOMPARE(fmt(vengine.moveDown(lines, false).cursors), QStringLiteral("9:9"));
    // keepAnchor：锚点保持，选区随光标调整
    vengine.setCursors(cs({{10, 10}}));
    QCOMPARE(fmt(vengine.moveUp(lines, true).cursors), QStringLiteral("10:7"));
    // 多光标同列上下移
    vengine.setCursors(cs({{2, 2}, {10, 10}}));
    McEditBatch upBatch = vengine.moveUp(lines, false);
    QVERIFY(upBatch.edits.isEmpty());
    QCOMPARE(fmt(upBatch.cursors), QStringLiteral("2:2 7:7"));
}

// TC-013：Ctrl+D：首次选词、依次追加、环绕跳过已选、全部选中后无操作；大小写敏感、不跨行
void TestMultiCursorEngine::tc013_selectNextOccurrence()
{
    const QString text = QStringLiteral("foo bar foo baz foo"); // 出现：0、8、16
    MultiCursorEngine engine;
    engine.setCursors(cs({{1, 1}})); // 光标在第一个 "foo" 内（无选区）
    QVERIFY(engine.selectNextOccurrence(text)); // 首次：选中当前单词
    QCOMPARE(fmt(engine.cursors()), QStringLiteral("0:3"));
    QVERIFY(engine.selectNextOccurrence(text)); // 追加下一个出现
    QCOMPARE(fmt(engine.cursors()), QStringLiteral("0:3 8:11"));
    QVERIFY(engine.selectNextOccurrence(text));
    QCOMPARE(fmt(engine.cursors()), QStringLiteral("0:3 8:11 16:19"));
    QVERIFY(!engine.selectNextOccurrence(text)); // 全部已选中：无变化
    QCOMPARE(engine.cursorCount(), 3);

    // 环绕：从文末选区环绕找到文首出现
    MultiCursorEngine wrap;
    wrap.setCursors(cs({{9, 9}})); // 第二个 "foo" 内
    QVERIFY(wrap.selectNextOccurrence(text));
    QCOMPARE(fmt(wrap.cursors()), QStringLiteral("8:11"));
    QVERIFY(wrap.selectNextOccurrence(text)); // 后方 16
    QCOMPARE(fmt(wrap.cursors()), QStringLiteral("8:11 16:19"));
    QVERIFY(wrap.selectNextOccurrence(text)); // 环绕到文首 0
    QCOMPARE(fmt(wrap.cursors()), QStringLiteral("0:3 8:11 16:19"));
    QVERIFY(!wrap.selectNextOccurrence(text));

    // 大小写敏感：目标 "Foo" 无其他精确出现
    MultiCursorEngine csEngine;
    const QString mixed = QStringLiteral("Foo foo fOO");
    csEngine.setCursors(cs({{1, 1}}));
    QVERIFY(csEngine.selectNextOccurrence(mixed));
    QCOMPARE(fmt(csEngine.cursors()), QStringLiteral("0:3"));
    QVERIFY(!csEngine.selectNextOccurrence(mixed));
    QCOMPARE(csEngine.cursorCount(), 1);

    // 不跨行：选区含换行返回 false；单词不跨行
    MultiCursorEngine nl;
    nl.setCursors(cs({{0, 3}})); // 选中 "ab\n"
    QVERIFY(!nl.selectNextOccurrence(QStringLiteral("ab\ncd ab")));
    QCOMPARE(fmt(nl.cursors()), QStringLiteral("0:3"));
    MultiCursorEngine nlWord;
    nlWord.setCursors(cs({{1, 1}}));
    QVERIFY(nlWord.selectNextOccurrence(QStringLiteral("fo\no"))); // 词 "fo" 止于换行
    QCOMPARE(fmt(nlWord.cursors()), QStringLiteral("0:2"));

    // 空白处无词：无操作
    MultiCursorEngine space;
    space.setCursors(cs({{3, 3}}));
    QVERIFY(!space.selectNextOccurrence(QStringLiteral("foo bar")));

    // 多个空光标：各自选中所在词
    MultiCursorEngine multi;
    multi.setCursors(cs({{1, 1}, {9, 9}}));
    QVERIFY(multi.selectNextOccurrence(QStringLiteral("foo bar foo baz")));
    QCOMPARE(fmt(multi.cursors()), QStringLiteral("0:3 8:11"));

    // 中文：按连续非空白非标点成词；中文标点为边界
    MultiCursorEngine zh;
    zh.setCursors(cs({{1, 1}}));
    QVERIFY(zh.selectNextOccurrence(QStringLiteral("代码 注释 代码")));
    QCOMPARE(fmt(zh.cursors()), QStringLiteral("0:2"));
    QVERIFY(zh.selectNextOccurrence(QStringLiteral("代码 注释 代码")));
    QCOMPARE(fmt(zh.cursors()), QStringLiteral("0:2 6:8"));
    MultiCursorEngine zhPunct;
    zhPunct.setCursors(cs({{1, 1}}));
    QVERIFY(zhPunct.selectNextOccurrence(QStringLiteral("你好，世界")));
    QCOMPARE(fmt(zhPunct.cursors()), QStringLiteral("0:2"));
}

// TC-014：Ctrl+Shift+L：全部出现各生成一个选区；从左到右非重叠
void TestMultiCursorEngine::tc014_selectAllOccurrences()
{
    MultiCursorEngine engine;
    engine.setCursors(cs({{1, 1}}));
    QCOMPARE(engine.selectAllOccurrences(QStringLiteral("cat dog cat cat")), 3);
    QCOMPARE(fmt(engine.cursors()), QStringLiteral("0:3 8:11 12:15"));

    // 从既有选区出发
    MultiCursorEngine sel;
    sel.setCursors(cs({{4, 7}}));
    QCOMPARE(sel.selectAllOccurrences(QStringLiteral("cat dog cat cat")), 1);
    QCOMPARE(fmt(sel.cursors()), QStringLiteral("4:7"));

    // 重叠出现按非重叠从左到右只取一个
    MultiCursorEngine overlap;
    overlap.setCursors(cs({{1, 1}}));
    QCOMPARE(overlap.selectAllOccurrences(QStringLiteral("aaaa")), 1);
    QCOMPARE(fmt(overlap.cursors()), QStringLiteral("0:4"));

    // 无目标（空文档/无词处）：无变化，返回当前选区数
    MultiCursorEngine none;
    none.setCursors(cs({{1, 1}}));
    QCOMPARE(none.selectAllOccurrences(QStringLiteral("   ")), 1);
    QCOMPARE(fmt(none.cursors()), QStringLiteral("1:1"));
}

// TC-015：Shift+Alt+I：选区覆盖到的每行行末各生成空选区光标，原选区清除
void TestMultiCursorEngine::tc015_endOfLines()
{
    const QString text = QStringLiteral("ab\ncdef\ngh"); // 行末：2、7、10
    MultiCursorEngine engine;
    engine.setCursors(cs({{0, 5}})); // 跨第 1~2 行部分选中
    engine.endOfLines(text);
    QCOMPARE(fmt(engine.cursors()), QStringLiteral("2:2 7:7"));

    MultiCursorEngine full;
    full.setCursors(cs({{0, 10}})); // 覆盖全部 3 行
    full.endOfLines(text);
    QCOMPARE(fmt(full.cursors()), QStringLiteral("2:2 7:7 10:10"));

    // 单行选区只出一行；多光标混合并去重
    MultiCursorEngine mixed;
    mixed.setCursors(cs({{0, 5}, {8, 10}}));
    mixed.endOfLines(text);
    QCOMPARE(fmt(mixed.cursors()), QStringLiteral("2:2 7:7 10:10"));

    MultiCursorEngine single;
    single.setCursors(cs({{8, 10}}));
    single.endOfLines(text);
    QCOMPARE(fmt(single.cursors()), QStringLiteral("10:10"));
}

// TC-016：列复制：按光标顺序返回各选区文本（无选区=空串；反向选区同样正确）
void TestMultiCursorEngine::tc016_selectedTexts()
{
    MultiCursorEngine engine;
    engine.setCursors(cs({{0, 3}, {7, 4}, {13, 13}}));
    QCOMPARE(engine.selectedTexts(QStringLiteral("one two three")),
             QStringList() << QStringLiteral("one") << QStringLiteral("two") << QString());
}

// TC-017：粘贴行数匹配：第 i 行分发到第 i 个光标；有选区则替换
void TestMultiCursorEngine::tc017_pasteLines_distribute()
{
    const QString text = QStringLiteral("ab\ncd\nef");
    MultiCursorEngine engine;
    engine.setCursors(cs({{1, 1}, {4, 4}, {7, 7}}));
    McEditBatch batch = engine.pasteLines(QStringList{QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z")});
    verifyBatchInvariants(batch);
    QCOMPARE(applyEdits(text, batch.edits), QStringLiteral("aXb\ncYd\neZf"));
    QCOMPARE(fmt(batch.cursors), QStringLiteral("2:2 6:6 10:10"));

    // 有选区的光标被对应行替换
    MultiCursorEngine sel;
    sel.setCursors(cs({{0, 2}, {4, 4}}));
    McEditBatch selBatch = sel.pasteLines(QStringList{QStringLiteral("Q"), QStringLiteral("R")});
    QCOMPARE(applyEdits(text, selBatch.edits), QStringLiteral("Q\ncRd\nef"));
    QCOMPARE(fmt(selBatch.cursors), QStringLiteral("1:1 4:4"));

    // 单光标单行：同样逐行分发（即常规插入）
    MultiCursorEngine single;
    single.setCursors(cs({{1, 1}}));
    McEditBatch singleBatch = single.pasteLines(QStringList{QStringLiteral("XY")});
    QCOMPARE(applyEdits(text, singleBatch.edits), QStringLiteral("aXYb\ncd\nef"));
    QCOMPARE(fmt(singleBatch.cursors), QStringLiteral("3:3"));
}

// TC-018：粘贴行数不匹配：每个光标插入以 "\n" 连接的整份文本
void TestMultiCursorEngine::tc018_pasteLines_wholeText()
{
    const QString text = QStringLiteral("ab\ncd\nef");
    const QStringList lines{QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"),
                            QStringLiteral("D"), QStringLiteral("E")};
    const QString full = lines.join(QLatin1Char('\n'));
    MultiCursorEngine engine;
    engine.setCursors(cs({{1, 1}, {4, 4}, {7, 7}}));
    McEditBatch batch = engine.pasteLines(lines);
    verifyBatchInvariants(batch);
    const QString expected = QStringLiteral("a") + full + QStringLiteral("b\nc") + full
                             + QStringLiteral("d\ne") + full + QStringLiteral("f");
    QCOMPARE(applyEdits(text, batch.edits), expected);
    QCOMPARE(fmt(batch.cursors), QStringLiteral("10:10 22:22 34:34"));
}

// TC-019：Esc 退出：仅保留按位置排序最末一个光标（保 anchor/position）
void TestMultiCursorEngine::tc019_clearToLast()
{
    MultiCursorEngine engine;
    engine.setCursors(cs({{1, 1}, {4, 7}, {9, 9}}));
    QVERIFY(engine.isMultiCursor());
    engine.clearToLast();
    QCOMPARE(engine.cursorCount(), 1);
    QVERIFY(!engine.isMultiCursor());
    QCOMPARE(fmt(engine.cursors()), QStringLiteral("9:9"));

    // 末光标带选区：anchor/position 原样保留
    MultiCursorEngine sel;
    sel.setCursors(cs({{1, 1}, {5, 9}}));
    sel.clearToLast();
    QCOMPARE(fmt(sel.cursors()), QStringLiteral("5:9"));
}

// TC-020：编辑后光标汇聚到同一位置时合并为一个，光标数减少
void TestMultiCursorEngine::tc020_mergeAfterEdit()
{
    // 相邻光标同时 Backspace：两处编辑独立生效，光标汇聚后合并
    MultiCursorEngine engine;
    engine.setCursors(cs({{2, 2}, {3, 3}}));
    McEditBatch batch = engine.backspace(QStringLiteral("abcd"));
    QCOMPARE(applyEdits(QStringLiteral("abcd"), batch.edits), QStringLiteral("ad"));
    QCOMPARE(batch.edits.size(), 2);
    QCOMPARE(fmt(batch.cursors), QStringLiteral("1:1")); // 汇聚合并

    // 同行多光标 Home 全部到行首后合并（合并结果体现在批次光标中）
    MultiCursorEngine home;
    home.setCursors(cs({{1, 1}, {3, 3}}));
    McEditBatch homeBatch = home.moveHome(QStringLiteral("abcd"), false);
    QCOMPARE(fmt(homeBatch.cursors), QStringLiteral("0:0"));
    QCOMPARE(homeBatch.cursors.size(), 1);
}

// TC-021：撤销原子性：editBlock 按 start 降序手工应用引擎批次，一次 undo 整批回滚；
// 同时验证相邻光标（位置 5、6 同时 Backspace）编辑互不破坏
void TestMultiCursorEngine::tc021_undoAtomicity()
{
    // ---- 相邻光标 Backspace：两处删除都生效 ----
    const QString original = QStringLiteral("0123456789");
    MultiCursorEngine engine;
    engine.setCursors(cs({{5, 5}, {6, 6}}));
    McEditBatch batch = engine.backspace(original);
    verifyBatchInvariants(batch);
    QCOMPARE(batch.edits.size(), 2); // 删除 '4'（[4,5)）与 '5'（[5,6)）两条编辑

    QTextDocument doc;
    doc.setPlainText(original);
    QTextCursor cur(&doc);
    cur.beginEditBlock(); // 单批编辑合为一次撤销
    for (const McEdit &e : std::as_const(batch.edits)) { // 已按 start 降序
        cur.setPosition(e.start);
        cur.setPosition(e.end, QTextCursor::KeepAnchor);
        cur.insertText(e.text);
    }
    cur.endEditBlock();
    QCOMPARE(doc.toPlainText(), QStringLiteral("01236789")); // '4'、'5' 均被删除

    QVERIFY(doc.isUndoAvailable());
    doc.undo(); // 一次 undo 完整回滚整批
    QCOMPARE(doc.toPlainText(), original);

    // ---- 三光标插入 + 一次 undo/redo ----
    const QString text3 = QStringLiteral("abc\ndef\nghi");
    MultiCursorEngine multi;
    multi.setCursors(cs({{1, 1}, {5, 5}, {9, 9}}));
    McEditBatch insertBatch = multi.insertText(QStringLiteral("XY"));
    verifyBatchInvariants(insertBatch);
    QCOMPARE(insertBatch.edits.size(), 3);

    QTextDocument doc3;
    doc3.setPlainText(text3);
    QTextCursor cur3(&doc3);
    cur3.beginEditBlock();
    for (const McEdit &e : std::as_const(insertBatch.edits)) {
        cur3.setPosition(e.start);
        cur3.setPosition(e.end, QTextCursor::KeepAnchor);
        cur3.insertText(e.text);
    }
    cur3.endEditBlock();
    const QString after = QStringLiteral("aXYbc\ndXYef\ngXYhi");
    QCOMPARE(doc3.toPlainText(), after);

    doc3.undo(); // 一次回滚三处插入
    QCOMPARE(doc3.toPlainText(), text3);
    doc3.redo(); // 一次重做恢复
    QCOMPARE(doc3.toPlainText(), after);
}

// TC-022：边界安全：空文档与单行文档上各操作无崩溃、无越界、语义合理
void TestMultiCursorEngine::tc022_emptyAndSingleLineDocs()
{
    // ---- 空文档 ----
    const QString empty;
    MultiCursorEngine engine;
    engine.setCursors(cs({{0, 0}}));

    QVERIFY(engine.backspace(empty).edits.isEmpty());
    QCOMPARE(fmt(engine.backspace(empty).cursors), QStringLiteral("0:0"));
    QVERIFY(engine.deleteChar(empty).edits.isEmpty());
    QCOMPARE(fmt(engine.deleteChar(empty).cursors), QStringLiteral("0:0"));
    QCOMPARE(fmt(engine.moveLeft(empty, false, false).cursors), QStringLiteral("0:0"));
    QCOMPARE(fmt(engine.moveRight(empty, false, true).cursors), QStringLiteral("0:0"));
    QCOMPARE(fmt(engine.moveUp(empty, false).cursors), QStringLiteral("0:0"));
    QCOMPARE(fmt(engine.moveDown(empty, true).cursors), QStringLiteral("0:0"));
    QCOMPARE(fmt(engine.moveHome(empty, false).cursors), QStringLiteral("0:0"));
    QCOMPARE(fmt(engine.moveEnd(empty, true).cursors), QStringLiteral("0:0"));
    QVERIFY(!engine.selectNextOccurrence(empty));
    QCOMPARE(engine.selectAllOccurrences(empty), 1); // 无目标无变化
    engine.endOfLines(empty);
    QCOMPARE(fmt(engine.cursors()), QStringLiteral("0:0"));
    QVERIFY(!engine.addCursorAbove(empty));
    QVERIFY(!engine.addCursorBelow(empty));
    QCOMPARE(fmt(MultiCursorEngine::columnCursors(empty, 0, 0)), QStringLiteral("0:0"));

    McEditBatch insertEmpty = engine.insertText(QStringLiteral("a"));
    QCOMPARE(applyEdits(empty, insertEmpty.edits), QStringLiteral("a"));
    QCOMPARE(fmt(insertEmpty.cursors), QStringLiteral("1:1"));
    McEditBatch nlEmpty = engine.insertNewline();
    QCOMPARE(applyEdits(empty, nlEmpty.edits), QStringLiteral("\n"));
    QCOMPARE(fmt(nlEmpty.cursors), QStringLiteral("1:1"));

    // ---- 单行无换行文档 ----
    const QString single = QStringLiteral("abc");
    MultiCursorEngine one;
    one.setCursors(cs({{1, 1}}));
    QVERIFY(!one.addCursorAbove(single));
    QVERIFY(!one.addCursorBelow(single));
    QCOMPARE(fmt(MultiCursorEngine::columnCursors(single, 0, 3)), QStringLiteral("0:3"));
    QCOMPARE(fmt(one.moveUp(single, false).cursors), QStringLiteral("1:1"));
    QCOMPARE(fmt(one.moveDown(single, false).cursors), QStringLiteral("1:1"));
    QCOMPARE(fmt(one.moveEnd(single, false).cursors), QStringLiteral("3:3"));
    QCOMPARE(fmt(one.moveHome(single, false).cursors), QStringLiteral("0:0"));
    QVERIFY(one.selectNextOccurrence(single)); // 选中整词 "abc"
    QCOMPARE(fmt(one.cursors()), QStringLiteral("0:3"));
    QVERIFY(!one.selectNextOccurrence(single)); // 无更多出现
    MultiCursorEngine oneEnd;
    oneEnd.setCursors(cs({{3, 3}}));
    QVERIFY(oneEnd.deleteChar(single).edits.isEmpty()); // 文末 Delete 无操作
    MultiCursorEngine oneStart;
    oneStart.setCursors(cs({{0, 0}}));
    QVERIFY(oneStart.backspace(single).edits.isEmpty()); // 文首 Backspace 无操作
    MultiCursorEngine oneSel;
    oneSel.setCursors(cs({{0, 3}}));
    QCOMPARE(oneSel.selectedTexts(single), QStringList{single}); // 单行选区=整行

    // ---- 越界光标防御：位置超长时钳制到文档末，不越界 ----
    MultiCursorEngine far;
    far.setCursors(cs({{99, 99}}));
    McEditBatch farBack = far.backspace(single);
    QCOMPARE(applyEdits(single, farBack.edits), QStringLiteral("ab")); // 钳制文末删 'c'
    QCOMPARE(fmt(farBack.cursors), QStringLiteral("2:2"));
    MultiCursorEngine far2;
    far2.setCursors(cs({{99, 99}}));
    QVERIFY(far2.deleteChar(single).edits.isEmpty()); // 钳制文末：无操作
    QCOMPARE(fmt(far2.moveEnd(single, false).cursors), QStringLiteral("3:3"));
    QCOMPARE(fmt(far2.moveHome(single, false).cursors), QStringLiteral("0:0"));
}

// TC-023：Ctrl+D 命中与现有选区严格重叠即跳过（边界相接不算重叠，允许追加）；
// 混合光标状态（末位为空光标）回退取最后一个有选区光标为目标
void TestMultiCursorEngine::tc023_selectNextOccurrence_overlapSkip()
{
    // ---- 部分重叠：文档 "aaaa" 手动选 [1,3)，出现 [0,2)/[1,3)/[2,4)
    // 均与 [1,3) 严格重叠 → 全部跳过，光标集不变 ----
    MultiCursorEngine partial;
    partial.setCursors(cs({{1, 3}}));
    QVERIFY(!partial.selectNextOccurrence(QStringLiteral("aaaa")));
    QCOMPARE(fmt(partial.cursors()), QStringLiteral("1:3"));

    // 后续输入只替换原选区：不再出现重叠编辑在装配时“后到先弃”、
    // 输入静默丢弃成 "Xaa" 且光标映射畸变的问题
    //（"aaaa" 的 [1,3) 两个字符替换为 "X" → "aXa"）
    McEditBatch batch = partial.insertText(QStringLiteral("X"));
    verifyBatchInvariants(batch);
    QCOMPARE(applyEdits(QStringLiteral("aaaa"), batch.edits), QStringLiteral("aXa"));
    QCOMPARE(fmt(batch.cursors), QStringLiteral("2:2"));

    // ---- 边界相接不算重叠：[0,2) 与现有 [2,4) 仅共享端点 2，
    // 不共享字符 → 允许追加（VS Code 语义）----
    MultiCursorEngine touching;
    touching.setCursors(cs({{2, 4}}));
    QVERIFY(touching.selectNextOccurrence(QStringLiteral("aaaa")));
    QCOMPARE(fmt(touching.cursors()), QStringLiteral("0:2 2:4"));

    // ---- 相邻出现（连续重复词）："foofoo" 选 [0,3) 后 Ctrl+D
    // 应追加紧邻的 [3,6) ----
    MultiCursorEngine adjacent;
    adjacent.setCursors(cs({{0, 3}}));
    QVERIFY(adjacent.selectNextOccurrence(QStringLiteral("foofoo")));
    QCOMPARE(fmt(adjacent.cursors()), QStringLiteral("0:3 3:6"));

    // ---- 完全不相交的出现仍正常追加 ----
    MultiCursorEngine disjoint;
    disjoint.setCursors(cs({{3, 5}}));
    QVERIFY(disjoint.selectNextOccurrence(QStringLiteral("aa aa aa")));
    QCOMPARE(fmt(disjoint.cursors()), QStringLiteral("3:5 6:8"));

    // ---- 混合光标状态：末位空光标回退取最后一个有选区光标为目标 ----
    // 光标 [0,2)（选区 "aa"）+ [7,7]（空）→ 以 "aa" 为目标追加 [3,5)
    MultiCursorEngine mixed;
    mixed.setCursors(cs({{0, 2}, {7, 7}}));
    QVERIFY(mixed.selectNextOccurrence(QStringLiteral("aa aa aa")));
    QCOMPARE(fmt(mixed.cursors()), QStringLiteral("0:2 3:5 7:7"));
}

// TC-V001（tc024）：视觉列选路径的逐行区间对更新 updateColumnSelectPairs——
// 仅列选状态中生效、转光标后按 position 升序去重、不改变列选状态机；
// finishColumnSelect 定格后不再生效（产物即普通多选区，可正常编辑）
void TestMultiCursorEngine::tc024_columnSelectPairsUpdate()
{
    const QString text = QStringLiteral("aaaa\nbbbb\ncccc");

    // ---- 未进入列选状态：无操作（光标保持、不置列选标志）----
    MultiCursorEngine idle;
    idle.setCursors(cs({{0, 0}}));
    idle.updateColumnSelectPairs({{1, 3}, {6, 8}});
    QVERIFY(!idle.isColumnSelecting());
    QCOMPARE(fmt(idle.cursors()), QStringLiteral("0:0"));

    // ---- 列选中：乱序输入按 position 排序；同对重复去重；空区间（s==e）
    // 与相邻区间（前对 end==后对 start）保留为独立光标；列选状态不变 ----
    MultiCursorEngine engine;
    engine.startColumnSelect(text, 1, 1); // 锚 = 游标 = 1（进入状态机）
    QVERIFY(engine.isColumnSelecting());
    engine.updateColumnSelectPairs(
                {{6, 8}, {1, 3}, {6, 8}, {3, 5}, {11, 11}});
    QVERIFY(engine.isColumnSelecting()); // 区间对更新不改变状态机
    QCOMPARE(fmt(engine.cursors()), QStringLiteral("1:3 3:5 6:8 11:11"));

    // ---- 结束：定格为普通多选区，此后区间对更新不再生效 ----
    engine.finishColumnSelect();
    QVERIFY(!engine.isColumnSelecting());
    engine.updateColumnSelectPairs({{0, 15}});
    QCOMPARE(fmt(engine.cursors()), QStringLiteral("1:3 3:5 6:8 11:11"));

    // 定格产物可正常编辑：每选区（含空区间插入点）替换为 "="。
    // [11,11) 为行 3 第 2 字符前的空区间（插入点），在 "cccc" 前插出 "c=ccc"
    McEditBatch batch = engine.insertText(QStringLiteral("="));
    verifyBatchInvariants(batch);
    QCOMPARE(applyEdits(text, batch.edits), QStringLiteral("a==b=b\nc=ccc"));
    // 各选区替换后光标均落在其 "=" 之后：前序编辑累计平移 −2/−3 字符
    QCOMPARE(fmt(batch.cursors), QStringLiteral("2:2 3:3 5:5 9:9"));
}

int runMultiCursorEngineTests(int argc, char *argv[])
{
    TestMultiCursorEngine t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_multicursorengine.moc"
