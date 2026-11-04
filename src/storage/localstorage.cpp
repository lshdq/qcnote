#include "storage/localstorage.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringDecoder>

#include "platform/gbkcodec.h"
#include "storage/encodingdetector.h"

namespace {

const QByteArray &utf8Bom()
{
    static const QByteArray bom("\xEF\xBB\xBF", 3);
    return bom;
}

/// UTF-8 字节 → 文本（调用方保证已剥离 BOM）。
/// QStringDecoder 遇非法序列默认替换为 U+FFFD：读路径不因非法序列失败
/// （保证文件始终可打开），识别正确性由 EncodingDetector 的严格校验保证。
/// Flag::Stateless：整块单次解码无跨调用状态需求，且使尾部截断的多字节序列
/// 以 U+FFFD 呈现而不是被有状态解码器静默吞掉（编辑器不得静默丢字节）。
QString decodeUtf8Bytes(const QByteArray &bytes)
{
    QStringDecoder decoder(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    return decoder.decode(bytes); // 返回类型 QString 触发真实解码（decode 本身返回惰性包装）
}

/// 换行风格探测 + 归一化（CRLF/孤立 CR → LF）并填充 LoadResult（成功路径）
DocumentStorage::LoadResult assembleLoadResult(const QString &text, Document::Encoding encoding,
                                               bool hasBom)
{
    DocumentStorage::LoadResult result;
    result.lineEnding = text.contains(QStringLiteral("\r\n"))
                            ? Document::LineEnding::Crlf
                            : Document::LineEnding::Lf;
    QString normalized = text;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QChar(u'\r'), QChar(u'\n'));
    result.content = normalized;
    result.encoding = encoding;
    result.hasBom = hasBom;
    result.ok = true; // 成功路径显式置位（LoadResult::ok 默认 false）
    return result;
}

} // namespace

LocalStorage::LocalStorage(QObject *parent)
    : DocumentStorage(parent)
{
}

QString LocalStorage::scheme() const
{
    return QStringLiteral("file");
}

bool LocalStorage::readRawBytes(const QUrl &url, QByteArray *raw, QString *error) const
{
    if (error)
        error->clear();

    const QString path = url.toLocalFile();
    if (path.isEmpty()) {
        if (error)
            *error = tr("不是有效的本地文件地址：%1").arg(url.toString());
        return false;
    }

    const QFileInfo info(path);
    if (!info.exists()) {
        if (error)
            *error = tr("文件不存在：%1").arg(path);
        return false;
    }
    if (info.isDir()) {
        if (error)
            *error = tr("目标是目录而不是文件：%1").arg(path);
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = tr("无法打开文件 %1：%2").arg(path, file.errorString());
        return false;
    }
    *raw = file.readAll();
    return true;
}

DocumentStorage::LoadResult LocalStorage::read(const QUrl &url, QString *error)
{
    QByteArray raw;
    if (!readRawBytes(url, &raw, error))
        return LoadResult();

    const EncodingDetector::DetectionResult detected = EncodingDetector::detect(raw);
    if (detected.encoding == Document::Encoding::Utf8) {
        // BOM 已由识别判定：显式剥离后再解码，不依赖解码器自身的 BOM 行为
        const QByteArray body = detected.hasBom ? raw.mid(3) : raw;
        return assembleLoadResult(decodeUtf8Bytes(body), Document::Encoding::Utf8, detected.hasBom);
    }

    // GBK：解码交由 GbkCodec（识别器只判定候选，见 EncodingDetector 注释）
    QString text;
    QString gbkError;
    if (!GbkCodec::decode(raw, &text, &gbkError)) {
        if (error)
            *error = gbkError.isEmpty() ? tr("无法以 GBK 解码文件：%1").arg(url.toString())
                                        : gbkError;
        return LoadResult();
    }
    return assembleLoadResult(text, Document::Encoding::Gbk, false);
}

DocumentStorage::LoadResult LocalStorage::read(const QUrl &url, Document::Encoding forcedEncoding,
                                               QString *error)
{
    QByteArray raw;
    if (!readRawBytes(url, &raw, error))
        return LoadResult();

    if (forcedEncoding == Document::Encoding::Utf8) {
        // 强制 UTF-8：磁盘带 BOM 则剥离并记录（内容不含 BOM 字符）
        const bool hasBom = raw.startsWith(utf8Bom());
        const QByteArray body = hasBom ? raw.mid(3) : raw;
        return assembleLoadResult(decodeUtf8Bytes(body), Document::Encoding::Utf8, hasBom);
    }

    QString text;
    QString gbkError;
    if (!GbkCodec::decode(raw, &text, &gbkError)) {
        if (error)
            *error = gbkError.isEmpty() ? tr("无法以 GBK 解码文件：%1").arg(url.toString())
                                        : gbkError;
        return LoadResult();
    }
    return assembleLoadResult(text, Document::Encoding::Gbk, false);
}

bool LocalStorage::write(const QUrl &url, const QString &content, Document::LineEnding lineEnding,
                         Document::Encoding encoding, bool writeBom, QString *error)
{
    if (error)
        error->clear();

    const QString path = url.toLocalFile();
    if (path.isEmpty()) {
        if (error)
            *error = tr("不是有效的本地文件地址：%1").arg(url.toString());
        return false;
    }

    QString out = content;
    if (lineEnding == Document::LineEnding::Crlf) {
        out.replace(QStringLiteral("\r\n"), QStringLiteral("\n")); // 防御：内容理论上已是 LF
        out.replace(QChar(u'\n'), QStringLiteral("\r\n"));
    }

    QByteArray bytes;
    if (encoding == Document::Encoding::Utf8) {
        bytes = out.toUtf8();
        if (writeBom)
            bytes.prepend(utf8Bom()); // BOM 是元数据：仅 writeBom 时前置，不混入正文
    } else {
        // GBK：全量可编码校验在打开目标文件之前完成（DEC-006）——
        // 含不可编码字符时立即失败，磁盘不被触碰
        QString gbkError;
        if (!GbkCodec::encode(out, &bytes, &gbkError)) {
            if (error)
                *error = gbkError.isEmpty() ? tr("内容无法以 GBK 编码写入：%1").arg(path)
                                            : gbkError;
            return false;
        }
    }

    QSaveFile file(path); // 原子写：先写临时文件，commit 时再替换
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = tr("无法写入文件 %1：%2").arg(path, file.errorString());
        return false;
    }
    file.write(bytes);
    if (!file.commit()) {
        if (error)
            *error = tr("写入文件失败 %1：%2").arg(path, file.errorString());
        return false;
    }
    return true;
}
