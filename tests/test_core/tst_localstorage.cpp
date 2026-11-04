#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include "document/document.h"
#include "storage/localstorage.h"

// TC-007~TC-010：LocalStorage 读写行为（双编码）
class TestLocalStorage : public QObject
{
    Q_OBJECT

private slots:
    void lfRoundTrip();
    void crlfReadNormalizedToLf();
    void crlfWritePreservesCrlf();
    void crOnlyReadNormalizedToLf();
    void mixedLineEndingsReadNormalizedToLf();
    void bomStrippedOnRead();
    void writtenFileHasNoBom();
    void readMissingFileReportsError();
    void writeInvalidPathReportsError();
    void readDirectoryReportsError();
    void writeTruncatesExistingLongerFile();
    void gbkWriteReadRoundTrip();
    void gbkWriteIgnoresWriteBomFlag();
    void forcedGbkReadReturnsGbkWithoutBom();
    void forcedUtf8ReadOfGbkBytesYieldsReplacements();
    void forcedUtf8ReadStripsBomAndRecordsIt();
    void utf8BomWrittenBackWhenRequested();
    void gbkUnencodableWriteFailsAndKeepsDisk();
    void autoDetectGbkOnRead();
    void invalidUtf8FileOpensAsGbkWithReplacements();

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

    static const QByteArray &bomBytes()
    {
        static const QByteArray bom("\xEF\xBB\xBF", 3);
        return bom;
    }
};

// LF 往返一致：写 LF 读回 LF，风格记录为 Lf；默认 UTF-8 无 BOM
void TestLocalStorage::lfRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LocalStorage storage;
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("lf.txt")));

    QString error;
    QVERIFY(storage.write(url, QStringLiteral("a\nb\nc"), Document::LineEnding::Lf,
                          Document::Encoding::Utf8, false, &error));
    QCOMPARE(error, QString());

    const DocumentStorage::LoadResult loaded = storage.read(url, &error);
    QCOMPARE(error, QString());
    QCOMPARE(loaded.ok, true);
    QCOMPARE(loaded.content, QStringLiteral("a\nb\nc"));
    QCOMPARE(loaded.lineEnding, Document::LineEnding::Lf);
    QCOMPARE(loaded.encoding, Document::Encoding::Utf8);
    QCOMPARE(loaded.hasBom, false);
}

// CRLF 文件读取：内容归一化为 LF，风格记录为 Crlf
void TestLocalStorage::crlfReadNormalizedToLf()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("crlf.txt"));
    writeRawBytes(path, QByteArrayLiteral("a\r\nb\r\nc\r\n"));

    LocalStorage storage;
    QString error;
    const DocumentStorage::LoadResult loaded = storage.read(QUrl::fromLocalFile(path), &error);
    QCOMPARE(error, QString());
    QCOMPARE(loaded.ok, true);
    QCOMPARE(loaded.content, QStringLiteral("a\nb\nc\n"));
    QCOMPARE(loaded.lineEnding, Document::LineEnding::Crlf);
}

// CRLF 风格写回：文件字节包含 "\r\n"，不残留孤立 "\n"
void TestLocalStorage::crlfWritePreservesCrlf()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("crlf-out.txt"));

    LocalStorage storage;
    QString error;
    QVERIFY(storage.write(QUrl::fromLocalFile(path), QStringLiteral("a\nb\nc"),
                          Document::LineEnding::Crlf, Document::Encoding::Utf8, false, &error));
    QCOMPARE(error, QString());

    const QByteArray raw = readRawBytes(path);
    QCOMPARE(raw, QByteArrayLiteral("a\r\nb\r\nc"));
    QVERIFY(!raw.contains('\n') || raw.count("\r\n") == raw.count("\n"));
}

// CR-only 文件读取：归一化为 LF，风格记录为 Lf
void TestLocalStorage::crOnlyReadNormalizedToLf()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("cr.txt"));
    writeRawBytes(path, QByteArrayLiteral("a\rb\rc"));

    LocalStorage storage;
    QString error;
    const DocumentStorage::LoadResult loaded = storage.read(QUrl::fromLocalFile(path), &error);
    QCOMPARE(error, QString());
    QCOMPARE(loaded.ok, true);
    QCOMPARE(loaded.content, QStringLiteral("a\nb\nc"));
    QCOMPARE(loaded.lineEnding, Document::LineEnding::Lf);
}

// 混合换行（CRLF/LF/CR 同一文件，需求"输入"明确支持）：全部归一化为 LF，风格按含 CRLF 记录为 Crlf
void TestLocalStorage::mixedLineEndingsReadNormalizedToLf()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("mixed.txt"));
    writeRawBytes(path, QByteArrayLiteral("a\r\nb\nc\rd"));

    LocalStorage storage;
    QString error;
    const DocumentStorage::LoadResult loaded = storage.read(QUrl::fromLocalFile(path), &error);
    QCOMPARE(error, QString());
    QCOMPARE(loaded.content, QStringLiteral("a\nb\nc\nd"));
    QCOMPARE(loaded.ok, true);
    QCOMPARE(loaded.lineEnding, Document::LineEnding::Crlf); // 含 "\r\n" 即按 CRLF 记录
}

// 带 BOM 的 UTF-8 文件读取：BOM 被剥离，不进入内容，hasBom 记录为 true
void TestLocalStorage::bomStrippedOnRead()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("bom.txt"));
    writeRawBytes(path, bomBytes() + QByteArrayLiteral("hello"));

    LocalStorage storage;
    QString error;
    const DocumentStorage::LoadResult loaded = storage.read(QUrl::fromLocalFile(path), &error);
    QCOMPARE(error, QString());
    QCOMPARE(loaded.ok, true);
    QCOMPARE(loaded.content, QStringLiteral("hello"));
    QCOMPARE(loaded.encoding, Document::Encoding::Utf8);
    QCOMPARE(loaded.hasBom, true);
}

// 写出的文件无 BOM（默认 writeBom=false）
void TestLocalStorage::writtenFileHasNoBom()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("nobom.txt"));

    LocalStorage storage;
    QString error;
    QVERIFY(storage.write(QUrl::fromLocalFile(path), QStringLiteral("hello"),
                          Document::LineEnding::Lf, Document::Encoding::Utf8, false, &error));
    QCOMPARE(error, QString());

    const QByteArray raw = readRawBytes(path);
    QVERIFY(!raw.startsWith(bomBytes()));
    QCOMPARE(raw, QByteArrayLiteral("hello"));
}

// 读不存在的文件：返回空内容且 error 非空
void TestLocalStorage::readMissingFileReportsError()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LocalStorage storage;
    QString error;
    const DocumentStorage::LoadResult loaded =
        storage.read(QUrl::fromLocalFile(dir.filePath(QStringLiteral("missing.txt"))), &error);
    QVERIFY(!error.isEmpty());
    QVERIFY(!loaded.ok);
    QVERIFY(loaded.content.isEmpty());
}

// 写非法路径（不存在的目录）：返回 false 且 error 非空
void TestLocalStorage::writeInvalidPathReportsError()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QUrl url = QUrl::fromLocalFile(
        dir.filePath(QStringLiteral("no/such/dir/out.txt")));

    LocalStorage storage;
    QString error;
    QVERIFY(!storage.write(url, QStringLiteral("x"), Document::LineEnding::Lf,
                           Document::Encoding::Utf8, false, &error));
    QVERIFY(!error.isEmpty());
}

// 读目录路径：返回错误而不是把目录当文件读（TC-016 核心层近缘）
void TestLocalStorage::readDirectoryReportsError()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LocalStorage storage;
    QString error;
    const DocumentStorage::LoadResult loaded =
        storage.read(QUrl::fromLocalFile(dir.path()), &error);
    QVERIFY(!error.isEmpty());
    QVERIFY(!loaded.ok);
    QVERIFY(loaded.content.isEmpty());
}

// 覆盖写：目标文件原内容更长时写入后截断，无旧字节残留（TC-014 往返一致补充）
void TestLocalStorage::writeTruncatesExistingLongerFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("truncate.txt"));
    writeRawBytes(path, QByteArrayLiteral("0123456789ABCDEF")); // 16 字节旧内容

    LocalStorage storage;
    QString error;
    QVERIFY(storage.write(QUrl::fromLocalFile(path), QStringLiteral("ab"),
                          Document::LineEnding::Lf, Document::Encoding::Utf8, false, &error));
    QCOMPARE(error, QString());

    QCOMPARE(readRawBytes(path), QByteArrayLiteral("ab")); // 截断，无残留
}

// TC-009：GBK 往返——写出的字节为显式 CP936 编码（无 BOM 前缀），自动识别读回内容一致；
// 强制 GBK 读取同样还原
void TestLocalStorage::gbkWriteReadRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LocalStorage storage;
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("gbk.txt")));
    const QString text = QStringLiteral("\u4F60\u597D\uFF0Cworld\n\u7B2C\u4E8C\u884C"); // 你好，world\n第二行

    QString error;
    QVERIFY(storage.write(url, text, Document::LineEnding::Crlf, Document::Encoding::Gbk,
                          false, &error));
    QCOMPARE(error, QString());
    // 显式断言磁盘字节（你好=C4E3BAC3 ，=A3AC world=ASCII CRLF=0D0A 第=B5DA 二=B6FE 行=D0D0）
    QCOMPARE(readRawBytes(url.toLocalFile()),
             QByteArrayLiteral("\xC4\xE3\xBA\xC3\xA3\xAC" "world\r\n\xB5\xDA\xB6\xFE\xD0\xD0"));

    // 自动识别读取：判为 GBK，内容与换行风格还原
    const DocumentStorage::LoadResult autoLoaded = storage.read(url, &error);
    QCOMPARE(error, QString());
    QCOMPARE(autoLoaded.ok, true);
    QCOMPARE(autoLoaded.encoding, Document::Encoding::Gbk);
    QCOMPARE(autoLoaded.hasBom, false);
    QCOMPARE(autoLoaded.content, text);
    QCOMPARE(autoLoaded.lineEnding, Document::LineEnding::Crlf);

    // 强制 GBK 读取：同样还原
    const DocumentStorage::LoadResult forced = storage.read(url, Document::Encoding::Gbk, &error);
    QCOMPARE(error, QString());
    QCOMPARE(forced.ok, true);
    QCOMPARE(forced.encoding, Document::Encoding::Gbk);
    QCOMPARE(forced.content, text);
}

// GBK 写入忽略 writeBom 标志（GBK 无 BOM 概念）：磁盘不出现 EF BB BF 前缀
void TestLocalStorage::gbkWriteIgnoresWriteBomFlag()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("gbk-bomflag.txt"));

    LocalStorage storage;
    QString error;
    QVERIFY(storage.write(QUrl::fromLocalFile(path), QStringLiteral("\u4F60\u597D"),
                          Document::LineEnding::Lf, Document::Encoding::Gbk, true, &error));
    QCOMPARE(readRawBytes(path), QByteArrayLiteral("\xC4\xE3\xBA\xC3")); // 无 BOM 前缀
}

// 强制 GBK 读取：hasBom 恒 false（即便磁盘字节恰好以 EF BB BF 开头也按 GBK 解码）
void TestLocalStorage::forcedGbkReadReturnsGbkWithoutBom()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("forced-gbk.txt"));
    writeRawBytes(path, QByteArrayLiteral("\xC4\xE3\xBA\xC3"));

    LocalStorage storage;
    QString error;
    const DocumentStorage::LoadResult loaded =
        storage.read(QUrl::fromLocalFile(path), Document::Encoding::Gbk, &error);
    QCOMPARE(error, QString());
    QCOMPARE(loaded.ok, true);
    QCOMPARE(loaded.encoding, Document::Encoding::Gbk);
    QCOMPARE(loaded.hasBom, false);
    QCOMPARE(loaded.content, QStringLiteral("\u4F60\u597D"));
}

// TC-006：强制 UTF-8 读取 GBK 字节：不崩溃、正常返回，内容为替换符呈现的乱码
void TestLocalStorage::forcedUtf8ReadOfGbkBytesYieldsReplacements()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("forced-utf8.txt"));
    writeRawBytes(path, QByteArrayLiteral("\xC4\xE3\xBA\xC3"));

    LocalStorage storage;
    QString error;
    const DocumentStorage::LoadResult loaded =
        storage.read(QUrl::fromLocalFile(path), Document::Encoding::Utf8, &error);
    QCOMPARE(error, QString());
    QCOMPARE(loaded.ok, true);
    QCOMPARE(loaded.encoding, Document::Encoding::Utf8);
    QCOMPARE(loaded.hasBom, false);
    QVERIFY(!loaded.content.isEmpty());
    for (const QChar &c : loaded.content)
        QCOMPARE(c, QChar(0xFFFD)); // 乱码以替换符呈现
}

// 强制 UTF-8 读取带 BOM 文件：BOM 剥离且 hasBom=true
void TestLocalStorage::forcedUtf8ReadStripsBomAndRecordsIt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("forced-bom.txt"));
    writeRawBytes(path, bomBytes() + QStringLiteral("你好").toUtf8());

    LocalStorage storage;
    QString error;
    const DocumentStorage::LoadResult loaded =
        storage.read(QUrl::fromLocalFile(path), Document::Encoding::Utf8, &error);
    QCOMPARE(error, QString());
    QCOMPARE(loaded.ok, true);
    QCOMPARE(loaded.encoding, Document::Encoding::Utf8);
    QCOMPARE(loaded.hasBom, true);
    QCOMPARE(loaded.content, QStringLiteral("你好")); // 内容不含 BOM 字符
}

// TC-008：writeBom=true 时磁盘以 EF BB BF 开头，BOM 后为合法 UTF-8
void TestLocalStorage::utf8BomWrittenBackWhenRequested()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("bom-out.txt"));

    LocalStorage storage;
    QString error;
    QVERIFY(storage.write(QUrl::fromLocalFile(path), QStringLiteral("你好"),
                          Document::LineEnding::Lf, Document::Encoding::Utf8, true, &error));
    QCOMPARE(error, QString());

    const QByteArray raw = readRawBytes(path);
    QVERIFY(raw.startsWith(bomBytes()));
    QCOMPARE(raw, bomBytes() + QStringLiteral("你好").toUtf8());

    // 自动识别读回：Utf8 + BOM，内容不含 BOM
    const DocumentStorage::LoadResult loaded = storage.read(QUrl::fromLocalFile(path), &error);
    QCOMPARE(loaded.encoding, Document::Encoding::Utf8);
    QCOMPARE(loaded.hasBom, true);
    QCOMPARE(loaded.content, QStringLiteral("你好"));
}

// TC-010：GBK 不可编码字符阻止保存——返回 false、error 含字符与行号、磁盘原字节未被改动
void TestLocalStorage::gbkUnencodableWriteFailsAndKeepsDisk()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("keep.txt"));
    const QByteArray original = QByteArrayLiteral("\xC4\xE3\xBA\xC3"); // 磁盘原有 GBK 内容
    writeRawBytes(path, original);

    LocalStorage storage;
    QString error;
    const QString text = QStringLiteral("abc\n\U0001F600\u4F60"); // 第 2 行含 emoji
    QVERIFY(!storage.write(QUrl::fromLocalFile(path), text, Document::LineEnding::Lf,
                           Document::Encoding::Gbk, false, &error));
    QVERIFY(error.contains(QStringLiteral("2")));       // 行号定位（第 2 行）
    QVERIFY(error.contains(QStringLiteral("\U0001F600"))); // 不可编码字符
    QCOMPARE(readRawBytes(path), original);             // 磁盘未被触碰
}

// TC-004 存储层侧：GBK 文件自动识别读取——判为 GBK 且内容正确解码
void TestLocalStorage::autoDetectGbkOnRead()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("auto-gbk.txt"));
    writeRawBytes(path, QByteArrayLiteral("\xC4\xE3\xBA\xC3"));

    LocalStorage storage;
    QString error;
    const DocumentStorage::LoadResult loaded = storage.read(QUrl::fromLocalFile(path), &error);
    QCOMPARE(error, QString());
    QCOMPARE(loaded.ok, true);
    QCOMPARE(loaded.encoding, Document::Encoding::Gbk);
    QCOMPARE(loaded.content, QStringLiteral("\u4F60\u597D"));
}

// TC-005 端到端：非法 UTF-8 且 GBK 也无法完全解码的文件仍可打开——
// 严格 UTF-8 校验失败（0x81 首字节为孤立 continuation byte）→ 判 GBK；
// GBK 解码 0x81 0x39 非法对（0x39 不在 0x40-0xFE 次字节区间）→ U+FFFD
// 呈现且后续内容正常解码（文件不因含坏字节而打开失败）
void TestLocalStorage::invalidUtf8FileOpensAsGbkWithReplacements()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("bad-bytes.txt"));
    writeRawBytes(path, QByteArrayLiteral("\x81\x39\xC4\xE3\xBA\xC3"));

    LocalStorage storage;
    QString error;
    const DocumentStorage::LoadResult loaded = storage.read(QUrl::fromLocalFile(path), &error);
    QCOMPARE(error, QString());
    QCOMPARE(loaded.ok, true);                              // 文件仍可打开
    QCOMPARE(loaded.encoding, Document::Encoding::Gbk);    // 回退判为 GBK
    QCOMPARE(loaded.hasBom, false);
    QCOMPARE(loaded.content,
             QString(QChar(0xFFFD)) + QStringLiteral("9\u4F60\u597D")); // U+FFFD + 9 + 你好
}

int runLocalStorageTests(int argc, char *argv[])
{
    TestLocalStorage t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_localstorage.moc"
