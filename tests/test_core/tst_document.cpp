#include <QCryptographicHash>
#include <QSet>
#include <QSignalSpy>
#include <QTest>
#include <QUuid>
#include <QUrl>

#include "document/document.h"

// TC-015：Document 状态行为
class TestDocument : public QObject
{
    Q_OBJECT

private slots:
    void defaultState();
    void uidIsUniqueAcrossDocuments();
    void uidIsStableAcrossReads();
    void uidMatchesUuidFormat();
    void savedContentHashTracksBaseline();
    void setContentMarksModified();
    void sameContentDoesNotChangeModified();
    void revertToBaselineClearsModified();
    void markSavedUpdatesBaseline();
    void applyLoadedStateResetsBaseline();
    void modifiedChangedEmitsOnlyOnToggle();
    void newDocumentRevertToEmptyClearsModified();
    void displayNameFollowsUrl();
    void filePathFollowsUrl();
    void lineEndingDefaultAndSet();
    void cursorPositionDefaultAndSet();
    void sameCursorPositionDoesNotEmitSignal();
    void applyLoadedStateKeepsCursorPosition();
    void encodingDefaultAndSet();
    void sameEncodingDoesNotEmitSignal();
    void hasBomDefaultFalseAndSetByLoadedState();
    void applyLoadedStateRefreshesEncodingAndClearsModified();
};

void TestDocument::defaultState()
{
    Document doc;
    QCOMPARE(doc.url(), QUrl());
    QCOMPARE(doc.content(), QString());
    QCOMPARE(doc.modified(), false);
    QCOMPARE(doc.displayName(), QStringLiteral("未命名"));
    QCOMPARE(doc.filePath(), QString());
    QCOMPARE(doc.lineEnding(), Document::LineEnding::Lf);
    QCOMPARE(doc.encoding(), Document::Encoding::Utf8); // 新建文档默认 UTF-8
    QCOMPARE(doc.hasBom(), false);                      // 默认无 BOM
}

// uid 唯一性：多个文档的 uid 互不相同（草稿缓存条目主键，DEC-002）
void TestDocument::uidIsUniqueAcrossDocuments()
{
    QSet<QString> uids;
    for (int i = 0; i < 8; ++i) {
        Document doc;
        uids.insert(doc.uid());
    }
    QCOMPARE(uids.size(), 8);
}

// uid 恒定性：同一文档多次读取一致；编辑/改 url/markSaved 均不改变
void TestDocument::uidIsStableAcrossReads()
{
    Document doc;
    const QString first = doc.uid();
    doc.setContent(QStringLiteral("编辑不改变 uid"));
    doc.setUrl(QUrl::fromLocalFile(QStringLiteral("C:/tmp/anything.txt")));
    doc.markSaved();
    QCOMPARE(doc.uid(), first);
}

// uid 格式：无大括号的规范 UUID 字符串，QUuid::fromString 可解析回非空值
void TestDocument::uidMatchesUuidFormat()
{
    Document doc;
    QVERIFY(!doc.uid().startsWith(QLatin1Char('{')));
    QVERIFY(!doc.uid().endsWith(QLatin1Char('}')));
    QVERIFY(!QUuid::fromString(doc.uid()).isNull());
}

// savedContentHash 基线语义：与手工 SHA-256(基线 UTF-8) 一致；修改内容后
// 哈希不变（仍是原基线指纹）；markSaved 后更新为新基线哈希
void TestDocument::savedContentHashTracksBaseline()
{
    const auto sha256Hex = [](const QString &text) {
        return QString::fromLatin1(
                QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
    };

    Document doc;
    doc.applyLoadedState(QUrl::fromLocalFile(QStringLiteral("C:/tmp/hash.txt")),
                         QStringLiteral("abc"), Document::LineEnding::Lf,
                         Document::Encoding::Utf8, false);
    QCOMPARE(doc.savedContentHash(), sha256Hex(QStringLiteral("abc"))); // 与手工计算一致

    doc.setContent(QStringLiteral("abcX"));
    QCOMPARE(doc.savedContentHash(), sha256Hex(QStringLiteral("abc"))); // 修改不改基线

    doc.markSaved();
    QCOMPARE(doc.savedContentHash(), sha256Hex(QStringLiteral("abcX"))); // 新基线
}

// 不同内容写入置脏
void TestDocument::setContentMarksModified()
{
    Document doc;
    doc.setContent(QStringLiteral("hello"));
    QCOMPARE(doc.content(), QStringLiteral("hello"));
    QCOMPARE(doc.modified(), true);
}

// 同值再写：modified 保持不变（不置脏也不清除）。
// 基线比较语义下：置脏态同值再写仍脏（内容仍在基线之外）；干净态（基线已随
// markSaved 对齐当前内容）同值再写早退，仍干净。
void TestDocument::sameContentDoesNotChangeModified()
{
    Document doc;
    QCOMPARE(doc.modified(), false);

    doc.setContent(QStringLiteral("x"));
    QCOMPARE(doc.modified(), true); // 基线为空串，"x" ≠ 基线
    doc.setContent(QStringLiteral("x")); // 同值再写，保持 true
    QCOMPARE(doc.modified(), true);

    doc.markSaved(); // 基线对齐当前内容 "x"：干净态
    QCOMPARE(doc.modified(), false);
    doc.setContent(doc.content()); // 同值再写，保持 false
    QCOMPARE(doc.modified(), false);
}

// TC-001（缺陷主用例）：编辑后撤消回写回到基线内容，modified 自动清脏；
// modifiedChanged 仅在脏↔干净翻转时发射（恰 2 次：置脏一次、清脏一次）
void TestDocument::revertToBaselineClearsModified()
{
    Document doc;
    doc.applyLoadedState(QUrl::fromLocalFile(QStringLiteral("C:/tmp/revert.txt")),
                         QStringLiteral("abc"), Document::LineEnding::Lf,
                         Document::Encoding::Utf8, false);
    QCOMPARE(doc.modified(), false);

    QSignalSpy spy(&doc, &Document::modifiedChanged);
    doc.setContent(QStringLiteral("abcX")); // 编辑置脏（干净→脏，发射）
    QCOMPARE(doc.modified(), true);
    doc.setContent(QStringLiteral("abc")); // 撤消回写回到基线（脏→干净，发射）
    QCOMPARE(doc.modified(), false);
    QCOMPARE(spy.count(), 2);
}

// TC-002：markSaved 后基线更新为保存时内容，撤回到该内容清脏、离开则置脏
void TestDocument::markSavedUpdatesBaseline()
{
    Document doc;
    doc.applyLoadedState(QUrl::fromLocalFile(QStringLiteral("C:/tmp/saved.txt")),
                         QStringLiteral("abc"), Document::LineEnding::Lf,
                         Document::Encoding::Utf8, false);
    doc.setContent(QStringLiteral("abcX")); // 置脏
    doc.markSaved();                        // 保存成功：基线 = "abcX"
    QCOMPARE(doc.modified(), false);

    doc.setContent(QStringLiteral("abc")); // 相对新基线是变化：置脏
    QCOMPARE(doc.modified(), true);
    doc.setContent(QStringLiteral("abcX")); // 撤回到保存时内容：清脏
    QCOMPARE(doc.modified(), false);
}

// TC-003：applyLoadedState 重置基线为本次加载内容（加载后必为干净态）
void TestDocument::applyLoadedStateResetsBaseline()
{
    Document doc;
    doc.applyLoadedState(QUrl::fromLocalFile(QStringLiteral("C:/tmp/reload.txt")),
                         QStringLiteral("abc"), Document::LineEnding::Lf,
                         Document::Encoding::Utf8, false);
    doc.setContent(QStringLiteral("abcX")); // 置脏
    QCOMPARE(doc.modified(), true);

    doc.applyLoadedState(QUrl::fromLocalFile(QStringLiteral("C:/tmp/reload.txt")),
                         QStringLiteral("NEW"), Document::LineEnding::Lf,
                         Document::Encoding::Utf8, false);
    QCOMPARE(doc.content(), QStringLiteral("NEW"));
    QCOMPARE(doc.modified(), false); // 基线已重置为 "NEW"

    doc.setContent(QStringLiteral("NEW")); // 同值再写：保持干净
    QCOMPARE(doc.modified(), false);
}

// TC-004：modifiedChanged 仅在脏↔干净翻转时发射，脏值之间切换不发射
void TestDocument::modifiedChangedEmitsOnlyOnToggle()
{
    Document doc;
    doc.applyLoadedState(QUrl::fromLocalFile(QStringLiteral("C:/tmp/toggle.txt")),
                         QStringLiteral("abc"), Document::LineEnding::Lf,
                         Document::Encoding::Utf8, false);
    QSignalSpy spy(&doc, &Document::modifiedChanged);

    doc.setContent(QStringLiteral("abd")); // 干净→脏：发射
    QCOMPARE(doc.modified(), true);
    doc.setContent(QStringLiteral("abx")); // 仍脏（脏值间切换）：不发射
    QCOMPARE(doc.modified(), true);
    doc.setContent(QStringLiteral("abc")); // 脏→干净：发射
    QCOMPARE(doc.modified(), false);
    QCOMPARE(spy.count(), 2);
}

// TC-005：新建空文档（基线为空串）全部撤空后清脏
void TestDocument::newDocumentRevertToEmptyClearsModified()
{
    Document doc;
    QCOMPARE(doc.modified(), false);
    doc.setContent(QStringLiteral("x"));
    QCOMPARE(doc.modified(), true);
    doc.setContent(QString()); // 撤空回到基线
    QCOMPARE(doc.modified(), false);
}

// displayName 随 url 变化：有 url 取文件名，url 清空后回到未命名
void TestDocument::displayNameFollowsUrl()
{
    Document doc(QStringLiteral("未命名-1"));
    QCOMPARE(doc.displayName(), QStringLiteral("未命名-1"));

    doc.setUrl(QUrl::fromLocalFile(QStringLiteral("C:/tmp/readme.txt")));
    QCOMPARE(doc.displayName(), QStringLiteral("readme.txt"));

    doc.setUrl(QUrl());
    QCOMPARE(doc.displayName(), QStringLiteral("未命名-1"));
}

// filePath 由 url 派生：本地 url 返回绝对路径，无 url 为空
void TestDocument::filePathFollowsUrl()
{
    Document doc;
    QCOMPARE(doc.filePath(), QString());

    doc.setUrl(QUrl::fromLocalFile(QStringLiteral("C:/tmp/readme.txt")));
    QCOMPARE(doc.filePath(), QStringLiteral("C:/tmp/readme.txt"));

    doc.setUrl(QUrl());
    QCOMPARE(doc.filePath(), QString());
}

// lineEnding 默认 Lf，可设置（供加载时记录原文件风格）
void TestDocument::lineEndingDefaultAndSet()
{
    Document doc;
    QCOMPARE(doc.lineEnding(), Document::LineEnding::Lf);
    doc.setLineEnding(Document::LineEnding::Crlf);
    QCOMPARE(doc.lineEnding(), Document::LineEnding::Crlf);
}

// cursorPosition 默认 0，可读写
void TestDocument::cursorPositionDefaultAndSet()
{
    Document doc;
    QCOMPARE(doc.cursorPosition(), 0);

    doc.setCursorPosition(42);
    QCOMPARE(doc.cursorPosition(), 42);

    doc.setCursorPosition(0);
    QCOMPARE(doc.cursorPosition(), 0);

    doc.setCursorPosition(-1); // 负值下界保护：钳为 0
    QCOMPARE(doc.cursorPosition(), 0);
}

// 同值重复设置不发射 cursorPositionChanged（防 QML 回写环）
void TestDocument::sameCursorPositionDoesNotEmitSignal()
{
    Document doc;
    doc.setCursorPosition(7);

    QSignalSpy spy(&doc, &Document::cursorPositionChanged);
    doc.setCursorPosition(7); // 同值
    QCOMPARE(spy.count(), 0);

    doc.setCursorPosition(8); // 不同值：发射一次
    QCOMPARE(spy.count(), 1);
    QCOMPARE(doc.cursorPosition(), 8);
}

// applyLoadedState 不改动 cursorPosition（另存覆盖重载/按编码重载均不动光标；
// 重载后的光标钳制由 DocumentManager 负责）
void TestDocument::applyLoadedStateKeepsCursorPosition()
{
    Document doc;
    doc.setCursorPosition(15);

    doc.applyLoadedState(QUrl::fromLocalFile(QStringLiteral("C:/tmp/x.txt")),
                         QStringLiteral("loaded"), Document::LineEnding::Crlf,
                         Document::Encoding::Gbk, false);
    QCOMPARE(doc.cursorPosition(), 15); // 光标位置保持不变
}

// TC-011：encoding 默认 Utf8，可设置（供按编码保存/会话记忆使用）
void TestDocument::encodingDefaultAndSet()
{
    Document doc;
    QCOMPARE(doc.encoding(), Document::Encoding::Utf8);

    QSignalSpy spy(&doc, &Document::encodingChanged);
    doc.setEncoding(Document::Encoding::Gbk);
    QCOMPARE(doc.encoding(), Document::Encoding::Gbk);
    QCOMPARE(spy.count(), 1);
}

// 同值 setEncoding 不发信号（与 setLineEnding 同构）
void TestDocument::sameEncodingDoesNotEmitSignal()
{
    Document doc;
    doc.setEncoding(Document::Encoding::Gbk);

    QSignalSpy spy(&doc, &Document::encodingChanged);
    doc.setEncoding(Document::Encoding::Gbk);
    QCOMPARE(spy.count(), 0);

    doc.setEncoding(Document::Encoding::Utf8);
    QCOMPARE(spy.count(), 1);
}

// TC-011：hasBom 默认 false；由 applyLoadedState 记录（磁盘事实），变化发信号
void TestDocument::hasBomDefaultFalseAndSetByLoadedState()
{
    Document doc;
    QCOMPARE(doc.hasBom(), false);

    QSignalSpy spy(&doc, &Document::hasBomChanged);
    doc.applyLoadedState(QUrl::fromLocalFile(QStringLiteral("C:/tmp/bom.txt")),
                         QStringLiteral("hi"), Document::LineEnding::Lf,
                         Document::Encoding::Utf8, true);
    QCOMPARE(doc.hasBom(), true);
    QCOMPARE(spy.count(), 1);
}

// TC-011：applyLoadedState 刷新编码/BOM 且不置脏（加载路径语义）
void TestDocument::applyLoadedStateRefreshesEncodingAndClearsModified()
{
    Document doc;
    doc.setContent(QStringLiteral("dirty")); // 置脏
    QCOMPARE(doc.modified(), true);

    doc.applyLoadedState(QUrl::fromLocalFile(QStringLiteral("C:/tmp/g.txt")),
                         QStringLiteral("disk"), Document::LineEnding::Lf,
                         Document::Encoding::Gbk, false);
    QCOMPARE(doc.encoding(), Document::Encoding::Gbk);
    QCOMPARE(doc.hasBom(), false);
    QCOMPARE(doc.content(), QStringLiteral("disk"));
    QCOMPARE(doc.modified(), false); // 不置脏（加载后干净）
}

int runDocumentTests(int argc, char *argv[])
{
    TestDocument t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_document.moc"
