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

int runDocumentTests(int argc, char *argv[])
{
    TestDocument t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_document.moc"
