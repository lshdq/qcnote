// qcnote 纯逻辑核心层：外部文件改动检测器（外部文件改动检测需求；轮询 +
// 两级校验，无 Win32/UI 依赖——stat 查询与磁盘内容判定均注入，可 Catch2
// 全覆盖单测）。
//
// 语义要点（来源：需求-外部文件改动检测 + 决策记录 20261124180950）：
// - DEC-001 轮询而非目录通知：本类不持有时钟/线程/定时器，poll() 由上层
//   （main.cpp timer id 7 默认 1000ms + 切换标签/窗口激活的立即触发点）
//   调用；单线程事件驱动顺序执行，无竞态。poll 传入的文档指针在调用期间
//   由调用方保证有效（与 DraftCacheManager 挂起列表同一约定）；
// - DEC-002 两级校验：先 stat 初筛——mtime+size 与快照一致则本轮跳过（内容
//   判定 0 次调用，正常编辑会话零额外 IO）；stat 变化才调内容判定（磁盘
//   归一化内容哈希是否 == baselineHash），消除 touch/mtime 变但内容同的
//   误报。stat 变但内容同 → 静默推进快照为新 stat（防每轮重读）；
// - 快照以 uid 为键（uid 构造后不可变：另存为/重命名后 url 变化主键稳定），
//   快照内记录 url 用于失配重建（DEC-005）：poll 遇无快照或快照 url != 文档
//   url → 自动建立快照，本轮不产出 finding（openPath/saveAs/rename 兜底）；
//   保存等写路径成功后由上层显式调 refreshBaseline 刷新。poll 不清理未传入
//   文档的快照（仅活动文档受检，切回标签时按旧快照继续比对，不漏检）；
// - 判定三分（Verdict）：Modified（磁盘内容 != 基线：干净 → UI 自动重载、
//   脏 → 三选框）/ Deleted（确认不存在，同一次删除事件只报一次：报出时
//   快照推进到「不存在」并置删除态标记，该标记是恢复判定的依据）/
//   Restored（删除态中恢复且内容 == 基线，UI 静默清除删除提示）；恢复但
//   内容已变 → Modified（不遗漏决策，UI 按干净/脏管线处理）；
// - stat 查询结果三分：exists=true 正常采样 / exists=false 确认不存在（真实
//   删除）/ nullopt 查询失败（网络抖动、权限等非「确认不存在」错误）——
//   失败仅本轮跳过，快照与判定状态不变，绝不误报删除；
// - DEC-004 acknowledge（「保留修改」抑制）：记录用户已对当前 stat 版本
//   （快照最近采样）决策，同版本不再产出 Modified，stat 再变化恢复产出——
//   与「finding 产出即推进快照」双保险（模态期间不 poll，同版本本就没有
//   重报路径；ack 记录额外覆盖 stat 回退到已决策版本的情形）；
// - DEC-006 边界语义：exists=false 的采样归一化（mtime/size 按 0，缺失态
//   收敛为单值，防查询方垃圾值制造「不存在的两个版本」）；注入缺省降级
//   见构造参数说明；确认不存在时已决策 stat 版本随之失效（删除重建后的
//   同 stat 版本视为新事件，不沿用「保留」抑制）。
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {

class Document;

/// 磁盘文件 stat 采样（注入查询的载荷）。mtime+size 共同构成「stat 版本」。
struct FileStatSample {
    bool exists = false;     ///< false = 确认不存在（真实删除，非查询失败）
    long long mtimeMs = 0;   ///< 最后修改时间（epoch 毫秒）；exists=false 时无意义（按 0 归一）
    long long size = 0;      ///< 文件字节数；exists=false 时无意义（按 0 归一）

    bool operator==(const FileStatSample&) const = default;
};

/// stat 查询注入：path → 采样；nullopt = 查询失败（网络/权限抖动，本轮跳过
/// 不判定删除）。生产接线（main.cpp）= 文件属性/last_write_time + file_size；
/// 测试 mock（内存表）。
using StatQuery = std::function<std::optional<FileStatSample>(const std::filesystem::path&)>;

/// 内容判定注入：返回磁盘归一化内容（LocalStorage::read → UTF-8、LF、无 BOM）
/// 的 SHA-256（core::sha256Hex）是否 == 文档 baselineHash()。生产由 main.cpp
/// 接 LocalStorage::read + sha256Hex；测试 mock。判定语义与草稿恢复的
/// DiskHashQuery 一致：磁盘未被外部修改 ⇔ 哈希相符。
using ContentQuery = std::function<bool(const std::filesystem::path&, const Document&)>;

/// 判定类别（poll 产出）
enum class Verdict {
    Modified, ///< 磁盘内容 != baselineHash（干净 → UI 自动重载；脏 → 三选框）
    Deleted,  ///< 文件确认不存在（提示一次；文档保持打开，保存时重建）
    Restored, ///< 删除态中文件恢复且内容 == 基线（UI 静默清除删除提示）
};

/// 单条判定：doc 指向 poll 传入的文档（调用栈内有效，UI 应立即处理）
struct Finding {
    const Document* doc = nullptr;
    Verdict verdict = Verdict::Modified;

    bool operator==(const Finding&) const = default;
};

class ExternalChangeMonitor {
public:
    /// @param statQuery stat 查询注入；null → 全部按查询失败处理（poll 惰性
    ///        无动作、不建立快照——未接线时不产生任何副作用）
    /// @param contentQuery 内容判定注入；null → stat 变化时按「内容与基线
    ///        一致」处理（静默推进快照；检测退化为纯 stat 跟踪，不产出
    ///        Modified——宁漏报不误报）
    explicit ExternalChangeMonitor(StatQuery statQuery = nullptr,
                                    ContentQuery contentQuery = nullptr);

    // —— 基线与决策管理（上层写路径/用户决策后显式调用）——
    /// 写路径成功后刷新快照：按文档当前 url 重新采样并整体重置（清除删除态
    /// 与已决策版本——新基线下旧决策失效，DEC-005 关键写路径辅以显式刷新）。
    /// url 为空（未命名）→ 等价 forget；查询失败（nullopt）→ 保持原快照
    /// （下轮 poll 按既有快照比对自愈，不误报）。
    void refreshBaseline(const Document& doc);
    /// 记录「用户已对当前 stat 版本决策」（DEC-004「保留修改」）：把快照最近
    /// 采样记为已决策版本，同版本 poll 不再产出 Modified；stat 再变化恢复
    /// 产出。无快照时无操作。
    void acknowledge(const Document& doc);
    /// 文档关闭后清理快照（uid 遗留不泄漏，TC-011）；不存在时幂等
    void forget(const std::string& uid);

    /// 一轮检测：仅检测传入文档（上层只传活动文档，TC-106/108；未传入文档
    /// 的快照保留）。返回按传入顺序的判定列表；无 url / 空指针文档跳过
    ///（连 stat 查询都不发起，TC-012）。
    std::vector<Finding> poll(const std::vector<Document*>& docs);

    /// 当前跟踪的快照条目数（测试/诊断：forget 清理验证）
    std::size_t snapshotCount() const { return m_snapshots.size(); }

private:
    /// uid → 快照：最近一次采样的 stat + 失配判定用 url + 判定状态。
    /// stat 与快照一致 → 本轮跳过（零内容 IO）；失配走变更管线
    struct Snapshot {
        std::filesystem::path url;    ///< 建立时的文档 url（≠ 当前 url → 重建，DEC-005）
        FileStatSample stat;          ///< 最近采样（已归一化；exists=false 表示删除态基线）
        bool deletedReported = false; ///< 删除提示态：Restored 仅自此态产出
        bool acknowledged = false;    ///< DEC-004：是否已记录用户决策的 stat 版本
        FileStatSample ackStat;       ///< 已决策的 stat 版本（acknowledged 时有效）
    };

    /// exists=false 采样归一化（mtime/size 按 0：缺失态收敛为单值）
    static FileStatSample normalizeSample(FileStatSample sample);
    /// 注入采样（null 注入 → nullopt）；返回值已归一化
    std::optional<FileStatSample> sampleStat(const std::filesystem::path& url) const;
    /// 注入内容判定（null 注入 → true 即视为与基线一致）
    bool contentMatchesBaseline(const std::filesystem::path& url, const Document& doc) const;
    /// 以 doc 当前 url + 给定采样整体重置快照（清除全部判定状态；stat 须已归一）
    void resetSnapshot(const Document& doc, FileStatSample stat);
    Snapshot* snapshotFor(const std::string& uid);

    StatQuery m_statQuery;
    ContentQuery m_contentQuery;
    std::unordered_map<std::string, Snapshot> m_snapshots; ///< uid → 快照（单线程，无锁）
};

} // namespace core
