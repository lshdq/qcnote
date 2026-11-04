#include <QByteArray>
#include <QTest>

#include "document/document.h"
#include "storage/encodingdetector.h"

// TC-001~TC-005：编码自动识别策略（BOM → 严格 UTF-8 校验 → GBK 回退）
class TestEncodingDetector : public QObject
{
    Q_OBJECT

private slots:
    void utf8BomDetected();
    void bomOnlyDetected();
    void pureAsciiDetectedAsUtf8();
    void utf8ChineseAndEmojiDetected();
    void gbkBytesDetected();
    void invalidUtf8FallsBackToGbk();
    void danglingMultibyteTailFallsBackToGbk();
    void emptyInputIsUtf8();
};

// TC-001：EF BB BF + UTF-8 中文 → Utf8 + BOM
void TestEncodingDetector::utf8BomDetected()
{
    const QByteArray raw = QByteArrayLiteral("\xEF\xBB\xBF") + QStringLiteral("你好")
                                                     .toUtf8();
    const auto result = EncodingDetector::detect(raw);
    QCOMPARE(result.encoding, Document::Encoding::Utf8);
    QCOMPARE(result.hasBom, true);
}

// 仅 3 字节 BOM（空内容带 BOM）：① 优先命中 → Utf8 + BOM
void TestEncodingDetector::bomOnlyDetected()
{
    const auto result = EncodingDetector::detect(QByteArrayLiteral("\xEF\xBB\xBF"));
    QCOMPARE(result.encoding, Document::Encoding::Utf8);
    QCOMPARE(result.hasBom, true);
}

// TC-003：纯 ASCII 是 UTF-8 子集 → Utf8 无 BOM
void TestEncodingDetector::pureAsciiDetectedAsUtf8()
{
    const auto result = EncodingDetector::detect(QByteArrayLiteral("plain ascii text 123"));
    QCOMPARE(result.encoding, Document::Encoding::Utf8);
    QCOMPARE(result.hasBom, false);
}

// TC-002：合法 UTF-8 多字节序列（中文 + emoji）→ Utf8 无 BOM
void TestEncodingDetector::utf8ChineseAndEmojiDetected()
{
    const QByteArray raw = QStringLiteral("你好，世界 \U0001F600\nsecond line").toUtf8();
    const auto result = EncodingDetector::detect(raw);
    QCOMPARE(result.encoding, Document::Encoding::Utf8);
    QCOMPARE(result.hasBom, false);
}

// TC-004：“你好”的 GBK 字节（C4 E3 BA C3）不是合法 UTF-8 → Gbk
void TestEncodingDetector::gbkBytesDetected()
{
    const auto result = EncodingDetector::detect(QByteArrayLiteral("\xC4\xE3\xBA\xC3"));
    QCOMPARE(result.encoding, Document::Encoding::Gbk);
    QCOMPARE(result.hasBom, false);
}

// TC-005：非法 UTF-8 序列（连续高位字节）严格校验失败 → 回退 Gbk
void TestEncodingDetector::invalidUtf8FallsBackToGbk()
{
    const auto result = EncodingDetector::detect(QByteArrayLiteral("\x81\x81\x81\x81"));
    QCOMPARE(result.encoding, Document::Encoding::Gbk);
    QCOMPARE(result.hasBom, false);
}

// 尾部截断的多字节序列（合法前缀 + 悬空前导字节）也判为非法：
// 有状态解码器会静默吞掉悬空字节误判 UTF-8，检测必须用 Stateless 严格校验
void TestEncodingDetector::danglingMultibyteTailFallsBackToGbk()
{
    const auto result = EncodingDetector::detect(QByteArrayLiteral("ab\xE4"));
    QCOMPARE(result.encoding, Document::Encoding::Gbk);
    QCOMPARE(result.hasBom, false);
}

// 空输入：空文件是合法 UTF-8（无 BOM）
void TestEncodingDetector::emptyInputIsUtf8()
{
    const auto result = EncodingDetector::detect(QByteArray());
    QCOMPARE(result.encoding, Document::Encoding::Utf8);
    QCOMPARE(result.hasBom, false);
}

int runEncodingDetectorTests(int argc, char *argv[])
{
    TestEncodingDetector t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_encodingdetector.moc"
