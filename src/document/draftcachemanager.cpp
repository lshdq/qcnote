#include "document/draftcachemanager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QJSEngine>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QQmlEngine>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVariantMap>

#include <algorithm>
#include <limits>
#include <utility>

#include "document/document.h"
#include "document/documentmanager.h"
#include "document/sessionmanager.h"

namespace {
/// restoreDrafts 阶段一全量解析出的条目（同 URL 去重与分类处理共用）
struct ParsedEntry
{
    QString path;         ///< 条目文件绝对路径
    QString uid;          ///< 文档 uid（同 URL 平局时按字典序保留较大者）
    QUrl url;             ///< 文档 URL（空 = 未命名条目，不参与去重）
    QString name;         ///< 记录的显示名
    QString content;      ///< 缓存内容
    int cursor = 0;       ///< 缓存光标（应用时钳制到内容长度）
    qint64 savedAt = 0;   ///< 写入时间戳（秒）；缺失/非法按 0（最旧）
    QString baselineHash; ///< 写入时刻基线指纹
};

/// 同 URL 新旧判定（审查应改 #1 去重规则）：savedAt 最新者胜；完全相同按
/// uid 字典序保留较大者——确定性平局规则，不依赖目录枚举顺序
bool entryNewerThan(const ParsedEntry &lhs, const ParsedEntry &rhs)
{
    if (lhs.savedAt != rhs.savedAt)
        return lhs.savedAt > rhs.savedAt;
    return lhs.uid > rhs.uid;
}
} // namespace

DraftCacheManager *DraftCacheManager::s_instance = nullptr;

DraftCacheManager::DraftCacheManager(DocumentManager *manager, const QString &cacheDirPath,
                                     QObject *parent)
    : QObject(parent)
    , m_manager(manager)
{
    // 缓存目录：测试注入显式路径；缺省优先便携模式（与 qcnote.ini 同目录的
    // drafts/，复用 SessionManager 的便携检测保持目录语义单一事实源），无标记
    // 时为用户数据目录（AppLocalDataLocation）下 drafts/（DEC-004）
    if (!cacheDirPath.isEmpty()) {
        m_cacheDir = cacheDirPath;
    } else {
        const QString portableIni =
                SessionManager::resolvePortableIniPath(QCoreApplication::applicationDirPath());
        const QString base = !portableIni.isEmpty()
                ? QFileInfo(portableIni).absolutePath()
                : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        m_cacheDir = QDir(base).filePath(QStringLiteral("drafts"));
    }

    // 防抖：1000ms 单触发聚合高频编辑（崩溃丢失窗口约 1 秒），CoarseTimer 精度足够
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(1000);
    m_debounce.setTimerType(Qt::CoarseTimer);
    connect(&m_debounce, &QTimer::timeout, this, &DraftCacheManager::flushPending);

    if (m_manager) {
        // documentsChanged 承担补连职责：newDocument/restoreUntitledDraft 不发
        // documentOpened，统一经 documentsChanged 遍历对比集合连接新文档
        connect(m_manager, &DocumentManager::documentsChanged, this,
                &DraftCacheManager::connectDocumentSignals);
        // 关闭时仍 modified：UI“不保存”放弃路径 → 删除条目（放弃即真丢，不恢复）。
        // 此时 doc 尚未析构（deleteLater），可安全读 uid/modified（DEC-003）
        connect(m_manager, &DocumentManager::documentClosed, this, [this](Document *doc) {
            if (doc && doc->modified())
                removeEntry(doc);
        });
        connectDocumentSignals(); // 构造时管理器可能已有文档（复用既有实例的场景）
    }

    // 退出兜底：防抖挂起状态在退出前落盘（正常退出时 pending 应已清空，无副作用）
    connect(qApp, &QCoreApplication::aboutToQuit, this, &DraftCacheManager::flush);

    // 失焦即写（DEC-003）：切窗口（失焦）是强杀前的高概率路径，立即落盘把
    // 丢失窗口压缩到失焦前 1 秒。C++ 单测环境为 QCoreApplication（无该
    // 信号），qobject_cast 判空跳过（防御：不假设 qApp 必为 QGuiApplication）
    if (auto *guiApp = qobject_cast<QGuiApplication *>(qApp))
        connect(guiApp, &QGuiApplication::applicationStateChanged, this,
                &DraftCacheManager::onApplicationStateChanged);
}

DraftCacheManager::~DraftCacheManager()
{
    flush(); // 兜底落盘（挂起的脏内容在析构前写出）
    if (s_instance == this)
        s_instance = nullptr;
}

DraftCacheManager *DraftCacheManager::create(QQmlEngine *qmlEngine, QJSEngine *jsEngine)
{
    // 单例生命周期说明：与 SessionManager 一致，实例无 QObject 父对象，由 C++
    // 侧持有直至进程退出；所有权固定为 CppOwnership（QML 引擎不负责销毁）。
    // 经 DocumentManager::create 保证管理器单例先于本单例存在（幂等：已有实例
    // 直接复用，空时创建并登记），消除对 QML 侧引用顺序的隐式依赖
    if (!s_instance)
        s_instance = new DraftCacheManager(DocumentManager::create(qmlEngine, jsEngine));
    QJSEngine::setObjectOwnership(s_instance, QQmlEngine::CppOwnership);
    return s_instance;
}

DraftCacheManager *DraftCacheManager::instance()
{
    return s_instance;
}

void DraftCacheManager::restoreDrafts()
{
    if (!m_manager)
        return; // 无管理器（异常时序）：不动作
    m_conflicts.clear();

    // 进入时记录会话恢复设定的活动标签：代开路径 openUrls 会激活新开文档，
    // 恢复完成后须统一复位（TC-015：恢复不抢占活动标签）
    Document *const activeBefore = m_manager->activeDocument();
    // 首个恢复成功的文档（QPointer 防悬垂）：会话无活动标签时激活它（DEC-002，
    // 修复「未命名草稿恢复后界面停留在欢迎页/空白内容区」）；resolveConflicts
    // 路径不改激活（冲突恢复时用户已在交互中）
    QPointer<Document> firstRestored;

    // 关闭现场还原（DEC-001/003）：会话恢复期暂存的未命名位置/活动 uid 经
    // SessionManager::instance() 消费；无会话实例（C++ 测试环境）时全部回退
    // 现有规则（位置 -1 追加、无一级激活匹配）
    SessionManager *const session = SessionManager::instance();
    const QString pendingActiveUid = session ? session->pendingActiveDraftUid() : QString();
    QPointer<Document> activeUidMatch; // 一级激活候选：条目 uid 与会话 activeUid 同源的恢复文档

    QDir dir(m_cacheDir);
    if (!dir.exists())
        return; // 无缓存目录：首启/正常退出后无残留，行为与现状一致

    // 阶段一：全量解析条目（损坏/非对象/版本不符：静默删除跳过，不阻断启动，
    // TC-012）。解析与处理拆为两阶段，是为在分类处理前完成同 URL 去重
    QList<ParsedEntry> parsed;
    for (const QFileInfo &info : dir.entryInfoList({ QStringLiteral("*.json") }, QDir::Files)) {
        const QString path = info.absoluteFilePath();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "草稿缓存条目读取失败，已删除：" << path;
            QFile::remove(path);
            continue;
        }
        QJsonParseError parseError {};
        const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parseError);
        file.close();
        if (parseError.error != QJsonParseError::NoError || !json.isObject()
                || json.object().value(QStringLiteral("version")).toInt() != 1) {
            qWarning() << "草稿缓存条目损坏或版本不符，已删除：" << path;
            QFile::remove(path);
            continue;
        }
        const QJsonObject obj = json.object();
        ParsedEntry item;
        item.path = path;
        item.uid = obj.value(QStringLiteral("uid")).toString();
        item.url = QUrl(obj.value(QStringLiteral("url")).toString());
        item.name = obj.value(QStringLiteral("name")).toString();
        item.content = obj.value(QStringLiteral("content")).toString();
        item.cursor = obj.value(QStringLiteral("cursor")).toInt();
        // savedAt 缺失/类型非法/非整数值时 toInteger 均返回 0：按最旧处理
        item.savedAt = obj.value(QStringLiteral("savedAt")).toInteger();
        item.baselineHash = obj.value(QStringLiteral("baselineHash")).toString();
        parsed.append(item);
    }

    // 阶段二：同 URL 去重（审查应改 #1）。动机：冲突框 Esc（未决）后条目跨
    // 会话残留，之后编辑同一文件再次崩溃会在缓存目录产生同 URL 双条目；若
    // 不在分类前去重，静默路径与 resolveConflicts 均按目录枚举顺序逐条
    // setContent/删条目，较旧草稿可能覆盖较新草稿，导致较新草稿静默丢失。
    // 规则：url 非空条目按 URL 分组仅保留 savedAt 最新一条（平局按 uid 字典
    // 序保留较大者，不依赖枚举顺序），落选条目立即删除文件（qWarning 记录、
    // 静默容错）；未命名条目（url 为空）各自独立成文档，不参与去重。经此
    // 保证后续静默应用与 conflictsFound/resolveConflicts 处理的均为最新
    // 条目，同一 url 至多一项冲突
    QList<ParsedEntry> entries;
    QHash<QUrl, int> newestOfUrl; // url -> entries 中该 url 当前保留条目的下标
    for (const ParsedEntry &candidate : std::as_const(parsed)) {
        if (candidate.url.isEmpty()) {
            entries.append(candidate); // 未命名条目：不去重
            continue;
        }
        const auto it = newestOfUrl.find(candidate.url);
        if (it == newestOfUrl.end()) {
            newestOfUrl.insert(candidate.url, entries.size());
            entries.append(candidate);
            continue;
        }
        ParsedEntry &kept = entries[it.value()];
        if (entryNewerThan(candidate, kept)) {
            qWarning() << "同 URL 重复草稿条目，去重删除较旧条目：" << kept.path;
            QFile::remove(kept.path); // 删除失败静默容错：不阻断其余条目恢复
            kept = candidate;
        } else {
            qWarning() << "同 URL 重复草稿条目，去重删除较旧条目：" << candidate.path;
            QFile::remove(candidate.path);
        }
    }

    // 阶段三 a：有 url 条目先行处理（代开 / 静默应用 / 冲突暂存，去重后相对
    // 序不变）。文件文档先于未命名条目全部就位——会话恢复的文件本就已按编号
    // 占位，代开的文件追加在尾部，均不改变阶段三 b 按位插入所依赖的
    // 「文件先就位」时序（DEC-002 恢复时序的自然延伸）
    for (const ParsedEntry &entry : std::as_const(entries)) {
        if (entry.url.isEmpty())
            continue; // 未命名条目在阶段三 b 按会话位置升序处理

        // 有 url 条目：会话恢复未打开该文件时先尝试代开（本地文件存在且存储可用）
        Document *target = m_manager->documentForUrl(entry.url);
        if (!target && entry.url.isLocalFile() && QFileInfo(entry.url.toLocalFile()).isFile()
                && m_manager->storageForUrl(entry.url)) {
            m_manager->openUrls({ entry.url });
            target = m_manager->documentForUrl(entry.url);
        }
        if (!target) {
            // 文件缺失/打开失败：冲突条目（missing=true），暂存不删文件
            const QString displayName =
                    !entry.url.fileName().isEmpty() ? entry.url.fileName() : entry.name;
            m_conflicts.append(
                    { entry.path, entry.url, displayName, true, entry.content, entry.cursor });
            continue;
        }

        // 基线指纹比对：磁盘加载归一化内容 vs 条目记录的写入时刻基线
        if (target->savedContentHash() == entry.baselineHash) {
            // 磁盘未被外部修改：静默应用为未保存修改（不激活、不弹窗），删条目；
            // 应用触发 contentChanged 会按新 uid 正常再入防抖（预期的续写行为）
            target->setContent(entry.content);
            target->setCursorPosition(
                    qBound(0, entry.cursor, static_cast<int>(entry.content.length())));
            if (!firstRestored)
                firstRestored = target; // 代开后的静默应用也算恢复成功
            // pendingActiveUid 为空是常态（活动为文件或无记录），空串不参与一级匹配（DEC-007）
            if (!activeUidMatch && !pendingActiveUid.isEmpty() && entry.uid == pendingActiveUid)
                activeUidMatch = target; // 一级激活候选（uid 同源：会话记 activeUid 后该文档被另存为的极端时序）
            QFile::remove(entry.path);
        } else {
            // 外部应用已修改磁盘：冲突条目（missing=false），暂存不删文件、不覆盖
            m_conflicts.append(
                    { entry.path, entry.url, entry.name, false, entry.content, entry.cursor });
        }
    }

    // 阶段三 b：未命名条目按会话位置升序稳定插入（DEC-002）。会话条目编号是
    // 标签栏完整序列的绝对位置（暂存为 0 基）：会话恢复后文件文档已按编号就位，
    // 未命名条目升序逐个插入其绝对位置可证最终顺序与关闭时一致；某些文件缺失
    // 被跳过时编号整体前移，restoreUntitledDraft 内 qBound 钳制（目标位置与
    // 当前列表长度取 min）的稳定插入仍保证未命名条目彼此的相对顺序正确（近似
    // 接受，见需求边界「位置越界」）。无位置记录（草稿多于会话/旧格式/无
    // 会话实例）的条目排最后、恢复时按追加处理，组内保持枚举序（DEC-006）
    QList<int> unnamedOrder;
    for (int i = 0; i < entries.size(); ++i) {
        if (entries.at(i).url.isEmpty())
            unnamedOrder.append(i);
    }
    const auto recordedPosition = [session](const ParsedEntry &entry) {
        return session ? session->pendingDraftPosition(entry.uid) : -1;
    };
    const auto sortKey = [&recordedPosition](const ParsedEntry &entry) {
        // 无位置记录：排序键取 int 最大值（排最后），不复用 -1 追加哨兵的
        // 双重语义（-1 在升序中会排最前，与「排最后」矛盾，DEC-006）
        const int recorded = recordedPosition(entry);
        return recorded < 0 ? std::numeric_limits<int>::max() : recorded;
    };
    std::stable_sort(unnamedOrder.begin(), unnamedOrder.end(),
                     [&](int lhs, int rhs) {
                         return sortKey(entries.at(lhs)) < sortKey(entries.at(rhs));
                     });
    for (const int idx : unnamedOrder) {
        const ParsedEntry &entry = entries.at(idx);
        // 未命名条目：按会话位置按位重建未命名文档（内容即缓存、沿用记录显示
        // 名），光标钳制恢复，删条目（DEC-007）；无位置记录时显式传 -1 走
        // 追加语义，不依赖哨兵巧合（DEC-006）
        const int insertIndex = recordedPosition(entry) < 0 ? -1 : recordedPosition(entry);
        Document *restored = m_manager->restoreUntitledDraft(entry.content, entry.name,
                                                             insertIndex);
        restored->setCursorPosition(
                qBound(0, entry.cursor, static_cast<int>(entry.content.length())));
        if (!firstRestored)
            firstRestored = restored;
        // pendingActiveUid 为空是常态（活动为文件或无记录），空串不参与一级匹配（DEC-007）
        if (!activeUidMatch && !pendingActiveUid.isEmpty() && entry.uid == pendingActiveUid)
            activeUidMatch = restored; // 一级激活候选（关闭时的活动未命名标签）
        QFile::remove(entry.path);
    }

    if (!m_conflicts.isEmpty()) {
        // 汇总一次发出（DEC-006）：QML 弹单一对话框，经 resolveConflicts 统一处理
        QVariantList conflicts;
        conflicts.reserve(m_conflicts.size());
        for (const PendingConflict &conflict : std::as_const(m_conflicts)) {
            conflicts.append(QVariantMap{
                    { QStringLiteral("url"), conflict.url },
                    { QStringLiteral("name"), conflict.name },
                    { QStringLiteral("missing"), conflict.missing },
            });
        }
        emit conflictsFound(conflicts);
    }

    // 末尾激活三级优先（DEC-003），依序取第一个命中者：
    // ① 会话 activeUid 匹配的本轮恢复文档——用户关闭时真实活动标签的最精确
    //    表达（含活动标签为未命名的场景；无匹配时如有无内容未命名标签不恢复
    //    等，自然落入下一级）；
    // ② 会话恢复设定的活动标签复位（activeBefore）——代开路径 openUrls 会把
    //    活动标签顶成新开文档，此处统一复位；其余路径本就未改活动文档，
    //    setActiveDocument 同值早退无副作用（TC-015）；
    // ③ 空场景激活首个恢复文档（上一工作流 DEC-002 规则兜底）——会话无活动
    //    标签（activeBefore 为空：会话无文档、仅有未命名草稿等）时激活它，
    //    避免恢复后停留在欢迎页/空白内容区
    if (activeUidMatch && m_manager->documents().contains(activeUidMatch.data()))
        m_manager->setActiveDocument(activeUidMatch.data());
    else if (activeBefore && m_manager->documents().contains(activeBefore))
        m_manager->setActiveDocument(activeBefore);
    else if (!activeBefore && firstRestored && m_manager->documents().contains(firstRestored.data()))
        m_manager->setActiveDocument(firstRestored.data());
}

void DraftCacheManager::resolveConflicts(bool restore)
{
    // 恢复前若无活动文档：结束后激活首个恢复的文档（QPointer 防悬垂 +
    // documents().contains 校验，风格对齐 restoreDrafts 末尾激活逻辑）——
    // 与 DEC-002 空场景规则对齐，避免冲突恢复成功但界面停留在欢迎页
    // （DEC-006）；用户已有活动标签（activeBefore 非空）时不抢占。
    // 注：冲突恢复是用户显式交互决策的路径，不扩展会话位置/activeUid 匹配
    // （按位还原与一级激活仅在启动自动恢复 restoreDrafts 生效），维持
    // 上一工作流 DEC-006 的既有规则
    Document *const activeBefore = (restore && m_manager) ? m_manager->activeDocument() : nullptr;
    QPointer<Document> firstRestored;

    for (const PendingConflict &conflict : std::as_const(m_conflicts)) {
        Document *target = m_manager ? m_manager->documentForUrl(conflict.url) : nullptr;
        if (restore) {
            // 恢复：文档存在 → 应用缓存内容（光标钳制）；文件缺失条目 → 重建未命名文档
            if (target) {
                target->setContent(conflict.content);
                target->setCursorPosition(
                        qBound(0, conflict.cursor, static_cast<int>(conflict.content.length())));
                if (!firstRestored)
                    firstRestored = target;
            } else if (m_manager) {
                Document *restored =
                        m_manager->restoreUntitledDraft(conflict.content, conflict.name);
                restored->setCursorPosition(
                        qBound(0, conflict.cursor, static_cast<int>(conflict.content.length())));
                if (!firstRestored)
                    firstRestored = restored;
            }
        }
        // 应用/丢弃均删除条目文件（DEC-007）
        QFile::remove(conflict.entryPath);
    }
    m_conflicts.clear();

    // 恢复前无活动标签且本次确有恢复文档：激活首个恢复文档（丢弃分支不激活）
    if (!activeBefore && firstRestored && m_manager
            && m_manager->documents().contains(firstRestored.data()))
        m_manager->setActiveDocument(firstRestored.data());
}

void DraftCacheManager::flush()
{
    if (m_debounce.isActive())
        flushPending(); // 防抖挂起：立即落盘（flushPending 内部停表）
}

void DraftCacheManager::onApplicationStateChanged(Qt::ApplicationState state)
{
    // 失焦即写（DEC-003）：仅 ApplicationInactive 触发——切窗口（含为强杀做
    // 准备的切换）时立即落盘，把崩溃丢失窗口压缩到失焦前 1 秒；Active/
    // Suspended 等状态不动作（不重复写、不在恢复前台时做 IO）。无挂起时
    // flushPending 仅停表清空集合，天然幂等
    if (state == Qt::ApplicationInactive)
        flushPending();
}

void DraftCacheManager::flushPending()
{
    m_debounce.stop();
    if (!m_manager) {
        m_pending.clear(); // 管理器已析构：无法校验存活，直接丢弃挂起
        return;
    }
    const QList<Document *> documents = m_manager->documents();
    const QSet<Document *> alive(documents.cbegin(), documents.cend());
    for (Document *doc : std::as_const(m_pending)) {
        // 悬垂防护：仅 documents() 中的文档可写（约定同 SessionManager，集合中
        // 指针仅作键）；已关闭/内容已回到基线的文档不写
        if (alive.contains(doc) && doc->modified())
            writeEntry(doc);
    }
    m_pending.clear();
}

void DraftCacheManager::connectDocumentSignals()
{
    if (!m_manager)
        return;
    const QList<Document *> documents = m_manager->documents();
    // 收缩记录集合：离开列表的文档被对比剔除（QObject 析构已自动断连信号）
    m_connectedDocuments.intersect(QSet<Document *>(documents.cbegin(), documents.cend()));
    for (Document *doc : documents) {
        if (m_connectedDocuments.contains(doc))
            continue;
        connect(doc, &Document::contentChanged, this, [this, doc]() {
            if (doc->modified()) {
                // 编辑置脏：挂起防抖写（start 具备 restart 语义，聚合高频编辑）
                m_pending.insert(doc);
                m_debounce.start();
            } else {
                // 内容撤回到基线：立即删除条目并移出挂起集合
                removeEntry(doc);
            }
        });
        connect(doc, &Document::modifiedChanged, this, [this, doc]() {
            if (!doc->modified())
                // 变 false：保存/另存为/重载覆盖路径（无 contentChanged）→ 删条目（DEC-003）
                removeEntry(doc);
        });
        m_connectedDocuments.insert(doc);
    }
}

void DraftCacheManager::writeEntry(Document *doc)
{
    // 目录不存在先建（含多级父目录）；建不出来只能静默容错（不打断编辑）
    if (!QDir(m_cacheDir).exists() && !QDir().mkpath(m_cacheDir)) {
        qWarning() << "草稿缓存目录创建失败：" << m_cacheDir;
        return;
    }

    QJsonObject entry;
    entry.insert(QStringLiteral("version"), 1);
    entry.insert(QStringLiteral("uid"), doc->uid());
    entry.insert(QStringLiteral("url"), doc->url().toString());
    entry.insert(QStringLiteral("name"), doc->displayName());
    entry.insert(QStringLiteral("content"), doc->content());
    entry.insert(QStringLiteral("cursor"), doc->cursorPosition());
    entry.insert(QStringLiteral("lineEnding"),
                 doc->lineEnding() == Document::LineEnding::Crlf ? QStringLiteral("crlf")
                                                                  : QStringLiteral("lf"));
    entry.insert(QStringLiteral("encoding"),
                 doc->encoding() == Document::Encoding::Gbk ? QStringLiteral("gbk")
                                                            : QStringLiteral("utf8"));
    entry.insert(QStringLiteral("hasBom"), doc->hasBom());
    entry.insert(QStringLiteral("baselineHash"), doc->savedContentHash());
    entry.insert(QStringLiteral("savedAt"),
                 static_cast<qint64>(QDateTime::currentSecsSinceEpoch()));

    // QSaveFile 原子写：崩溃/断电不产生半截条目
    QSaveFile file(entryPath(doc->uid()));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "草稿缓存条目打开失败：" << file.fileName();
        return;
    }
    file.write(QJsonDocument(entry).toJson(QJsonDocument::Compact));
    if (!file.commit())
        qWarning() << "草稿缓存条目写入失败：" << file.fileName();
}

void DraftCacheManager::removeEntry(Document *doc)
{
    if (!doc)
        return;
    m_pending.remove(doc);
    const QString path = entryPath(doc->uid());
    if (QFile::exists(path) && !QFile::remove(path))
        qWarning() << "草稿缓存条目删除失败：" << path;
}

QString DraftCacheManager::entryPath(const QString &uid) const
{
    return QDir(m_cacheDir).filePath(uid + QStringLiteral(".json"));
}
