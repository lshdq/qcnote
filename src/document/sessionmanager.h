#pragma once

#include <QtQmlIntegration/qqmlintegration.h>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include <memory>

class Document;
class DocumentManager;
class QSettings;
class QQmlEngine;
class QJSEngine;

/**
 * @brief 会话恢复管理器（QML 单例）。
 *
 * 统一持有会话文件（QSettings，IniFormat）：
 * - [documents]：按编号记录上次打开的文件（URL、光标）与活动文档 URL；
 * - [window]：窗口 normal 几何（x/y/width/height）与最大化标志。
 *
 * 文档集合、活动文档、各文档 url 与光标的变化经 500ms 防抖统一写盘；
 * 恢复过程中置 m_restoring 屏蔽连锁写，恢复完成统一写一次。
 * 可脱离 QML 引擎在 C++ 测试中直接 new 使用（注入 DocumentManager
 * 与会话文件路径）；QML 侧经 create() 复用同一实例。
 */
class SessionManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    /**
     * @param manager 文档管理器（生命周期须覆盖本实例）
     * @param filePath 会话文件路径；为空时使用用户目录 qcnote.ini
     *                （IniFormat + UserScope，组织名/应用名均为 qcnote）；
     *                测试注入临时路径，不污染真实配置
     */
    explicit SessionManager(DocumentManager *manager, const QString &filePath = QString(),
                            QObject *parent = nullptr);
    ~SessionManager() override;

    /// QML 单例工厂：复用同一实例（生命周期归 C++ 管理），内部经 DocumentManager::create
    /// 保证管理器单例先于本单例存在（消除 QML 侧引用顺序的隐式依赖）
    static SessionManager *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);

    /// 恢复上次会话：按原顺序重开文件、逐文档恢复光标、恢复活动文档；缺失文件静默跳过
    Q_INVOKABLE void restoreDocuments();

    /// 校验后的窗口几何 {x, y, width, height, maximized}；无数据或位置离屏返回空 map（QML 用默认值）
    Q_INVOKABLE QVariantMap windowState() const;

    /// 记录窗口 normal 几何与最大化标志（缓存到成员 + 防抖写盘）
    Q_INVOKABLE void saveWindowState(int x, int y, int width, int height, bool maximized);

    /// 立即写盘（防抖挂起时用；退出经 aboutToQuit 兜底触发）
    void flush();

    /**
     * @brief 窗口几何校验（纯函数，可测）。
     *
     * 宽钳制到 [800, 所有屏幕最大可用宽]、高钳制到 [500, 所有屏幕最大可用高]
     * （多屏取各屏最大值；屏幕列表为空时仅下限钳制，屏幕过小时下限优先）；
     * 最终矩形须与任一屏幕相交，否则返回空 map（显示器变更后由 QML 回退默认几何）。
     */
    static QVariantMap validateWindowState(int x, int y, int width, int height, bool maximized,
                                           const QList<QRect> &screenGeometries);

private:
    void scheduleSave();
    /// 立即写盘（防抖超时、恢复完成对齐、flush 挂起时调用）
    void saveNow();
    /// 为管理器中尚未连接的文档补连 url/光标信号。documentOpened 只覆盖 openUrls
    /// 路径（newDocument 不发它），故经 documentsChanged 遍历对比集合完成连接与清理；
    /// 已析构文档由 QObject 自动断连信号，集合中悬垂指针仅作键、不解引用
    void connectDocumentSignals();

    static SessionManager *s_instance;

    QPointer<DocumentManager> m_manager;   ///< QPointer 防御“管理器先于本对象析构”的时序（如测试多引擎场景）：销毁自动置空，各判空点随即命中、安全跳过不崩溃
    std::unique_ptr<QSettings> m_settings;
    QTimer m_debounce;                     ///< 防抖：500ms 单触发
    bool m_restoring = false;              ///< 恢复中标志：屏蔽连锁 scheduleSave
    QSet<Document *> m_connectedDocuments; ///< 已连接信号文档（documentsChanged 时对比收缩）

    int m_windowX = 0;
    int m_windowY = 0;
    int m_windowWidth = 0;
    int m_windowHeight = 0;
    bool m_windowMaximized = false;
    bool m_windowValid = false; ///< 本实例是否持有有效窗口几何（决定写或移除 [window] 节）
};
