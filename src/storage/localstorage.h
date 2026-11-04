#pragma once

#include "storage/documentstorage.h"

class QByteArray;

/**
 * @brief 本地文件存储（file://）。
 *
 * 读：自动识别编码（EncodingDetector：BOM → 严格 UTF-8 校验 → GBK），
 *     或按调用方强制编码解码；UTF-8 先显式剥离 BOM（不进内容），GBK 经
 *     GbkCodec（CP936）解码，非法序列以 U+FFFD 呈现；CRLF/CR 归一化为 LF，
 *     记录原文件换行风格（含 "\r\n" 视为 Crlf，否则 Lf）。
 * 写：QSaveFile 原子写；UTF-8（writeBom 时前置 EF BB BF）或 GBK
 *     （不可编码即写盘前失败，磁盘不被触碰）；lineEnding==Crlf 时 "\n" 写为 "\r\n"。
 */
class LocalStorage : public DocumentStorage
{
    Q_OBJECT

public:
    explicit LocalStorage(QObject *parent = nullptr);

    QString scheme() const override;
    LoadResult read(const QUrl &url, QString *error) override;
    LoadResult read(const QUrl &url, Document::Encoding forcedEncoding, QString *error) override;
    bool write(const QUrl &url, const QString &content, Document::LineEnding lineEnding,
               Document::Encoding encoding, bool writeBom, QString *error) override;

private:
    /// 校验 url（本地路径、存在且是常规文件）并读取全部原始字节；失败置 *error 返回 false
    bool readRawBytes(const QUrl &url, QByteArray *raw, QString *error) const;
};
