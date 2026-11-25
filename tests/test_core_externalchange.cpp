// 外部文件改动检测（core::ExternalChangeMonitor）单测：TC-001..TC-012
//（测试用例-外部文件改动检测 + 决策记录 20261124180950 / DEC-006 边界语义）。
//
// 全量 mock 注入（内存文件系统模拟 stat 与磁盘归一化内容，经 sha256Hex 与
// Document::baselineHash 同口径比较），不碰真实磁盘；并断言 stat/内容查询
// 调用次数，钉住 DEC-002 两级校验（stat 未变零内容 IO、每轮 stat 恰一次）。
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/document.h"
#include "core/externalchangemonitor.h"
#include "core/textutil.h"

using namespace core;

namespace {

const std::filesystem::path kFile = L"D:/watch/a.txt";
const std::filesystem::path kOtherFile = L"D:/watch/b.txt";

/// 内存文件系统：url → (stat, 磁盘归一化内容)。磁盘内容以归一化字符串表达，
/// contentQuery 经 sha256Hex 与 baselineHash 同口径比较（等价于生产注入）。
/// 「确认不存在」用 remove（条目消失）表达；statFails 注入「查询失败」。
struct MockDisk {
    struct Entry {
        FileStatSample stat;
        std::string content; ///< 磁盘归一化内容（UTF-8、LF 契约）
    };
    std::map<std::filesystem::path, Entry> files;
    bool statFails = false;   ///< true → stat 查询返回 nullopt（非「确认不存在」）
    int statCalls = 0;        ///< stat 查询调用次数（TC-002/012 断言）
    int contentCalls = 0;     ///< 内容判定调用次数（TC-002/003 初筛验证）

    void put(const std::filesystem::path& url, long long mtimeMs, std::string_view content) {
        files[url] = Entry{FileStatSample{true, mtimeMs,
                                          static_cast<long long>(content.size())},
                           std::string(content)};
    }
    void remove(const std::filesystem::path& url) { files.erase(url); }

    StatQuery statQuery() {
        return [this](const std::filesystem::path& url) -> std::optional<FileStatSample> {
            ++statCalls;
            if (statFails)
                return std::nullopt; // 查询失败（非确认不存在）
            const auto it = files.find(url);
            if (it == files.end())
                return FileStatSample{false, 0, 0}; // 确认不存在
            return it->second.stat;
        };
    }
    ContentQuery contentQuery() {
        return [this](const std::filesystem::path& url, const Document& doc) -> bool {
            ++contentCalls;
            const auto it = files.find(url);
            if (it == files.end())
                return false; // 读不到 ≠ 等于基线（防御分支；poll 不会对缺失文件走此路径）
            return sha256Hex(it->second.content) == doc.baselineHash();
        };
    }
};

/// 打开态文档：url + 内容 + 基线（= 内容本身，加载后必为干净态）
Document loadedDoc(const std::filesystem::path& url, std::string_view content) {
    Document doc;
    doc.applyLoadedState(url, std::string(content), LineEnding::Lf, TextEncoding::Utf8, false);
    return doc;
}

/// 单文档一轮 poll 的快捷断言：返回 findings（调用点继续断言细节）
std::vector<Finding> pollOne(ExternalChangeMonitor& monitor, Document& doc) {
    return monitor.poll({&doc});
}

} // namespace

// ===========================================================================
// TC-001: 干净文档外部修改判定 Modified
// ===========================================================================
TEST_CASE("外部改动检测：干净文档外部修改判定 Modified（TC-001）", "[core][watch]") {
    MockDisk disk;
    disk.put(kFile, 1000, "alpha");
    Document doc = loadedDoc(kFile, "alpha");
    ExternalChangeMonitor monitor(disk.statQuery(), disk.contentQuery());

    // 首轮：自动建立快照（DEC-005 兜底），无 finding、不读内容
    REQUIRE(pollOne(monitor, doc).empty());
    REQUIRE(disk.contentCalls == 0);

    // 外部修改：mtime+size 变化且内容与基线不同
    disk.put(kFile, 2000, "beta-external");
    const std::vector<Finding> findings = pollOne(monitor, doc);
    REQUIRE(findings.size() == 1);
    REQUIRE(findings[0].doc == &doc);
    REQUIRE(findings[0].verdict == Verdict::Modified);
    REQUIRE(disk.contentCalls == 1); // stat 变化才读内容，恰一次
}

// ===========================================================================
// TC-002: stat 未变跳过（不读内容）
// ===========================================================================
TEST_CASE("外部改动检测：stat 未变跳过且不读内容（TC-002）", "[core][watch]") {
    MockDisk disk;
    disk.put(kFile, 1000, "alpha");
    Document doc = loadedDoc(kFile, "alpha");
    ExternalChangeMonitor monitor(disk.statQuery(), disk.contentQuery());
    REQUIRE(pollOne(monitor, doc).empty()); // 建快照

    disk.statCalls = 0;
    disk.contentCalls = 0;
    REQUIRE(pollOne(monitor, doc).empty());
    REQUIRE(disk.statCalls == 1);    // 每轮 stat 恰好一次（单文件）
    REQUIRE(disk.contentCalls == 0); // stat 未变：内容判定 0 次
}

// ===========================================================================
// TC-003: touch 不误报（mtime 变、内容同），快照推进防重读
// ===========================================================================
TEST_CASE("外部改动检测：touch 不误报且快照推进（TC-003）", "[core][watch]") {
    MockDisk disk;
    disk.put(kFile, 1000, "alpha");
    Document doc = loadedDoc(kFile, "alpha");
    ExternalChangeMonitor monitor(disk.statQuery(), disk.contentQuery());
    REQUIRE(pollOne(monitor, doc).empty()); // 建快照（v1）

    disk.put(kFile, 3000, "alpha");        // touch：mtime 变、内容（与 size）不变
    REQUIRE(pollOne(monitor, doc).empty());
    REQUIRE(disk.contentCalls == 1);       // stat 变化 → 恰读一次内容，确认与基线一致

    REQUIRE(pollOne(monitor, doc).empty()); // 同 stat 再 poll
    REQUIRE(disk.contentCalls == 1);        // ← 快照已推进的可失败断言（不推进则恒为 2）
}

// ===========================================================================
// TC-004: 文件删除判定 Deleted，同一次删除只报一次
// ===========================================================================
TEST_CASE("外部改动检测：文件删除判定 Deleted 且只报一次（TC-004）", "[core][watch]") {
    MockDisk disk;
    disk.put(kFile, 1000, "alpha");
    Document doc = loadedDoc(kFile, "alpha");
    ExternalChangeMonitor monitor(disk.statQuery(), disk.contentQuery());
    REQUIRE(pollOne(monitor, doc).empty()); // 建快照

    disk.remove(kFile);
    const std::vector<Finding> findings = pollOne(monitor, doc);
    REQUIRE(findings.size() == 1);
    REQUIRE(findings[0].doc == &doc);
    REQUIRE(findings[0].verdict == Verdict::Deleted);
    REQUIRE(disk.contentCalls == 0); // 删除判定不读内容

    // 删除态下多轮 poll 不重复报（UI 只提示一次）
    REQUIRE(pollOne(monitor, doc).empty());
    REQUIRE(pollOne(monitor, doc).empty());
}

// ===========================================================================
// TC-005: stat 查询失败不误报删除、快照/状态不变
// ===========================================================================
TEST_CASE("外部改动检测：stat 查询失败不误报删除且状态不变（TC-005）", "[core][watch]") {
    MockDisk disk;
    disk.put(kFile, 1000, "alpha");
    Document doc = loadedDoc(kFile, "alpha");
    ExternalChangeMonitor monitor(disk.statQuery(), disk.contentQuery());
    REQUIRE(pollOne(monitor, doc).empty()); // 建快照（v1，文件存在）

    disk.statFails = true;
    REQUIRE(pollOne(monitor, doc).empty()); // 查询失败：本轮跳过
    REQUIRE(disk.contentCalls == 0);

    disk.statFails = false;
    disk.remove(kFile);
    const std::vector<Finding> findings = pollOne(monitor, doc);
    REQUIRE(findings.size() == 1);
    REQUIRE(findings[0].verdict == Verdict::Deleted);
    // ↑ 失败轮若误推进了快照/删除态，此处不会首次报 Deleted（可失败断言）
}

// ===========================================================================
// TC-006: 删除后恢复原内容判定 Restored 静默
// ===========================================================================
TEST_CASE("外部改动检测：删除后恢复原内容判定 Restored（TC-006）", "[core][watch]") {
    MockDisk disk;
    disk.put(kFile, 1000, "alpha");
    Document doc = loadedDoc(kFile, "alpha");
    ExternalChangeMonitor monitor(disk.statQuery(), disk.contentQuery());
    REQUIRE(pollOne(monitor, doc).empty()); // 建快照

    disk.remove(kFile);
    REQUIRE(pollOne(monitor, doc).size() == 1); // Deleted

    disk.put(kFile, 4000, "alpha"); // 恢复且内容 == 基线
    const std::vector<Finding> findings = pollOne(monitor, doc);
    REQUIRE(findings.size() == 1);
    REQUIRE(findings[0].doc == &doc);
    REQUIRE(findings[0].verdict == Verdict::Restored); // UI 侧静默清除删除态
    REQUIRE(disk.contentCalls == 1);                   // 恢复轮恰一次内容判定

    REQUIRE(pollOne(monitor, doc).empty()); // 恢复后同 stat 无 finding
}

// ===========================================================================
// TC-007: 删除后恢复为不同内容按 Modified 处理（不遗漏决策）
// ===========================================================================
TEST_CASE("外部改动检测：删除后恢复为不同内容按 Modified（TC-007）", "[core][watch]") {
    MockDisk disk;
    disk.put(kFile, 1000, "alpha");
    Document doc = loadedDoc(kFile, "alpha");
    ExternalChangeMonitor monitor(disk.statQuery(), disk.contentQuery());
    REQUIRE(pollOne(monitor, doc).empty()); // 建快照

    disk.remove(kFile);
    REQUIRE(pollOne(monitor, doc).size() == 1); // Deleted

    disk.put(kFile, 4000, "beta-restored"); // 恢复但内容 != 基线
    const std::vector<Finding> findings = pollOne(monitor, doc);
    REQUIRE(findings.size() == 1);
    REQUIRE(findings[0].verdict == Verdict::Modified); // 直接 Modified（UI 按干净/脏管线处理）
}

// ===========================================================================
// TC-008: refreshBaseline 后同 stat 不报（保存路径防自触发）
// ===========================================================================
TEST_CASE("外部改动检测：refreshBaseline 后同 stat 不报（TC-008）", "[core][watch]") {
    MockDisk disk;
    disk.put(kFile, 1000, "alpha");
    Document doc = loadedDoc(kFile, "alpha");
    ExternalChangeMonitor monitor(disk.statQuery(), disk.contentQuery());
    REQUIRE(pollOne(monitor, doc).empty()); // 建快照 v1

    // 模拟：外部改过（stat 已到 v2）后我们自己保存（写盘 + markSaved）
    doc.setContent("gamma-local-edit");
    disk.put(kFile, 2000, "gamma-local-edit");
    doc.markSaved();              // 基线 = 当前内容（与磁盘一致）
    monitor.refreshBaseline(doc); // 写路径成功后显式刷新（DEC-005）

    disk.contentCalls = 0;
    REQUIRE(pollOne(monitor, doc).empty()); // stat 与刷新后快照一致：不报
    REQUIRE(disk.contentCalls == 0);        // ← 不刷新则此处会多一次内容读取（可失败断言）
}

// ===========================================================================
// TC-009: url 变化后快照自动重建（另存为/重命名路径）
// ===========================================================================
TEST_CASE("外部改动检测：url 变化后快照自动重建（TC-009）", "[core][watch]") {
    MockDisk disk;
    disk.put(kFile, 1000, "alpha");
    disk.put(kOtherFile, 5000, "alpha"); // 另存后的新文件（stat 与旧快照无关）
    Document doc = loadedDoc(kFile, "alpha");
    ExternalChangeMonitor monitor(disk.statQuery(), disk.contentQuery());
    REQUIRE(pollOne(monitor, doc).empty()); // 旧 url 建快照

    doc.setUrl(kOtherFile); // 模拟另存为/重命名（uid 不变、url 变）
    disk.contentCalls = 0;
    REQUIRE(pollOne(monitor, doc).empty()); // url 失配 → 自动重建，本轮无 finding
    REQUIRE(disk.contentCalls == 0);        // 重建轮只采样，不读内容

    // 快照已按新 url 重建：新 url 文件再变化 → 正常报 Modified
    disk.put(kOtherFile, 6000, "beta-after-saveas");
    const std::vector<Finding> findings = pollOne(monitor, doc);
    REQUIRE(findings.size() == 1);
    REQUIRE(findings[0].verdict == Verdict::Modified);
}

// ===========================================================================
// TC-010: 「保留」抑制同版本重复报；stat 再变化恢复产出
// ===========================================================================
TEST_CASE("外部改动检测：「保留」抑制同版本重复报（TC-010）", "[core][watch]") {
    MockDisk disk;
    disk.put(kFile, 1000, "alpha");
    Document doc = loadedDoc(kFile, "alpha");
    ExternalChangeMonitor monitor(disk.statQuery(), disk.contentQuery());
    REQUIRE(pollOne(monitor, doc).empty()); // 建快照 v1
    doc.setContent("local-dirty");          // 脏文档（三选框「保留」决策场景）

    disk.put(kFile, 2000, "beta-external"); // 外部修改 → Modified（弹框）
    REQUIRE(pollOne(monitor, doc).size() == 1);
    monitor.acknowledge(doc); // 用户选「保留修改」（DEC-004）

    SECTION("同一 stat 版本再次 poll 无 finding") {
        REQUIRE(pollOne(monitor, doc).empty());
    }
    SECTION("stat 再变化（新版本）恢复报 Modified") {
        disk.put(kFile, 3000, "gamma-newer");
        const std::vector<Finding> findings = pollOne(monitor, doc);
        REQUIRE(findings.size() == 1);
        REQUIRE(findings[0].verdict == Verdict::Modified);
    }
    SECTION("stat 回退到已决策版本亦不重报（决策按 stat 版本生效，非快照位置）") {
        // 磁盘内容先回到基线（快照静默推进到 v3）……
        disk.put(kFile, 3000, "alpha");
        REQUIRE(pollOne(monitor, doc).empty());
        // ……再回到已决策的 v2（stat 完全相同）：内容 != 基线但版本已决策
        disk.put(kFile, 2000, "beta-external");
        REQUIRE(pollOne(monitor, doc).empty()); // ← 无 ack 版本记录则此处必重报
    }
}

// ===========================================================================
// TC-011: 关闭文档后快照清理（uid 遗留不泄漏，同 url 新文档不干扰）
// ===========================================================================
TEST_CASE("外部改动检测：关闭文档后快照清理（TC-011）", "[core][watch]") {
    MockDisk disk;
    disk.put(kFile, 1000, "alpha");
    Document first = loadedDoc(kFile, "alpha");
    ExternalChangeMonitor monitor(disk.statQuery(), disk.contentQuery());
    REQUIRE(pollOne(monitor, first).empty()); // 旧文档建快照
    REQUIRE(monitor.snapshotCount() == 1);

    monitor.forget(first.uid()); // 关闭旧文档：uid 清理
    REQUIRE(monitor.snapshotCount() == 0);

    // 同 url 的新文档（新 uid）：关闭期间文件已被外部修改
    Document second = loadedDoc(kFile, "alpha");
    disk.put(kFile, 2000, "beta-external");
    REQUIRE(pollOne(monitor, second).empty()); // 首轮按新 uid 建快照：无 finding
                                               //（若误按 url 复用旧快照则此处必报 Modified）
    REQUIRE(monitor.snapshotCount() == 1);

    disk.put(kFile, 3000, "gamma-newer");
    const std::vector<Finding> findings = pollOne(monitor, second);
    REQUIRE(findings.size() == 1);
    REQUIRE(findings[0].doc == &second);
    REQUIRE(findings[0].verdict == Verdict::Modified);
}

// ===========================================================================
// TC-012: 未命名/无 url 文档跳过（无 stat 查询）
// ===========================================================================
TEST_CASE("外部改动检测：未命名文档跳过且不发起 stat 查询（TC-012）", "[core][watch]") {
    MockDisk disk;
    disk.put(kFile, 1000, "alpha");
    Document untitled(L"未命名-1"); // 无 url
    ExternalChangeMonitor monitor(disk.statQuery(), disk.contentQuery());

    REQUIRE(pollOne(monitor, untitled).empty());
    REQUIRE(disk.statCalls == 0);          // 连 stat 查询都不发起
    REQUIRE(disk.contentCalls == 0);
    REQUIRE(monitor.snapshotCount() == 0); // 不建快照

    untitled.setUrl(kFile); // 有 url 后进入正常管线（首轮自动建立）
    REQUIRE(pollOne(monitor, untitled).empty());
    REQUIRE(disk.statCalls == 1);
    REQUIRE(monitor.snapshotCount() == 1);
}

// ===========================================================================
// TC-106/108 的核心层基础：poll 只检测传入文档，未传入文档的快照保留
//（切回标签时按旧快照比对 → 立即检出变化，不因缺席轮重建基线而漏检）
// ===========================================================================
TEST_CASE("外部改动检测：poll 仅检测传入文档且未传入文档快照保留", "[core][watch]") {
    MockDisk disk;
    disk.put(kFile, 1000, "alpha");
    disk.put(kOtherFile, 1000, "bravo");
    Document docA = loadedDoc(kFile, "alpha");
    Document docB = loadedDoc(kOtherFile, "bravo");
    ExternalChangeMonitor monitor(disk.statQuery(), disk.contentQuery());
    REQUIRE(pollOne(monitor, docA).empty());
    REQUIRE(pollOne(monitor, docB).empty());
    REQUIRE(monitor.snapshotCount() == 2);

    // 只传 A（A 活动）：B 的磁盘被外部修改——不检测、不产出
    disk.put(kOtherFile, 5000, "bravo-external");
    REQUIRE(pollOne(monitor, docA).empty());
    REQUIRE(monitor.snapshotCount() == 2); // B 快照仍保留（不因缺席被清理）

    // B 重新成为活动文档（模拟切换标签立即检测）：按旧快照比对 → Modified
    const std::vector<Finding> findings = pollOne(monitor, docB);
    REQUIRE(findings.size() == 1);
    REQUIRE(findings[0].doc == &docB);
    REQUIRE(findings[0].verdict == Verdict::Modified);
}

// ===========================================================================
// 注入缺省的降级行为（DEC-006 防御默认）
// ===========================================================================
TEST_CASE("外部改动检测：注入缺省的降级行为", "[core][watch]") {
    Document doc = loadedDoc(kFile, "alpha");

    SECTION("无任何注入：按查询失败处理，惰性无动作") {
        ExternalChangeMonitor monitor;
        REQUIRE(pollOne(monitor, doc).empty());
        REQUIRE(monitor.snapshotCount() == 0); // 不建快照
    }
    SECTION("仅 stat 注入：内容注入缺失按「内容一致」降级") {
        MockDisk disk;
        disk.put(kFile, 1000, "alpha");
        ExternalChangeMonitor monitor(disk.statQuery(), nullptr);
        REQUIRE(pollOne(monitor, doc).empty()); // 建快照
        disk.put(kFile, 2000, "beta-external"); // stat 变化、内容注入缺失
        REQUIRE(pollOne(monitor, doc).empty()); // 不产出 Modified（宁漏报不误报）
    }
}
