#include <QSignalSpy>
#include <QTest>
#include <QUrl>

#include "document/document.h"

// TC-015：Document 状态行为
class TestDocument : public QObject
{
    Q_OBJECT

private slots:
    void defaultState();
    void setContentMarksModified();
    void sameContentDoesNotChangeModified();
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

// 不同内容写入置脏
void TestDocument::setContentMarksModified()
{
    Document doc;
    doc.setContent(QStringLiteral("hello"));
    QCOMPARE(doc.content(), QStringLiteral("hello"));
    QCOMPARE(doc.modified(), true);
}

// 同值再写：modified 保持不变（不置脏也不清除）
void TestDocument::sameContentDoesNotChangeModified()
{
    Document doc;
    QCOMPARE(doc.modified(), false);

    doc.setContent(QStringLiteral("x"));
    QCOMPARE(doc.modified(), true);
    doc.setContent(QStringLiteral("x")); // 同值再写，保持 true
    QCOMPARE(doc.modified(), true);

    doc.setModified(false);
    doc.setContent(doc.content()); // 同值再写，保持 false
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
