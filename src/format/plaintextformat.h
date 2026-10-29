#pragma once

#include "format/documentformat.h"

/**
 * @brief 纯文本格式：默认兜底格式，任何文件都可按纯文本打开。
 */
class PlainTextFormat : public DocumentFormat
{
    Q_OBJECT

public:
    explicit PlainTextFormat(QObject *parent = nullptr);

    QString id() const override;
    QString displayName() const override;
    QStringList nameFilters() const override;

    /// 兜底格式：恒返回 true，未知扩展名的文件最终由本格式处理
    bool matchesFileName(const QString &fileName) const override;
};
