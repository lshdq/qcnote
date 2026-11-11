#include "core/documentmanager.h"

#include <algorithm>

#include "core/localstorage.h"

namespace core {

DocumentManager::DocumentManager(bool registerDefaults) {
    if (!registerDefaults)
        return;
    // 注册默认扩展点：纯文本兜底格式 + 本地文件存储（兜底格式最后生效）
    m_ownedPlainText = std::make_unique<PlainTextFormat>();
    m_plainTextFormat = m_ownedPlainText.get();
    m_formats.push_back(m_plainTextFormat);
    m_ownedLocalStorage = std::make_unique<LocalStorage>();
    m_storages.push_back(m_ownedLocalStorage.get());
}

DocumentManager::~DocumentManager() = default;

std::size_t DocumentManager::documentCount() const {
    return m_documents.size();
}

Document* DocumentManager::documentAt(std::size_t index) const {
    return index < m_documents.size() ? m_documents[index].get() : nullptr;
}

std::vector<Document*> DocumentManager::documents() const {
    std::vector<Document*> docs;
    docs.reserve(m_documents.size());
    for (const auto& doc : m_documents)
        docs.push_back(doc.get());
    return docs;
}

Document* DocumentManager::activeDocument() const {
    return m_activeDocument;
}

void DocumentManager::setActiveDocument(Document* document) {
    if (m_activeDocument == document)
        return;
    m_activeDocument = document;
    activeDocumentChanged.emit();
}

void DocumentManager::activate(std::size_t index) {
    if (index < m_documents.size())
        setActiveDocument(m_documents[index].get());
}

bool DocumentManager::displayNameInUse(const std::wstring& name) const {
    return std::any_of(m_documents.cbegin(), m_documents.cend(),
                       [&name](const std::unique_ptr<Document>& doc) {
                           return doc->displayName() == name;
                       });
}

Document* DocumentManager::newDocument() {
    // 编号器递增且跳过已占用显示名（M4 恢复未命名草稿沿用编号时防撞；
    // 显示名冲突可能来自「文件名恰为 未命名-N 的已打开文档」等场景）
    std::wstring name;
    do {
        ++m_untitledCounter;
        name = L"未命名-" + std::to_wstring(m_untitledCounter);
    } while (displayNameInUse(name));
    return appendDocument(std::make_unique<Document>(std::move(name)));
}

Document* DocumentManager::restoreUntitledDraft(const std::wstring& untitledName,
                                                int insertIndex) {
    // 显示名尽量沿用记录值；记录名形如 未命名-N 时推进计数器（后续 newDocument
    // 跳过占用号），与现有文档 displayName 冲突时按计数器生成新名
    //（旧 Qt 版 restoreUntitledDraft 同语义）
    std::wstring name = untitledName;
    if (name.empty()) {
        ++m_untitledCounter;
        name = L"未命名-" + std::to_wstring(m_untitledCounter);
    } else {
        constexpr wchar_t kPrefix[] = L"未命名-";
        const size_t prefixLen = std::wcslen(kPrefix);
        if (name.size() > prefixLen && name.compare(0, prefixLen, kPrefix) == 0) {
            unsigned long n = 0;
            bool digits = true;
            for (size_t i = prefixLen; i < name.size(); ++i) {
                if (name[i] < L'0' || name[i] > L'9') {
                    digits = false;
                    break;
                }
                n = n * 10 + static_cast<unsigned long>(name[i] - L'0');
            }
            if (digits)
                m_untitledCounter = (std::max)(m_untitledCounter, static_cast<int>(n));
        }
    }
    while (displayNameInUse(name)) {
        ++m_untitledCounter;
        name = L"未命名-" + std::to_wstring(m_untitledCounter);
    }

    auto doc = std::make_unique<Document>(std::move(name));
    Document* raw = doc.get();
    wireDocumentCallbacks(raw);
    // 插入位置：-1（默认哨兵）追加（既有调用方行为）；其余值（含越界正数与
    // 负数）钳制到 [0, size]（越界/草稿多于会话记录的场景按钳制处理，
    // 旧 Qt 版 qBound 同语义）
    const int count = static_cast<int>(m_documents.size());
    const int index =
        insertIndex == -1 ? count : (std::max)(0, (std::min)(insertIndex, count));
    m_documents.insert(m_documents.begin() + index, std::move(doc));
    // 先入列再发 documentsChanged（编辑器实例/事件订阅就位）；不激活——
    // 草稿恢复不抢占活动标签（TC-015），激活由 M4-B 按恢复计划统一执行
    documentsChanged.emit();
    return raw;
}

std::vector<OpenError> DocumentManager::openPaths(const std::vector<std::filesystem::path>& paths) {
    std::vector<OpenError> failures;
    for (const auto& path : paths) {
        auto result = openPath(path);
        if (!result)
            failures.push_back(std::move(result.error()));
    }
    return failures;
}

std::expected<Document*, OpenError>
DocumentManager::openPath(const std::filesystem::path& path, std::optional<TextEncoding> forced) {
    if (path.empty())
        return nullptr; // 空路径（非法输入）：跳过，不误激活未命名文档、不报错中断

    if (Document* existing = documentForPath(path)) {
        // 重复打开同一路径：激活已有文档，不新建（编码不变更——已在打开的文档以现状为准）
        setActiveDocument(existing);
        return existing;
    }

    DocumentStorage* storage = storageForUrl(path);
    if (!storage)
        return std::unexpected(OpenError{path, L"不支持的存储类型：" + path.wstring()});

    auto loaded = storage->read(path, forced);
    if (!loaded)
        return std::unexpected(OpenError{path, loaded.error().message});

    auto doc = std::make_unique<Document>(L"");
    doc->applyLoadedState(path, std::move(loaded->content), loaded->lineEnding, loaded->encoding,
                          loaded->hasBom);
    return appendDocument(std::move(doc));
}

Document* DocumentManager::appendDocument(std::unique_ptr<Document> doc) {
    Document* raw = doc.get();
    wireDocumentCallbacks(raw);
    m_documents.push_back(std::move(doc));
    documentsChanged.emit();
    setActiveDocument(raw); // 新建/打开即激活
    return raw;
}

void DocumentManager::wireDocumentCallbacks(Document* doc) {
    // Document 单订阅回调 → 管理器多播事件按文档转发（M2-B UI / M4 草稿订阅）
    doc->onContentChanged = [this, doc] { documentContentChanged.emit(doc); };
    doc->onModifiedChanged = [this, doc] { documentModifiedChanged.emit(doc); };
    doc->onMetadataChanged = [this, doc] { documentMetadataChanged.emit(doc); };
}

std::expected<void, StorageError> DocumentManager::save(Document* doc) {
    if (!doc)
        return std::unexpected(StorageError{StorageErrorKind::InvalidUrl, L"文档对象无效"});
    const std::filesystem::path& url = doc->url();
    if (url.empty())
        return std::unexpected(
            StorageError{StorageErrorKind::InvalidUrl, L"文档尚未保存到磁盘，请先另存为"});

    DocumentStorage* storage = storageForUrl(url);
    if (!storage)
        return std::unexpected(
            StorageError{StorageErrorKind::UnsupportedScheme,
                         L"不支持的存储类型：" + url.wstring()});

    auto written =
        storage->write(url, doc->content(), doc->encoding(), doc->hasBom(), doc->lineEnding());
    if (!written)
        return std::unexpected(written.error());
    doc->markSaved(); // 写盘成功：基线重置为当前内容（清脏）
    return {};
}

SaveAsResult DocumentManager::saveAs(Document* doc, const std::filesystem::path& path) {
    if (!doc || path.empty())
        return SaveAsResult{false, StorageError{StorageErrorKind::InvalidUrl, L"另存路径无效"}};

    DocumentStorage* storage = storageForUrl(path);
    if (!storage) {
        return SaveAsResult{false, StorageError{StorageErrorKind::UnsupportedScheme,
                                                L"不支持的存储类型：" + path.wstring()}};
    }

    // 另存按 doc 当前编码与 BOM 状态写出（未命名文档经 saveWithEncoding 设过编码后
    // 走此路径，BOM 取历史值）；遮蔽目标须在 setUrl 前定位（此后 doc 自身 url 即目标）
    const TextEncoding encoding = doc->encoding();
    Document* shadowed = documentForPath(path);

    auto written =
        storage->write(path, doc->content(), encoding, doc->hasBom(), doc->lineEnding());
    if (!written)
        return SaveAsResult{false, written.error()};

    doc->setUrl(path); // displayName 随 url 更新
    doc->markSaved();  // 写盘成功：基线重置（清脏）

    if (shadowed && shadowed != doc) {
        // 遮蔽重载（DEC-008）：磁盘刚被本内容覆盖，按刚写出的编码强制重读
        //（不自动识别：刚写盘字节编码已知，重识别存在 GBK 误判 UTF-8 风险），
        // 只同步内容与元数据（不动光标），被遮蔽文档随后恢复干净态
        auto reloaded = storage->read(path, encoding);
        if (!reloaded)
            return SaveAsResult{true, reloaded.error()};
        shadowed->applyLoadedState(path, std::move(reloaded->content), reloaded->lineEnding,
                                   reloaded->encoding, reloaded->hasBom);
    }
    return SaveAsResult{true, std::nullopt};
}

void DocumentManager::close(std::size_t index) {
    if (index >= m_documents.size())
        return;
    Document* doc = m_documents[index].get();
    Document* prev = index > 0 ? m_documents[index - 1].get() : nullptr;
    Document* next =
        index + 1 < m_documents.size() ? m_documents[index + 1].get() : nullptr;

    // 先移出列表（集合变更），再发 documentClosed（订阅者仍可安全访问 doc——
    // 草稿清理需读 uid），unique_ptr 持有至函数返回才析构
    std::unique_ptr<Document> held = std::move(m_documents[index]);
    m_documents.erase(m_documents.begin() + static_cast<std::ptrdiff_t>(index));
    documentClosed.emit(doc);
    documentsChanged.emit();
    if (m_activeDocument == doc)
        setActiveDocument(prev ? prev : next); // 优先前一个，其次后一个，无则置空
}

void DocumentManager::closeDocument(Document* doc) {
    const auto it = std::find_if(m_documents.cbegin(), m_documents.cend(),
                                 [doc](const std::unique_ptr<Document>& d) { return d.get() == doc; });
    if (it != m_documents.cend())
        close(static_cast<std::size_t>(it - m_documents.cbegin()));
}

std::expected<void, StorageError> DocumentManager::reloadWithEncoding(Document* doc,
                                                                      TextEncoding encoding) {
    if (!doc)
        return std::unexpected(StorageError{StorageErrorKind::InvalidUrl, L"文档对象无效"});
    const std::filesystem::path& url = doc->url();
    if (url.empty()) {
        return std::unexpected(StorageError{
            StorageErrorKind::InvalidUrl, L"文档尚未保存到磁盘，无法按编码重新加载"});
    }

    DocumentStorage* storage = storageForUrl(url);
    if (!storage) {
        return std::unexpected(StorageError{StorageErrorKind::UnsupportedScheme,
                                            L"不支持的存储类型：" + url.wstring()});
    }

    auto loaded = storage->read(url, encoding);
    if (!loaded) {
        // 读取失败：保留原文档内容（不产生半更新状态）
        return std::unexpected(loaded.error());
    }

    // 刷新内容/换行/编码/BOM（不置脏、不动光标），随后把光标钳制到新内容长度内
    //（光标为字节偏移，见 Document::cursorPosition）
    const std::size_t newLength = loaded->content.size();
    doc->applyLoadedState(url, std::move(loaded->content), loaded->lineEnding, loaded->encoding,
                          loaded->hasBom);
    if (doc->cursorPosition() > static_cast<int>(newLength))
        doc->setCursorPosition(static_cast<int>(newLength));
    return {};
}

std::expected<void, StorageError> DocumentManager::saveWithEncoding(Document* doc,
                                                                    TextEncoding encoding) {
    if (!doc)
        return std::unexpected(StorageError{StorageErrorKind::InvalidUrl, L"文档对象无效"});

    const TextEncoding previous = doc->encoding();
    doc->setEncoding(encoding); // 先设编码：未命名文档也要携带（后续另存为按此编码写出）

    const std::filesystem::path& url = doc->url();
    if (url.empty())
        return std::unexpected(StorageError{
            StorageErrorKind::InvalidUrl,
            L"文档尚未保存到磁盘，请先另存为"}); // 编码已设置并保留（旧语义），UI 转另存为

    DocumentStorage* storage = storageForUrl(url);
    if (!storage) {
        doc->setEncoding(previous); // 回滚：保存未发生，编码标记不得误导状态栏
        return std::unexpected(StorageError{StorageErrorKind::UnsupportedScheme,
                                            L"不支持的存储类型：" + url.wstring()});
    }

    // UTF-8 目标按 BOM 历史写回；GBK 目标 hasBom 由 LocalStorage 忽略
    auto written =
        storage->write(url, doc->content(), encoding, doc->hasBom(), doc->lineEnding());
    if (!written) {
        doc->setEncoding(previous); // 回滚（GBK 不可编码等失败时编码标记不产生误导）
        return std::unexpected(written.error()); // 磁盘未被触碰，基线不更新、脏标记保留
    }
    doc->markSaved();
    return {};
}

Document* DocumentManager::documentForPath(const std::filesystem::path& path) const {
    if (path.empty())
        return nullptr; // 空 path 不对应任何文档（未命名文档不可按空 path 检索）
    const std::filesystem::path normalized = path.lexically_normal();
    for (const auto& doc : m_documents) {
        if (doc->url().lexically_normal() == normalized)
            return doc.get();
    }
    return nullptr;
}

bool DocumentManager::hasUnsavedChanges() const {
    return std::any_of(m_documents.cbegin(), m_documents.cend(),
                       [](const std::unique_ptr<Document>& doc) { return doc->modified(); });
}

void DocumentManager::registerFormat(DocumentFormat& format) {
    // 警告：注册恒匹配格式会遮蔽其后注册的具体格式（formatForUrl 仅跳过内置兜底）
    if (std::find(m_formats.begin(), m_formats.end(), &format) != m_formats.end())
        return; // 重复注册忽略
    m_formats.push_back(&format);
}

DocumentFormat* DocumentManager::formatForPath(const std::filesystem::path& path) const {
    const std::wstring fileName = path.filename().wstring();
    // 兜底格式最后生效：必须先让具体格式参与匹配，否则后注册的扩展格式永远不可达
    for (const DocumentFormat* format : m_formats) {
        if (format == m_plainTextFormat)
            continue; // 跳过兜底格式
        if (format->matchesFileName(fileName))
            return const_cast<DocumentFormat*>(format);
    }
    return m_plainTextFormat; // 无具体格式命中：兜底为纯文本（可能为 nullptr，未注册时）
}

std::vector<std::wstring> DocumentManager::nameFilters() const {
    std::vector<std::wstring> filters;
    for (const DocumentFormat* format : m_formats) {
        for (const std::wstring& filter : format->nameFilters()) {
            if (std::find(filters.begin(), filters.end(), filter) == filters.end())
                filters.push_back(filter); // 保序去重
        }
    }
    return filters;
}

void DocumentManager::registerStorage(DocumentStorage& storage) {
    if (std::find(m_storages.begin(), m_storages.end(), &storage) != m_storages.end())
        return;
    m_storages.push_back(&storage);
}

DocumentStorage* DocumentManager::storageForUrl(const std::filesystem::path& path) const {
    for (DocumentStorage* storage : m_storages) {
        if (storage->supports(path))
            return storage;
    }
    return nullptr; // 未注册 scheme：调用方报「不支持的存储类型」
}

} // namespace core
