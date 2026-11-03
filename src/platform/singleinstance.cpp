#include "platform/singleinstance.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlEngine>

#include <utility>

#include "document/documentmanager.h"

namespace {
/// 连接既有实例的超时（毫秒）：正常无实例时立即失败，1000ms 兜底对端慢启动
constexpr int kConnectTimeoutMs = 1000;
/// 单连接分帧缓冲上限：超出（无换行的超长数据）视为异常客户端，断开防御
constexpr qsizetype kMaxFrameBufferBytes = 4 * 1024 * 1024;
} // namespace

SingleInstance *SingleInstance::s_instance = nullptr;

SingleInstance::SingleInstance(const QString &serverName, QObject *parent)
    : QObject(parent)
    , m_serverName(serverName.isEmpty() ? defaultServerName() : serverName)
{
}

SingleInstance::~SingleInstance()
{
    // m_server 及其子 socket 随 QObject 父子关系自动释放；服务名端点由
    // 下次启动的 removeServer 清理（Windows 命名管道随进程关闭自动消失）
    if (s_instance == this)
        s_instance = nullptr;
}

SingleInstance *SingleInstance::create(QQmlEngine *qmlEngine, QJSEngine *jsEngine)
{
    // 与 SessionManager::create 一致：实例无 QObject 父对象，由 C++ 侧持有
    // 直至进程退出；所有权固定 CppOwnership（QML 引擎不负责销毁）。
    // main 在引擎加载前先以 create(nullptr, nullptr) 创建并初始化（本工厂
    // 不依赖引擎参数），QML 侧后续 create() 复用同一实例——全局仅一个监听者
    Q_UNUSED(qmlEngine)
    Q_UNUSED(jsEngine)
    if (!s_instance)
        s_instance = new SingleInstance(defaultServerName());
    QJSEngine::setObjectOwnership(s_instance, QQmlEngine::CppOwnership);
    return s_instance;
}

SingleInstance *SingleInstance::instance()
{
    return s_instance;
}

QString SingleInstance::defaultServerName()
{
    // Windows 登录名为 USERNAME（其他平台多为 USER）；不可得时回退 qcnote
    QString user = qEnvironmentVariable("USERNAME");
    if (user.isEmpty())
        user = qEnvironmentVariable("USER");
    if (user.isEmpty())
        return QStringLiteral("qcnote");
    return QStringLiteral("qcnote-%1").arg(user);
}

SingleInstance::StartupRole SingleInstance::initialize(const QList<QUrl> &urls)
{
    if (m_initialized)
        return m_role; // 防御重复初始化（main 只调用一次，幂等返回上次结果）
    m_initialized = true;

    // 1) 既有主实例在场则转发：连接成功即认为对方存活，写完整帧后交由对方处理
    QLocalSocket probe;
    probe.connectToServer(m_serverName);
    if (probe.waitForConnected(kConnectTimeoutMs)) {
        probe.write(encodeFrame(urls)); // 空列表也发送：对方仅前置窗口不开新标签
        probe.waitForBytesWritten(kConnectTimeoutMs);
        // 兜底冲刷：Windows 同进程（测试）或对端事件循环繁忙时，写入完成通知
        // 须经事件循环送达——超时后数据可能仍滞留内部缓冲，随析构丢失；
        // 有界泵事件直至缓冲清空（跨进程正常场景已清空，立即跳过）
        QDeadlineTimer flushDeadline(kConnectTimeoutMs);
        while (probe.bytesToWrite() > 0 && !flushDeadline.hasExpired())
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        probe.disconnectFromServer();
        m_role = StartupRole::Forwarded;
        return m_role;
    }

    // 2) 无既有实例：清理上次崩溃可能的残留端点后监听，成为主实例
    QLocalServer::removeServer(m_serverName);
    m_server = new QLocalServer(this);
    // 仅同用户可连接（Unix 套接字文件权限；Windows 命名管道为机器级命名空间、
    // 跨登录会话共享——这正是服务名带用户名后缀的原因；跨用户访问由管道
    // 安全描述符限制，UserAccessOption 即显式收紧为仅当前用户）
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    connect(m_server, &QLocalServer::newConnection, this, &SingleInstance::onNewConnection);
    if (m_server->listen(m_serverName)) {
        m_role = StartupRole::Primary;
    } else {
        // 降级：无 IPC（后续右键转发不可用），但本地启动流程不中断
        qWarning().noquote()
            << QStringLiteral("SingleInstance：监听本地服务失败（%1），单实例转发不可用")
                   .arg(m_server->errorString());
        m_role = StartupRole::PrimaryDegraded;
    }
    appendPendingUrls(urls); // 本次启动参数入队（管理器已存在则立即刷出）
    return m_role;
}

void SingleInstance::openPendingUrls()
{
    flushPendingUrls();
}

QByteArray SingleInstance::encodeFrame(const QList<QUrl> &urls)
{
    // 单行紧凑 JSON + '\n' 结尾（协议帧整体，可直接 write 到 socket）
    QJsonArray array;
    for (const QUrl &url : urls)
        array.append(url.toString());
    QJsonObject object;
    object.insert(QStringLiteral("urls"), array);
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

QList<QUrl> SingleInstance::decodeFrame(const QByteArray &frame, bool *ok)
{
    if (ok)
        *ok = false;
    QByteArray payload = frame;
    if (payload.endsWith('\n'))
        payload.chop(1); // 容忍带结尾换行的输入（分帧后通常已剥离）
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return {};
    const QJsonValue urlsValue = document.object().value(QStringLiteral("urls"));
    if (!urlsValue.isArray())
        return {};
    QList<QUrl> urls;
    const QJsonArray array = urlsValue.toArray();
    for (const QJsonValue &value : array) {
        if (!value.isString())
            return {}; // 元素类型错误视为坏帧
        const QString text = value.toString();
        if (text.isEmpty())
            continue; // 空串跳过（防御，不影响整帧成功）
        urls.append(QUrl(text));
    }
    if (ok)
        *ok = true;
    return urls;
}

void SingleInstance::onNewConnection()
{
    while (QLocalSocket *socket = m_server->nextPendingConnection()) {
        socket->setParent(m_server); // 生命周期随服务（断开后 deleteLater 回收）
        m_buffers.insert(socket, QByteArray());
        connect(socket, &QLocalSocket::readyRead, this, &SingleInstance::onReadyRead);
        connect(socket, &QLocalSocket::disconnected, this, &SingleInstance::onSocketDisconnected);
        // destroyed 兜底清理（不经 disconnected 的销毁路径）：仅作键移除，不解引用
        connect(socket, &QObject::destroyed, this, [this, socket] { m_buffers.remove(socket); });
    }
}

void SingleInstance::onReadyRead()
{
    auto *socket = qobject_cast<QLocalSocket *>(sender());
    if (!socket || !m_buffers.contains(socket))
        return;
    QByteArray &buffer = m_buffers[socket];
    buffer.append(socket->readAll());
    if (buffer.size() > kMaxFrameBufferBytes) {
        // 无换行的超长数据：异常客户端，丢弃缓冲并断开，防内存无限增长
        qWarning("SingleInstance：IPC 帧超过缓冲上限，断开该连接");
        m_buffers.remove(socket);
        socket->abort();
        socket->deleteLater();
        return;
    }
    // 按 '\n' 分帧：半包留在缓冲累积，一次到达多帧则逐个处理（粘包切分）
    int newline = buffer.indexOf('\n');
    while (newline != -1) {
        const QByteArray frame = buffer.left(newline);
        buffer.remove(0, newline + 1);
        handleFrame(frame);
        newline = buffer.indexOf('\n');
    }
}

void SingleInstance::onSocketDisconnected()
{
    auto *socket = qobject_cast<QLocalSocket *>(sender());
    if (!socket)
        return;
    m_buffers.remove(socket);
    socket->deleteLater();
}

void SingleInstance::handleFrame(const QByteArray &frame)
{
    bool ok = false;
    const QList<QUrl> urls = decodeFrame(frame, &ok);
    if (!ok) {
        qWarning("SingleInstance：忽略无法解析的 IPC 帧");
        return;
    }
    appendPendingUrls(urls);      // 追加待打开队列；管理器就绪则立即刷出（发 urlsOpened）
    emit activationRequested();   // 仅 IPC 到达路径：请求前置窗口
}

void SingleInstance::appendPendingUrls(const QList<QUrl> &urls)
{
    if (urls.isEmpty())
        return;
    m_pendingUrls += urls;
    if (DocumentManager::instance())
        flushPendingUrls(); // 管理器已就绪（如 QML 建好后再收到 IPC）：立即打开
}

void SingleInstance::flushPendingUrls()
{
    if (m_pendingUrls.isEmpty() || !DocumentManager::instance())
        return; // 无待打开项，或管理器未就绪：保留队列，待 openPendingUrls 再试

    const QList<QUrl> urls = std::exchange(m_pendingUrls, QList<QUrl>());
    DocumentManager::instance()->openUrls(urls);
    emit urlsOpened(urls);
}
