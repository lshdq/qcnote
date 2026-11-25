#include "core/externalchangemonitor.h"

#include <utility>

#include "core/document.h"

namespace core {

ExternalChangeMonitor::ExternalChangeMonitor(StatQuery statQuery, ContentQuery contentQuery)
    : m_statQuery(std::move(statQuery)), m_contentQuery(std::move(contentQuery)) {}

FileStatSample ExternalChangeMonitor::normalizeSample(FileStatSample sample) {
    if (!sample.exists) {
        // 缺失状态下 mtime/size 无意义：归一收敛为单值，防止查询方返回的
        // 垃圾值制造「不存在的两个版本」（DEC-006）
        sample.mtimeMs = 0;
        sample.size = 0;
    }
    return sample;
}

std::optional<FileStatSample>
ExternalChangeMonitor::sampleStat(const std::filesystem::path& url) const {
    if (!m_statQuery)
        return std::nullopt; // 未注入：等同查询失败（惰性无动作，见头注释）
    const std::optional<FileStatSample> sampled = m_statQuery(url);
    if (!sampled)
        return std::nullopt; // 查询失败（nullopt）：向调用方传播，本轮跳过
    return normalizeSample(*sampled);
}

bool ExternalChangeMonitor::contentMatchesBaseline(const std::filesystem::path& url,
                                                    const Document& doc) const {
    if (!m_contentQuery)
        return true; // 未注入：按内容一致处理（宁漏报不误报，见头注释）
    return m_contentQuery(url, doc);
}

void ExternalChangeMonitor::resetSnapshot(const Document& doc, FileStatSample stat) {
    Snapshot snapshot;
    snapshot.url = doc.url();
    snapshot.stat = stat; // 调用方保证已归一化
    m_snapshots[doc.uid()] = std::move(snapshot); // 覆盖或新建；判定状态全清
}

ExternalChangeMonitor::Snapshot*
ExternalChangeMonitor::snapshotFor(const std::string& uid) {
    const auto it = m_snapshots.find(uid);
    return it == m_snapshots.end() ? nullptr : &it->second;
}

void ExternalChangeMonitor::refreshBaseline(const Document& doc) {
    if (doc.url().empty()) {
        forget(doc.uid()); // 未命名：无可跟踪目标，清掉旧快照防遗留
        return;
    }
    const std::optional<FileStatSample> sample = sampleStat(doc.url());
    if (!sample)
        return; // 查询失败：保持原快照（下轮 poll 自愈，见头注释）
    resetSnapshot(doc, *sample);
}

void ExternalChangeMonitor::acknowledge(const Document& doc) {
    const auto it = m_snapshots.find(doc.uid());
    if (it == m_snapshots.end())
        return; // 无快照：尚无已观测版本，无决策可记录
    it->second.acknowledged = true;
    it->second.ackStat = it->second.stat; // 快照最近采样 = 用户刚决策的版本
}

void ExternalChangeMonitor::forget(const std::string& uid) {
    m_snapshots.erase(uid); // 不存在时幂等
}

std::vector<Finding> ExternalChangeMonitor::poll(const std::vector<Document*>& docs) {
    std::vector<Finding> findings;
    for (const Document* doc : docs) {
        if (doc == nullptr)
            continue;
        const std::filesystem::path& url = doc->url();
        if (url.empty())
            continue; // 未命名文档不检测（TC-012：stat 查询都不发起）

        const std::optional<FileStatSample> sampled = sampleStat(url);
        if (!sampled)
            continue; // 查询失败：本轮跳过，快照/状态不变（TC-005：不误报删除）

        Snapshot* snapshot = snapshotFor(doc->uid());
        if (snapshot == nullptr || snapshot->url != url) {
            // DEC-005 自动建立/重建（openPath/saveAs/rename 兜底）：本轮只采样
            // 建快照，不判定不产出 finding（url 变化 = 新基线，旧删除态/决策失效）
            resetSnapshot(*doc, *sampled);
            continue;
        }

        if (*sampled == snapshot->stat)
            continue; // stat 未变：零内容 IO（TC-002）；删除态同采样不重复报

        if (!sampled->exists) {
            // 确认不存在（快照存在 → 真实删除，含恢复后的再次删除）
            snapshot->stat = *sampled;
            snapshot->acknowledged = false; // 文件消亡：旧 stat 版本决策失效（DEC-006）
            snapshot->ackStat = {};
            if (!snapshot->deletedReported) {
                snapshot->deletedReported = true;
                findings.push_back(Finding{doc, Verdict::Deleted});
            }
            continue; // 已处于删除态：不重复报（UI 只提示一次）
        }

        // 文件存在且 stat 变化 → 第二级内容判定（唯一产生内容 IO 的路径）
        const bool restoredFromMissing = !snapshot->stat.exists; // 不存在 → 存在
        const bool contentMatches = contentMatchesBaseline(url, *doc);
        snapshot->stat = *sampled; // 快照推进（touch/已决策版本防每轮重读）

        if (contentMatches) {
            if (restoredFromMissing && snapshot->deletedReported) {
                snapshot->deletedReported = false;                  // 删除提示态解除
                findings.push_back(Finding{doc, Verdict::Restored}); // TC-006 静默复原
            }
            // 其余（touch 内容同 / 未提示过的缺失→出现）：静默无 finding（TC-003）
            continue;
        }

        // 内容 != 基线 → Modified 管线（删除后恢复为不同内容亦走此路，TC-007：
        // 不遗漏决策，UI 按干净/脏管线处理）
        if (restoredFromMissing && snapshot->deletedReported)
            snapshot->deletedReported = false; // 文件已回，删除提示不再有意义
        if (snapshot->acknowledged && *sampled == snapshot->ackStat)
            continue; // DEC-004：已决策的同 stat 版本不重复产出（「保留修改」）
        findings.push_back(Finding{doc, Verdict::Modified});
    }
    return findings;
}

} // namespace core
