#include <QByteArray>
#include <QTest>

#include "platform/gbkcodec.h"

// TC-GBK：GbkCodec（Win32 CP936）编解码行为。
// 字节样本全部显式给出（QByteArrayLiteral 十六进制），禁止依赖源文件编码，
// 保证测试在任何编辑器/编译器编码设置下结果确定。
class TestGbkCodec : public QObject
{
    Q_OBJECT

private slots:
    void decodeExplicitGbkBytes();
    void encodeExplicitGbkBytes();
    void roundTripMixedContent();
    void decodeInvalidTrailByteBecomesReplacementAndContinues();
    void decodeLoneLeadAtEndBecomesReplacement();
    void decodeInvalidLeadByteFFBecomesReplacement();
    void decodeEuroByte80();
    void decodeEmptyInput();
    void encodeEmptyInput();
    void encodeRejectsEmojiWithCharAndLine();
    void encodeRejectsRareHanWithMessage();
    void encodeReportsFirstBadCharOnCorrectLine();
    void encodeFullwidthPunctuationAllowed();
    void encodeFailureProducesNoOutput();

private:
    static const QString &replacement()
    {
        static const QString rep(QChar(0xFFFD));
        return rep;
    }
};

// “你好”的 GBK 字节 C4 E3 BA C3 → 解码为 U+4F60 U+597D
void TestGbkCodec::decodeExplicitGbkBytes()
{
    QString text;
    QString error;
    QVERIFY(GbkCodec::decode(QByteArrayLiteral("\xC4\xE3\xBA\xC3"), &text, &error));
    QCOMPARE(error, QString());
    QCOMPARE(text, QStringLiteral("\u4F60\u597D")); // 你好
}

// “你好” → GBK 字节 C4 E3 BA C3（显式字节断言，不依赖实现自证）
void TestGbkCodec::encodeExplicitGbkBytes()
{
    QByteArray bytes;
    QString error;
    QVERIFY(GbkCodec::encode(QStringLiteral("\u4F60\u597D"), &bytes, &error));
    QCOMPARE(error, QString());
    QCOMPARE(bytes, QByteArrayLiteral("\xC4\xE3\xBA\xC3"));
}

// 中英混排 + 标点 + 多行往返：编码字节与手工码表一致，解码还原同一内容
void TestGbkCodec::roundTripMixedContent()
{
    const QString text = QStringLiteral("\u4F60\u597D\uFF0Cworld\n\u7B2C\u4E8C\u884C"); // 你好，world\n第二行
    QByteArray bytes;
    QString error;
    QVERIFY(GbkCodec::encode(text, &bytes, &error));
    // 你好=C4E3BAC3 ，=A3AC world=ASCII \n=0A 第=B5DA 二=B6FE 行=D0D0
    QCOMPARE(bytes,
             QByteArrayLiteral("\xC4\xE3\xBA\xC3\xA3\xAC" "world\n\xB5\xDA\xB6\xFE\xD0\xD0"));

    QString decoded;
    QVERIFY(GbkCodec::decode(bytes, &decoded, &error));
    QCOMPARE(decoded, text);
}

// 非法次字节（0x81 0x39：0x39 不在 0x40-0xFE 区间）：非法首字节置 U+FFFD 后前进 1 字节，
// 其后的 ASCII '9' 与后续 GBK 对仍正常解码（不崩溃、不丢内容）
void TestGbkCodec::decodeInvalidTrailByteBecomesReplacementAndContinues()
{
    QString text;
    QString error;
    // 你(OK) + 81 39(非法对) + BA C3(好)
    QVERIFY(GbkCodec::decode(QByteArrayLiteral("\xC4\xE3\x81\x39\xBA\xC3"), &text, &error));
    QCOMPARE(text, QStringLiteral("\u4F60") + replacement() + QStringLiteral("9\u597D"));
}

// 末尾孤立首字节（0xC4 无次字节）：单个 U+FFFD，前面的正常内容保留
void TestGbkCodec::decodeLoneLeadAtEndBecomesReplacement()
{
    QString text;
    QString error;
    QVERIFY(GbkCodec::decode(QByteArrayLiteral("\xC4\xE3\xC4"), &text, &error));
    QCOMPARE(text, QStringLiteral("\u4F60") + replacement());
}

// 0xFF 不在 GBK 首字节区间（0x81-0xFE）：U+FFFD 且后续内容正常
void TestGbkCodec::decodeInvalidLeadByteFFBecomesReplacement()
{
    QString text;
    QString error;
    QVERIFY(GbkCodec::decode(QByteArrayLiteral("\xFF\xC4\xE3\xBA\xC3"), &text, &error));
    QCOMPARE(text, replacement() + QStringLiteral("\u4F60\u597D"));
}

// 单字节 0x80 按 CP936 定义解码为 U+20AC（欧元符）
void TestGbkCodec::decodeEuroByte80()
{
    QString text;
    QString error;
    QVERIFY(GbkCodec::decode(QByteArrayLiteral("\x80"), &text, &error));
    QCOMPARE(text, QString(QChar(0x20AC)));
}

void TestGbkCodec::decodeEmptyInput()
{
    QString text = QStringLiteral("stale");
    QString error;
    QVERIFY(GbkCodec::decode(QByteArray(), &text, &error));
    QCOMPARE(text, QString());
    QCOMPARE(error, QString());
}

void TestGbkCodec::encodeEmptyInput()
{
    QByteArray bytes = QByteArrayLiteral("stale");
    QString error;
    QVERIFY(GbkCodec::encode(QString(), &bytes, &error));
    QCOMPARE(bytes, QByteArray());
    QCOMPARE(error, QString());
}

// emoji（U+1F600 代理对）不可编码：失败且错误消息含该字符与第 1 行
void TestGbkCodec::encodeRejectsEmojiWithCharAndLine()
{
    const QString text = QStringLiteral("\U0001F600"); // 😀
    QByteArray bytes;
    QString error;
    QVERIFY(!GbkCodec::encode(text, &bytes, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(error.contains(text));            // 错误消息含不可编码字符本身
    QVERIFY(error.contains(QStringLiteral("1"))); // 行号（第 1 行）
    QVERIFY(error.contains(QStringLiteral("GBK")));
}

// GBK 外生僻字（U+3400，CJK 扩展 A）同样拒绝保存
void TestGbkCodec::encodeRejectsRareHanWithMessage()
{
    const QString text = QStringLiteral("\u4F60\u3400"); // 你 + 㐀
    QByteArray bytes;
    QString error;
    QVERIFY(!GbkCodec::encode(text, &bytes, &error));
    QVERIFY(error.contains(QStringLiteral("\u3400")));
}

// 多行内容：首个不可编码字符的行号定位正确（第 3 行的 emoji，其后还有一个生僻字）
void TestGbkCodec::encodeReportsFirstBadCharOnCorrectLine()
{
    const QString text = QStringLiteral("\u4F60\u597D\nabc\n\U0001F600\u3400");
    QByteArray bytes;
    QString error;
    QVERIFY(!GbkCodec::encode(text, &bytes, &error));
    QVERIFY(error.contains(QStringLiteral("\u7B2C 3 \u884C"))); // “第 3 行”
    QVERIFY(error.contains(QStringLiteral("\U0001F600")));      // 首个不可编码字符（emoji）
}

// 全角标点在 GBK 码表内（，=A3AC）：正常编码（防误伤常见中文标点）
void TestGbkCodec::encodeFullwidthPunctuationAllowed()
{
    QByteArray bytes;
    QString error;
    QVERIFY(GbkCodec::encode(QStringLiteral("\uFF0C\uFF01\uFF1F"), &bytes, &error)); // ，！？
    QCOMPARE(bytes, QByteArrayLiteral("\xA3\xAC\xA3\xA1\xA3\xBF"));
}

// 编码失败时 out 不产出（调用方不得写盘的契约前提）
void TestGbkCodec::encodeFailureProducesNoOutput()
{
    QByteArray bytes = QByteArrayLiteral("stale");
    QString error;
    QVERIFY(!GbkCodec::encode(QStringLiteral("\U0001F600"), &bytes, &error));
    QCOMPARE(bytes, QByteArray());
}

int runGbkCodecTests(int argc, char *argv[])
{
    TestGbkCodec t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_gbkcodec.moc"
