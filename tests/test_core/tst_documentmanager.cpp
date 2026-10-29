#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include "document/document.h"
#include "document/documentmanager.h"
#include "format/documentformat.h"
#include "format/plaintextformat.h"
#include "storage/localstorage.h"

// TC-015：DocumentManager 行为
class TestDocumentManager : public QObject
{
    Q_OBJECT

private slots:
    void newDocumentsAreNamedAndActivated();
    void openUrlsCreatesDocumentWithContent();
    void openUrlsDeduplicatesSameUrl();
    void openMissingFileEmitsOpenFailedAndContinues();
    void openUrlWithoutStorageEmitsOpenFailed();
    void formatForUrlFallsBackToPlainText();
    void storageForUrlMatchesScheme();
    void saveRequiresUrl();
    void saveClearsModifiedAndWritesDisk();
    void saveAsUpdatesUrlAndDisplayName();
    void saveAsOverwritesExistingFile();
    void saveAsToInvalidPathFailsAndKeepsContent();
    void closeDocumentActivatesNeighbor();
    void closingInactiveDocumentKeepsActive();
    void hasUnsavedChangesToggles();
    void nameFiltersAggregatesRegisteredFormats();

private:
    static void writeRawBytes(const QString &path, const QByteArray &bytes)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(file.write(bytes) == bytes.size());
        file.close();
    }

    static QByteArray readRawBytes(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return QByteArray();
        return file.readAll();
    }
};

// 连续新建：命名 未命名-1 / 未命名-2 且逐个激活
void TestDocumentManager::newDocumentsAreNamedAndActivated()
{
    DocumentManager mgr;
    QCOMPARE(mgr.documents().size(), 0);
    QCOMPARE(mgr.activeDocument(), nullptr);

    mgr.newDocument();
    QCOMPARE(mgr.documents().size(), 1);
    Document *first = mgr.documents().constFirst();
    QCOMPARE(first->displayName(), QStringLiteral("未命名-1"));
    QCOMPARE(mgr.activeDocument(), first);

    mgr.newDocument();
    QCOMPARE(mgr.documents().size(), 2);
    Document *second = mgr.documents().at(1);
    QCOMPARE(second->displayName(), QStringLiteral("未命名-2"));
    QCOMPARE(mgr.activeDocument(), second);
}

// 打开临时文件：生成文档、内容正确、干净、激活、发 documentOpened
void TestDocumentManager::openUrlsCreatesDocumentWithContent()
{
    DocumentManager mgr;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("hello.txt"));
    writeRawBytes(path, QByteArrayLiteral("hello\nworld"));

    QSignalSpy openedSpy(&mgr, &DocumentManager::documentOpened);
    mgr.openUrls({ QUrl::fromLocalFile(path) });

    QCOMPARE(mgr.documents().size(), 1);
    Document *doc = mgr.documents().constFirst();
    QCOMPARE(doc->content(), QStringLiteral("hello\nworld"));
    QCOMPARE(doc->modified(), false);
    QCOMPARE(doc->displayName(), QStringLiteral("hello.txt"));
    QCOMPARE(mgr.activeDocument(), doc);
    QCOMPARE(openedSpy.count(), 1);
}

// 重复打开同一 URL：文档数不变，激活已有文档
void TestDocumentManager::openUrlsDeduplicatesSameUrl()
{
    DocumentManager mgr;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("dup.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("dup"));

    QSignalSpy openedSpy(&mgr, &DocumentManager::documentOpened);
    mgr.openUrls({ url });
    Document *first = mgr.activeDocument();

    mgr.openUrls({ url }); // 第二次打开同一 URL
    QCOMPARE(mgr.documents().size(), 1);
    QCOMPARE(mgr.activeDocument(), first);
    QCOMPARE(openedSpy.count(), 1);
}

// 打开不存在文件：发 openFailed，不崩溃，继续处理后续 URL
void TestDocumentManager::openMissingFileEmitsOpenFailedAndContinues()
{
    DocumentManager mgr;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QUrl missing = QUrl::fromLocalFile(dir.filePath(QStringLiteral("missing.txt")));
    const QString goodPath = dir.filePath(QStringLiteral("good.txt"));
    writeRawBytes(goodPath, QByteArrayLiteral("ok"));

    QSignalSpy spy(&mgr, &DocumentManager::openFailed);
    mgr.openUrls({ missing, QUrl::fromLocalFile(goodPath) });

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toUrl(), missing);
    QVERIFY(!spy.at(0).at(1).toString().isEmpty());

    QCOMPARE(mgr.documents().size(), 1);
    QCOMPARE(mgr.documents().constFirst()->content(), QStringLiteral("ok"));
}

// 打开无可用存储的 URL（无 scheme）：发 openFailed，不新建文档（openUrls 的 storage 空分支）
void TestDocumentManager::openUrlWithoutStorageEmitsOpenFailed()
{
    DocumentManager mgr;
    QSignalSpy spy(&mgr, &DocumentManager::openFailed);
    mgr.openUrls({ QUrl(QStringLiteral("noscheme.txt")) });

    QCOMPARE(spy.count(), 1);
    QCOMPARE(mgr.documents().size(), 0);
}

// 格式匹配：未知扩展（.md）与已知扩展（.txt）均兜底为 PlainTextFormat
void TestDocumentManager::formatForUrlFallsBackToPlainText()
{
    DocumentManager mgr;
    DocumentFormat *mdFormat =
        mgr.formatForUrl(QUrl::fromLocalFile(QStringLiteral("C:/notes/readme.md")));
    QVERIFY(mdFormat != nullptr);
    QCOMPARE(mdFormat->id(), QStringLiteral("plaintext"));
    QVERIFY(qobject_cast<PlainTextFormat *>(mdFormat) != nullptr);

    DocumentFormat *txtFormat =
        mgr.formatForUrl(QUrl::fromLocalFile(QStringLiteral("C:/notes/a.txt")));
    QVERIFY(txtFormat != nullptr);
    QCOMPARE(txtFormat->id(), QStringLiteral("plaintext"));
}

// 存储分发：file:// 命中 LocalStorage，http:// 返回 nullptr
void TestDocumentManager::storageForUrlMatchesScheme()
{
    DocumentManager mgr;
    DocumentStorage *fileStorage =
        mgr.storageForUrl(QUrl(QStringLiteral("file:///C:/notes/a.txt")));
    QVERIFY(fileStorage != nullptr);
    QCOMPARE(fileStorage->scheme(), QStringLiteral("file"));
    QVERIFY(qobject_cast<LocalStorage *>(fileStorage) != nullptr);

    QCOMPARE(mgr.storageForUrl(QUrl(QStringLiteral("http://example.com/a.txt"))), nullptr);
}

// url 为空的文档：save 返回 false（UI 转另存为）
void TestDocumentManager::saveRequiresUrl()
{
    DocumentManager mgr;
    mgr.newDocument();
    Document *doc = mgr.activeDocument();
    QCOMPARE(mgr.save(doc), false);
}

// 保存：清除 modified，磁盘内容更新且按记录的风格写回
void TestDocumentManager::saveClearsModifiedAndWritesDisk()
{
    DocumentManager mgr;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("save.txt"));
    writeRawBytes(path, QByteArrayLiteral("one\r\ntwo")); // CRLF 原文件

    mgr.openUrls({ QUrl::fromLocalFile(path) });
    Document *doc = mgr.activeDocument();
    QCOMPARE(doc->lineEnding(), Document::LineEnding::Crlf);

    doc->setContent(QStringLiteral("one\ntwo\nthree"));
    QCOMPARE(doc->modified(), true);

    QCOMPARE(mgr.save(doc), true);
    QCOMPARE(doc->modified(), false);
    QCOMPARE(readRawBytes(path), QByteArrayLiteral("one\r\ntwo\r\nthree")); // 保持 CRLF
}

// 另存为：url / displayName 更新，modified 清除，文件写入新路径
void TestDocumentManager::saveAsUpdatesUrlAndDisplayName()
{
    DocumentManager mgr;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    mgr.newDocument();
    Document *doc = mgr.activeDocument();
    doc->setContent(QStringLiteral("abc"));
    QCOMPARE(doc->modified(), true);

    const QUrl target = QUrl::fromLocalFile(dir.filePath(QStringLiteral("saved.txt")));
    QCOMPARE(mgr.saveAs(doc, target), true);

    QCOMPARE(doc->url(), target);
    QCOMPARE(doc->displayName(), QStringLiteral("saved.txt"));
    QCOMPARE(doc->modified(), false);
    QCOMPARE(readRawBytes(target.toLocalFile()), QByteArrayLiteral("abc"));
}

// 另存为到已存在文件：目标旧内容被完全替换（TC-005 覆盖场景）
void TestDocumentManager::saveAsOverwritesExistingFile()
{
    DocumentManager mgr;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString target = dir.filePath(QStringLiteral("exists.txt"));
    writeRawBytes(target, QByteArrayLiteral("OLD-CONTENT"));

    mgr.newDocument();
    Document *doc = mgr.activeDocument();
    doc->setContent(QStringLiteral("NEW"));

    QCOMPARE(mgr.saveAs(doc, QUrl::fromLocalFile(target)), true);
    QCOMPARE(readRawBytes(target), QByteArrayLiteral("NEW")); // 旧内容被替换
}

// 另存为到非法路径：返回 false、发 saveFailed、内容与修改标记不丢（TC-016 核心行为）
void TestDocumentManager::saveAsToInvalidPathFailsAndKeepsContent()
{
    DocumentManager mgr;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    mgr.newDocument();
    Document *doc = mgr.activeDocument();
    doc->setContent(QStringLiteral("keep me"));

    QSignalSpy spy(&mgr, &DocumentManager::saveFailed);
    const QUrl bad = QUrl::fromLocalFile(dir.filePath(QStringLiteral("no/such/dir/x.txt")));
    QCOMPARE(mgr.saveAs(doc, bad), false);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(doc->content(), QStringLiteral("keep me")); // 内容不丢
    QCOMPARE(doc->modified(), true);                     // 仍标记未保存
}

// 关闭文档：关闭活动文档时激活邻近（优先前一个，其次后一个，无则 nullptr）
void TestDocumentManager::closeDocumentActivatesNeighbor()
{
    DocumentManager mgr;
    mgr.newDocument(); // A
    mgr.newDocument(); // B
    mgr.newDocument(); // C
    Document *a = mgr.documents().at(0);
    Document *b = mgr.documents().at(1);
    Document *c = mgr.documents().at(2);

    QSignalSpy closedSpy(&mgr, &DocumentManager::documentClosed);

    // 关闭活动文档 B（中间）：激活前一个 A
    mgr.setActiveDocument(b);
    mgr.closeDocument(b);
    QCOMPARE(mgr.documents().size(), 2);
    QCOMPARE(mgr.activeDocument(), a);
    QCOMPARE(closedSpy.count(), 1);

    // 关闭活动文档 A（最前）：无前一个 → 激活后一个 C
    mgr.setActiveDocument(a);
    mgr.closeDocument(a);
    QCOMPARE(mgr.documents().size(), 1);
    QCOMPARE(mgr.activeDocument(), c);

    // 关闭最后一个 C：无邻近 → nullptr
    mgr.setActiveDocument(c);
    mgr.closeDocument(c);
    QCOMPARE(mgr.documents().size(), 0);
    QCOMPARE(mgr.activeDocument(), nullptr);
}

// 关闭非活动文档：活动文档不变
void TestDocumentManager::closingInactiveDocumentKeepsActive()
{
    DocumentManager mgr;
    mgr.newDocument(); // A
    mgr.newDocument(); // B（激活）
    Document *a = mgr.documents().at(0);
    Document *b = mgr.documents().at(1);
    QCOMPARE(mgr.activeDocument(), b);

    mgr.closeDocument(a);
    QCOMPARE(mgr.documents().size(), 1);
    QCOMPARE(mgr.activeDocument(), b);
}

// hasUnsavedChanges 随 modified 变化
void TestDocumentManager::hasUnsavedChangesToggles()
{
    DocumentManager mgr;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QCOMPARE(mgr.hasUnsavedChanges(), false);

    mgr.newDocument();
    QCOMPARE(mgr.hasUnsavedChanges(), false); // 新文档干净

    mgr.activeDocument()->setContent(QStringLiteral("x"));
    QCOMPARE(mgr.hasUnsavedChanges(), true);

    mgr.saveAs(mgr.activeDocument(), QUrl::fromLocalFile(dir.filePath(QStringLiteral("u.txt"))));
    QCOMPARE(mgr.hasUnsavedChanges(), false);
}

// nameFilters：聚合已注册格式的过滤器（去重），供 FileDialog 使用
void TestDocumentManager::nameFiltersAggregatesRegisteredFormats()
{
    DocumentManager mgr;
    const QStringList filters = mgr.nameFilters();
    QVERIFY(filters.contains(QStringLiteral("文本文件 (*.txt)")));
    QVERIFY(filters.contains(QStringLiteral("所有文件 (*)")));
    QCOMPARE(filters.count(QStringLiteral("所有文件 (*)")), 1); // 去重
}

int runDocumentManagerTests(int argc, char *argv[])
{
    TestDocumentManager t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_documentmanager.moc"
