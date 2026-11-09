#pragma once

#include <QtQmlIntegration/qqmlintegration.h>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

class Document;
class DocumentManager;
class QQmlEngine;
class QJSEngine;

/**
 * @brief 未保存修改临时缓存（草稿缓存）管理器（QML 单例）。
 *
 * 为所有已修改（脏）文档建立临时缓存：编辑防抖落盘、启动时恢复，最大限度
 * 避免崩溃/强杀丢失未保存修改。要点：
 * - 条目主键为文档 uid（<uid>.json，QSaveFile 原子写）：未命名文档可缓存、
 *   另存为后 URL 变化不影响主键（DEC-002）；
 * - 写入：contentChanged 且 modified → 1000ms 单触发防抖（restart 聚合高频
 *   编辑），只重写变化文档的条目；写失败 qWarning 静默，不打断编辑（DEC-005）；
 * - 清理：modified 变 false（保存/另存为/撤回基线/重载覆盖）或关闭时仍
 *   modified（“不保存”放弃路径）→ 删除条目（DEC-003）；
 * - 失焦即写：QGuiApplication::applicationStateChanged(ApplicationInactive)
 *   时立即 flushPending()（DEC-003）——强杀前切窗口是高概率路径，把丢失
 *   窗口从「最后 1 秒防抖」压缩到「失焦前 1 秒」；C++ 单测环境
 *   （QCoreApplication，无该信号）构造时判空跳过连接；
 * - 恢复：restoreDrafts() 在会话恢复之后调用；同 URL 多条目先去重（仅留
 *   savedAt 最新一条，平局按 uid 字典序保留较大者，落选条目即删，防止
 *   Esc 未决残留 + 再崩溃产生的较旧草稿覆盖较新草稿）；磁盘未被外部修改
 *   （基线指纹相符）→ 静默应用为未保存修改；不符/文件缺失 → conflictsFound
 *   交 QML 弹窗，经 resolveConflicts() 应用或丢弃；应用/丢弃后即删条目
 *   （DEC-007）；未命名条目按会话记录位置升序按位插入（关闭现场还原
 *   DEC-002），激活三级优先（activeUid 匹配 > 会话活动复位 > 首个恢复
 *   文档，DEC-003）；
 * - 目录定位：便携模式（复用 SessionManager::resolvePortableIniPath 检测）
 *   为 ini 同目录 drafts/，否则 AppLocalDataLocation 下 drafts/（DEC-004）。
 * 可脱离 QML 引擎在 C++ 测试中直接 new（注入 DocumentManager 与缓存目录）；
 * QML 侧经 create() 复用同一实例。
 */
class DraftCacheManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    /**
     * @param manager 文档管理器（生命周期须覆盖本实例）
     * @param cacheDirPath 缓存目录；为空时按便携模式/用户数据目录解析默认值
     *                    （DEC-004），测试注入临时目录，不污染真实配置
     */
    explicit DraftCacheManager(DocumentManager *manager, const QString &cacheDirPath = QString(),
                               QObject *parent = nullptr);
    ~DraftCacheManager() override;

    /// QML 单例工厂：复用同一实例（生命周期归 C++ 管理），内部经
    /// DocumentManager::create 保证管理器单例先于本单例存在（同 SessionManager）
    static DraftCacheManager *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);

    /// 取当前实例（可能为 nullptr），供其他单例/调试协同
    static DraftCacheManager *instance();

    /// 启动恢复（QML 在 SessionManager.restoreDocuments() 之后调用一次）：
    /// 先全量解析并对同 URL 条目去重（仅留 savedAt 最新一条，平局按 uid 字典
    /// 序保留较大者，落选条目即删）；有 url 条目先行处理（代开/静默应用/冲突
    /// 暂存）；未命名条目按会话记录位置升序按位插入还原标签顺序（无位置
    /// 记录/无会话实例的追加尾部，DEC-002 稳定插入）；激活三级优先：会话
    /// activeUid 匹配 > 会话活动标签复位 > 空场景激活首个恢复文档（DEC-003）
    Q_INVOKABLE void restoreDrafts();

    /// 冲突处理（conflictsFound 之后由 QML 调用）：restore=true 应用缓存内容
    /// （文件缺失条目恢复为未命名文档），false 丢弃缓存；两者均删除条目文件
    Q_INVOKABLE void resolveConflicts(bool restore);

    /// 防抖挂起立即写盘（测试/退出兜底）
    void flush();

    /// 失焦即写（DEC-003）：应用切到后台（ApplicationInactive）时立即把
    /// 防抖挂起的脏文档落盘——用户强杀前的「输入 → 切窗口」是高概率路径，
    /// 把丢失窗口压缩到失焦前 1 秒。构造函数自动连接
    /// QGuiApplication::applicationStateChanged 到本方法；C++ 单测可直接
    /// 调用（无需真实窗口失焦）。其余状态（Active/Suspended 等）不动作
    void onApplicationStateChanged(Qt::ApplicationState state);

signals:
    /// 恢复时发现冲突（磁盘被外部修改/文件缺失）：每项为
    /// QVariantMap{url: QUrl, name: QString, missing: bool}，QML 汇总弹窗后
    /// 经 resolveConflicts 统一处理（DEC-006）
    void conflictsFound(const QVariantList &conflicts);

private:
    /// 恢复过程中暂存的冲突条目（含完整恢复数据，等 QML 决策）
    struct PendingConflict
    {
        QString entryPath;      ///< 条目文件绝对路径（决策后删除）
        QUrl url;
        QString name;
        bool missing = false;   ///< true=文件缺失/无法打开
        QString content;        ///< 缓存内容
        int cursor = 0;         ///< 缓存光标（应用时钳制到内容长度）
    };

    /// 为管理器中尚未连接的文档补连 contentChanged/modifiedChanged 信号
    /// （documentOpened 只覆盖 openUrls 路径，newDocument/restoreUntitledDraft
    /// 不发它，故经 documentsChanged 遍历对比集合完成连接与清理；已析构文档
    /// 由 QObject 自动断连信号，集合中悬垂指针仅作键、不解引用）
    void connectDocumentSignals();
    /// 防抖到期：逐个校验文档仍存活（documents() 成员）且仍 modified 后写条目
    void flushPending();
    /// 把单个脏文档的条目原子写入缓存目录（失败 qWarning 静默）
    void writeEntry(Document *doc);
    /// 删除文档对应条目并移出挂起集合（清理路径共用，DEC-003）
    void removeEntry(Document *doc);
    /// 条目文件完整路径（<uid>.json）
    QString entryPath(const QString &uid) const;

    static DraftCacheManager *s_instance;

    QPointer<DocumentManager> m_manager; ///< QPointer 防御“管理器先于本对象析构”的时序（同 SessionManager）：销毁自动置空，各判空点随即命中、安全跳过不崩溃
    QString m_cacheDir;                  ///< 缓存目录（构造时定死：注入/便携/用户数据）
    QTimer m_debounce;                   ///< 防抖：1000ms 单触发（CoarseTimer）
    QSet<Document *> m_pending;          ///< 防抖挂起的待写文档（悬垂指针仅作键，flush 时校验存活）
    QSet<Document *> m_connectedDocuments; ///< 已连接信号文档（documentsChanged 时对比收缩）
    QList<PendingConflict> m_conflicts;  ///< restoreDrafts 暂存的冲突（resolveConflicts 后清空）
};
