#include "storage/localstorage.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

LocalStorage::LocalStorage(QObject *parent)
    : DocumentStorage(parent)
{
}

QString LocalStorage::scheme() const
{
    return QStringLiteral("file");
}

DocumentStorage::LoadResult LocalStorage::read(const QUrl &url, QString *error)
{
    if (error)
        error->clear();

    LoadResult result;
    const QString path = url.toLocalFile();
    if (path.isEmpty()) {
        if (error)
            *error = tr("不是有效的本地文件地址：%1").arg(url.toString());
        return result;
    }

    const QFileInfo info(path);
    if (!info.exists()) {
        if (error)
            *error = tr("文件不存在：%1").arg(path);
        return result;
    }
    if (info.isDir()) {
        if (error)
            *error = tr("目标是目录而不是文件：%1").arg(path);
        return result;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = tr("无法打开文件 %1：%2").arg(path, file.errorString());
        return result;
    }
    const QByteArray raw = file.readAll();

    // Qt 6 的 QString::fromUtf8 默认剥离开头的 UTF-8 BOM（BOM 不进入内容）
    QString text = QString::fromUtf8(raw);

    // 记录原文件换行风格：含 "\r\n" 视为 CRLF
    result.lineEnding = text.contains(QStringLiteral("\r\n"))
                            ? Document::LineEnding::Crlf
                            : Document::LineEnding::Lf;

    // 归一化：CRLF -> LF，孤立的 CR -> LF
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QChar(u'\r'), QChar(u'\n'));

    result.content = text;
    result.ok = true; // 成功路径显式置位（LoadResult::ok 默认 false）
    return result;
}

bool LocalStorage::write(const QUrl &url, const QString &content, Document::LineEnding lineEnding, QString *error)
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

    QSaveFile file(path); // 原子写：先写临时文件，commit 时再替换
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = tr("无法写入文件 %1：%2").arg(path, file.errorString());
        return false;
    }
    file.write(out.toUtf8()); // UTF-8，无 BOM
    if (!file.commit()) {
        if (error)
            *error = tr("写入文件失败 %1：%2").arg(path, file.errorString());
        return false;
    }
    return true;
}
