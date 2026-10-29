#include "format/plaintextformat.h"

PlainTextFormat::PlainTextFormat(QObject *parent)
    : DocumentFormat(parent)
{
}

QString PlainTextFormat::id() const
{
    return QStringLiteral("plaintext");
}

QString PlainTextFormat::displayName() const
{
    return QStringLiteral("纯文本");
}

QStringList PlainTextFormat::nameFilters() const
{
    return { QStringLiteral("文本文件 (*.txt)"), QStringLiteral("所有文件 (*)") };
}

bool PlainTextFormat::matchesFileName(const QString &fileName) const
{
    Q_UNUSED(fileName);
    return true; // 兜底格式：任何文件都可用纯文本打开
}
