#pragma once

#include "storage/documentstorage.h"

/**
 * @brief 本地文件存储（file://）。
 *
 * 读：UTF-8 解码（容忍并剥离 BOM），CRLF/CR 归一化为 LF，
 *     记录原文件换行风格（含 "\r\n" 视为 Crlf，否则 Lf）。
 * 写：QSaveFile 原子写，UTF-8 无 BOM；lineEnding==Crlf 时 "\n" 写为 "\r\n"。
 */
class LocalStorage : public DocumentStorage
{
    Q_OBJECT

public:
    explicit LocalStorage(QObject *parent = nullptr);

    QString scheme() const override;
    LoadResult read(const QUrl &url, QString *error) override;
    bool write(const QUrl &url, const QString &content, Document::LineEnding lineEnding, QString *error) override;
};
