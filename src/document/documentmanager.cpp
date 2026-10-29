#include "document/documentmanager.h"

#include <QQmlEngine>
#include <QJSEngine>

#include <algorithm>

#include "format/documentformat.h"
#include "format/plaintextformat.h"
#include "storage/localstorage.h"

DocumentManager *DocumentManager::s_instance = nullptr;

DocumentManager::DocumentManager(QObject *parent)
    : QObject(parent)
{
    // QML 增量列表模型（与 m_documents 同步维护，见 newDocument/openUrls/closeDocument）
    m_documentsModel = new DocumentListModel(this);
    // 注册默认扩展点：纯文本兜底格式 + 本地文件存储
    m_plainTextFormat = new PlainTextFormat(this);
    m_formats.append(m_plainTextFormat);
    m_storages.append(new LocalStorage(this));
}

DocumentManager::~DocumentManager()
{
    if (s_instance == this)
        s_instance = nullptr;
}

DocumentManager *DocumentManager::create(QQmlEngine *qmlEngine, QJSEngine *jsEngine)
{
    Q_UNUSED(qmlEngine)
    Q_UNUSED(jsEngine)
    // 单例生命周期说明：实例无 QObject 父对象，由 C++ 侧持有直至进程退出；
    // 所有权固定为 CppOwnership（QML 引擎不负责销毁）。
    // 仅支持单 QML 引擎使用：多引擎共享同一实例是未定义行为，当前不支持。
    if (!s_instance)
        s_instance = new DocumentManager();
    QJSEngine::setObjectOwnership(s_instance, QQmlEngine::CppOwnership);
    return s_instance;
}

QList<Document *> DocumentManager::documents() const
{
    return m_documents;
}

DocumentListModel *DocumentManager::documentsModel() const
{
    return m_documentsModel;
}

Document *DocumentManager::activeDocument() const
{
    return m_activeDocument;
}

void DocumentManager::setActiveDocument(Document *document)
{
    if (m_activeDocument == document)
        return;
    m_activeDocument = document;
    emit activeDocumentChanged();
}

void DocumentManager::newDocument()
{
    ++m_untitledCounter;
    auto *doc = new Document(QStringLiteral("未命名-%1").arg(m_untitledCounter), this);
    m_documents.append(doc);
    // 先建 model 行（rowsInserted，视图只新增对应 delegate），再发 documentsChanged
    m_documentsModel->insert(m_documents.size() - 1, doc);
    emit documentsChanged();
    setActiveDocument(doc);
}

void DocumentManager::openUrls(const QList<QUrl> &urls)
{
    for (const QUrl &url : urls) {
        if (url.isEmpty())
            continue; // 空 URL（非法输入）：跳过，不误激活未命名文档、不报错中断

        if (Document *existing = documentForUrl(url)) {
            // 重复打开同一 URL：激活已有文档，不新建
            setActiveDocument(existing);
            continue;
        }

        DocumentStorage *storage = storageForUrl(url);
        if (!storage) {
            emit openFailed(url, tr("没有可用于打开该地址的存储：%1").arg(url.toString()));
            continue;
        }

        QString error;
        const DocumentStorage::LoadResult loaded = storage->read(url, &error);
        // 双保险：ok 未置位（存储实现漏设 error 的防御）或 error 非空均视为失败
        if (!loaded.ok || !error.isEmpty()) {
            emit openFailed(url, error.isEmpty() ? tr("读取失败：%1").arg(url.toString()) : error);
            continue; // 失败不中断后续 URL 的处理
        }

        auto *doc = new Document(QString(), this);
        doc->applyLoadedState(url, loaded.content, loaded.lineEnding);
        m_documents.append(doc);
        // 先建 model 行（rowsInserted，视图只新增对应 delegate），再发 documentsChanged
        m_documentsModel->insert(m_documents.size() - 1, doc);
        emit documentsChanged();
        emit documentOpened(doc);
        setActiveDocument(doc);
    }
}

bool DocumentManager::save(Document *doc)
{
    if (!doc)
        return false;

    const QUrl url = doc->url();
    if (url.isEmpty())
        return false; // 无路径：UI 应转另存为流程

    DocumentStorage *storage = storageForUrl(url);
    if (!storage) {
        emit saveFailed(doc, tr("没有可用于保存该地址的存储：%1").arg(url.toString()));
        return false;
    }

    QString error;
    if (!storage->write(url, doc->content(), doc->lineEnding(), &error)) {
        emit saveFailed(doc, error.isEmpty() ? tr("写入失败：%1").arg(url.toString()) : error);
        return false;
    }
    doc->setModified(false);
    return true;
}

bool DocumentManager::saveAs(Document *doc, const QUrl &url)
{
    if (!doc || url.isEmpty())
        return false;

    DocumentStorage *storage = storageForUrl(url);
    if (!storage) {
        emit saveFailed(doc, tr("没有可用于保存该地址的存储：%1").arg(url.toString()));
        return false;
    }

    QString error;
    if (!storage->write(url, doc->content(), doc->lineEnding(), &error)) {
        emit saveFailed(doc, error.isEmpty() ? tr("写入失败：%1").arg(url.toString()) : error);
        return false;
    }

    // 另存目标若已被其他已打开文档占用：磁盘文件刚被本内容覆盖，须在 setUrl 前先找到
    // 对方文档并重载，否则其内存旧内容（modified=false）会在后续保存时静默覆盖新内容
    Document *shadowed = documentForUrl(url);

    doc->setUrl(url); // displayName 随 url 更新
    doc->setModified(false);

    if (shadowed && shadowed != doc) {
        QString reloadError;
        const DocumentStorage::LoadResult reloaded = storage->read(url, &reloadError);
        if (!reloaded.ok || !reloadError.isEmpty()) {
            // 重载失败：对方文档内存态已过期，发 openFailed 提示（其 url 仍指向被覆盖的文件）
            emit openFailed(url, reloadError.isEmpty()
                                     ? tr("重新读取被覆盖的文件失败：%1").arg(url.toString())
                                     : reloadError);
        } else {
            // 只同步内容与换行风格并清除修改标记（内容已等价磁盘），不动光标等 UI 状态
            shadowed->applyLoadedState(url, reloaded.content, reloaded.lineEnding);
        }
    }
    return true;
}

void DocumentManager::closeDocument(Document *doc)
{
    const int index = m_documents.indexOf(doc);
    if (index < 0)
        return;

    Document *prev = index > 0 ? m_documents.at(index - 1) : nullptr;
    Document *next = index + 1 < m_documents.size() ? m_documents.at(index + 1) : nullptr;

    // 先删 model 行（rowsRemoved，视图只销毁对应 delegate），再同步 C++ 列表
    m_documentsModel->removeAt(index);
    m_documents.removeAt(index);
    emit documentClosed(doc);
    emit documentsChanged();

    if (m_activeDocument == doc)
        setActiveDocument(prev ? prev : next); // 优先前一个，其次后一个

    doc->deleteLater();
}

Document *DocumentManager::documentForUrl(const QUrl &url) const
{
    if (url.isEmpty())
        return nullptr; // 空 URL 不对应任何文档（未命名文档 url 虽为空，但不可按空 URL 检索）
    for (Document *doc : m_documents) {
        if (doc->url() == url)
            return doc;
    }
    return nullptr;
}

bool DocumentManager::hasUnsavedChanges() const
{
    return std::any_of(m_documents.cbegin(), m_documents.cend(),
                       [](const Document *doc) { return doc->modified(); });
}

QStringList DocumentManager::nameFilters() const
{
    QStringList filters;
    for (const DocumentFormat *format : m_formats)
        filters.append(format->nameFilters());
    filters.removeDuplicates();
    return filters;
}

void DocumentManager::registerFormat(DocumentFormat *format)
{
    if (!format || m_formats.contains(format))
        return; // 空指针与重复注册忽略
    m_formats.append(format);
}

DocumentFormat *DocumentManager::formatForUrl(const QUrl &url) const
{
    const QString fileName = url.fileName();
    // 兜底格式最后生效：PlainTextFormat::matchesFileName 恒真，
    // 必须先让具体格式参与匹配，否则后注册的扩展格式永远不可达
    for (DocumentFormat *format : m_formats) {
        if (format == m_plainTextFormat)
            continue; // 跳过兜底格式
        if (format->matchesFileName(fileName))
            return format;
    }
    return m_plainTextFormat; // 无具体格式命中：兜底为纯文本
}

DocumentStorage *DocumentManager::storageForUrl(const QUrl &url) const
{
    for (DocumentStorage *storage : m_storages) {
        if (storage->supports(url))
            return storage;
    }
    return nullptr;
}
