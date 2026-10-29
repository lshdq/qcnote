#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include "document/document.h"
#include "document/documentlistmodel.h"
#include "document/documentmanager.h"
#include "format/documentformat.h"
#include "format/plaintextformat.h"
#include "storage/localstorage.h"

// 测试用格式扩展点：仅匹配 *.tst（验证注册的具体格式优先于兜底格式生效）
class FakeFormat : public DocumentFormat
{
    Q_OBJECT

public:
    QString id() const override { return QStringLiteral("fake"); }
    QString displayName() const override { return QStringLiteral("测试格式"); }
    QStringList nameFilters() const override
    {
        return { QStringLiteral("测试 (*.tst)") };
    }
};

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
    void openUrlsSkipsEmptyUrl();
    void formatForUrlFallsBackToPlainText();
    void formatForUrlPrefersRegisteredFormatOverFallback();
    void storageForUrlMatchesScheme();
    void saveRequiresUrl();
    void saveClearsModifiedAndWritesDisk();
    void saveAsUpdatesUrlAndDisplayName();
    void saveAsOverwritesExistingFile();
    void saveAsLeavesOriginalFileUntouched();
    void saveAsToOpenedUrlReloadsOtherDocument();
    void saveAsToInvalidPathFailsAndKeepsContent();
    void closeDocumentActivatesNeighbor();
    void closingInactiveDocumentKeepsActive();
    void hasUnsavedChangesToggles();
    void nameFiltersAggregatesRegisteredFormats();
    void documentsModelEmitsInsertOnNewDocument();
    void documentsModelRemovesOnlyClosedRow();
    void documentsModelExposesDocumentRole();

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

// 空 URL 防御：openUrls({空 URL}) 跳过处理——不误激活未命名文档、不发 openFailed
void TestDocumentManager::openUrlsSkipsEmptyUrl()
{
    DocumentManager mgr;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("real.txt"));
    writeRawBytes(path, QByteArrayLiteral("real"));

    mgr.openUrls({ QUrl::fromLocalFile(path) }); // 打开真实文件
    mgr.newDocument();                            // 未命名文档（url 为空）
    Document *fileDoc = mgr.documents().at(0);
    Document *untitled = mgr.documents().at(1);
    mgr.setActiveDocument(fileDoc); // 活动文档切回文件文档

    QSignalSpy failSpy(&mgr, &DocumentManager::openFailed);
    mgr.openUrls({ QUrl() }); // 空 URL：跳过

    QCOMPARE(failSpy.count(), 0);                       // 不报错
    QCOMPARE(mgr.documents().size(), 2);                // 不新建
    QCOMPARE(mgr.activeDocument(), fileDoc);            // 不误激活未命名文档
    QCOMPARE(mgr.documentForUrl(QUrl()), nullptr);      // 空 URL 检索不到任何文档
    QVERIFY(untitled != nullptr);
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

// 注册的具体格式优先于兜底格式：*.tst 命中 FakeFormat（扩展点契约），未匹配扩展仍兜底 plaintext
void TestDocumentManager::formatForUrlPrefersRegisteredFormatOverFallback()
{
    DocumentManager mgr;
    FakeFormat fake;
    mgr.registerFormat(&fake);

    DocumentFormat *tstFormat =
        mgr.formatForUrl(QUrl::fromLocalFile(QStringLiteral("C:/notes/data.tst")));
    QVERIFY(tstFormat != nullptr);
    QCOMPARE(tstFormat->id(), QStringLiteral("fake"));
    QVERIFY(qobject_cast<FakeFormat *>(tstFormat) != nullptr);

    // 未匹配的扩展名仍兜底为 plaintext（兜底格式最后生效）
    DocumentFormat *mdFormat =
        mgr.formatForUrl(QUrl::fromLocalFile(QStringLiteral("C:/notes/readme.md")));
    QVERIFY(mdFormat != nullptr);
    QCOMPARE(mdFormat->id(), QStringLiteral("plaintext"));
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

// 另存为后原文件不被修改：打开 A → 修改 → 另存为 B，A 磁盘内容保持原样（TC-005 预期）
void TestDocumentManager::saveAsLeavesOriginalFileUntouched()
{
    DocumentManager mgr;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString original = dir.filePath(QStringLiteral("original.txt"));
    writeRawBytes(original, QByteArrayLiteral("ORIGINAL"));

    mgr.openUrls({ QUrl::fromLocalFile(original) });
    Document *doc = mgr.activeDocument();
    doc->setContent(QStringLiteral("CHANGED"));

    const QUrl target = QUrl::fromLocalFile(dir.filePath(QStringLiteral("copy.txt")));
    QCOMPARE(mgr.saveAs(doc, target), true);
    QCOMPARE(readRawBytes(target.toLocalFile()), QByteArrayLiteral("CHANGED"));
    QCOMPARE(readRawBytes(original), QByteArrayLiteral("ORIGINAL")); // 原文件不动
}

// 另存为到其他已打开文档的 URL：写盘覆盖后重载对方文档，防止其旧内存内容后续保存时静默覆盖新内容
void TestDocumentManager::saveAsToOpenedUrlReloadsOtherDocument()
{
    DocumentManager mgr;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pathA = dir.filePath(QStringLiteral("a.txt"));
    const QString pathB = dir.filePath(QStringLiteral("b.txt"));
    writeRawBytes(pathA, QByteArrayLiteral("A-original\r\nsecond")); // A 原文件为 CRLF
    writeRawBytes(pathB, QByteArrayLiteral("B-original"));

    mgr.openUrls({ QUrl::fromLocalFile(pathA), QUrl::fromLocalFile(pathB) });
    Document *docA = mgr.documents().at(0);
    Document *docB = mgr.documents().at(1);
    QCOMPARE(docA->lineEnding(), Document::LineEnding::Crlf);
    QCOMPARE(docB->content(), QStringLiteral("B-original"));

    docA->setContent(QStringLiteral("A-new\ncontent")); // 修改 A（置脏）
    const QUrl target = QUrl::fromLocalFile(pathB);
    QCOMPARE(mgr.saveAs(docA, target), true);

    // B 被重载：内容等于 A 的新内容（内存 LF 归一化），干净，换行风格按磁盘新内容记录
    QCOMPARE(docB->content(), QStringLiteral("A-new\ncontent"));
    QCOMPARE(docB->modified(), false);
    QCOMPARE(docB->lineEnding(), Document::LineEnding::Crlf); // A 的风格写回，重载按磁盘记录
    // 磁盘确已写入新内容（CRLF 按记录风格写回）
    QCOMPARE(readRawBytes(pathB), QByteArrayLiteral("A-new\r\ncontent"));
    // A 另存成功：url 更新、干净
    QCOMPARE(docA->url(), target);
    QCOMPARE(docA->modified(), false);
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

// B1 增量模型：newDocument 触发 rowsInserted（只插入对应行）、count 属性同步、at/indexOf 一致
void TestDocumentManager::documentsModelEmitsInsertOnNewDocument()
{
    DocumentManager mgr;
    DocumentListModel *model = mgr.documentsModel();
    QCOMPARE(model->count(), 0);

    QSignalSpy insertedSpy(model, &QAbstractItemModel::rowsInserted);
    QSignalSpy removedSpy(model, &QAbstractItemModel::rowsRemoved);
    QSignalSpy countSpy(model, &DocumentListModel::countChanged); // QML 绑定依赖该 NOTIFY

    mgr.newDocument();
    QCOMPARE(insertedSpy.count(), 1);              // 只一次插入通知
    QCOMPARE(insertedSpy.at(0).at(1).toInt(), 0);  // first = 0
    QCOMPARE(insertedSpy.at(0).at(2).toInt(), 0);  // last = 0
    QCOMPARE(removedSpy.count(), 0);
    QCOMPARE(countSpy.count(), 1);
    QCOMPARE(model->count(), 1);
    QCOMPARE(model->property("count").toInt(), 1); // Q_PROPERTY 与实际行数同步
    QCOMPARE(model->rowCount(), 1);
    QCOMPARE(model->at(0), mgr.activeDocument());
    QCOMPARE(model->indexOf(mgr.activeDocument()), 0);
    QCOMPARE(model->indexOf(nullptr), -1);         // 空指针不入列表

    mgr.newDocument();
    QCOMPARE(insertedSpy.count(), 2);
    QCOMPARE(insertedSpy.at(1).at(1).toInt(), 1);  // 第二次插入到第 1 行
    QCOMPARE(insertedSpy.at(1).at(2).toInt(), 1);
    QCOMPARE(countSpy.count(), 2);
    QCOMPARE(model->count(), 2);
    QCOMPARE(model->at(1), mgr.activeDocument());
    QCOMPARE(model->indexOf(model->at(0)), 0);
    QCOMPARE(model->indexOf(model->at(1)), 1);
    QCOMPARE(mgr.documents().size(), 2);           // C++ 列表与模型同步
}

// B1 增量模型核心场景：打开 A、B → 关 A → count=1 且 B 仍在 index 0（只移除对应行）
void TestDocumentManager::documentsModelRemovesOnlyClosedRow()
{
    DocumentManager mgr;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pathA = dir.filePath(QStringLiteral("a.txt"));
    const QString pathB = dir.filePath(QStringLiteral("b.txt"));
    writeRawBytes(pathA, QByteArrayLiteral("A"));
    writeRawBytes(pathB, QByteArrayLiteral("B"));

    QSignalSpy insertedSpy(mgr.documentsModel(), &QAbstractItemModel::rowsInserted);
    mgr.openUrls({ QUrl::fromLocalFile(pathA), QUrl::fromLocalFile(pathB) });
    QCOMPARE(insertedSpy.count(), 2); // A、B 各一次行插入
    QCOMPARE(mgr.documentsModel()->count(), 2);
    Document *docA = mgr.documents().at(0);
    Document *docB = mgr.documents().at(1);
    QCOMPARE(mgr.documentsModel()->at(0), docA);
    QCOMPARE(mgr.documentsModel()->at(1), docB);
    QCOMPARE(mgr.documentsModel()->indexOf(docA), 0);
    QCOMPARE(mgr.documentsModel()->indexOf(docB), 1);

    QSignalSpy removedSpy(mgr.documentsModel(), &QAbstractItemModel::rowsRemoved);
    mgr.closeDocument(docA); // 关 A
    QCOMPARE(removedSpy.count(), 1);               // 只一次移除通知
    QCOMPARE(removedSpy.at(0).at(1).toInt(), 0);   // 移除的是第 0 行（A）
    QCOMPARE(removedSpy.at(0).at(2).toInt(), 0);
    QCOMPARE(mgr.documentsModel()->count(), 1);    // count=1
    QCOMPARE(mgr.documentsModel()->at(0), docB);   // B 仍在 index 0
    QCOMPARE(mgr.documentsModel()->indexOf(docB), 0);
    QCOMPARE(mgr.documents().size(), 1);           // C++ 列表同步
    QCOMPARE(mgr.documents().constFirst(), docB);

    mgr.closeDocument(docB); // 全部关闭：count 归零
    QCOMPARE(removedSpy.count(), 2);
    QCOMPARE(mgr.documentsModel()->count(), 0);
    QCOMPARE(mgr.documentsModel()->at(0), nullptr); // 越界防御
}

// B1 增量模型：document 角色返回行对应 Document*（供 QML delegate 的 required property 注入）
void TestDocumentManager::documentsModelExposesDocumentRole()
{
    DocumentManager mgr;
    DocumentListModel *model = mgr.documentsModel();
    mgr.newDocument();
    mgr.newDocument();
    Document *first = mgr.documents().at(0);
    Document *second = mgr.documents().at(1);

    QCOMPARE(model->roleNames().value(DocumentListModel::DocumentRole),
             QByteArrayLiteral("document"));
    QCOMPARE(model->index(0, 0).data(DocumentListModel::DocumentRole).value<Document *>(), first);
    QCOMPARE(model->index(1, 0).data(DocumentListModel::DocumentRole).value<Document *>(), second);
    // 无效行与未实现角色返回无效值；at 越界（负数/超界）返回 nullptr 不崩溃
    QVERIFY(!model->index(2, 0).isValid());
    QVERIFY(!model->index(0, 0).data(Qt::DisplayRole).isValid());
    QCOMPARE(model->at(-1), nullptr);
    QCOMPARE(model->at(2), nullptr);
}

// 模型行信号：newDocument 只触发一次 rowsInserted 且 count 同步（不触发 rowsRemoved）
// 模型增量维护与 at/indexOf 一致性已由上方 B1 用例（documentsModelEmitsInsertOnNewDocument、
// documentsModelRemovesOnlyClosedRow、documentsModelExposesDocumentRole）完整覆盖。

int runDocumentManagerTests(int argc, char *argv[])
{
    TestDocumentManager t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_documentmanager.moc"
