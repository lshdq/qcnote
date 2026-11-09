#include "document/documentmanager.h"

#include <QQmlEngine>
#include <QJSEngine>
#include <QRegularExpression>

#include <algorithm>
#include <optional>

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
    // 记录最新实例，供 instance() 获取（构造即登记；析构时置空）
    s_instance = this;
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

DocumentManager *DocumentManager::instance()
{
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

Document *DocumentManager::restoreUntitledDraft(const QString &content, const QString &untitledName)
{
    // 显示名尽量沿用记录值；记录名形如 未命名-N 时推进计数器（后续 newDocument
    // 跳过占用号），与现有文档 displayName 冲突时按计数器生成新名
    QString name = untitledName;
    if (name.isEmpty()) {
        ++m_untitledCounter;
        name = QStringLiteral("未命名-%1").arg(m_untitledCounter);
    } else {
        static const QRegularExpression numbered(QStringLiteral("^未命名-(\\d+)$"));
        const auto match = numbered.match(name);
        if (match.hasMatch())
            m_untitledCounter = qMax(m_untitledCounter, match.captured(1).toInt());
    }
    const auto nameExists = [this](const QString &candidate) {
        for (const Document *doc : m_documents) {
            if (doc->displayName() == candidate)
                return true;
        }
        return false;
    };
    while (nameExists(name)) {
        ++m_untitledCounter;
        name = QStringLiteral("未命名-%1").arg(m_untitledCounter);
    }

    auto *doc = new Document(name, this);
    m_documents.append(doc);
    // 先建 model 行（rowsInserted，视图只新增对应 delegate），再发 documentsChanged；
    // 不激活、不发 documentOpened（草稿恢复不抢占活动标签，TC-015）
    m_documentsModel->insert(m_documents.size() - 1, doc);
    emit documentsChanged();
    // 入列后再写内容：DraftCacheManager 等经 documentsChanged 补连信号后，
    // setContent 触发的 contentChanged 按新 uid 正常进入草稿续写防抖（预期行为）
    doc->setContent(content);
    return doc;
}

void DocumentManager::openUrls(const QList<QUrl> &urls)
{
    for (const QUrl &url : urls)
        openSingle(url, std::nullopt); // 失败已在 openSingle 内发 openFailed，继续后续 URL
}

void DocumentManager::openUrlWithEncoding(const QUrl &url, Document::Encoding encoding)
{
    openSingle(url, encoding);
}

Document *DocumentManager::openSingle(const QUrl &url,
                                      const std::optional<Document::Encoding> &forcedEncoding)
{
    if (url.isEmpty())
        return nullptr; // 空 URL（非法输入）：跳过，不误激活未命名文档、不报错中断

    if (Document *existing = documentForUrl(url)) {
        // 重复打开同一 URL：激活已有文档，不新建（编码不变更——已在打开的文档以现状为准）
        setActiveDocument(existing);
        return existing;
    }

    DocumentStorage *storage = storageForUrl(url);
    if (!storage) {
        emit openFailed(url, tr("没有可用于打开该地址的存储：%1").arg(url.toString()));
        return nullptr;
    }

    QString error;
    const DocumentStorage::LoadResult loaded = forcedEncoding
            ? storage->read(url, *forcedEncoding, &error)
            : storage->read(url, &error);
    // 双保险：ok 未置位（存储实现漏设 error 的防御）或 error 非空均视为失败
    if (!loaded.ok || !error.isEmpty()) {
        emit openFailed(url, error.isEmpty() ? tr("读取失败：%1").arg(url.toString()) : error);
        return nullptr;
    }

    auto *doc = new Document(QString(), this);
    doc->applyLoadedState(url, loaded.content, loaded.lineEnding, loaded.encoding, loaded.hasBom);
    m_documents.append(doc);
    // 先建 model 行（rowsInserted，视图只新增对应 delegate），再发 documentsChanged
    m_documentsModel->insert(m_documents.size() - 1, doc);
    emit documentsChanged();
    emit documentOpened(doc);
    setActiveDocument(doc);
    return doc;
}

void DocumentManager::reloadWithEncoding(Document *doc, Document::Encoding encoding)
{
    if (!doc)
        return; // 空指针防御：QML 传错对象时静默忽略
    const QUrl url = doc->url();
    if (url.isEmpty()) {
        emit openFailed(url, tr("文档尚未保存到磁盘，无法按编码重新加载"));
        return;
    }

    DocumentStorage *storage = storageForUrl(url);
    if (!storage) {
        emit openFailed(url, tr("没有可用于读取该地址的存储：%1").arg(url.toString()));
        return;
    }

    QString error;
    const DocumentStorage::LoadResult loaded = storage->read(url, encoding, &error);
    if (!loaded.ok || !error.isEmpty()) {
        // 读取失败：保留原文档内容（不产生半更新状态）
        emit openFailed(url, error.isEmpty() ? tr("重新读取文件失败：%1").arg(url.toString()) : error);
        return;
    }

    // 刷新内容/换行/编码/BOM（不置脏、不动光标），随后把光标钳制到新内容长度内
    doc->applyLoadedState(url, loaded.content, loaded.lineEnding, loaded.encoding, loaded.hasBom);
    if (doc->cursorPosition() > loaded.content.size())
        doc->setCursorPosition(static_cast<int>(loaded.content.size()));
}

bool DocumentManager::saveWithEncoding(Document *doc, Document::Encoding encoding)
{
    if (!doc)
        return false;

    const Document::Encoding previous = doc->encoding();
    doc->setEncoding(encoding); // 先设编码：未命名文档也要携带（后续另存为按此编码写出）

    const QUrl url = doc->url();
    if (url.isEmpty())
        return false; // 未命名：UI 应转另存为流程（编码已设置并保留，不发 saveFailed）

    DocumentStorage *storage = storageForUrl(url);
    if (!storage) {
        doc->setEncoding(previous); // 回滚：保存未发生，编码标记不得误导状态栏
        emit saveFailed(doc, tr("没有可用于保存该地址的存储：%1").arg(url.toString()));
        return false;
    }

    QString error;
    // UTF-8 目标按 BOM 历史写回；GBK 目标 writeBom 由 LocalStorage 忽略
    if (!storage->write(url, doc->content(), doc->lineEnding(), encoding, doc->hasBom(), &error)) {
        doc->setEncoding(previous); // 回滚（DEC-005：GBK 不可编码等失败时编码标记不产生误导）
        emit saveFailed(doc, error.isEmpty() ? tr("写入失败：%1").arg(url.toString()) : error);
        return false; // 磁盘未被触碰（不可编码校验先于写盘），基线不更新、脏标记保留
    }
    doc->markSaved(); // 写盘成功：基线重置为当前内容（modified 清脏）
    return true;
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
    if (!storage->write(url, doc->content(), doc->lineEnding(), doc->encoding(), doc->hasBom(),
                        &error)) {
        emit saveFailed(doc, error.isEmpty() ? tr("写入失败：%1").arg(url.toString()) : error);
        return false;
    }
    doc->markSaved(); // 写盘成功：基线重置为当前内容（modified 清脏）
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

    // 另存为按 doc 当前编码与 BOM 状态写出（未命名文档经 saveWithEncoding 设过编码后走此路径）
    const Document::Encoding encoding = doc->encoding();
    QString error;
    if (!storage->write(url, doc->content(), doc->lineEnding(), encoding, doc->hasBom(), &error)) {
        emit saveFailed(doc, error.isEmpty() ? tr("写入失败：%1").arg(url.toString()) : error);
        return false;
    }

    // 另存目标若已被其他已打开文档占用：磁盘文件刚被本内容覆盖，须在 setUrl 前先找到
    // 对方文档并重载，否则其内存旧内容（modified=false）会在后续保存时静默覆盖新内容
    Document *shadowed = documentForUrl(url);

    doc->setUrl(url); // displayName 随 url 更新
    doc->markSaved(); // 写盘成功：基线重置为当前内容（modified 清脏）

    if (shadowed && shadowed != doc) {
        QString reloadError;
        // 按刚写出的编码强制重读（不自动识别：刚写盘的字节编码已知，重识别存在
        // GBK 字节被误判 UTF-8 的理论风险），只同步内容与元数据并清除修改标记
        const DocumentStorage::LoadResult reloaded = storage->read(url, encoding, &reloadError);
        if (!reloaded.ok || !reloadError.isEmpty()) {
            // 重载失败：对方文档内存态已过期，发 openFailed 提示（其 url 仍指向被覆盖的文件）
            emit openFailed(url, reloadError.isEmpty()
                                     ? tr("重新读取被覆盖的文件失败：%1").arg(url.toString())
                                     : reloadError);
        } else {
            // 不动光标等 UI 状态（内容已等价磁盘）
            shadowed->applyLoadedState(url, reloaded.content, reloaded.lineEnding,
                                       reloaded.encoding, reloaded.hasBom);
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
    // 警告：注册恒匹配格式（如 id 为 "plaintext" 的其他实例）会遮蔽其后注册的
    // 具体格式；formatForUrl 仅跳过内置兜底实例，无法跳过这些外部实例。
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
