// qcnote 纯逻辑核心层：未保存修改临时缓存（草稿缓存，M4-A，
// 旧 src/document/draftcachemanager 语义移植；DEC-013 草稿用 JSON）。
//
// 与旧版的结构性差异：本层不持有 DocumentManager、不直接执行恢复——
// restore() 输出「操作计划」RestoreOutcome（应用/新建/插入位置/激活目标），
// 由 M4-B 按时序执行（可全量单测；恢复分类语义与旧版逐条对齐）。
//
// 语义要点（来源：旧 draftcachemanager.cpp + 决策记录 20261106182451 /
// 20261109130224 / 20261109154023）：
// - 条目主键 uid（<uid>.json 原子写）：未命名可缓存、另存为后 url 变化主键
//  不变（DEC-002）；字段 version/url/untitledName/content/cursor/lineEnding/
//   encoding/hasBom/baselineHash/savedAt（savedAt 为注入时钟 epoch ms；
//   旧版为秒，仅参与新旧比较、语义等价）；
// - 写入：内容变化且脏 → 1000ms 防抖（Debouncer 时钟注入）只重写变化条目；
//   写失败静默容错（返回 false 不抛，DEC-005）；
// - 清理：内容回基线（保存/另存/撤销回基线/编码重载）删条目；关闭仍脏
//  （「不保存」放弃路径）删条目；干净关闭无条目（DEC-003）；
// - 恢复分类（restore，纯计划）：损坏/版本不符 → 静默删跳过；同 URL 去重
//  （仅留 savedAt 最新，缺失按 0 最旧，平局按 uid 字典序保留较大者——
//   确定性规则不依赖枚举顺序；落选立即删文件，DEC-009）；url 非空且磁盘
//   哈希==baselineHash → 静默恢复计划（应用为未保存修改，光标钳制，删条目）；
//   哈希不符或文件缺失 → 冲突列表（DEC-006 单框汇总）；url 空 → 未命名重建
//   计划（内容即缓存、沿用 untitledName）；未命名按会话绝对位置升序稳定
//   插入（无位置排最后，DEC-002/006）；激活三级优先：activeUid 匹配 >
//   会话活动保持 > 首个恢复文档（DEC-003 + 兜底）；
// - 冲突解决（resolveConflicts）：恢复=应用（磁盘缺失转未命名重建）；应用/
//   丢弃均删条目文件（DEC-007）。
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <algorithm>

#include "core/encoding.h"
#include "core/sessionmanager.h"
#include "core/textutil.h"

namespace core {

class Document;

/// 草稿条目（<uid>.json 的内存形态；content 为 UTF-8/LF 契约内容）
struct DraftEntry {
    std::string uid;                  ///< 文档 uid（条目主键；同 URL 平局比较用）
    std::filesystem::path url;        ///< 空 = 未命名条目
    std::wstring untitledName;        ///< 记录时显示名（url 文档为文件名；冲突/重建回退）
    std::string content;              ///< UTF-8、LF、无 BOM
    int cursor = 0;                   ///< 字节偏移（构造/读取时钳制到内容长度）
    LineEnding lineEnding = LineEnding::Lf;
    TextEncoding encoding = TextEncoding::Utf8;
    bool hasBom = false;
    std::string baselineHash;         ///< 写入时刻基线 SHA-256（hex 小写）
    long long savedAt = 0;            ///< epoch ms；缺失/非法按 0（最旧）

    bool operator==(const DraftEntry&) const = default;
};

/// 磁盘状态查询注入（恢复分类）：url → 磁盘加载归一化内容（LF、无 BOM）的
/// SHA-256；文件缺失/不可读 → nullopt。生产接线（M4-B）＝LocalStorage 读 +
/// sha256Hex；测试 mock。判定语义与旧版一致（磁盘未被外部修改 ⇔ 哈希相符）
using DiskHashQuery = std::function<std::optional<std::string>(const std::filesystem::path&)>;

/// 单条恢复动作（M4-B 执行；本层只产出计划）
struct DraftRestorePlan {
    enum class Target {
        ApplyToUrlDoc, ///< 应用到 url 文档（已打开则直接应用；未打开先代开再应用）
        NewUntitled,   ///< 重建未命名文档（内容即缓存、沿用 untitledName、标记已修改）
    };
    Target target = Target::NewUntitled;
    DraftEntry entry;                 ///< 内容/光标（已钳制）/元数据
    std::filesystem::path url;        ///< ApplyToUrlDoc 的目标 url
    int insertIndex = -1;             ///< NewUntitled：会话绝对位置（-1=追加尾部；
                                      ///<  应用时经 clampInsertIndex 钳制）

    bool operator==(const DraftRestorePlan&) const = default;
};

/// 冲突条目（待用户决策；磁盘被外部修改或文件缺失，DEC-006）
struct DraftConflict {
    DraftEntry entry;
    std::filesystem::path entryPath;  ///< 条目文件（决策后删除）
    bool missing = false;             ///< true = 文件缺失/不可读
    std::wstring displayName;         ///< 冲突框显示名（url 文件名，回退记录名）
};

/// 恢复结果（操作计划；不直接操作 DocumentManager）
struct RestoreOutcome {
    /// 处理顺序：url 条目（条目文件名序）→ 未命名（会话位置升序，无位置最后）
    std::vector<DraftRestorePlan> restored;
    std::vector<DraftConflict> conflicts;

    /// 激活计划：≥0 = 激活 restored[activatedRestoredIndex]；-1 = 维持会话恢复
    /// 设定的活动标签（activeBefore 复位，TC-015 不抢占语义）
    int activatedRestoredIndex = -1;

    bool empty() const { return restored.empty() && conflicts.empty(); }
};

/// 冲突解决结果（resolveConflicts 返回；plans 并入 restored 序列执行）
struct ConflictResolution {
    std::vector<DraftRestorePlan> plans; ///< restore=true 时每个冲突一条计划
    bool activateFirst = false;          ///< restore 且当前无活动文档且确有恢复
                                         ///<（空场景激活首个恢复文档，DEC-006 对齐）
};

class DraftCacheManager {
public:
    /// 时钟注入（epoch ms；savedAt 来源）。空时钟按 0（最旧）
    using Clock = std::function<long long()>;

    /// @param draftsDir 草稿目录（SessionManager::resolvePaths 解析后注入）
    explicit DraftCacheManager(std::filesystem::path draftsDir, Clock clock = nullptr);

    // —— 条目构建与持久化 ——
    /// 从文档构建条目（cursor 钳制到内容字节长度；savedAt 注入）
    static DraftEntry entryFor(const Document& doc, long long savedAt);
    /// 条目文件完整路径 <draftsDir>/<uid>.json
    static std::filesystem::path entryPath(const std::filesystem::path& draftsDir,
                                           const std::string& uid);
    /// 条目原子写入（目录不存在自动创建）；失败返回 false（静默容错，DEC-005）
    static bool persist(const std::filesystem::path& draftsDir, const DraftEntry& entry);
    /// 解析单个条目文件：损坏/非对象/版本不符/字段类型非法回退默认 → 部分宽容；
    /// 仅「无法解析为 JSON 对象」或 version≠1 返回 nullopt（调用方删文件）
    static std::optional<DraftEntry> readEntry(const std::filesystem::path& path);
    /// 删除条目文件（存在时）；不存在视为成功（幂等）
    static bool removeEntryFile(const std::filesystem::path& draftsDir, const std::string& uid);

    // —— 写入协调（防抖管线；时钟注入，B 层经 DocumentManager 事件驱动）——
    /// 内容变化：modified → 挂起防抖写（restart 聚合）；回基线 → 立即删条目
    ///（DEC-003 内容回到基线语义）
    void onContentChanged(Document* doc, long long now);
    /// modified 翻转为 false（保存/另存为/编码重载路径，无 contentChanged）→ 删条目
    void onModifiedCleared(Document* doc);
    /// 文档关闭（documentClosed 时序：移除后、销毁前）：仍脏 → 删条目（放弃即
    /// 真丢）；干净 → 仅摘出挂起（本就无条目）。挂起文档销毁前必须先经此通知
    void onDocumentClosed(Document* doc);
    /// 防抖到期：true = 到期并已写出全部挂起条目（仅写仍 modified 的文档）
    bool flushDue(long long now);
    /// 立即写出全部挂起（失焦即写 DEC-003 / 退出兜底；无视到期）
    void flushNow();

    bool hasPending() const { return !m_pending.empty(); }
    std::size_t pendingCount() const { return m_pending.size(); }
    const std::filesystem::path& draftsDir() const { return m_draftsDir; }

    // —— 恢复（纯计划；直接删损坏/去重落选/已应用条目文件）——
    /// @param session 会话数据（未命名位置/激活依据；B 层先跑会话恢复再调本函数）
    /// @param diskHash 磁盘状态查询（null 视为全部缺失 → 全部冲突）
    static RestoreOutcome restore(const std::filesystem::path& draftsDir, const SessionData& session,
                                  DiskHashQuery diskHash);

    /// 冲突解决（用户决策后由 M4-B 调用）：restore=true → 每个冲突一条计划
    ///（磁盘仍存在 → ApplyToUrlDoc；缺失 → NewUntitled 重建，DEC-006/007）；
    /// false → 仅丢弃。两者均删除条目文件并清空 conflicts。
    /// @param hasActiveDocument 当前是否存在活动文档（激活兜底判定）
    static ConflictResolution resolveConflicts(std::vector<DraftConflict>& conflicts,
                                               bool restore, bool hasActiveDocument,
                                               DiskHashQuery diskHash);

    /// 插入位置钳制（M4-B 应用 NewUntitled 计划时调用）：
    /// -1 → count（追加尾部）；其余钳制到 [0, count]（越界≈草稿多于会话记录）
    static int clampInsertIndex(int index, int count) {
        return index < 0 ? count : (std::min)(index, (std::max)(count, 0));
    }

private:
    void writePending();

    std::filesystem::path m_draftsDir;
    Clock m_clock;
    Debouncer m_debounce{1000};       ///< 1000ms 单触发（DEC-005）
    std::vector<Document*> m_pending; ///< 防抖挂起文档（单线程事件驱动；关闭经
                                      ///<  onDocumentClosed 摘除后指针必有效）
    void removeFromPending(const Document* doc);
    void removeEntryFor(const Document& doc);
};

} // namespace core
