#pragma once

#include <QtQmlIntegration/qqmlintegration.h>
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

class QLocalServer;
class QLocalSocket;
class QQmlEngine;
class QJSEngine;

/**
 * @brief 单实例转发（QML 单例）。
 *
 * 资源管理器右键“用qcnote打开”经 "<exe>" "%1" 启动 qcnote 时的单实例语义：
 * - 启动早期（main 中、引擎加载前）调用 initialize()：能连上既有服务 →
 *   参数经本地 IPC 转发，调用方直接以退出码 0 结束；连不上 → 清理残留后
 *   监听，本实例成为主实例（监听失败降级，启动流程不中断）。
 * - 主实例侧收到 URL：追加待打开队列；DocumentManager 已就绪则立即刷出
 *   （openUrls + urlsOpened），未就绪仅入队（QML 稍后经 openPendingUrls()
 *   刷出）；每次 IPC 到达均发 activationRequested()（供 QML 前置窗口）。
 * - IPC 协议（UTF-8）：单行 JSON {"urls":["file:///D:/a%20b.txt",...]} +
 *   '\n' 结尾。encodeFrame/decodeFrame 为静态纯函数，编解码可独立单测；
 *   服务端按连接缓冲、按 '\n' 分帧，容忍半包累积与粘包，可处理多个先后
 *   到达的连接。
 *
 * 服务名默认 qcnote-<USERNAME>（环境变量不可得时回退 qcnote）；测试注入
 * 随机名隔离，绝不以默认名监听。可脱离 QML 引擎在 C++ 测试中直接 new；
 * main 在引擎加载前经 create(nullptr, nullptr) 创建并初始化（默认服务名），
 * QML 侧 create() 复用同一实例（生命周期归 C++ 管理）。
 *
 * 本类必须保持不可默认构造（首参 serverName 必填）：Qt 6 单例构造模式判定
 * 中 std::is_default_constructible 优先级高于 create 工厂，若默认可构造，
 * QML 引擎会绕过 create() 直接 new SingleInstance()，导致与 main 预建实例
 * 分裂（IPC 服务与待打开队列归属另一对象）——缺陷 D1 教训，机制对齐
 * SessionManager（首参必填）。
 */
class SingleInstance : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    /// initialize() 的启动角色结果
    enum class StartupRole {
        Forwarded,       ///< 已转发给既有主实例（调用方应直接以退出码 0 结束）
        Primary,         ///< 本实例成为主实例（持有本地 IPC 服务）
        PrimaryDegraded, ///< 主实例但监听失败（无 IPC，启动流程不中断）
    };
    Q_ENUM(StartupRole)

    /**
     * @param serverName 本地 IPC 服务名；为空时用默认 qcnote-<USERNAME>
     *                   （测试注入随机名，避免与真实运行实例冲突）
     */
    explicit SingleInstance(const QString &serverName, QObject *parent = nullptr);
    ~SingleInstance() override;

    /// QML 单例工厂：复用同一实例（生命周期归 C++ 管理，QML 引擎不负责销毁）；
    /// main 在引擎加载前已先调用 create(nullptr, nullptr) 完成初始化
    static SingleInstance *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);

    /// C++ 侧单例访问（未经 create() 创建时为 nullptr）
    static SingleInstance *instance();

    /**
     * @brief 单实例初始化（main 在引擎加载前调用一次；重复调用返回上次结果）。
     *
     * 尝试连接既有服务（约 1000ms 超时）：成功 → 发送载荷并返回 Forwarded
     * （空列表也发送：对方仅前置窗口不开新标签）；失败 → removeServer 清残留
     * 后 listen：成功 → 本次启动 URL 入待打开队列并返回 Primary；失败 →
     * qWarning 警告并返回 PrimaryDegraded（URL 仍入队，仅无 IPC）。
     */
    StartupRole initialize(const QList<QUrl> &urls);

    /// 刷出全部待打开 URL：管理器存在时经 openUrls 打开发 urlsOpened；
    /// 管理器未就绪则保留队列（无丢失），供 QML 在就绪后再次调用
    Q_INVOKABLE void openPendingUrls();

    /// 默认服务名：qcnote-<USERNAME>（不可得时回退 qcnote）
    static QString defaultServerName();

    /// 编码为完整 IPC 帧：单行紧凑 JSON + '\n' 结尾（UTF-8，可直接 write）
    static QByteArray encodeFrame(const QList<QUrl> &urls);

    /// 解码一帧（容忍末尾 '\n'）：解析失败置 *ok=false 并返回空列表
    static QList<QUrl> decodeFrame(const QByteArray &frame, bool *ok = nullptr);

signals:
    /// 待打开 URL 已刷出（无论 IPC 到达后立即刷出还是 openPendingUrls 刷出）
    void urlsOpened(const QList<QUrl> &urls);

    /// 请求前置窗口（仅 IPC 到达路径发出；空列表转发仅前置不打开）
    void activationRequested();

private:
    void onNewConnection();
    void onReadyRead();
    void onSocketDisconnected();
    /// 处理一个完整帧：解码入队 → 管理器就绪则立即刷出 → 发 activationRequested
    void handleFrame(const QByteArray &frame);
    /// urls 非空时追加待打开队列；管理器已存在则立即刷出
    void appendPendingUrls(const QList<QUrl> &urls);
    /// 刷出待打开队列：管理器未就绪时保留队列（无丢失）
    void flushPendingUrls();

    static SingleInstance *s_instance;

    QString m_serverName;                        ///< IPC 服务名（构造时解析，空则默认）
    QLocalServer *m_server = nullptr;            ///< 主实例监听（Forwarded 路径不创建）
    QHash<QLocalSocket *, QByteArray> m_buffers; ///< 每连接分帧缓冲（半包累积、粘包切分）
    QList<QUrl> m_pendingUrls;                   ///< 待打开队列（管理器未就绪时暂存）
    StartupRole m_role = StartupRole::Primary;   ///< initialize 结果缓存（重复调用返回）
    bool m_initialized = false;                  ///< 是否已初始化
};
