#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

#include "document/document.h"

/**
 * @brief 文档存储扩展点（抽象基类）。
 *
 * 按 URL scheme 分发（当前仅 file://，后续网络存储按新 scheme 注册）。
 * 内容约定：内存中一律 LF、无 BOM（BOM 是元数据不进内容）；写入时按
 * lineEnding 写回换行、按 encoding 写回编码（UTF-8 可选 BOM；GBK 内容含
 * 不可编码字符时必须写盘前失败，不得静默替换）。
 */
class DocumentStorage : public QObject
{
    Q_OBJECT

public:
    struct LoadResult
    {
        QString content;
        Document::LineEnding lineEnding = Document::LineEnding::Lf;
        Document::Encoding encoding = Document::Encoding::Utf8; ///< 实际使用的编码（自动识别结果或强制值）
        bool hasBom = false; ///< 磁盘是否带 UTF-8 BOM（已剥离，不进内容）；GBK 恒 false
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
     * @brief 读取文档内容（自动识别编码）。
     * @param error 失败时置为非空错误消息（读取前清空）
     * @return 成功返回内容、换行风格与编码元数据；失败返回空内容
     */
    virtual LoadResult read(const QUrl &url, QString *error) = 0;

    /**
     * @brief 读取文档内容（按指定编码强制解码）。
     *
     * 用于纠正自动识别错误（按编码重新加载）与会话按记忆编码恢复：
     * 强制 Utf8 且磁盘带 BOM → 剥离且 hasBom=true；强制 Gbk → hasBom=false。
     * 解码失败的序列以替换字符呈现（保证文件可打开），不算读取失败。
     */
    virtual LoadResult read(const QUrl &url, Document::Encoding forcedEncoding, QString *error) = 0;

    /**
     * @brief 写入文档内容（实现方应保证原子写，避免半截文件）。
     *
     * encoding==Utf8 时 writeBom 控制 BOM 写回（有 BOM 历史的文档保持）；
     * encoding==Gbk 时忽略 writeBom（GBK 无 BOM 概念）。GBK 不可编码校验必须
     * 在写盘前完成：任一字符无直接 CP936 映射即返回 false（error 含字符与行号），
     * 磁盘不被触碰。
     * @param error 失败时置为非空错误消息
     */
    virtual bool write(const QUrl &url, const QString &content, Document::LineEnding lineEnding,
                       Document::Encoding encoding, bool writeBom, QString *error) = 0;
};
