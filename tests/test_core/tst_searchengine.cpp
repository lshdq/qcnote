#include <QTest>
#include <QTextCursor>
#include <QTextDocument>

#include "editor/searchengine.h"

#include <utility>

using qcnote::SearchEdit;
using qcnote::SearchEngine;
using qcnote::SearchMatch;
using qcnote::SearchOptions;
using qcnote::SearchResult;

// ===== 测试辅助 =====
namespace {

// 快捷构造选项
SearchOptions opts(bool caseSensitive, bool wholeWord, bool useRegex)
{
    SearchOptions o;
    o.caseSensitive = caseSensitive;
    o.wholeWord = wholeWord;
    o.useRegex = useRegex;
    return o;
}

// 编译并查找一步完成
SearchResult findAllWith(const QString &pattern, const SearchOptions &options, const QString &text)
{
    SearchEngine engine;
    engine.setPattern(pattern, options);
    return engine.findAll(text);
}

// 匹配列表转为可读字符串（[start,end) 空格分隔），便于 QCOMPARE 输出差异
QString fmt(const QList<SearchMatch> &matches)
{
    QStringList parts;
    for (const SearchMatch &m : matches)
        parts << QStringLiteral("[%1,%2)").arg(m.start).arg(m.end);
    return parts.join(QLatin1Char(' '));
}

QString fmt(const SearchResult &result)
{
    return fmt(result.matches);
}

// 按契约从后往前应用编辑（edits 已按 start 降序，原坐标保持有效）
QString applyEdits(QString text, const QList<SearchEdit> &edits)
{
    for (const SearchEdit &e : edits)
        text.replace(e.start, e.end - e.start, e.text);
    return text;
}

// 与 SearchController::applyEdits 同款契约：单 editBlock + 从后往前应用到
// QTextDocument（「替换」一步/「全部替换」整体 = 单次撤销）
void applyToDocument(QTextDocument &doc, const QList<SearchEdit> &edits)
{
    QTextCursor cur(&doc);
    cur.beginEditBlock();
    for (const SearchEdit &e : edits) {
        cur.setPosition(e.start);
        cur.setPosition(e.end, QTextCursor::KeepAnchor);
        cur.insertText(e.text);
    }
    cur.endEditBlock();
}

// 校验编辑批次不变量：按 start 严格降序且互不重叠、区间非空
void verifyEditsInvariants(const QList<SearchEdit> &edits)
{
    for (int i = 0; i + 1 < edits.size(); ++i)
        QVERIFY2(edits.at(i).start >= edits.at(i + 1).end,
                 "edits 必须按 start 降序且互不重叠（从后往前应用契约）");
    for (const SearchEdit &e : edits)
        QVERIFY2(e.end > e.start, "不应产生零长度编辑区间");
}

} // namespace

// 正则搜索与替换 ST1：引擎层 TC-001 至 TC-017 + 控制器层 TC-101 至 TC-103。
//
// 控制器层说明：Qt 6.11 起 QQuickTextDocument 的构造函数要求 QQuickTextEdit
// 作为父项（QQuickTextEdit 公共头未随 Qt 发布，项目规约禁止私有头），控制台
// 测试无法构造 QQuickTextDocument，SearchController 无法脱离 QML 场景实例化。
// 故按 tst_multicursorengine.cpp 对 MultiCursorController 的既有测法（其
// tc021 撤销原子性即“引擎批次 + 裸 QTextDocument 手工应用”），把控制器行为
// 全部下沉为引擎纯函数（nextMatchIndexFrom/clampedIndex 等），TC-101~103 以
// 引擎等效测试覆盖：编辑应用契约（单 editBlock 从后往前）、替换后定位下一
// 个匹配、外部内容变化后重算并钳制索引。控制器本身的 QML 属性接线由后续
// QML 集成任务在 test_editor 场景验证。
class TestSearchEngine : public QObject
{
    Q_OBJECT

private slots:
    void tc001_plainFind_basic();
    void tc002_plainFind_caseToggle();
    void tc003_plainFind_wholeWord();
    void tc004_plainFind_literalEscaping();
    void tc005_regexFind_basicAndCapture();
    void tc006_regexFind_invalidPattern();
    void tc007_zeroLength_skipped();
    void tc008_zeroLength_allSkipped();
    void tc009_wrapNavigation();
    void tc010_replacement_dollarGroupRef();
    void tc011_replacement_multiDigitAndNonexistent();
    void tc012_replaceAll_multiMatchCoordinates();
    void tc013_replacement_literalBackslashSequences();
    void tc014_casePlusRegexCombined();
    void tc015_chineseText();
    void tc016_emptyPatternOrEmptyDocument();
    void tc017_matchCountCap();
    void tc101_replaceAllSingleUndo();
    void tc102_replaceCurrentThenNext();
    void tc103_recomputeAfterContentChange();
};

// TC-001：纯文本查找基本：默认选项（不区分大小写/全字关/正则关）返回全部
// 出现的精确区间（0 基、含头不含尾）
void TestSearchEngine::tc001_plainFind_basic()
{
    const SearchResult r = findAllWith(QStringLiteral("foo"), SearchOptions{},
                                       QStringLiteral("foo bar foo baz"));
    QCOMPARE(fmt(r), QStringLiteral("[0,3) [8,11)"));
    QVERIFY(!r.overflowed);
    // captured[0] 为整体匹配文本
    QCOMPARE(r.matches.first().captured.at(0), QStringLiteral("foo"));
}

// TC-002：大小写开关：区分时仅精确大小写匹配，不区分时全部匹配
void TestSearchEngine::tc002_plainFind_caseToggle()
{
    const QString text = QStringLiteral("Foo foo FOO");
    QCOMPARE(fmt(findAllWith(QStringLiteral("foo"), opts(false, false, false), text)),
             QStringLiteral("[0,3) [4,7) [8,11)"));
    QCOMPARE(fmt(findAllWith(QStringLiteral("foo"), opts(true, false, false), text)),
             QStringLiteral("[4,7)"));
}

// TC-003：全字匹配：仅边界（非词字符或串端）独立的 foo 命中；foobar 不命中。
// foo.bar / foo! 中的 foo 两侧为 '.'/'!'（非词字符）→ 命中；查找串首尾不是
// 词字符时不加 \b（"++" 全字等价于关闭）
void TestSearchEngine::tc003_plainFind_wholeWord()
{
    const QString text = QStringLiteral("foo foobar foo.bar foo!");
    // foo@0（后跟空格）、foobar@4（后跟 b，词字符 → 不命中）、
    // foo@11（后跟 '.'）、foo@19（后跟 '!'）
    QCOMPARE(fmt(findAllWith(QStringLiteral("foo"), opts(false, true, false), text)),
             QStringLiteral("[0,3) [11,14) [19,22)"));
    // 全字关：全部 4 处
    QCOMPARE(fmt(findAllWith(QStringLiteral("foo"), opts(false, false, false), text)),
             QStringLiteral("[0,3) [4,7) [11,14) [19,22)"));
    // 查找串首尾非词字符：不加 \b，全字开关不影响结果
    QCOMPARE(fmt(findAllWith(QStringLiteral("++"), opts(false, true, false),
                             QStringLiteral("a++ b++"))),
             QStringLiteral("[1,3) [5,7)"));
}

// TC-004：特殊字符字面化：正则关时 . + ( [ \ 等元字符全部按字面字符匹配
void TestSearchEngine::tc004_plainFind_literalEscaping()
{
    const QString text = QStringLiteral("a.c abc a+c (x) [y] a\\d");
    QCOMPARE(fmt(findAllWith(QStringLiteral("a.c"), SearchOptions{}, text)),
             QStringLiteral("[0,3)")); // "abc" 不被 "a.c" 命中（点为字面）
    QCOMPARE(fmt(findAllWith(QStringLiteral("a+c"), SearchOptions{}, text)),
             QStringLiteral("[8,11)"));
    QCOMPARE(fmt(findAllWith(QStringLiteral("(x)"), SearchOptions{}, text)),
             QStringLiteral("[12,15)"));
    QCOMPARE(fmt(findAllWith(QStringLiteral("[y]"), SearchOptions{}, text)),
             QStringLiteral("[16,19)"));
    QCOMPARE(fmt(findAllWith(QStringLiteral("a\\d"), SearchOptions{}, text)),
             QStringLiteral("[20,23)"));
    QCOMPARE(fmt(findAllWith(QStringLiteral("\\"), SearchOptions{}, text)),
             QStringLiteral("[21,22)")); // 单个反斜杠也按字面匹配
}

// TC-005：正则查找基本与捕获：\d+ 命中两段数字区间
void TestSearchEngine::tc005_regexFind_basicAndCapture()
{
    const SearchResult r = findAllWith(QStringLiteral("\\d+"), opts(false, false, true),
                                       QStringLiteral("foo123bar456"));
    QCOMPARE(fmt(r), QStringLiteral("[3,6) [9,12)"));
    QCOMPARE(r.matches.at(0).captured.at(0), QStringLiteral("123"));
    QCOMPARE(r.matches.at(1).captured.at(0), QStringLiteral("456"));
}

// TC-006：无效模式：返回错误信息、不崩溃、匹配列表为空
void TestSearchEngine::tc006_regexFind_invalidPattern()
{
    const QStringList invalid{
        QStringLiteral("[unclosed"),
        QStringLiteral("a("),
        QStringLiteral("*x"),
    };
    for (const QString &pattern : invalid) {
        SearchEngine engine;
        engine.setPattern(pattern, opts(false, false, true));
        QVERIFY2(!engine.isValid(), qPrintable(QStringLiteral("应判定无效：%1").arg(pattern)));
        QVERIFY2(!engine.errorMessage().isEmpty(),
                 qPrintable(QStringLiteral("应带错误信息：%1").arg(pattern)));
        const SearchResult r = engine.findAll(QStringLiteral("abc"));
        QVERIFY(r.matches.isEmpty());
        QVERIFY(!r.overflowed);
    }
    // 纯文本模式下任意“看起来像坏正则”的串经字面化恒合法
    SearchEngine plain;
    plain.setPattern(QStringLiteral("[unclosed"), SearchOptions{});
    QVERIFY(plain.isValid());
    QVERIFY(plain.errorMessage().isEmpty());
}

// TC-007：零长度匹配跳过：a* 在 "abc" 仅命中首个 a（长度 1），无零长度区间、无死循环
void TestSearchEngine::tc007_zeroLength_skipped()
{
    const SearchResult r = findAllWith(QStringLiteral("a*"), opts(false, false, true),
                                       QStringLiteral("abc"));
    QCOMPARE(fmt(r), QStringLiteral("[0,1)"));
    QCOMPARE(fmt(findAllWith(QStringLiteral("b*"), opts(false, false, true),
                             QStringLiteral("abc"))),
             QStringLiteral("[1,2)"));
}

// TC-008：全零长度模式：所有命中均为零长度 → 匹配数为 0
void TestSearchEngine::tc008_zeroLength_allSkipped()
{
    const SearchResult r = findAllWith(QStringLiteral("q*"), opts(false, false, true),
                                       QStringLiteral("xyz"));
    QVERIFY(r.matches.isEmpty());
    QVERIFY(!r.overflowed);
}

// TC-009：环绕导航：next(2)→0、prev(0)→2（模环绕）；无匹配返回 -1；
// 无当前索引时 next 从 0、prev 从最后一个开始
void TestSearchEngine::tc009_wrapNavigation()
{
    QCOMPARE(SearchEngine::nextIndex(3, 2), 0);
    QCOMPARE(SearchEngine::prevIndex(3, 0), 2);
    QCOMPARE(SearchEngine::nextIndex(3, 0), 1);
    QCOMPARE(SearchEngine::prevIndex(3, 2), 1);
    // 单匹配环绕回自身
    QCOMPARE(SearchEngine::nextIndex(1, 0), 0);
    QCOMPARE(SearchEngine::prevIndex(1, 0), 0);
    // 无匹配
    QCOMPARE(SearchEngine::nextIndex(0, 0), -1);
    QCOMPARE(SearchEngine::prevIndex(0, -1), -1);
    // 无当前索引
    QCOMPARE(SearchEngine::nextIndex(3, -1), 0);
    QCOMPARE(SearchEngine::prevIndex(3, -1), 2);
}

// TC-010：替换串 $n 捕获组引用：$2.$1 与 \2.\1 等价
void TestSearchEngine::tc010_replacement_dollarGroupRef()
{
    const SearchResult r = findAllWith(QStringLiteral("(\\w+)@(\\w+)"), opts(false, false, true),
                                       QStringLiteral("user@host"));
    QCOMPARE(fmt(r), QStringLiteral("[0,9)"));
    const SearchMatch &m = r.matches.first();
    QCOMPARE(m.captured.size(), 3);
    QCOMPARE(m.captured.at(0), QStringLiteral("user@host"));
    QCOMPARE(m.captured.at(1), QStringLiteral("user"));
    QCOMPARE(m.captured.at(2), QStringLiteral("host"));
    QCOMPARE(SearchEngine::expandReplacement(m.captured, QStringLiteral("$2.$1")),
             QStringLiteral("host.user"));
    QCOMPARE(SearchEngine::expandReplacement(m.captured, QStringLiteral("\\2.\\1")),
             QStringLiteral("host.user")); // 两种语法等价
}

// TC-011：多位数组号与不存在组：$10 取第 10 组（不足时空串）；不存在组输出
// 空串；$$ 输出字面 $；$x 输出字面 $x
void TestSearchEngine::tc011_replacement_multiDigitAndNonexistent()
{
    // 10 个捕获组：captured = [整体, "1", "2", ..., "9", "x"]
    const SearchResult ten = findAllWith(
            QStringLiteral("(1)(2)(3)(4)(5)(6)(7)(8)(9)(x)"), opts(false, false, true),
            QStringLiteral("123456789x"));
    QCOMPARE(ten.matches.size(), 1);
    const QStringList captured10 = ten.matches.first().captured;
    QCOMPARE(captured10.size(), 11);
    QCOMPARE(SearchEngine::expandReplacement(captured10, QStringLiteral("$10")),
             QStringLiteral("x")); // $10 = 第 10 组，不是“第 1 组 + 字面 0”
    QCOMPARE(SearchEngine::expandReplacement(captured10, QStringLiteral("$9")),
             QStringLiteral("9"));

    // 仅 1 个组：captured = [整体, "user"]
    const SearchResult one = findAllWith(QStringLiteral("(\\w+)"), opts(false, false, true),
                                         QStringLiteral("user"));
    const QStringList captured1 = one.matches.first().captured;
    QCOMPARE(captured1.size(), 2);
    QCOMPARE(SearchEngine::expandReplacement(captured1, QStringLiteral("$10")),
             QString()); // 第 10 组不存在 → 空串
    QCOMPARE(SearchEngine::expandReplacement(captured1, QStringLiteral("$2")),
             QString());
    QCOMPARE(SearchEngine::expandReplacement(captured1, QStringLiteral("$$")),
             QStringLiteral("$"));
    QCOMPARE(SearchEngine::expandReplacement(captured1, QStringLiteral("$x")),
             QStringLiteral("$x")); // $ 后非数字：字面 $ 与该字符
    QCOMPARE(SearchEngine::expandReplacement(captured1, QStringLiteral("$1")),
             QStringLiteral("user"));
    QCOMPARE(SearchEngine::expandReplacement(captured1, QStringLiteral("$0")),
             QStringLiteral("user")); // $0 = 整体匹配（顺带支持）
    // 全角数字不是组引用："$１"（U+FF11）输出字面 "$１"，而非不存在的
    // 组引用展开为空串
    QCOMPARE(SearchEngine::expandReplacement(captured1, QStringLiteral("$１x")),
             QStringLiteral("$１x"));
    // 替换串以 $ / \ 结尾：末尾字符按字面输出（回归锚点）
    QCOMPARE(SearchEngine::expandReplacement(captured1, QStringLiteral("abc$")),
             QStringLiteral("abc$"));
    QCOMPARE(SearchEngine::expandReplacement(captured1, QStringLiteral("abc\\")),
             QStringLiteral("abc\\"));
}

// TC-012：全部替换多匹配坐标正确（含中文、多行、跨行偏移）；替换产物不参与
// 本轮（a→aa 不产生新编辑）
void TestSearchEngine::tc012_replaceAll_multiMatchCoordinates()
{
    // 中文与多行
    {
        const QString text = QStringLiteral("苹果\n香蕉\n苹果");
        const SearchResult r = findAllWith(QStringLiteral("苹果"), SearchOptions{}, text);
        QCOMPARE(fmt(r), QStringLiteral("[0,2) [6,8)"));
        const QList<SearchEdit> edits = SearchEngine::computeReplaceEdits(
                    text, r.matches, QStringLiteral("橘子"));
        verifyEditsInvariants(edits);
        QCOMPARE(edits.size(), 2);
        QCOMPARE(applyEdits(text, edits), QStringLiteral("橘子\n香蕉\n橘子"));
    }
    // 替换产物不参与本轮：3 个 a 各替换为 aa → 恰好 3 条编辑、6 个 a
    {
        const QString text = QStringLiteral("aaa");
        const SearchResult r = findAllWith(QStringLiteral("a"), SearchOptions{}, text);
        QCOMPARE(fmt(r), QStringLiteral("[0,1) [1,2) [2,3)"));
        const QList<SearchEdit> edits = SearchEngine::computeReplaceEdits(
                    text, r.matches, QStringLiteral("aa"));
        verifyEditsInvariants(edits);
        QCOMPARE(edits.size(), 3);
        QCOMPARE(applyEdits(text, edits), QStringLiteral("aaaaaa"));
    }
    // 行内多处 + 前段偏移不受后段替换影响（正则带组引用）
    {
        const QString text = QStringLiteral("k1=1 k2=22 k3=333");
        const SearchResult r = findAllWith(QStringLiteral("k(\\d+)=(\\d+)"),
                                           opts(false, false, true), text);
        QCOMPARE(fmt(r), QStringLiteral("[0,4) [5,10) [11,17)"));
        const QList<SearchEdit> edits = SearchEngine::computeReplaceEdits(
                    text, r.matches, QStringLiteral("K[$1]=$2"));
        verifyEditsInvariants(edits);
        QCOMPARE(applyEdits(text, edits), QStringLiteral("K[1]=1 K[2]=22 K[3]=333"));
    }
}

// TC-013：替换串含反斜杠序列：不做 \n/\t 转义解释，按字面字符输出；
// 真实换行字符本身原样通过
void TestSearchEngine::tc013_replacement_literalBackslashSequences()
{
    const SearchResult r = findAllWith(QStringLiteral("(\\w+)"), opts(false, false, true),
                                       QStringLiteral("ab"));
    const QStringList captured = r.matches.first().captured;
    QCOMPARE(SearchEngine::expandReplacement(captured, QStringLiteral("$1\\n")),
             QStringLiteral("ab\\n")); // “\n” 是反斜杠+字母 n 两个字面字符
    QCOMPARE(SearchEngine::expandReplacement(captured, QStringLiteral("\\t")),
             QStringLiteral("\\t"));
    QCOMPARE(SearchEngine::expandReplacement(captured, QStringLiteral("$1\n")),
             QStringLiteral("ab\n")); // 替换串里的真实换行字符原样输出
    // 纯文本模式（无捕获组）：\1 引用不存在的组 → 空串，反斜杠本身可字面输出
    const SearchResult plain = findAllWith(QStringLiteral("ab"), SearchOptions{},
                                           QStringLiteral("ab"));
    const QStringList plainCaptured = plain.matches.first().captured;
    QCOMPARE(SearchEngine::expandReplacement(plainCaptured, QStringLiteral("\\1")),
             QString());
    QCOMPARE(SearchEngine::expandReplacement(plainCaptured, QStringLiteral("a\\b")),
             QStringLiteral("a\\b"));
}

// TC-014：大小写 + 正则组合：[a-z]+ 在 "ABC abc" 区分时仅 abc，组合选项正确叠加
void TestSearchEngine::tc014_casePlusRegexCombined()
{
    const QString text = QStringLiteral("ABC abc");
    QCOMPARE(fmt(findAllWith(QStringLiteral("[a-z]+"), opts(true, false, true), text)),
             QStringLiteral("[4,7)"));
    QCOMPARE(fmt(findAllWith(QStringLiteral("[a-z]+"), opts(false, false, true), text)),
             QStringLiteral("[0,3) [4,7)"));
}

// TC-015：中文文本：纯文本与正则分别查找，区间按 Unicode 码位（UTF-16 单元）
// 计数正确，无字节偏移错位。正则用 PCRE2 原生码位区间写法 \x{4e00}-\x{9fa5}
//（等价于测试用例文档中的 \u4e00-\u9fa5；PCRE2 默认不识别 \uXXXX 转义）
void TestSearchEngine::tc015_chineseText()
{
    const QString text = QStringLiteral("你好 world 世界");
    QCOMPARE(fmt(findAllWith(QStringLiteral("世界"), SearchOptions{}, text)),
             QStringLiteral("[9,11)"));
    QCOMPARE(fmt(findAllWith(QStringLiteral("[\\x{4e00}-\\x{9fa5}]+"), opts(false, false, true),
                             text)),
             QStringLiteral("[0,2) [9,11)"));
}

// TC-016：空查找串 / 空文档：匹配数为 0，不报错
void TestSearchEngine::tc016_emptyPatternOrEmptyDocument()
{
    {
        SearchEngine engine;
        engine.setPattern(QString(), SearchOptions{});
        QVERIFY(engine.isValid());          // 空查找串不是错误
        QVERIFY(engine.errorMessage().isEmpty());
        QVERIFY(engine.findAll(QStringLiteral("abc")).matches.isEmpty());
    }
    {
        const SearchResult r = findAllWith(QStringLiteral("a"), SearchOptions{}, QString());
        QVERIFY(r.matches.isEmpty());
        QVERIFY(!r.overflowed);
    }
}

// TC-017：匹配数上限：>10000 匹配时计算在 10000 处停止、溢出标志为真；
// 恰好 10000 不误报
void TestSearchEngine::tc017_matchCountCap()
{
    {
        const QString text(10001, QLatin1Char('a'));
        const SearchResult r = findAllWith(QStringLiteral("a"), SearchOptions{}, text);
        QCOMPARE(r.matches.size(), SearchEngine::kMaxMatchCount);
        QVERIFY(r.overflowed);
    }
    {
        const QString text(SearchEngine::kMaxMatchCount, QLatin1Char('a'));
        const SearchResult r = findAllWith(QStringLiteral("a"), SearchOptions{}, text);
        QCOMPARE(r.matches.size(), SearchEngine::kMaxMatchCount);
        QVERIFY(!r.overflowed); // 恰好 10000：不溢出
    }
}

// TC-101：全部替换单次撤销（控制器 applyEdits 同款契约：引擎批次 + 裸
// QTextDocument 单 editBlock 从后往前应用）：一次 undo 完整回滚
void TestSearchEngine::tc101_replaceAllSingleUndo()
{
    const QString original = QStringLiteral("foo bar foo baz");
    SearchEngine engine;
    engine.setPattern(QStringLiteral("foo"), SearchOptions{});
    const SearchResult r = engine.findAll(original);
    QCOMPARE(r.matches.size(), 2);

    const QList<SearchEdit> edits = SearchEngine::computeReplaceEdits(
                original, r.matches, QStringLiteral("X"));
    verifyEditsInvariants(edits);
    QCOMPARE(edits.size(), 2);

    QTextDocument doc;
    doc.setPlainText(original);
    applyToDocument(doc, edits);
    QCOMPARE(doc.toPlainText(), QStringLiteral("X bar X baz"));

    QVERIFY(doc.isUndoAvailable());
    doc.undo(); // 一次 undo 完整回滚整批（全部替换整体单次撤销）
    QCOMPARE(doc.toPlainText(), original);

    // 中文多行同样整体回滚
    const QString zhOriginal = QStringLiteral("苹果\n香蕉\n苹果");
    SearchEngine zhEngine;
    zhEngine.setPattern(QStringLiteral("苹果"), SearchOptions{});
    const QList<SearchEdit> zhEdits = SearchEngine::computeReplaceEdits(
                zhOriginal, zhEngine.findAll(zhOriginal).matches, QStringLiteral("橘子"));
    QTextDocument zhDoc;
    zhDoc.setPlainText(zhOriginal);
    applyToDocument(zhDoc, zhEdits);
    QCOMPARE(zhDoc.toPlainText(), QStringLiteral("橘子\n香蕉\n橘子"));
    zhDoc.undo();
    QCOMPARE(zhDoc.toPlainText(), zhOriginal);
}

// TC-102：替换当前后跳下一个（控制器 replaceCurrent 的引擎等效序列）：
// 第 i 处被替换 → 匹配重算 → 从“替换文本之后”坐标起定位下一个匹配（环绕）
void TestSearchEngine::tc102_replaceCurrentThenNext()
{
    const QString original = QStringLiteral("foo a foo b foo");
    // 匹配 [0,3) [6,9) [12,15)：当前索引 1（模拟一次 findNext 后）
    SearchEngine engine;
    engine.setPattern(QStringLiteral("foo"), SearchOptions{});
    const SearchResult r = engine.findAll(original);
    QCOMPARE(fmt(r), QStringLiteral("[0,3) [6,9) [12,15)"));

    const int currentIndex = 1;
    const SearchMatch match = r.matches.at(currentIndex);
    SearchEdit edit;
    edit.start = match.start;
    edit.end = match.end;
    edit.text = SearchEngine::expandReplacement(match.captured, QStringLiteral("QUX"));
    const int caret = edit.start + edit.text.length(); // 替换文本之后

    // 单条编辑一步一次撤销
    QTextDocument doc;
    doc.setPlainText(original);
    applyToDocument(doc, QList<SearchEdit>{edit});
    QCOMPARE(doc.toPlainText(), QStringLiteral("foo a QUX b foo"));
    doc.undo();
    QCOMPARE(doc.toPlainText(), original);

    // 控制器序列：重算 + nextMatchIndexFrom(caret)
    const SearchResult after = engine.findAll(QStringLiteral("foo a QUX b foo"));
    QCOMPARE(fmt(after), QStringLiteral("[0,3) [12,15)"));
    const int next = SearchEngine::nextMatchIndexFrom(after.matches, caret);
    QCOMPARE(next, 1); // 从 9 起首个 start>=9 的匹配是 [12,15) → 下标 1
    QCOMPARE(after.matches.at(next).start, 12);

    // 环绕：替换最后一个匹配后，后方无匹配 → 环绕到第一个
    const SearchMatch last = r.matches.at(2);
    SearchEdit lastEdit{last.start, last.end, QStringLiteral("QUX")};
    const QString afterLastText = applyEdits(original, QList<SearchEdit>{lastEdit});
    QCOMPARE(afterLastText, QStringLiteral("foo a foo b QUX"));
    const SearchResult afterLast = engine.findAll(afterLastText);
    QCOMPARE(fmt(afterLast), QStringLiteral("[0,3) [6,9)"));
    QCOMPARE(SearchEngine::nextMatchIndexFrom(afterLast.matches, lastEdit.start + lastEdit.text.length()),
             0); // 15 之后无匹配 → 环绕到 0

    // 无匹配时 -1
    QCOMPARE(SearchEngine::nextMatchIndexFrom({}, 0), -1);
    // 无当前定位坐标（fromPos=0）从头开始：返回首个匹配下标 0
    //（replaceCurrent 语义中“光标在文档头”时定位第一个匹配的分支）
    QCOMPARE(SearchEngine::nextMatchIndexFrom(r.matches, 0), 0);
    QCOMPARE(SearchEngine::nextMatchIndexFrom(after.matches, 0), 0);
}

// TC-103：内容变更后重算（控制器 onDocumentContentsChanged 的引擎等效）：
// 匹配列表按新内容重算，当前索引钳制合法；原无当前匹配保持无
void TestSearchEngine::tc103_recomputeAfterContentChange()
{
    // clampedIndex 纯函数语义
    QCOMPARE(SearchEngine::clampedIndex(-1, 2), -1); // 原无当前：不凭空制造
    QCOMPARE(SearchEngine::clampedIndex(2, 2), 1);   // 匹配变少：钳到最后合法下标
    QCOMPARE(SearchEngine::clampedIndex(5, 2), 1);
    QCOMPARE(SearchEngine::clampedIndex(0, 0), -1);  // 无匹配
    QCOMPARE(SearchEngine::clampedIndex(1, 3), 1);   // 仍合法：不变

    // 完整序列：3 个匹配、当前索引 2（模拟两次 findNext），外部重设为 2 个匹配
    SearchEngine engine;
    engine.setPattern(QStringLiteral("foo"), SearchOptions{});
    const SearchResult before = engine.findAll(QStringLiteral("foo foo foo"));
    QCOMPARE(before.matches.size(), 3);
    int currentIndex = 2;

    QTextDocument doc; // 内容以文档为唯一事实源（控制器读 doc.toPlainText()）
    doc.setPlainText(QStringLiteral("foo foo foo"));
    doc.setPlainText(QStringLiteral("foo foo")); // 模拟外部整体重置
    const SearchResult after = engine.findAll(doc.toPlainText());
    QCOMPARE(fmt(after), QStringLiteral("[0,3) [4,7)"));
    currentIndex = SearchEngine::clampedIndex(currentIndex, after.matches.size());
    QCOMPARE(currentIndex, 1); // 钳制到合法范围
    QCOMPARE(after.matches.at(currentIndex).start, 4); // 当前匹配随新列表第 2 个
}

int runSearchTests(int argc, char *argv[])
{
    TestSearchEngine t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_searchengine.moc"
