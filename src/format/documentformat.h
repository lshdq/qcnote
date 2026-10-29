#pragma once

#include <QFileInfo>
#include <QObject>
#include <QStringList>

/**
 * @brief 文档格式扩展点（抽象基类）。
 *
 * 每种格式（纯文本、后续的 Markdown/JSON 等）实现本接口并在
 * DocumentManager 中注册；PlainTextFormat 是兜底格式，未知扩展名
 * 的文件最终由它处理。
 */
class DocumentFormat : public QObject
{
    Q_OBJECT

public:
    explicit DocumentFormat(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    ~DocumentFormat() override = default;

    /// 格式标识，如 "plaintext"
    virtual QString id() const = 0;

    /// 用户可见名称，如 "纯文本"
    virtual QString displayName() const = 0;

    /// FileDialog 文件名过滤器，如 {"文本文件 (*.txt)", "所有文件 (*)"}
    virtual QStringList nameFilters() const = 0;

    /**
     * @brief 文件名是否可由本格式处理。
     *
     * 默认实现：按 nameFilters 括号内通配符的扩展名做大小写不敏感匹配；
     * "*" 通配符视为匹配任意文件。兜底格式可重写为恒真。
     */
    virtual bool matchesFileName(const QString &fileName) const
    {
        const QString suffix = QFileInfo(fileName).suffix();
        if (suffix.isEmpty())
            return false;

        const QStringList filters = nameFilters();
        for (const QString &filter : filters) {
            const qsizetype open = filter.lastIndexOf(u'(');
            const qsizetype close = filter.lastIndexOf(u')');
            if (open < 0 || close <= open)
                continue;
            const QStringList patterns = filter.mid(open + 1, close - open - 1)
                                             .split(u' ', Qt::SkipEmptyParts);
            for (const QString &pattern : patterns) {
                if (pattern == QLatin1String("*"))
                    return true;
                if (pattern.startsWith(QLatin1String("*.")) && pattern.size() > 2
                    && pattern.mid(2).compare(suffix, Qt::CaseInsensitive) == 0) {
                    return true;
                }
            }
        }
        return false;
    }
};
