#pragma once

#include <QtQmlIntegration/qqmlintegration.h>
#include <QList>
#include <QObject>
#include <QUrl>

#include "document/document.h"

class DocumentFormat;
class DocumentStorage;
class QQmlEngine;
class QJSEngine;

/**
 * @brief 文档生命周期管理器（QML 单例）。
 *
 * 可脱离 QML 引擎在 C++ 测试中直接 new 使用；QML 侧通过
 * create(QQmlEngine*, QJSEngine*) 复用同一实例。
 * 构造函数中注册 PlainTextFormat（兜底格式）与 LocalStorage；
 * 其他格式经 registerFormat() 注册（兜底格式最后生效）。
 */
class DocumentManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(QList<Document *> documents READ documents NOTIFY documentsChanged)
    Q_PROPERTY(Document *activeDocument READ activeDocument WRITE setActiveDocument NOTIFY activeDocumentChanged)

public:
    explicit DocumentManager(QObject *parent = nullptr);
    ~DocumentManager() override;

    /// QML 单例工厂：复用同一实例（生命周期归 C++ 管理）
    static DocumentManager *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);

    QList<Document *> documents() const;
    Document *activeDocument() const;
    void setActiveDocument(Document *document);

    /// 新建"未命名-N"文档（N 从 1 递增）并激活
    Q_INVOKABLE void newDocument();

    /// 批量打开 URL；同 url 已存在则激活已有文档；空 URL 跳过；失败发 openFailed 并继续处理后续 URL
    Q_INVOKABLE void openUrls(const QList<QUrl> &urls);

    /// 保存；url 为空返回 false（UI 应转另存为）；失败发 saveFailed
    Q_INVOKABLE bool save(Document *doc);

    /// 另存为；成功后更新 doc 的 url（displayName 随之）并清除修改标记。
    /// 若目标 url 同时是其他已打开文档的 URL，写盘成功后会从磁盘重载该文档内容（防止旧内存态再次覆盖磁盘）
    Q_INVOKABLE bool saveAs(Document *doc, const QUrl &url);

    /// 关闭文档；激活邻近文档（优先前一个，其次后一个，无则 nullptr）
    Q_INVOKABLE void closeDocument(Document *doc);

    Q_INVOKABLE Document *documentForUrl(const QUrl &url) const;

    /// 是否存在未保存修改（任一文档 modified）
    Q_INVOKABLE bool hasUnsavedChanges() const;

    /// 聚合所有已注册格式的文件名过滤器（去重，供 FileDialog 使用）
    Q_INVOKABLE QStringList nameFilters() const;

    /// 注册文档格式扩展点（格式扩展入口，如 Markdown/JSON）。
    /// 不取得所有权：调用方保证格式对象生命周期覆盖管理器使用期（如以管理器为 QObject 父）。
    /// 重复注册（同指针）与空指针被忽略。
    void registerFormat(DocumentFormat *format);

    /// 按注册顺序找 matchesFileName 的格式；兜底 PlainTextFormat 恒真、最后生效（具体格式优先）
    DocumentFormat *formatForUrl(const QUrl &url) const;

    /// 按 supports() 匹配 scheme；无匹配返回 nullptr
    DocumentStorage *storageForUrl(const QUrl &url) const;

signals:
    void documentsChanged();
    void activeDocumentChanged();
    void documentOpened(Document *doc);
    void documentClosed(Document *doc);
    void openFailed(const QUrl &url, const QString &error);
    void saveFailed(Document *doc, const QString &error);

private:
    static DocumentManager *s_instance;

    QList<Document *> m_documents;
    Document *m_activeDocument = nullptr;
    int m_untitledCounter = 0;
    QList<DocumentFormat *> m_formats;
    QList<DocumentStorage *> m_storages;
    DocumentFormat *m_plainTextFormat = nullptr;
};
