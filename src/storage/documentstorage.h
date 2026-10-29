#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

#include "document/document.h"

/**
 * @brief 文档存储扩展点（抽象基类）。
 *
 * 按 URL scheme 分发（当前仅 file://，后续网络存储按新 scheme 注册）。
 * 内容约定：内存中一律 LF、无 BOM；写入时按 lineEnding 写回。
 */
class DocumentStorage : public QObject
{
    Q_OBJECT

public:
    struct LoadResult
    {
        QString content;
        Document::LineEnding lineEnding = Document::LineEnding::Lf;
        bool ok = false; ///< 成功标志：成功路径必须置 true；默认 false，防止实现漏设 error 时被误判为成功
    };

    explicit DocumentStorage(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    ~DocumentStorage() override = default;

    /// 本存储负责的 URL scheme，如 "file"
    virtual QString scheme() const = 0;

    /// 默认实现：url.scheme() == scheme()
    virtual bool supports(const QUrl &url) const
    {
        return url.scheme() == scheme();
    }

    /**
     * @brief 读取文档内容。
     * @param error 失败时置为非空错误消息（读取前清空）
     * @return 成功返回内容与换行风格；失败返回空内容
     */
    virtual LoadResult read(const QUrl &url, QString *error) = 0;

    /**
     * @brief 写入文档内容（实现方应保证原子写，避免半截文件）。
     * @param error 失败时置为非空错误消息
     */
    virtual bool write(const QUrl &url, const QString &content, Document::LineEnding lineEnding, QString *error) = 0;
};
