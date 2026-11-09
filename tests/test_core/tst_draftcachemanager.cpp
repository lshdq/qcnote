#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include "document/document.h"
#include "document/documentmanager.h"
#include "document/draftcachemanager.h"

// 草稿缓存核心行为：修改后 flush 的条目字段与目录自动创建、干净文档零写入、
// 五条清理路径（保存/另存为/放弃关闭/撤回基线/重载覆盖）、静默恢复（含代开
// 路径）、外部修改与文件缺失冲突（恢复/丢弃）、未命名条目恢复、损坏条目清除、
// 恢复不抢活动标签、挂起中关闭后 flush 的悬垂防护、restoreUntitledDraft 命名
// 规则、缓存目录不存在时恢复按空处理、便携/非便携目录定位（TC-016/TC-017）、
// 同 URL 去重（双条目静默路径/冲突路径/savedAt 平局 + 三条目多路去重混合
// 未命名条目，审查应改 #1）。
// 均为 C++ 直接 new（注入 DocumentManager 与临时缓存目录），不经 QML
// 单例工厂，不污染真实用户数据目录。

// RAII：便携目录定位构造级测试需临时占用测试进程 exe 目录（build/，与
// qcnote.exe 同目录）放置 qcnote.portable 标记并接收 drafts/ 缓存产物（与
// tst_sessionmanager 的 ExeDirPortableView 同一模式）。QVERIFY 失败仅 return
// 出测试函数，局部对象析构照常执行，保证 build 目录恢复原状；目录中已存在
// 任一目标文件（异常残留/他人产物）时不创建也不清理，调用方据此 QSKIP。
class ExeDirDraftPortableView
{
public:
    ExeDirDraftPortableView()
        : m_markerPath(QDir(QCoreApplication::applicationDirPath())
                           .filePath(QStringLiteral("qcnote.portable")))
        , m_iniPath(QDir(QCoreApplication::applicationDirPath())
                        .filePath(QStringLiteral("qcnote.ini")))
        , m_draftsPath(QDir(QCoreApplication::applicationDirPath())
                           .filePath(QStringLiteral("drafts")))
        , m_clean(!QFile::exists(m_markerPath) && !QFile::exists(m_iniPath)
                  && !QDir(m_draftsPath).exists())
    {
    }

    bool dirIsClean() const { return m_clean; }

    bool createMarker()
    {
        if (!m_clean)
            return false;
        QFile marker(m_markerPath);
        if (!marker.open(QIODevice::WriteOnly))
            return false;
        marker.close();
        return true;
    }

    QString draftsPath() const { return m_draftsPath; }

    ~ExeDirDraftPortableView()
    {
        if (!m_clean)
            return;
        QDir(m_draftsPath).removeRecursively(); // 便携分支产生的缓存条目
        QFile::remove(m_iniPath);               // 防御：本组测试不应产生会话 ini
        QFile::remove(m_markerPath);            // 本视图创建的标记
    }

private:
    QString m_markerPath;
    QString m_iniPath;
    QString m_draftsPath;
    bool m_clean;
};
class TestDraftCacheManager : public QObject
{
    Q_OBJECT

private slots:
    void flushWritesEntryWithAllFields();
    void cleanDocumentWritesNoEntry();
    void saveClearsEntry();
    void saveAsClearsEntry();
    void closeWhileModifiedClearsEntry();
    void revertToBaselineClearsEntry();
    void reloadWithEncodingClearsEntry();
    void silentRestoreAppliesUnsavedChanges();
    void restoreDraftsAutoOpensFileBackedDoc();
    void externalModificationConflictRestoreAppliesCache();
    void externalModificationConflictDiscardKeepsDisk();
    void duplicateUrlEntriesSilentRestoreKeepsNewest();
    void duplicateUrlEntriesConflictResolvesNewest();
    void duplicateUrlEntriesTieKeepsLargerUid();
    void tripleUrlEntriesDedupeKeepsNewestAmongThree();
    void missingFileConflictRestoresAsUntitled();
    void untitledEntryRestoresUntitledDocument();
    void corruptAndWrongVersionEntriesArePurged();
    void restoreDoesNotStealActiveTab();
    void restoreAutoOpenKeepsActiveTab();
    void closeWhilePendingThenFlushIsSafe();
    void restoreUntitledDraftNaming();
    void restoreWithMissingCacheDirIsNoop();
    void portableMarkerLocatesCacheNextToExe();
    void noMarkerKeepsExeDirCleanWithoutDrafts();

private:
    static void writeRawBytes(const QString &path, const QByteArray &bytes)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(file.write(bytes) == bytes.size());
        file.close();
    }

    static QString sha256Hex(const QString &text)
    {
        return QString::fromLatin1(
                QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
    }

    /// 读取缓存条目 JSON；打不开/解析失败返回空对象（调用方 QVERIFY 非空）
    static QJsonObject readEntry(const QString &cacheDir, const QString &uid)
    {
        QFile file(QDir(cacheDir).filePath(uid + QStringLiteral(".json")));
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }

    /// 直接手写一条合法草稿条目到缓存目录（同 URL 双条目用例构造磁盘状态：
    /// savedAt 显式指定以控制新旧，不受防抖时钟影响）
    static void writeDraftEntry(const QString &cacheDir, const QString &uid, const QUrl &url,
                                const QString &name, const QString &content, int cursor,
                                const QString &baselineHash, qint64 savedAt)
    {
        QJsonObject entry;
        entry.insert(QStringLiteral("version"), 1);
        entry.insert(QStringLiteral("uid"), uid);
        entry.insert(QStringLiteral("url"), url.toString());
        entry.insert(QStringLiteral("name"), name);
        entry.insert(QStringLiteral("content"), content);
        entry.insert(QStringLiteral("cursor"), cursor);
        entry.insert(QStringLiteral("lineEnding"), QStringLiteral("lf"));
        entry.insert(QStringLiteral("encoding"), QStringLiteral("utf8"));
        entry.insert(QStringLiteral("hasBom"), false);
        entry.insert(QStringLiteral("baselineHash"), baselineHash);
        entry.insert(QStringLiteral("savedAt"), savedAt);
        writeRawBytes(QDir(cacheDir).filePath(uid + QStringLiteral(".json")),
                      QJsonDocument(entry).toJson());
    }

    /// 缓存目录中的条目文件名列表（目录不存在视为空）
    static QStringList entryFiles(const QString &cacheDir)
    {
        return QDir(cacheDir).entryList({ QStringLiteral("*.json") }, QDir::Files);
    }
};

// 修改后 flush：条目存在且字段正确（version/uid/url/name/content/cursor/
// lineEnding/encoding/hasBom/baselineHash/savedAt）；缓存目录不存在时写条目前
// 自动创建。CRLF 文件覆盖 lineEnding=crlf 分支，GBK 文件覆盖 encoding=gbk 分支
void TestDraftCacheManager::flushWritesEntryWithAllFields()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts")); // 不存在：写时自动创建
    const QUrl urlCrlf = QUrl::fromLocalFile(dir.filePath(QStringLiteral("crlf.txt")));
    const QUrl urlGbk = QUrl::fromLocalFile(dir.filePath(QStringLiteral("gbk.txt")));
    writeRawBytes(urlCrlf.toLocalFile(), QByteArrayLiteral("line1\r\nline2"));   // CRLF
    writeRawBytes(urlGbk.toLocalFile(), QByteArrayLiteral("\xC4\xE3\xBA\xC3")); // GBK：你好

    DocumentManager mgr;
    DraftCacheManager cache(&mgr, cacheDir);
    mgr.openUrls({ urlCrlf, urlGbk });
    Document *docCrlf = mgr.documents().at(0);
    Document *docGbk = mgr.documents().at(1);
    QCOMPARE(docCrlf->lineEnding(), Document::LineEnding::Crlf);
    QCOMPARE(docGbk->encoding(), Document::Encoding::Gbk);
    QCOMPARE(docGbk->content(), QStringLiteral("\u4F60\u597D"));

    docCrlf->setContent(QStringLiteral("line1\nline2 edited"));
    docCrlf->setCursorPosition(7);
    docGbk->setContent(QStringLiteral("\u4F60\u597D\u4E16\u754C")); // 你好世界
    docGbk->setCursorPosition(2);
    cache.flush();

    QVERIFY(QDir(cacheDir).exists()); // 目录不存在时自动创建

    const QJsonObject entryCrlf = readEntry(cacheDir, docCrlf->uid());
    QVERIFY(!entryCrlf.isEmpty());
    QCOMPARE(entryCrlf.value(QStringLiteral("version")).toInt(), 1);
    QCOMPARE(entryCrlf.value(QStringLiteral("uid")).toString(), docCrlf->uid());
    QCOMPARE(QUrl(entryCrlf.value(QStringLiteral("url")).toString()), urlCrlf);
    QCOMPARE(entryCrlf.value(QStringLiteral("name")).toString(), QStringLiteral("crlf.txt"));
    QCOMPARE(entryCrlf.value(QStringLiteral("content")).toString(),
             QStringLiteral("line1\nline2 edited"));
    QCOMPARE(entryCrlf.value(QStringLiteral("cursor")).toInt(), 7);
    QCOMPARE(entryCrlf.value(QStringLiteral("lineEnding")).toString(), QStringLiteral("crlf"));
    QCOMPARE(entryCrlf.value(QStringLiteral("encoding")).toString(), QStringLiteral("utf8"));
    QCOMPARE(entryCrlf.value(QStringLiteral("hasBom")).toBool(), false);
    QCOMPARE(entryCrlf.value(QStringLiteral("baselineHash")).toString(),
             sha256Hex(QStringLiteral("line1\nline2"))); // 基线=磁盘加载归一化内容
    QVERIFY(entryCrlf.value(QStringLiteral("savedAt")).isDouble());

    const QJsonObject entryGbk = readEntry(cacheDir, docGbk->uid());
    QVERIFY(!entryGbk.isEmpty());
    QCOMPARE(entryGbk.value(QStringLiteral("encoding")).toString(), QStringLiteral("gbk"));
    QCOMPARE(entryGbk.value(QStringLiteral("content")).toString(),
             QStringLiteral("\u4F60\u597D\u4E16\u754C"));
    QCOMPARE(entryGbk.value(QStringLiteral("baselineHash")).toString(),
             sha256Hex(QStringLiteral("\u4F60\u597D")));
}

// 干净文档不写条目：打开后仅移动光标（modified 恒 false）→ flush 后无任何条目，
// 缓存目录也不创建（无脏内容即无落盘副作用）
void TestDraftCacheManager::cleanDocumentWritesNoEntry()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts"));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("clean.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("clean"));

    DocumentManager mgr;
    DraftCacheManager cache(&mgr, cacheDir);
    mgr.openUrls({ url });
    mgr.documents().constFirst()->setCursorPosition(3); // 光标变化不触发写
    cache.flush();

    QVERIFY(entryFiles(cacheDir).isEmpty());
    QVERIFY(!QDir(cacheDir).exists());
}

// 清理路径 1/5：save 成功 → modified 变 false → 条目删除
void TestDraftCacheManager::saveClearsEntry()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts"));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("s.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("abc"));

    DocumentManager mgr;
    DraftCacheManager cache(&mgr, cacheDir);
    mgr.openUrls({ url });
    Document *doc = mgr.documents().constFirst();
    doc->setContent(QStringLiteral("abcd"));
    cache.flush();
    QCOMPARE(entryFiles(cacheDir).size(), 1); // 先落条目

    QVERIFY(mgr.save(doc));
    QCOMPARE(entryFiles(cacheDir).size(), 0); // 保存后条目删除
}

// 清理路径 2/5：saveAs 成功 → 条目删除（条目以 uid 为主键，与 url 变化无关）
void TestDraftCacheManager::saveAsClearsEntry()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts"));

    DocumentManager mgr;
    DraftCacheManager cache(&mgr, cacheDir);
    mgr.newDocument();
    Document *doc = mgr.documents().constFirst();
    doc->setContent(QStringLiteral("draft"));
    cache.flush();
    QCOMPARE(entryFiles(cacheDir).size(), 1);

    const QUrl target = QUrl::fromLocalFile(dir.filePath(QStringLiteral("saved.txt")));
    QVERIFY(mgr.saveAs(doc, target));
    QCOMPARE(entryFiles(cacheDir).size(), 0);
}

// 清理路径 3/5：closeDocument 时仍 modified（“不保存”放弃路径）→ 条目删除，
// 放弃即真丢，不恢复
void TestDraftCacheManager::closeWhileModifiedClearsEntry()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts"));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("c.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("abc"));

    DocumentManager mgr;
    DraftCacheManager cache(&mgr, cacheDir);
    mgr.openUrls({ url });
    Document *doc = mgr.documents().constFirst();
    doc->setContent(QStringLiteral("abcd"));
    cache.flush();
    QCOMPARE(entryFiles(cacheDir).size(), 1);

    mgr.closeDocument(doc); // 关闭确认框选“不保存”
    QCOMPARE(entryFiles(cacheDir).size(), 0);
}

// 清理路径 4/5：setContent 撤回到基线（modified 变 false）→ 条目立即删除（无 flush）
void TestDraftCacheManager::revertToBaselineClearsEntry()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts"));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("r.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("abc"));

    DocumentManager mgr;
    DraftCacheManager cache(&mgr, cacheDir);
    mgr.openUrls({ url });
    Document *doc = mgr.documents().constFirst();
    doc->setContent(QStringLiteral("abcd"));
    cache.flush();
    QCOMPARE(entryFiles(cacheDir).size(), 1);

    doc->setContent(QStringLiteral("abc")); // 撤回到基线
    QCOMPARE(entryFiles(cacheDir).size(), 0); // contentChanged 处理中已删除
    QCOMPARE(doc->modified(), false);
}

// 清理路径 5/5：reloadWithEncoding 覆盖（基线重置、modified 清脏）→ 条目删除
void TestDraftCacheManager::reloadWithEncodingClearsEntry()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts"));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("re.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("\xC4\xE3\xBA\xC3")); // GBK：你好

    DocumentManager mgr;
    DraftCacheManager cache(&mgr, cacheDir);
    mgr.openUrls({ url }); // 自动识别 GBK
    Document *doc = mgr.documents().constFirst();
    QCOMPARE(doc->encoding(), Document::Encoding::Gbk);
    doc->setContent(QStringLiteral("\u4F60\u597D\u4E16\u754C"));
    cache.flush();
    QCOMPARE(entryFiles(cacheDir).size(), 1);

    mgr.reloadWithEncoding(doc, Document::Encoding::Utf8); // 磁盘内容覆盖内存
    QCOMPARE(doc->modified(), false);
    QCOMPARE(entryFiles(cacheDir).size(), 0);
}

// 静默恢复：条目 baselineHash 与磁盘一致 → openUrls 后 restoreDrafts 静默把
// 缓存内容应用为未保存修改（modified=true）、光标恢复、旧条目删除
// （新 uid 的续写条目在防抖挂起中，尚未写出）
void TestDraftCacheManager::silentRestoreAppliesUnsavedChanges()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts"));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("silent.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("base"));

    {
        DocumentManager mgr;
        DraftCacheManager cache(&mgr, cacheDir);
        mgr.openUrls({ url });
        Document *doc = mgr.documents().constFirst();
        doc->setContent(QStringLiteral("base-dirty"));
        doc->setCursorPosition(5);
        cache.flush();
        QCOMPARE(entryFiles(cacheDir).size(), 1);
    }

    {
        DocumentManager mgr;
        DraftCacheManager cache(&mgr, cacheDir);
        QSignalSpy conflictSpy(&cache, &DraftCacheManager::conflictsFound);
        mgr.openUrls({ url }); // 模拟会话恢复已打开该文档
        cache.restoreDrafts();

        QCOMPARE(conflictSpy.count(), 0); // 磁盘未被外部修改：无冲突
        Document *doc = mgr.documents().constFirst();
        QCOMPARE(doc->content(), QStringLiteral("base-dirty")); // 未保存修改恢复
        QCOMPARE(doc->modified(), true);
        QCOMPARE(doc->cursorPosition(), 5);       // 光标恢复
        QCOMPARE(entryFiles(cacheDir).size(), 0); // 旧条目已删
    }
}

// 会话未打开该文件时 restoreDrafts 自动代开（本地文件存在且存储可用），
// 随后静默应用缓存内容
void TestDraftCacheManager::restoreDraftsAutoOpensFileBackedDoc()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts"));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("auto.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("auto-base"));

    {
        DocumentManager mgr;
        DraftCacheManager cache(&mgr, cacheDir);
        mgr.openUrls({ url });
        Document *doc = mgr.documents().constFirst();
        doc->setContent(QStringLiteral("auto-dirty"));
        doc->setCursorPosition(4);
        cache.flush();
        QCOMPARE(entryFiles(cacheDir).size(), 1);
    }

    {
        DocumentManager mgr;
        DraftCacheManager cache(&mgr, cacheDir);
        QSignalSpy conflictSpy(&cache, &DraftCacheManager::conflictsFound);
        cache.restoreDrafts(); // 不 openUrls：恢复路径自动代开

        QCOMPARE(conflictSpy.count(), 0);
        QCOMPARE(mgr.documents().size(), 1);
        Document *doc = mgr.documents().constFirst();
        QCOMPARE(doc->url(), url);
        QCOMPARE(doc->content(), QStringLiteral("auto-dirty"));
        QCOMPARE(doc->modified(), true);
        QCOMPARE(doc->cursorPosition(), 4);
        QCOMPARE(entryFiles(cacheDir).size(), 0);
    }
}

// 外部修改冲突（恢复）：磁盘字节变化 → 基线指纹不符 → conflictsFound、内容
// 未被覆盖；resolveConflicts(true) 应用缓存内容为未保存修改、条目删除
void TestDraftCacheManager::externalModificationConflictRestoreAppliesCache()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts"));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("conflict.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("v1"));

    {
        DocumentManager mgr;
        DraftCacheManager cache(&mgr, cacheDir);
        mgr.openUrls({ url });
        Document *doc = mgr.documents().constFirst();
        doc->setContent(QStringLiteral("v1-dirty"));
        doc->setCursorPosition(3);
        cache.flush();
        QCOMPARE(entryFiles(cacheDir).size(), 1);
    }

    // 外部程序修改磁盘内容（条目基线指纹不再与磁盘相符）
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("v1-external"));

    {
        DocumentManager mgr;
        DraftCacheManager cache(&mgr, cacheDir);
        QSignalSpy conflictSpy(&cache, &DraftCacheManager::conflictsFound);
        mgr.openUrls({ url });
        cache.restoreDrafts();

        QCOMPARE(conflictSpy.count(), 1);
        const QVariantList conflicts = conflictSpy.takeFirst().at(0).toList();
        QCOMPARE(conflicts.size(), 1);
        const QVariantMap conflict = conflicts.constFirst().toMap();
        QCOMPARE(conflict.value(QStringLiteral("url")).toUrl(), url);
        QCOMPARE(conflict.value(QStringLiteral("name")).toString(),
                 QStringLiteral("conflict.txt"));
        QCOMPARE(conflict.value(QStringLiteral("missing")).toBool(), false);

        Document *doc = mgr.documents().constFirst();
        QCOMPARE(doc->content(), QStringLiteral("v1-external")); // 磁盘内容未被覆盖
        QCOMPARE(doc->modified(), false);

        cache.resolveConflicts(true); // 用户选「恢复」
        QCOMPARE(doc->content(), QStringLiteral("v1-dirty")); // 缓存应用为未保存修改
        QCOMPARE(doc->modified(), true);
        QCOMPARE(entryFiles(cacheDir).size(), 0); // 条目删除
    }
}

// 外部修改冲突（丢弃）：resolveConflicts(false) 保持磁盘内容、条目删除
void TestDraftCacheManager::externalModificationConflictDiscardKeepsDisk()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts"));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("discard.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("v1"));

    {
        DocumentManager mgr;
        DraftCacheManager cache(&mgr, cacheDir);
        mgr.openUrls({ url });
        Document *doc = mgr.documents().constFirst();
        doc->setContent(QStringLiteral("v1-dirty"));
        cache.flush();
        QCOMPARE(entryFiles(cacheDir).size(), 1);
    }

    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("v1-external"));

    {
        DocumentManager mgr;
        DraftCacheManager cache(&mgr, cacheDir);
        QSignalSpy conflictSpy(&cache, &DraftCacheManager::conflictsFound);
        mgr.openUrls({ url });
        cache.restoreDrafts();
        QCOMPARE(conflictSpy.count(), 1);

        cache.resolveConflicts(false); // 用户选「丢弃」
        Document *doc = mgr.documents().constFirst();
        QCOMPARE(doc->content(), QStringLiteral("v1-external")); // 使用磁盘内容
        QCOMPARE(doc->modified(), false);
        QCOMPARE(entryFiles(cacheDir).size(), 0); // 条目删除
    }
}

// 同 URL 双条目（savedAt 新旧不同）+ 磁盘未外部修改 → 静默恢复（审查应改
// #1）：较新条目 uid 字典序较小、先被枚举，若不去重，较旧条目会在其后被
// 应用并覆盖较新内容（旧草稿最终胜出）。去重须发生在分类处理之前——较旧
// 条目文件被立即删除，仅静默应用较新条目
void TestDraftCacheManager::duplicateUrlEntriesSilentRestoreKeepsNewest()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts"));
    QVERIFY(QDir().mkpath(cacheDir));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("dup-silent.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("dup-base"));

    const QString uidNewer = QStringLiteral("aaaa1111-0000-0000-0000-000000000001");
    const QString uidOlder = QStringLiteral("zzzz2222-0000-0000-0000-000000000002");
    const QString baseline = sha256Hex(QStringLiteral("dup-base")); // 基线均与磁盘相符
    // 较新条目（savedAt=200，uid 字典序小 → 先枚举）
    writeDraftEntry(cacheDir, uidNewer, url, QStringLiteral("dup-silent.txt"),
                    QStringLiteral("newer-draft"), 4, baseline, 200);
    // 较旧条目（savedAt=100，uid 字典序大 → 后枚举）
    writeDraftEntry(cacheDir, uidOlder, url, QStringLiteral("dup-silent.txt"),
                    QStringLiteral("older-draft"), 5, baseline, 100);
    QCOMPARE(entryFiles(cacheDir).size(), 2); // 前置：双条目确实在盘

    DocumentManager mgr;
    DraftCacheManager cache(&mgr, cacheDir);
    QSignalSpy conflictSpy(&cache, &DraftCacheManager::conflictsFound);
    mgr.openUrls({ url }); // 模拟会话恢复已打开该文档
    cache.restoreDrafts();

    QCOMPARE(conflictSpy.count(), 0); // 磁盘未被外部修改：静默恢复，无冲突
    Document *doc = mgr.documents().constFirst();
    QCOMPARE(doc->content(), QStringLiteral("newer-draft")); // 较新条目内容被应用
    QCOMPARE(doc->modified(), true);
    QCOMPARE(doc->cursorPosition(), 4);
    QCOMPARE(entryFiles(cacheDir).size(), 0); // 较旧条目去重删除 + 较新条目应用后删除
}

// 同 URL 双条目 + baselineHash 均与磁盘不符（外部修改）→ 冲突路径（审查应改
// #1）：去重发生在分类之前，conflictsFound 对同一 url 仅 1 项；若不去重会
// 弹出 2 项冲突，且 resolveConflicts(true) 按枚举顺序逐条 setContent，较旧
// 内容最终覆盖较新内容。此处验证冲突唯一且 resolveConflicts(true) 应用的
// 是较新条目内容
void TestDraftCacheManager::duplicateUrlEntriesConflictResolvesNewest()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts"));
    QVERIFY(QDir().mkpath(cacheDir));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("dup-conflict.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("v1"));

    const QString uidNewer = QStringLiteral("bbbb1111-0000-0000-0000-000000000001");
    const QString uidOlder = QStringLiteral("yyyy2222-0000-0000-0000-000000000002");
    const QString baseline = sha256Hex(QStringLiteral("v1")); // 两条目写入时刻磁盘均为 v1
    writeDraftEntry(cacheDir, uidNewer, url, QStringLiteral("dup-conflict.txt"),
                    QStringLiteral("newer-draft"), 3, baseline, 200);
    writeDraftEntry(cacheDir, uidOlder, url, QStringLiteral("dup-conflict.txt"),
                    QStringLiteral("older-draft"), 5, baseline, 100);

    // 外部程序修改磁盘：两条目基线指纹均不再相符
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("v1-external"));

    DocumentManager mgr;
    DraftCacheManager cache(&mgr, cacheDir);
    QSignalSpy conflictSpy(&cache, &DraftCacheManager::conflictsFound);
    mgr.openUrls({ url });
    cache.restoreDrafts();

    QCOMPARE(conflictSpy.count(), 1);
    const QVariantList conflicts = conflictSpy.takeFirst().at(0).toList();
    QCOMPARE(conflicts.size(), 1); // 同一 url 去重后至多一项冲突
    const QVariantMap conflict = conflicts.constFirst().toMap();
    QCOMPARE(conflict.value(QStringLiteral("url")).toUrl(), url);
    QCOMPARE(conflict.value(QStringLiteral("missing")).toBool(), false);

    Document *doc = mgr.documents().constFirst();
    QCOMPARE(doc->content(), QStringLiteral("v1-external")); // 决策前不覆盖磁盘内容

    cache.resolveConflicts(true); // 用户选「恢复」
    QCOMPARE(doc->content(), QStringLiteral("newer-draft")); // 应用的是较新条目
    QCOMPARE(doc->modified(), true);
    QCOMPARE(entryFiles(cacheDir).size(), 0); // 较旧条目去重删除 + 较新条目决策后删除
}

// 同 URL 双条目 savedAt 完全相同 → 确定性平局规则：按 uid 字典序保留较大者，
// 不依赖目录枚举顺序（uid 较小者先被枚举，最终内容仍为 uid 较大者）
void TestDraftCacheManager::duplicateUrlEntriesTieKeepsLargerUid()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts"));
    QVERIFY(QDir().mkpath(cacheDir));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("dup-tie.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("tie-base"));

    const QString uidSmaller = QStringLiteral("cccc3333-0000-0000-0000-000000000003");
    const QString uidLarger = QStringLiteral("dddd4444-0000-0000-0000-000000000004");
    const QString baseline = sha256Hex(QStringLiteral("tie-base"));
    writeDraftEntry(cacheDir, uidSmaller, url, QStringLiteral("dup-tie.txt"),
                    QStringLiteral("small-uid-draft"), 2, baseline, 500);
    writeDraftEntry(cacheDir, uidLarger, url, QStringLiteral("dup-tie.txt"),
                    QStringLiteral("large-uid-draft"), 6, baseline, 500);

    DocumentManager mgr;
    DraftCacheManager cache(&mgr, cacheDir);
    QSignalSpy conflictSpy(&cache, &DraftCacheManager::conflictsFound);
    mgr.openUrls({ url });
    cache.restoreDrafts();

    QCOMPARE(conflictSpy.count(), 0); // 基线相符：静默恢复
    Document *doc = mgr.documents().constFirst();
    QCOMPARE(doc->content(), QStringLiteral("large-uid-draft")); // uid 较大者胜出
    QCOMPARE(doc->cursorPosition(), 6);
    QCOMPARE(entryFiles(cacheDir).size(), 0); // 落选者去重删除 + 保留者应用后删除
}

// 同 URL 三条目多路去重 + 混合未命名条目（复测补充用例，审查应改 #1 独立
// 视角）：三条目 savedAt 乱序到达（100 → 300 → 200），其中第二条须替换已
// 保留的第一条（kept = candidate 分支），第三条再与替换后的保留者比较落选
// ——若去重记录的下标/对象在替换后失准，第三条（200）会与已被替换出的
// 第一条（100）比较胜出，最终错误应用中间条目内容。同时混入一个未命名
// 条目：不参与去重、不受同 URL 去重影响，独立恢复为未命名文档
void TestDraftCacheManager::tripleUrlEntriesDedupeKeepsNewestAmongThree()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts"));
    QVERIFY(QDir().mkpath(cacheDir));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("dup-triple.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("triple-base"));

    // 枚举顺序 = uid.json 文件名字典序：eee…1（最旧）→ eee…2（最新）→
    // eee…3（中间）→ uuu…9（未命名）
    const QString uidOldest = QStringLiteral("eeee1111-0000-0000-0000-000000000001");
    const QString uidNewest = QStringLiteral("eeee2222-0000-0000-0000-000000000002");
    const QString uidMiddle = QStringLiteral("eeee3333-0000-0000-0000-000000000003");
    const QString uidUntitled = QStringLiteral("uuuu9999-0000-0000-0000-000000000009");
    const QString baseline = sha256Hex(QStringLiteral("triple-base"));
    writeDraftEntry(cacheDir, uidOldest, url, QStringLiteral("dup-triple.txt"),
                    QStringLiteral("oldest-draft"), 1, baseline, 100);
    writeDraftEntry(cacheDir, uidNewest, url, QStringLiteral("dup-triple.txt"),
                    QStringLiteral("newest-draft"), 7, baseline, 300);
    writeDraftEntry(cacheDir, uidMiddle, url, QStringLiteral("dup-triple.txt"),
                    QStringLiteral("middle-draft"), 3, baseline, 200);
    // 未命名条目（savedAt 最小也不受去重影响）
    writeDraftEntry(cacheDir, uidUntitled, QUrl(), QStringLiteral("未命名-5"),
                    QStringLiteral("untitled-keep"), 2, QString(), 50);
    QCOMPARE(entryFiles(cacheDir).size(), 4); // 前置：三条 url 条目 + 一条未命名

    DocumentManager mgr;
    DraftCacheManager cache(&mgr, cacheDir);
    QSignalSpy conflictSpy(&cache, &DraftCacheManager::conflictsFound);
    cache.restoreDrafts(); // 会话未打开该文件：代开后静默应用

    QCOMPARE(conflictSpy.count(), 0); // 基线相符：无冲突
    QCOMPARE(mgr.documents().size(), 2); // url 文档（代开）+ 未命名恢复
    Document *docUrl = mgr.documents().at(0);
    QCOMPARE(docUrl->url(), url);
    QCOMPARE(docUrl->content(), QStringLiteral("newest-draft")); // 三路去重后仅最新应用
    QCOMPARE(docUrl->modified(), true);
    QCOMPARE(docUrl->cursorPosition(), 7);
    Document *docUntitled = mgr.documents().at(1);
    QCOMPARE(docUntitled->displayName(), QStringLiteral("未命名-5")); // 未命名不受去重影响
    QCOMPARE(docUntitled->content(), QStringLiteral("untitled-keep"));
    QCOMPARE(docUntitled->modified(), true);
    QCOMPARE(entryFiles(cacheDir).size(), 0); // 两个落选去重删除 + 保留者/未命名处理后删除
}

// 文件缺失冲突：条目 url 指向不存在的文件 → conflictsFound（missing=true）、
// 不自动打开；resolveConflicts(true) → 以未命名文档恢复内容（沿用记录名）
void TestDraftCacheManager::missingFileConflictRestoresAsUntitled()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts"));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("gone.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("old"));

    {
        DocumentManager mgr;
        DraftCacheManager cache(&mgr, cacheDir);
        mgr.openUrls({ url });
        Document *doc = mgr.documents().constFirst();
        doc->setContent(QStringLiteral("old-dirty"));
        doc->setCursorPosition(4);
        cache.flush();
        QCOMPARE(entryFiles(cacheDir).size(), 1);
    }

    QVERIFY(QFile::remove(url.toLocalFile())); // 文件被删除/移动

    {
        DocumentManager mgr;
        DraftCacheManager cache(&mgr, cacheDir);
        QSignalSpy conflictSpy(&cache, &DraftCacheManager::conflictsFound);
        cache.restoreDrafts();

        QCOMPARE(conflictSpy.count(), 1);
        const QVariantList conflicts = conflictSpy.takeFirst().at(0).toList();
        const QVariantMap conflict = conflicts.constFirst().toMap();
        QCOMPARE(conflict.value(QStringLiteral("url")).toUrl(), url);
        QCOMPARE(conflict.value(QStringLiteral("missing")).toBool(), true);
        QCOMPARE(mgr.documents().size(), 0); // 等用户决策，未先行恢复

        cache.resolveConflicts(true); // 用户选「恢复」：恢复为未命名文档
        QCOMPARE(mgr.documents().size(), 1);
        Document *restored = mgr.documents().constFirst();
        QCOMPARE(restored->content(), QStringLiteral("old-dirty"));
        QCOMPARE(restored->modified(), true);
        QCOMPARE(restored->displayName(), QStringLiteral("gone.txt")); // 沿用记录名
        QCOMPARE(entryFiles(cacheDir).size(), 0);
    }
}

// 未命名条目恢复：url 为空 → restoreUntitledDraft 路径、modified=true、
// 显示名沿用记录值、光标恢复、条目删除、不激活
void TestDraftCacheManager::untitledEntryRestoresUntitledDocument()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts"));

    {
        DocumentManager mgr;
        DraftCacheManager cache(&mgr, cacheDir);
        mgr.newDocument(); // 未命名-1
        Document *doc = mgr.documents().constFirst();
        doc->setContent(QStringLiteral("draft content"));
        doc->setCursorPosition(6);
        cache.flush();
        QCOMPARE(entryFiles(cacheDir).size(), 1);
    }

    {
        DocumentManager mgr;
        DraftCacheManager cache(&mgr, cacheDir);
        QSignalSpy conflictSpy(&cache, &DraftCacheManager::conflictsFound);
        cache.restoreDrafts();

        QCOMPARE(conflictSpy.count(), 0); // 未命名条目无冲突语义
        QCOMPARE(mgr.documents().size(), 1);
        Document *doc = mgr.documents().constFirst();
        QCOMPARE(doc->displayName(), QStringLiteral("未命名-1")); // 显示名沿用
        QCOMPARE(doc->content(), QStringLiteral("draft content"));
        QCOMPARE(doc->modified(), true); // 基线为空 → 必为脏
        QCOMPARE(doc->cursorPosition(), 6);
        QCOMPARE(mgr.activeDocument(), nullptr); // 不激活：不抢占活动标签
        QCOMPARE(entryFiles(cacheDir).size(), 0);
    }
}

// 损坏 JSON 与 version 不符：文件被删、无崩溃、无冲突信号；其余合法条目正常
// 恢复（cursor 超长时钳制到内容长度）
void TestDraftCacheManager::corruptAndWrongVersionEntriesArePurged()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts"));
    QVERIFY(QDir().mkpath(cacheDir));

    // 非法 JSON
    writeRawBytes(QDir(cacheDir).filePath(QStringLiteral("aaaa1111-0000-0000-0000-000000000001.json")),
                  QByteArrayLiteral("{ 这不是合法 JSON"));
    // 合法 JSON 但 version=2
    QJsonObject wrongVersion;
    wrongVersion.insert(QStringLiteral("version"), 2);
    writeRawBytes(QDir(cacheDir).filePath(QStringLiteral("aaaa2222-0000-0000-0000-000000000002.json")),
                  QJsonDocument(wrongVersion).toJson());
    // 合法未命名条目（cursor 超长：恢复时钳制到内容长度）
    QJsonObject good;
    good.insert(QStringLiteral("version"), 1);
    good.insert(QStringLiteral("uid"), QStringLiteral("aaaa3333-0000-0000-0000-000000000003"));
    good.insert(QStringLiteral("url"), QString());
    good.insert(QStringLiteral("name"), QStringLiteral("未命名-9"));
    good.insert(QStringLiteral("content"), QStringLiteral("good")); // 4 字符
    good.insert(QStringLiteral("cursor"), 999);
    good.insert(QStringLiteral("lineEnding"), QStringLiteral("lf"));
    good.insert(QStringLiteral("encoding"), QStringLiteral("utf8"));
    good.insert(QStringLiteral("hasBom"), false);
    good.insert(QStringLiteral("baselineHash"), QStringLiteral("x"));
    good.insert(QStringLiteral("savedAt"), 1);
    writeRawBytes(QDir(cacheDir).filePath(QStringLiteral("aaaa3333-0000-0000-0000-000000000003.json")),
                  QJsonDocument(good).toJson());

    DocumentManager mgr;
    DraftCacheManager cache(&mgr, cacheDir);
    QSignalSpy conflictSpy(&cache, &DraftCacheManager::conflictsFound);
    cache.restoreDrafts(); // 不崩溃

    QCOMPARE(conflictSpy.count(), 0);
    QCOMPARE(mgr.documents().size(), 1);
    Document *doc = mgr.documents().constFirst();
    QCOMPARE(doc->displayName(), QStringLiteral("未命名-9"));
    QCOMPARE(doc->content(), QStringLiteral("good"));
    QCOMPARE(doc->cursorPosition(), 4); // 999 钳制到内容长度
    QCOMPARE(doc->modified(), true);
    QCOMPARE(entryFiles(cacheDir).size(), 0); // 两个坏条目 + 已应用的合法条目全部删除
}

// 恢复不抢占活动标签：会话恢复后 active=B，草稿恢复（既有文档静默应用 +
// 未命名重建追加）不得改变 activeDocument（TC-015）
void TestDraftCacheManager::restoreDoesNotStealActiveTab()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts"));
    const QUrl urlA = QUrl::fromLocalFile(dir.filePath(QStringLiteral("a.txt")));
    const QUrl urlB = QUrl::fromLocalFile(dir.filePath(QStringLiteral("b.txt")));
    writeRawBytes(urlA.toLocalFile(), QByteArrayLiteral("a-origin"));
    writeRawBytes(urlB.toLocalFile(), QByteArrayLiteral("b-origin"));

    {
        DocumentManager mgr;
        DraftCacheManager cache(&mgr, cacheDir);
        mgr.openUrls({ urlA });
        mgr.documents().constFirst()->setContent(QStringLiteral("a-dirty"));
        cache.flush();
        mgr.newDocument(); // 未命名-1
        mgr.documents().constLast()->setContent(QStringLiteral("u-dirty"));
        cache.flush();
        QCOMPARE(entryFiles(cacheDir).size(), 2);
    }

    {
        DocumentManager mgr;
        DraftCacheManager cache(&mgr, cacheDir);
        mgr.openUrls({ urlA, urlB }); // 会话恢复：最后打开的 B 为活动标签
        Document *docB = mgr.documents().at(1);
        QCOMPARE(mgr.activeDocument(), docB);

        cache.restoreDrafts();
        QCOMPARE(mgr.activeDocument(), docB);        // 活动标签不被抢占
        QCOMPARE(mgr.documents().size(), 3);         // a、b + 恢复的未命名
        QCOMPARE(mgr.documents().at(0)->content(), QStringLiteral("a-dirty")); // 静默应用
        QCOMPARE(mgr.documents().at(2)->content(), QStringLiteral("u-dirty")); // 未命名重建
    }
}

// 恢复不抢占活动标签（代开路径）：会话恢复激活文档 A，另有文档 B 的缓存条目
// 且 B 未被打开 → restoreDrafts 代开 B（openUrls 会激活 B），恢复完成后活动
// 标签须复位为 A（TC-015）
void TestDraftCacheManager::restoreAutoOpenKeepsActiveTab()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts"));
    const QUrl urlA = QUrl::fromLocalFile(dir.filePath(QStringLiteral("active-a.txt")));
    const QUrl urlB = QUrl::fromLocalFile(dir.filePath(QStringLiteral("auto-b.txt")));
    writeRawBytes(urlA.toLocalFile(), QByteArrayLiteral("a-origin"));
    writeRawBytes(urlB.toLocalFile(), QByteArrayLiteral("b-origin"));

    {
        DocumentManager mgr;
        DraftCacheManager cache(&mgr, cacheDir);
        mgr.openUrls({ urlB });
        mgr.documents().constFirst()->setContent(QStringLiteral("b-dirty"));
        cache.flush();
        QCOMPARE(entryFiles(cacheDir).size(), 1);
    }

    {
        DocumentManager mgr;
        DraftCacheManager cache(&mgr, cacheDir);
        QSignalSpy conflictSpy(&cache, &DraftCacheManager::conflictsFound);
        mgr.openUrls({ urlA }); // 会话恢复：A 为活动标签，B 未被打开
        Document *docA = mgr.documents().constFirst();
        QCOMPARE(mgr.activeDocument(), docA);

        cache.restoreDrafts(); // 代开 B（openUrls 激活 B）后须复位为 A
        QCOMPARE(conflictSpy.count(), 0);
        QCOMPARE(mgr.activeDocument(), docA); // 活动标签被正确复位
        QCOMPARE(mgr.documents().size(), 2);
        QCOMPARE(mgr.documents().at(1)->url(), urlB);
        QCOMPARE(mgr.documents().at(1)->content(), QStringLiteral("b-dirty")); // 代开后静默应用
        QCOMPARE(entryFiles(cacheDir).size(), 0);
    }
}

// 悬垂防护：防抖挂起中 closeDocument（仍 modified → 条目删除、挂起清空）→
// flush() 不崩溃、不重建条目
void TestDraftCacheManager::closeWhilePendingThenFlushIsSafe()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts"));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("p.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("x"));

    DocumentManager mgr;
    DraftCacheManager cache(&mgr, cacheDir);
    mgr.openUrls({ url });
    Document *doc = mgr.documents().constFirst();
    doc->setContent(QStringLiteral("dirty1"));
    cache.flush();
    QCOMPARE(entryFiles(cacheDir).size(), 1);

    doc->setContent(QStringLiteral("dirty2")); // 防抖挂起（不等 1000ms 到期）
    mgr.closeDocument(doc);                    // 关闭时仍 modified → 条目删除
    QCOMPARE(entryFiles(cacheDir).size(), 0);

    cache.flush(); // 不崩溃
    QCOMPARE(entryFiles(cacheDir).size(), 0); // 也不重建
}

// restoreUntitledDraft 命名规则（DocumentManager 直测）：冲突换新名、沿用
// 非冲突记录名、记录名推进计数器（后续 newDocument 跳过占用号）、不激活、
// modified=true
void TestDraftCacheManager::restoreUntitledDraftNaming()
{
    DocumentManager mgr;
    mgr.newDocument(); // 未命名-1
    Document *first = mgr.documents().constFirst();

    Document *restored = mgr.restoreUntitledDraft(QStringLiteral("内容"), QStringLiteral("未命名-1"));
    QCOMPARE(restored->displayName(), QStringLiteral("未命名-2")); // 冲突 → 计数器新名
    QCOMPARE(restored->content(), QStringLiteral("内容"));
    QCOMPARE(restored->modified(), true);
    QCOMPARE(mgr.activeDocument(), first); // 不激活
    QCOMPARE(mgr.documents().size(), 2);

    mgr.newDocument();
    QCOMPARE(mgr.documents().constLast()->displayName(), QStringLiteral("未命名-3")); // 跳过占用号

    Document *restored2 = mgr.restoreUntitledDraft(QStringLiteral("b"), QStringLiteral("未命名-9"));
    QCOMPARE(restored2->displayName(), QStringLiteral("未命名-9")); // 非冲突沿用记录名
    mgr.newDocument();
    QCOMPARE(mgr.documents().constLast()->displayName(), QStringLiteral("未命名-10")); // 计数器已推进到 9
}

// TC-016（恢复半边）：注入的缓存目录不存在时 restoreDrafts 按空处理——
// 无文档、无冲突信号、不创建目录（首启/正常退出后无残留，启动行为与
// 现状一致，回归红线）。写入半边（写条目前自动建目录）由
// flushWritesEntryWithAllFields 覆盖。
void TestDraftCacheManager::restoreWithMissingCacheDirIsNoop()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString cacheDir = dir.filePath(QStringLiteral("drafts")); // 不存在

    DocumentManager mgr;
    DraftCacheManager cache(&mgr, cacheDir);
    QSignalSpy conflictSpy(&cache, &DraftCacheManager::conflictsFound);
    cache.restoreDrafts(); // 目录不存在：静默返回，无报错

    QCOMPARE(conflictSpy.count(), 0);
    QCOMPARE(mgr.documents().size(), 0);
    QVERIFY(!QDir(cacheDir).exists()); // 不因恢复而创建目录
}

// TC-017（便携分支）：exe 目录存在 qcnote.portable 标记 + 空注入路径 →
// 缓存条目落在 exe 旁 drafts/（与 qcnote.ini 同目录语义，复用
// SessionManager::resolvePortableIniPath 检测）。标记与产物由 RAII 视图
// 创建并清理，不污染 build 目录；test_core 已 RUN_SERIAL，便携窗口期内
// 与其他测试可执行文件不并行。
void TestDraftCacheManager::portableMarkerLocatesCacheNextToExe()
{
    ExeDirDraftPortableView exeDir;
    if (!exeDir.dirIsClean())
        QSKIP("build 目录已存在 qcnote.portable/qcnote.ini/drafts，跳过以免误动他人产物");

    QVERIFY(exeDir.createMarker());

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("portable-draft.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("p"));

    DocumentManager mgr;
    DraftCacheManager cache(&mgr, QString()); // 空注入路径：应命中便携模式
    mgr.openUrls({ url });
    Document *doc = mgr.documents().constFirst();
    doc->setContent(QStringLiteral("p-dirty"));
    cache.flush();

    QVERIFY(QDir(exeDir.draftsPath()).exists()); // drafts/ 生成于 exe 旁
    const QStringList entries = entryFiles(exeDir.draftsPath());
    QCOMPARE(entries.size(), 1); // 唯一条目即该文档
    QCOMPARE(entries.constFirst(), doc->uid() + QStringLiteral(".json"));
    QVERIFY(!readEntry(exeDir.draftsPath(), doc->uid()).isEmpty()); // 内容合法
}

// TC-017（非便携分支）：无标记 + 空注入路径 → 走 AppLocalDataLocation
// 默认目录，便携分支不得误触发：构造到析构全程 exe 旁不产生 drafts/。
// 默认目录指向真实 %LOCALAPPDATA% 用户数据区，故不驱动任何脏文档写盘
// （避免污染真实用户目录），仅做零副作用断言（与 tst_sessionmanager 的
// constructWithEmptyPathKeepsExeDirCleanWhenMarkerMissing 同模式）。
void TestDraftCacheManager::noMarkerKeepsExeDirCleanWithoutDrafts()
{
    ExeDirDraftPortableView exeDir;
    if (!exeDir.dirIsClean())
        QSKIP("build 目录已存在 qcnote.portable/qcnote.ini/drafts，跳过以免误动他人产物");

    DocumentManager mgr;
    DraftCacheManager cache(&mgr, QString()); // 空注入路径 + 无标记：默认用户目录
    // 注意：此处不调用 restoreDrafts——非便携默认目录指向真实
    // %LOCALAPPDATA%\qcnote\qcnote\drafts，环境若存在用户真实缓存条目会被
    // 读取应用/删除，单元测试不得触碰真实用户数据
    cache.flush(); // 无挂起：仅早退，不落盘
    QCOMPARE(mgr.documents().size(), 0);

    QVERIFY(!QDir(exeDir.draftsPath()).exists()); // exe 旁零副作用
}

int runDraftCacheManagerTests(int argc, char *argv[])
{
    // 防抖 QTimer 与 QSignalSpy 依赖事件循环（QCoreApplication 即可）；
    // DraftCacheManager 构造连接 qApp 的 aboutToQuit，需要应用实例存在
    QCoreApplication app(argc, argv);
    TestDraftCacheManager t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_draftcachemanager.moc"
