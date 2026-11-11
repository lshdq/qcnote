// qcnote 纯逻辑核心层：文档生命周期管理器（M2-A，旧 documentmanager.cpp 语义移植）。
//
// 职责：文档集合（有序，对应 Tab 顺序）+ 活动文档；新建（未命名-N 递增跳过
// 占用）、打开（重复路径激活已有、失败收集不中断）、保存/另存为（遮蔽其他已
// 打开文档时写盘后从磁盘重载被遮蔽文档——旧初始化 DEC-008）、关闭（激活邻居）、
// 按编码重载（换行重探测、光标钳制、不置脏）/按编码保存（一步转换、失败回滚）。
// 无窗口依赖；文件对话框不在本层（M2-B UI）。
// 通知机制（替代 Qt 信号）：MulticastEvent 多播事件，可多订阅（M2-B UI 与
// M4 会话/草稿同时订阅）；Document 级变更经单订阅回调接线后按文档转发。
#pragma once

#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "core/document.h"
#include "core/format.h"
#include "core/storage.h"

namespace core {

/// 多播事件（可多订阅；subscribe 返回 token，unsubscribe 退订）。
/// emit 时对当前订阅快照顺序调用；回调内退订的处理器本次仍可能被调用
/// （快照语义），新订阅的处理器本次不调用。
template <typename... Args>
class MulticastEvent {
public:
    using Handler = std::function<void(Args...)>;
    using Token = int;

    Token subscribe(Handler handler) {
        const Token token = ++m_nextToken;
        m_entries.push_back(Entry{token, std::move(handler)});
        return token;
    }

    void unsubscribe(Token token) {
        m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                       [token](const Entry& e) { return e.token == token; }),
                        m_entries.end());
    }

    void emit(Args... args) const {
        const std::vector<Entry> snapshot = m_entries;
        for (const Entry& e : snapshot) {
            if (e.handler)
                e.handler(args...);
        }
    }

private:
    struct Entry {
        Token token;
        Handler handler;
    };
    std::vector<Entry> m_entries;
    Token m_nextToken = 0;
};

/// 单个路径打开失败（路径 + 用户可见消息）
struct OpenError {
    std::filesystem::path path;
    std::wstring message;
};

/// 另存为结果：写盘成败 + 可能的错误。
/// written=false 且 error 有值：写盘失败（文档状态未变）；
/// written=true 且 error 有值：写盘成功但被遮蔽文档从磁盘重载失败
/// （极少见：写盘后磁盘又被第三方破坏；文档本身已更新成功）。
struct SaveAsResult {
    bool written = false;
    std::optional<StorageError> error;
};

class DocumentManager {
public:
    /// @param registerDefaults false 时不注册内置纯文本格式与本地存储
    ///（测试/扩展注入用；生产路径默认注册）
    explicit DocumentManager(bool registerDefaults = true);
    ~DocumentManager();

    DocumentManager(const DocumentManager&) = delete;
    DocumentManager& operator=(const DocumentManager&) = delete;

    // —— 集合与活动文档 ——
    std::size_t documentCount() const;
    Document* documentAt(std::size_t index) const;
    /// 文档列表（顺序即 Tab 顺序；拷贝返回，遍历期间结构变更安全由调用方保证）
    std::vector<Document*> documents() const;
    Document* activeDocument() const;
    void setActiveDocument(Document* document);
    void activate(std::size_t index);

    // —— 生命周期 ——
    /// 新建「未命名-N」文档并激活；N 由编号器递增、跳过已占用显示名
    Document* newDocument();

    /// 草稿恢复专用（M4-B）：按记录显示名与插入位置重建未命名文档。
    /// 不激活、不写内容——调用方随后 setCursorPosition + setContent（本函数
    /// 已触发 documentsChanged，编辑器实例与事件订阅就位，内容变化沿正常
    /// 管线传播）。显示名沿用 untitledName（形如 未命名-N 时编号器推进到 N，
    /// 后续 newDocument 跳过占用号；空名按编号器生成）；与现有文档 displayName
    /// 冲突时编号器递增生成新名。insertIndex：-1 追加尾部；其余值（含越界
    /// 正数与非哨兵负数）钳制到 [0, count]（旧 Qt 版 qBound 同语义；B 层
    /// 计划值只产生 -1 或 ≥0，经 clampInsertIndex 后与此契约一致）。
    Document* restoreUntitledDraft(const std::wstring& untitledName, int insertIndex);

    /// 批量打开：重复路径激活已有（编码不变更）；失败逐个收集返回（不中断后续）；
    /// 空路径跳过。成功打开的最后一个成为活动文档
    std::vector<OpenError> openPaths(const std::vector<std::filesystem::path>& paths);

    /// 单路径打开；forced 有值按指定编码解码（会话记忆编码/纠正识别错误入口）。
    /// 空路径返回 nullptr 成功值（跳过，不报错——旧 openSingle 语义）
    std::expected<Document*, OpenError>
    openPath(const std::filesystem::path& path, std::optional<TextEncoding> forced = std::nullopt);

    /// 保存。无 url 返回 InvalidUrl 类错误（UI 捕获后转另存为流程）；
    /// 按文档编码/BOM/换行状态写出；成功重置基线（清脏）
    std::expected<void, StorageError> save(Document* doc);

    /// 另存为：成功后更新 url（displayName 随之）并清脏。遮蔽语义（DEC-008）：
    /// 目标路径同时是其他已打开文档的 url 时，写盘后按刚写出的编码从磁盘强制
    /// 重读并重载该文档（防止其内存旧内容再次保存时覆盖磁盘新内容）
    SaveAsResult saveAs(Document* doc, const std::filesystem::path& path);

    /// 关闭（激活邻居：优先前一个，其次后一个，无则活动文档置空）
    void close(std::size_t index);
    void closeDocument(Document* doc);

    /// 按磁盘强制解码重载：换行重探测、编码/BOM 刷新、不置脏（未保存修改被
    /// 丢弃，确认框由 UI 层负责）；光标钳制到新内容字节长度。未命名文档返回
    /// InvalidUrl 错误；读取失败保留原文档内容并返回错误
    std::expected<void, StorageError> reloadWithEncoding(Document* doc, TextEncoding encoding);

    /// 按指定编码立即保存（编码转换，一步完成：设编码 + 保存）。
    /// UTF-8 目标 BOM 取文档历史值（hasBom）；GBK 目标忽略 BOM。
    /// 成功清脏；未命名文档保留已设编码并返回 InvalidUrl（UI 转另存为，另存为
    /// 按已设编码写出）；失败（含 GBK 不可编码）回滚编码设置并返回错误
    std::expected<void, StorageError> saveWithEncoding(Document* doc, TextEncoding encoding);

    Document* documentForPath(const std::filesystem::path& path) const;
    bool hasUnsavedChanges() const;

    // —— 格式注册表（不转移所有权；调用方保证生命周期覆盖管理器使用期）——
    void registerFormat(DocumentFormat& format);       ///< 重复注册忽略（引用参数无空指针形态）
    DocumentFormat* formatForPath(const std::filesystem::path& path) const;
    /// 聚合全部格式名过滤器（保序去重，供文件对话框）
    std::vector<std::wstring> nameFilters() const;

    // —— 存储注册表（不转移所有权）——
    void registerStorage(DocumentStorage& storage);
    DocumentStorage* storageForUrl(const std::filesystem::path& path) const;

    // —— 多播事件（M2-B UI / M4 会话草稿订阅）——
    MulticastEvent<> documentsChanged;            ///< 文档集合（数量/顺序）变化
    MulticastEvent<> activeDocumentChanged;       ///< 活动文档切换（含置空）
    MulticastEvent<Document*> documentClosed;     ///< 文档移除后、销毁前发出（可读 uid 清草稿）
    MulticastEvent<Document*> documentModifiedChanged; ///< 某文档脏↔干净翻转
    MulticastEvent<Document*> documentContentChanged;  ///< 某文档内容变化（草稿防抖/编辑器同步）
    MulticastEvent<Document*> documentMetadataChanged; ///< 某文档元数据变化（url/显示名/编码/换行/BOM/光标）

private:
    Document* appendDocument(std::unique_ptr<Document> doc);
    void wireDocumentCallbacks(Document* doc);
    bool displayNameInUse(const std::wstring& name) const;

    std::vector<std::unique_ptr<Document>> m_documents; // 有序（Tab 顺序）；管理器持有所有权
    Document* m_activeDocument = nullptr;
    int m_untitledCounter = 0; // 未命名编号器（单调递增，跨关闭不复位——旧版语义）

    std::vector<DocumentFormat*> m_formats; // 非拥有
    std::vector<DocumentStorage*> m_storages; // 非拥有
    DocumentFormat* m_plainTextFormat = nullptr; // 兜底格式（内置，拥有权见下）
    std::unique_ptr<PlainTextFormat> m_ownedPlainText;
    std::unique_ptr<class LocalStorage> m_ownedLocalStorage;
};

} // namespace core
