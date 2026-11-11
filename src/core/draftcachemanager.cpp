#include "core/draftcachemanager.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <limits>
#include <system_error>

#include "core/document.h"
#include "core/inifile.h"

namespace core {

namespace {

/// 解析出的条目（附文件路径；去重与分类共用）
struct ParsedDraft {
    std::filesystem::path file;
    DraftEntry entry;
};

/// 同 URL 新旧判定（DEC-009）：savedAt 最新者胜；平局按 uid 字典序保留较大者
///（确定性，不依赖目录枚举顺序）
bool newerThan(const DraftEntry& lhs, const DraftEntry& rhs) {
    if (lhs.savedAt != rhs.savedAt)
        return lhs.savedAt > rhs.savedAt;
    return lhs.uid > rhs.uid;
}

/// 会话记录的未命名绝对位置（uid 匹配；无记录 -1）
int recordedPosition(const SessionData& session, const std::string& uid) {
    if (uid.empty())
        return -1;
    for (const auto& entry : session.documents) {
        if (entry.uid == uid)
            return entry.position;
    }
    return -1;
}

std::wstring fileNameOf(const std::filesystem::path& url) {
    const std::filesystem::path name = url.filename();
    return name.empty() ? std::wstring() : name.wstring();
}

} // namespace

// ---------------------------------------------------------------------------
// 条目构建与持久化
// ---------------------------------------------------------------------------

DraftEntry DraftCacheManager::entryFor(const Document& doc, long long savedAt) {
    DraftEntry entry;
    entry.uid = doc.uid();
    entry.url = doc.url();
    entry.untitledName = doc.displayName(); // url 文档为文件名（冲突显示回退）
    entry.content = doc.content();
    entry.cursor = std::clamp(doc.cursorPosition(), 0, static_cast<int>(entry.content.size()));
    entry.lineEnding = doc.lineEnding();
    entry.encoding = doc.encoding();
    entry.hasBom = doc.hasBom();
    entry.baselineHash = doc.baselineHash();
    entry.savedAt = savedAt;
    return entry;
}

std::filesystem::path DraftCacheManager::entryPath(const std::filesystem::path& draftsDir,
                                                   const std::string& uid) {
    return draftsDir / (uid + ".json");
}

bool DraftCacheManager::persist(const std::filesystem::path& draftsDir, const DraftEntry& entry) {
    nlohmann::json json;
    json["version"] = 1;
    json["uid"] = entry.uid;
    json["url"] = SessionManager::pathToUrlValue(entry.url);
    json["untitledName"] = wideToUtf8(entry.untitledName);
    json["content"] = entry.content;
    json["cursor"] = entry.cursor;
    json["lineEnding"] = entry.lineEnding == LineEnding::Crlf ? "crlf" : "lf";
    json["encoding"] = entry.encoding == TextEncoding::Gbk ? "gbk" : "utf8";
    json["hasBom"] = entry.hasBom;
    json["baselineHash"] = entry.baselineHash;
    json["savedAt"] = entry.savedAt;
    // dump 不做 ASCII 转义：内容为 UTF-8/LF 契约字节，明文可读（DEC-008）
    return writeFileAtomically(entryPath(draftsDir, entry.uid), json.dump(),
                               /*createParentDirs=*/true);
}

std::optional<DraftEntry> DraftCacheManager::readEntry(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
        return std::nullopt;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return std::nullopt;
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    const nlohmann::json json = nlohmann::json::parse(bytes, nullptr, /*allow_exceptions=*/false);
    if (json.is_discarded() || !json.is_object())
        return std::nullopt; // 损坏：静默删跳过（不阻断启动）
    const auto version = json.find("version");
    if (version == json.end() || !version->is_number_integer() || version->get<long long>() != 1)
        return std::nullopt; // 版本不符：静默删跳过

    DraftEntry entry;
    // 字段级宽容：类型非法/缺失按默认（旧版 toString()/toInt() 同语义）
    entry.uid = json.value("uid", "");
    entry.url = SessionManager::urlValueToPath(json.value("url", ""));
    // 旧字段名 "name" 容忍（Qt 版资产；新写一律 untitledName）
    entry.untitledName = utf8ToWide(json.value("untitledName", json.value("name", "")));
    entry.content = json.value("content", "");
    entry.cursor = json.value("cursor", 0);
    entry.lineEnding = json.value("lineEnding", "lf") == "crlf" ? LineEnding::Crlf
                                                                : LineEnding::Lf;
    const std::string encoding = json.value("encoding", "utf8");
    entry.encoding = encoding == "gbk" ? TextEncoding::Gbk : TextEncoding::Utf8;
    entry.hasBom = json.value("hasBom", false);
    entry.baselineHash = json.value("baselineHash", "");
    entry.savedAt = json.value("savedAt", 0LL);
    // 光标钳制到内容字节长度（文件被外部改短/记录损坏时防越界）
    entry.cursor = std::clamp(entry.cursor, 0, static_cast<int>(entry.content.size()));
    return entry;
}

bool DraftCacheManager::removeEntryFile(const std::filesystem::path& draftsDir,
                                        const std::string& uid) {
    std::error_code ec;
    const std::filesystem::path path = entryPath(draftsDir, uid);
    if (!std::filesystem::exists(path, ec))
        return true; // 不存在视为成功（幂等；正常路径本就无条目）
    return std::filesystem::remove(path, ec);
}

// ---------------------------------------------------------------------------
// 写入协调（防抖管线）
// ---------------------------------------------------------------------------

DraftCacheManager::DraftCacheManager(std::filesystem::path draftsDir, Clock clock)
    : m_draftsDir(std::move(draftsDir))
    , m_clock(std::move(clock)) {}

void DraftCacheManager::removeFromPending(const Document* doc) {
    m_pending.erase(std::remove(m_pending.begin(), m_pending.end(), doc), m_pending.end());
}

void DraftCacheManager::removeEntryFor(const Document& doc) {
    removeFromPending(&doc);
    removeEntryFile(m_draftsDir, doc.uid());
}

void DraftCacheManager::onContentChanged(Document* doc, long long now) {
    if (!doc)
        return;
    if (doc->modified()) {
        // 编辑置脏：挂起防抖写（trigger restart 语义聚合高频编辑）
        if (std::find(m_pending.begin(), m_pending.end(), doc) == m_pending.end())
            m_pending.push_back(doc);
        m_debounce.trigger(now);
    } else {
        // 内容撤回到基线：立即删条目并移出挂起（DEC-003）
        removeEntryFor(*doc);
    }
}

void DraftCacheManager::onModifiedCleared(Document* doc) {
    if (!doc)
        return;
    // 保存/另存为/重载覆盖路径（无 contentChanged）→ 删条目（DEC-003）
    removeEntryFor(*doc);
}

void DraftCacheManager::onDocumentClosed(Document* doc) {
    if (!doc)
        return;
    removeFromPending(doc);
    if (doc->modified())
        removeEntryFile(m_draftsDir, doc->uid()); // 放弃路径：仍脏 → 删条目（真丢）
}

bool DraftCacheManager::flushDue(long long now) {
    if (!m_debounce.flushDue(now))
        return false;
    writePending();
    return true;
}

void DraftCacheManager::flushNow() {
    m_debounce.cancel();
    writePending();
}

void DraftCacheManager::writePending() {
    // 写出前逐个校验仍脏：保存/回基线路径经 onModifiedCleared/onContentChanged
    // 已摘除，此处为防御性复核（对齐旧版 flushPending 的 modified 检查）
    for (Document* doc : m_pending) {
        if (doc && doc->modified()) {
            const long long savedAt = m_clock ? m_clock() : 0;
            persist(m_draftsDir, entryFor(*doc, savedAt)); // 失败静默容错（DEC-005）
        }
    }
    m_pending.clear();
}

// ---------------------------------------------------------------------------
// 恢复（操作计划）
// ---------------------------------------------------------------------------

RestoreOutcome DraftCacheManager::restore(const std::filesystem::path& draftsDir,
                                          const SessionData& session, DiskHashQuery diskHash) {
    RestoreOutcome outcome;
    std::error_code ec;
    if (!std::filesystem::is_directory(draftsDir, ec))
        return outcome; // 无缓存目录：首启/正常退出后无残留，行为与现状一致

    // 阶段一：全量解析（*.json 扩展名大小写不敏感；条目文件名排序保证确定性
    //——文件名即 uid，等价旧版 QDir Name 序；不依赖目录枚举顺序）
    std::vector<ParsedDraft> parsed;
    std::error_code iterEc;
    for (std::filesystem::directory_iterator it(draftsDir, iterEc), end;
         it != end; it.increment(iterEc)) {
        if (iterEc)
            break;
        std::error_code entryEc;
        const std::filesystem::path file = it->path();
        std::wstring ext = file.extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (ext != L".json" || !it->is_regular_file(entryEc))
            continue;
        auto entry = readEntry(file);
        if (!entry) {
            std::error_code rmEc;
            std::filesystem::remove(file, rmEc); // 损坏/版本不符：静默删除跳过
            continue;
        }
        if (entry->uid.empty())
            entry->uid = wideToUtf8(file.stem().wstring()); // 容忍：以文件名为键
        parsed.push_back(ParsedDraft{file, std::move(*entry)});
    }
    std::sort(parsed.begin(), parsed.end(), [](const ParsedDraft& a, const ParsedDraft& b) {
        return a.file.wstring() < b.file.wstring();
    });

    // 阶段二：同 URL 去重（仅 url 非空；savedAt 最新，平局 uid 字典序大者；
    // 落选立即删文件。动机：冲突框未决残留 + 再崩溃的同 URL 双条目，防止较旧
    // 草稿覆盖较新草稿——DEC-009。未命名条目各自独立，不参与去重）
    std::vector<ParsedDraft> kept;
    for (auto& candidate : parsed) {
        if (candidate.entry.url.empty()) {
            kept.push_back(std::move(candidate)); // 未命名：不去重
            continue;
        }
        const std::wstring urlKey = candidate.entry.url.wstring();
        auto existing = std::find_if(kept.begin(), kept.end(), [&](const ParsedDraft& k) {
            return !k.entry.url.empty() && k.entry.url.wstring() == urlKey;
        });
        if (existing == kept.end()) {
            kept.push_back(std::move(candidate));
            continue;
        }
        if (newerThan(candidate.entry, existing->entry)) {
            std::filesystem::remove(existing->file, ec); // 删除失败静默容错：不阻断
            *existing = std::move(candidate);
        } else {
            std::filesystem::remove(candidate.file, ec);
        }
    }

    // 会话恢复后将存在活动文档 ⇔ 任一 url 条目磁盘可用（B 层会话恢复打开成功
    // 的判定近似：文件存在即可读；打开失败的边缘场景由 activeBefore 为空的
    // B 层兜底路径覆盖）
    bool sessionOpenedAny = false;
    for (const auto& entry : session.documents) {
        if (!entry.url.empty() && diskHash && diskHash(entry.url)) {
            sessionOpenedAny = true;
            break;
        }
    }

    // 阶段三 a：url 条目先行（代开/静默应用/冲突暂存——分类前已去重，处理序
    // 即文件名序；文件文档先于未命名就位保证阶段三 b 按位插入的时序，DEC-002）
    for (auto& item : kept) {
        if (item.entry.url.empty())
            continue;
        const std::optional<std::string> disk = diskHash ? diskHash(item.entry.url)
                                                         : std::nullopt;
        if (disk && *disk == item.entry.baselineHash) {
            // 磁盘未被外部修改：静默恢复（应用为未保存修改，不弹窗不激活），
            // 删条目（应用触发的续写按新 uid 正常再入防抖——预期的续写行为）
            DraftRestorePlan plan;
            plan.target = DraftRestorePlan::Target::ApplyToUrlDoc;
            plan.entry = item.entry;
            plan.url = item.entry.url;
            outcome.restored.push_back(std::move(plan));
            std::filesystem::remove(item.file, ec);
        } else {
            // 外部已修改磁盘 / 文件缺失：冲突条目（暂存不删文件、不覆盖）
            DraftConflict conflict;
            conflict.entry = item.entry;
            conflict.entryPath = item.file;
            conflict.missing = !disk.has_value();
            const std::wstring fileName = fileNameOf(item.entry.url);
            conflict.displayName = !fileName.empty() ? fileName : item.entry.untitledName;
            outcome.conflicts.push_back(std::move(conflict));
        }
    }

    // 阶段三 b：未命名条目按会话绝对位置升序稳定插入（DEC-002/006）。
    // 会话条目编号是标签栏完整序列的绝对位置：会话恢复后文件已按编号就位，
    // 未命名升序逐个插入绝对位置，最终顺序与关闭时一致；某些文件缺失被跳过
    // 时编号整体前移，B 层 clampInsertIndex 钳制的近似维持未命名彼此相对序
    //（DEC-008）。无位置记录（草稿多于会话/旧格式/无会话）排最后、按追加处理
    std::vector<std::size_t> unnamedOrder;
    for (std::size_t i = 0; i < kept.size(); ++i) {
        if (kept[i].entry.url.empty())
            unnamedOrder.push_back(i);
    }
    const auto sortKey = [&](std::size_t i) {
        const int recorded = recordedPosition(session, kept[i].entry.uid);
        return recorded < 0 ? std::numeric_limits<int>::max() : recorded;
    };
    std::stable_sort(unnamedOrder.begin(), unnamedOrder.end(),
                     [&](std::size_t lhs, std::size_t rhs) {
                         return sortKey(lhs) < sortKey(rhs);
                     });
    for (const std::size_t idx : unnamedOrder) {
        DraftRestorePlan plan;
        plan.target = DraftRestorePlan::Target::NewUntitled;
        plan.entry = kept[idx].entry;
        plan.insertIndex = recordedPosition(session, kept[idx].entry.uid); // -1=追加
        outcome.restored.push_back(std::move(plan));
        std::filesystem::remove(kept[idx].file, ec);
    }

    // 激活三级优先（DEC-003 + 兜底）：
    // ① 会话 activeUid 匹配的本轮恢复条目——关闭时真实活动标签的最精确表达
    //  （空串不参与匹配，DEC-007；冲突未决条目不参与——尚未恢复）；
    // ② 会话恢复设定的活动标签保持（B 层 no-op / activeBefore 复位）；
    // ③ 空场景（会话一个文件都没打开成功）激活首个恢复文档——避免恢复后
    //    停留在欢迎页（DEC-002 兜底）
    const std::string activeUid = session.activeUid.value_or("");
    if (!activeUid.empty()) {
        for (std::size_t i = 0; i < outcome.restored.size(); ++i) {
            if (outcome.restored[i].entry.uid == activeUid) {
                outcome.activatedRestoredIndex = static_cast<int>(i);
                break;
            }
        }
    }
    if (outcome.activatedRestoredIndex < 0 && !sessionOpenedAny && !outcome.restored.empty())
        outcome.activatedRestoredIndex = 0;

    return outcome;
}

ConflictResolution DraftCacheManager::resolveConflicts(std::vector<DraftConflict>& conflicts,
                                                       bool restore, bool hasActiveDocument,
                                                       DiskHashQuery diskHash) {
    ConflictResolution result;
    for (auto& conflict : conflicts) {
        // 应用/丢弃均删除条目文件（DEC-007——避免 uid 漂移歧义 + 孤儿清理）
        std::error_code ec;
        std::filesystem::remove(conflict.entryPath, ec);
        if (!restore)
            continue;
        DraftRestorePlan plan;
        plan.entry = conflict.entry;
        const std::optional<std::string> disk =
            diskHash ? diskHash(conflict.entry.url) : std::nullopt;
        if (disk.has_value()) {
            // 磁盘仍在（外部修改冲突）：应用到 url 文档（B 层打开/复用后应用）
            plan.target = DraftRestorePlan::Target::ApplyToUrlDoc;
            plan.url = conflict.entry.url;
        } else {
            // 磁盘缺失：恢复为未命名文档（沿用冲突显示名，旧版同款）
            plan.target = DraftRestorePlan::Target::NewUntitled;
            plan.entry.untitledName = conflict.displayName;
            plan.insertIndex = -1;
        }
        result.plans.push_back(std::move(plan));
    }
    conflicts.clear();
    // 恢复前无活动标签且确有恢复文档：激活首个恢复（DEC-006 对齐——用户已有
    // 活动标签时不抢占；丢弃分支不激活）
    result.activateFirst = restore && !hasActiveDocument && !result.plans.empty();
    return result;
}

} // namespace core
