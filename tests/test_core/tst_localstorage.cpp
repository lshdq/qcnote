#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include "document/document.h"
#include "storage/localstorage.h"

// TC-014：LocalStorage 读写行为
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

// LF 往返一致：写 LF 读回 LF，风格记录为 Lf
void TestLocalStorage::lfRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    LocalStorage storage;
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("lf.txt")));

    QString error;
    QVERIFY(storage.write(url, QStringLiteral("a\nb\nc"), Document::LineEnding::Lf, &error));
    QCOMPARE(error, QString());

    const DocumentStorage::LoadResult loaded = storage.read(url, &error);
    QCOMPARE(error, QString());
    QCOMPARE(loaded.content, QStringLiteral("a\nb\nc"));
    QCOMPARE(loaded.lineEnding, Document::LineEnding::Lf);
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
                          Document::LineEnding::Crlf, &error));
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
    QCOMPARE(loaded.lineEnding, Document::LineEnding::Crlf); // 含 "\r\n" 即按 CRLF 记录
}

// 带 BOM 的 UTF-8 文件读取：BOM 被剥离，不进入内容
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
    QCOMPARE(loaded.content, QStringLiteral("hello"));
}

// 写出的文件无 BOM
void TestLocalStorage::writtenFileHasNoBom()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("nobom.txt"));

    LocalStorage storage;
    QString error;
    QVERIFY(storage.write(QUrl::fromLocalFile(path), QStringLiteral("hello"),
                          Document::LineEnding::Lf, &error));
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
    QVERIFY(!storage.write(url, QStringLiteral("x"), Document::LineEnding::Lf, &error));
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
                          Document::LineEnding::Lf, &error));
    QCOMPARE(error, QString());

    QCOMPARE(readRawBytes(path), QByteArrayLiteral("ab")); // 截断，无残留
}

int runLocalStorageTests(int argc, char *argv[])
{
    TestLocalStorage t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_localstorage.moc"
