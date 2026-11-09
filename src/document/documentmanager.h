#pragma once

#include <QtQmlIntegration/qqmlintegration.h>
#include <QList>
#include <QObject>
#include <QUrl>

#include "document/document.h"
#include "document/documentlistmodel.h"

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
    /// QML 视图专用：行级增量模型（增删只通知对应行，delegate 不全量重建）。
    /// C++ 侧请继续使用 documents()（QList 直读）。
    Q_PROPERTY(DocumentListModel *documentsModel READ documentsModel CONSTANT)
    Q_PROPERTY(Document *activeDocument READ activeDocument WRITE setActiveDocument NOTIFY activeDocumentChanged)

public:
    explicit DocumentManager(QObject *parent = nullptr);
    ~DocumentManager() override;

    /// QML 单例工厂：复用同一实例（生命周期归 C++ 管理）
    static DocumentManager *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);

    /// 取当前实例（可能为 nullptr），供 SessionManager 等其他单例获取既有管理器
    static DocumentManager *instance();

    QList<Document *> documents() const;
    /// 返回内部模型的同一实例（构造时创建、以本管理器为父，CONSTANT 不变）
    DocumentListModel *documentsModel() const;
    Document *activeDocument() const;
    void setActiveDocument(Document *document);

    /// 新建"未命名-N"文档（N 从 1 递增）并激活
    Q_INVOKABLE void newDocument();

    /**
     * @brief 恢复未命名草稿：创建文档并写入内容（基线为空 → modified 必为 true）。
     *
     * 显示名尽量沿用 untitledName，与现有文档 displayName 冲突时按未命名计数器
     * 生成新名；记录名形如 未命名-N 时同步推进计数器（后续 newDocument 跳过占用号）。
     * 只追加文档（建 model 行、发 documentsChanged），不激活、不发 documentOpened
     * （草稿恢复不抢占活动标签，见需求 TC-015）。
     */
    Document *restoreUntitledDraft(const QString &content, const QString &untitledName);

    /// 批量打开 URL；同 url 已存在则激活已有文档；空 URL 跳过；失败发 openFailed 并继续处理后续 URL
    Q_INVOKABLE void openUrls(const QList<QUrl> &urls);

    /// 单 URL 按指定编码打开（纠正识别错误的打开入口；会话按记忆编码恢复也走此路径）。
    /// 同 url 已存在则激活已有文档；空 URL 跳过；失败发 openFailed
    Q_INVOKABLE void openUrlWithEncoding(const QUrl &url, Document::Encoding encoding);

    /**
     * @brief 按指定编码从磁盘重新加载（纠正自动识别错误）。
     *
     * 以磁盘内容为准覆盖内存（未保存修改被丢弃；确认框由 QML 层负责，DEC-008）。
     * 成功后刷新内容/换行/编码/BOM，不置脏，光标钳制到新内容长度内；
     * url 为空（未命名）发 openFailed 提示并返回；读取失败发 openFailed 且保留原文档内容。
     */
    Q_INVOKABLE void reloadWithEncoding(Document *doc, Document::Encoding encoding);

    /**
     * @brief 按指定编码立即保存（编码转换，DEC-005 一步完成：设编码 + 保存）。
     *
     * UTF-8 目标 writeBom 取 doc->hasBom()；GBK 目标忽略 BOM。成功清除修改标记；
     * 无 url（未命名）保留已设编码返回 false（UI 转另存为，另存为按已设编码写出）；
     * 失败（含 GBK 不可编码）回滚编码设置、发 saveFailed 返回 false（编码标记不产生误导）。
     */
    Q_INVOKABLE bool saveWithEncoding(Document *doc, Document::Encoding encoding);

    /// 保存；url 为空返回 false（UI 应转另存为）；失败发 saveFailed。按 doc 的编码与 BOM 状态写出
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

    /**
     * @brief 打开单个 URL 的公共路径（openUrls 与 openUrlWithEncoding 共用）。
     *
     * forcedEncoding 有值时按指定编码强制解码，否则自动识别；重复 URL 激活已有
     * 文档；空 URL 返回 nullptr（不报错）；失败发 openFailed 返回 nullptr。
     * 成功则新建文档（应用 encoding/hasBom 元数据）并激活。
     */
    Document *openSingle(const QUrl &url, const std::optional<Document::Encoding> &forcedEncoding);

    QList<Document *> m_documents;
    DocumentListModel *m_documentsModel = nullptr;
    Document *m_activeDocument = nullptr;
    int m_untitledCounter = 0;
    QList<DocumentFormat *> m_formats;
    QList<DocumentStorage *> m_storages;
    DocumentFormat *m_plainTextFormat = nullptr;
};
