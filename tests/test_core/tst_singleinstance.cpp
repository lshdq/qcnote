#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QList>
#include <QLocalSocket>
#include <QMetaType>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>
#include <QUuid>
#include <QVariant>

#include <type_traits>

#include "document/documentmanager.h"
#include "platform/singleinstance.h"

// 编译期钉住缺陷 D1：若 SingleInstance 变回可默认构造，QML 引擎会绕过
// create 工厂直接 new，与 main 预建实例分裂
static_assert(!std::is_default_constructible_v<SingleInstance>,
              "SingleInstance 必须不可默认构造：否则 QML 引擎绕过 create 工厂导致单例分裂（D1）");

// 单实例转发：编解码纯函数往返、同进程回环（立即刷出）、主/从选举（转发语义）、
// 队列兜底（管理器未就绪 → openPendingUrls 刷出）、半包/粘包分帧。
// 所有用例使用随机服务名（绝不以默认服务名 qcnote-<用户名> 监听，避免与真实
// 运行实例冲突），对象为栈上临时实例，析构即释放监听与连接。
// DocumentManager::instance() 可能被同进程其他测试残留实例占用：需要
// “未就绪”前置的用例先以 { DocumentManager resetter(nullptr); } 构造-析构
// 对把 instance() 归零；需要“已就绪”的用例持本地管理器，断言只依赖本地对象。
class TestSingleInstance : public QObject
{
    Q_OBJECT

private slots:
    void frameRoundTrip();
    void loopbackOpensAndActivates();
    void primaryElectionForwardsToListener();
    void pendingQueueFlushesWhenManagerReady();
    void partialAndCoalescedFrames();
    void defaultServerNameIsSane();
};

/// 随机服务名：qcnote-test-si-<uuid>，与真实 qcnote 服务名空间隔离
static QString randomServerName()
{
    return QStringLiteral("qcnote-test-si-") + QUuid::createUuid().toString(QUuid::Id128);
}

/// 泵事件直至客户端写入缓冲清空（有界兜底）。Windows 同进程回环下
/// waitForBytesWritten 会超时（服务端 accept 依赖同线程事件循环，等待函数
/// 内部不泵通用事件），须以 processEvents 推动写完成通知送达
static void flushClientSocket(QLocalSocket *socket, int timeoutMs = 5000)
{
    QDeadlineTimer deadline(timeoutMs);
    while (!deadline.hasExpired() && socket->bytesToWrite() > 0)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

// 编解码纯函数往返：空格/中文/引号路径经 QUrl 编码后无损（带/不带结尾换行均
// 可解）；空列表合法；坏帧（半截 JSON、urls 非数组、空帧）ok=false 且返回空
void TestSingleInstance::frameRoundTrip()
{
    const QList<QUrl> urls = {
        QUrl::fromUserInput(QStringLiteral("D:/notes/a b.txt")),
        QUrl::fromUserInput(QStringLiteral("D:/笔记/新建 文档.txt")),
        QUrl::fromUserInput(QStringLiteral("D:/quo\"te'.txt")),
        QUrl(QStringLiteral("file:///E:/c%20d.txt")),
    };

    const QByteArray frame = SingleInstance::encodeFrame(urls);
    QVERIFY(frame.endsWith('\n'));           // 帧以 '\n' 结尾
    const QByteArray singleLine = frame.chopped(1);
    QVERIFY(!singleLine.contains('\n'));     // JSON 本体为单行

    bool ok = false;
    QCOMPARE(SingleInstance::decodeFrame(frame, &ok), urls); // 带结尾换行
    QVERIFY(ok);
    QCOMPARE(SingleInstance::decodeFrame(singleLine, &ok), urls); // 不带换行
    QVERIFY(ok);

    QCOMPARE(SingleInstance::decodeFrame(SingleInstance::encodeFrame({}), &ok),
             QList<QUrl>());
    QVERIFY(ok); // 空列表为合法载荷（仅前置窗口）

    ok = true;
    QVERIFY(SingleInstance::decodeFrame("{\"urls\":", &ok).isEmpty());
    QVERIFY(!ok); // 半截 JSON
    ok = true;
    QVERIFY(SingleInstance::decodeFrame("{\"urls\":42}", &ok).isEmpty());
    QVERIFY(!ok); // urls 非数组
    ok = true;
    QVERIFY(SingleInstance::decodeFrame("", &ok).isEmpty());
    QVERIFY(!ok); // 空帧
}

// 同进程回环：随机服务名主实例 + QLocalSocket 客户端发帧 → 本地管理器在场时
// 立即刷出 urlsOpened（URL 无损）并发 activationRequested（仅 IPC 路径）；
// 支持多个先后到达的连接（分帧缓冲按连接隔离）
void TestSingleInstance::loopbackOpensAndActivates()
{
    DocumentManager manager(nullptr); // 本地管理器在场 → 走“立即刷出”路径
    const QString name = randomServerName();
    SingleInstance si(name);
    QCOMPARE(si.initialize({}), SingleInstance::StartupRole::Primary);

    QSignalSpy openedSpy(&si, &SingleInstance::urlsOpened);
    QSignalSpy activationSpy(&si, &SingleInstance::activationRequested);
    const QList<QUrl> urls{ QUrl(QStringLiteral("file:///D:/loop%20one.txt")) };

    QLocalSocket client;
    client.connectToServer(name);
    QVERIFY(client.waitForConnected(5000));
    client.write(SingleInstance::encodeFrame(urls));
    flushClientSocket(&client);
    client.disconnectFromServer();

    QTRY_COMPARE(openedSpy.count(), 1);
    QCOMPARE(openedSpy.at(0).at(0).value<QList<QUrl>>(), urls);
    QTRY_COMPARE(activationSpy.count(), 1);

    // 第二个连接（前一连接已断开）：分帧缓冲互不串扰，仍正确解析
    QLocalSocket client2;
    client2.connectToServer(name);
    QVERIFY(client2.waitForConnected(5000));
    const QList<QUrl> urls2{ QUrl(QStringLiteral("file:///D:/loop%20two.txt")) };
    client2.write(SingleInstance::encodeFrame(urls2));
    flushClientSocket(&client2);
    client2.disconnectFromServer();

    QTRY_COMPARE(openedSpy.count(), 2);
    QCOMPARE(openedSpy.at(1).at(0).value<QList<QUrl>>(), urls2);
    QTRY_COMPARE(activationSpy.count(), 2);
}

// 主/从选举：全新随机名首个 initialize → Primary；同名第二对象（首实例仍
// 监听）initialize → Forwarded 且自身不本地打开；首实例收到数据并立即刷出
void TestSingleInstance::primaryElectionForwardsToListener()
{
    DocumentManager manager(nullptr); // 首实例侧管理器在场 → 收到即刷出
    const QString name = randomServerName();

    SingleInstance first(name);
    QCOMPARE(first.initialize({}), SingleInstance::StartupRole::Primary);

    QSignalSpy openedSpy(&first, &SingleInstance::urlsOpened);
    QSignalSpy activationSpy(&first, &SingleInstance::activationRequested);

    SingleInstance second(name);
    QSignalSpy secondOpenedSpy(&second, &SingleInstance::urlsOpened);
    const QList<QUrl> urls{ QUrl(QStringLiteral("file:///D:/elect%20ion.txt")) };
    QCOMPARE(second.initialize(urls), SingleInstance::StartupRole::Forwarded);
    QCOMPARE(secondOpenedSpy.count(), 0); // 转发方不本地打开

    QTRY_COMPARE(openedSpy.count(), 1); // 首实例收到数据
    QCOMPARE(openedSpy.at(0).at(0).value<QList<QUrl>>(), urls);
    QTRY_COMPARE(activationSpy.count(), 1);
}

// 队列兜底（管理器未就绪）：initialize 的启动参数仅入队不发信号；
// openPendingUrls 在无管理器时保留队列（无丢失），管理器就绪后刷出全部
// 待打开 URL；队列清空后再次调用无新信号、无崩溃
void TestSingleInstance::pendingQueueFlushesWhenManagerReady()
{
    { DocumentManager resetter(nullptr); } // 构造登记 → 析构置空：确保 instance() == nullptr

    const QString name = randomServerName();
    SingleInstance si(name);
    QSignalSpy openedSpy(&si, &SingleInstance::urlsOpened);
    const QList<QUrl> urls{ QUrl(QStringLiteral("file:///D:/queue%20one.txt")),
                            QUrl(QStringLiteral("file:///D:/queue%20two.txt")) };

    QCOMPARE(si.initialize(urls), SingleInstance::StartupRole::Primary); // 参数入队
    QVERIFY(openedSpy.isEmpty()); // 管理器未就绪：不刷出

    si.openPendingUrls(); // 管理器仍不存在：保留队列
    QVERIFY(openedSpy.isEmpty());

    DocumentManager manager(nullptr); // 就绪
    si.openPendingUrls();
    QCOMPARE(openedSpy.count(), 1);
    QCOMPARE(openedSpy.at(0).at(0).value<QList<QUrl>>(), urls); // 无丢失、顺序保持

    si.openPendingUrls(); // 队列已空：无新信号、无崩溃
    QCOMPARE(openedSpy.count(), 1);
}

// 半包/粘包容忍：一帧分两段到达（前半段无 '\n' 不解析）；两帧一次写入
// 逐帧独立解析（每帧一次 urlsOpened，帧内 URL 批量携带）
void TestSingleInstance::partialAndCoalescedFrames()
{
    DocumentManager manager(nullptr); // 立即刷出路径：每帧到达即一次 urlsOpened
    const QString name = randomServerName();
    SingleInstance si(name);
    QCOMPARE(si.initialize({}), SingleInstance::StartupRole::Primary);

    QSignalSpy openedSpy(&si, &SingleInstance::urlsOpened);

    const QByteArray frameA =
        SingleInstance::encodeFrame({ QUrl(QStringLiteral("file:///D:/half%20a.txt")) });
    const QByteArray frameB =
        SingleInstance::encodeFrame({ QUrl(QStringLiteral("file:///D:/half%20b.txt")),
                                      QUrl(QStringLiteral("file:///D:/half%20c.txt")) });
    const QByteArray frameC =
        SingleInstance::encodeFrame({ QUrl(QStringLiteral("file:///D:/half%20d.txt")) });

    QLocalSocket client;
    client.connectToServer(name);
    QVERIFY(client.waitForConnected(5000));

    // 半包：前半段（不含 '\n'）先送达服务端，不产生任何帧
    client.write(frameA.left(frameA.size() / 2));
    flushClientSocket(&client); // 推入系统缓冲并让服务端先读取前半段
    QTest::qWait(50);
    QCOMPARE(openedSpy.count(), 0);
    // 后半段补齐 → 整帧解析
    client.write(frameA.mid(frameA.size() / 2));
    flushClientSocket(&client);
    QTRY_COMPARE(openedSpy.count(), 1);
    QCOMPARE(openedSpy.at(0).at(0).value<QList<QUrl>>(),
             QList<QUrl>{ QUrl(QStringLiteral("file:///D:/half%20a.txt")) });

    // 粘包：两帧一次写入 → 各自独立解析（frameB 的两个 URL 同帧批量携带）
    client.write(frameB);
    client.write(frameC);
    flushClientSocket(&client);
    QTRY_COMPARE(openedSpy.count(), 3);
    const QList<QUrl> expectedB{ QUrl(QStringLiteral("file:///D:/half%20b.txt")),
                                 QUrl(QStringLiteral("file:///D:/half%20c.txt")) };
    QCOMPARE(openedSpy.at(1).at(0).value<QList<QUrl>>(), expectedB);
    QCOMPARE(openedSpy.at(2).at(0).value<QList<QUrl>>(),
             QList<QUrl>{ QUrl(QStringLiteral("file:///D:/half%20d.txt")) });
}

// 默认服务名：非空且与用户名联动（USERNAME/USER 可得时带 -<用户名> 后缀）
void TestSingleInstance::defaultServerNameIsSane()
{
    const QString name = SingleInstance::defaultServerName();
    QVERIFY(!name.isEmpty());
    QString user = qEnvironmentVariable("USERNAME");
    if (user.isEmpty())
        user = qEnvironmentVariable("USER");
    if (!user.isEmpty())
        QCOMPARE(name, QStringLiteral("qcnote-%1").arg(user));
    else
        QCOMPARE(name, QStringLiteral("qcnote"));
}

int runSingleInstanceTests(int argc, char *argv[])
{
    // QLocalServer/QLocalSocket 只依赖核心事件循环，无 GUI 依赖
    QCoreApplication app(argc, argv);
    qRegisterMetaType<QList<QUrl>>(); // QSignalSpy 携带 QList<QUrl> 参数取值
    TestSingleInstance t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_singleinstance.moc"
