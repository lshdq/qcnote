// 启动参数打开 + 单实例转发 QML 集成测试（需求"资源管理器右键菜单"
// TC-007 / TC-008 / TC-014 的 QML 集成部分）。
//
// 测试方式：offscreen 平台下复刻 main.cpp 的完整启动序列——引擎加载前
// SingleInstance::create(nullptr, nullptr) + initialize(<启动参数>)，
// 再经共享引擎实例化完整 Main.qml（Component.onCompleted 走恢复会话 →
// SingleInstance.openPendingUrls() → 首启提示分支）。断言：
//   tc007：恢复会话文档后叠加打开启动参数文件并激活；同 URL 仅激活不重复；
//          QML 的 SingleInstance 单例与 main 创建的 C++ 实例为同一对象
//          （singletonInstance 一致性——openPendingUrls 队列刷出的前提）
//   tc008：QML 运行中，从实例经 IPC（QLocalSocket 发帧）转发的 URL 被
//          主实例叠加打开并激活（urlsOpened / activationRequested 信号）
//   tc014：多个启动参数按序叠加（由 tc007 的顺序断言覆盖，另见
//          tests/test_core/tst_singleinstance.cpp 的队列保序单测）
//
// 隔离（对应约束见 .localdoc/需求文档-资源管理器右键菜单.md 非功能需求）：
// - USERNAME 在 main 中注入随机值：SingleInstance 默认服务名
//   qcnote-<USERNAME> 随机化，绝不监听/连接真实用户实例的默认管道；
// - 会话文件经 QSettings::setPath 重定向到临时目录，不污染真实用户配置；
// - QCNOTE_TEST_SHELL_NOOP=1 隔离首启右键菜单提示（不弹框、不写注册表）；
// - GUI 红线：全程 offscreen，无可见窗口。
//
// 与 tst_sessionrestore.cpp 的 Bench 同构（共享引擎 + Main {} 内联装载）；
// initialize 仅一次（幂等），全测试类共享同一主实例序列，与生产一致。

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocalSocket>
#include <QMetaType>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QUuid>

#include "document/document.h"
#include "document/documentmanager.h"
#include "platform/singleinstance.h"

namespace {

// 进程级资源目录：会话重定向基目录与测试文档所在（退出时自动清理）
QTemporaryDir &resourceDir()
{
    static QTemporaryDir dir;
    return dir;
}

// 重定向后的会话文件完整路径（QSettings::setPath 基目录 + <组织>/<应用>.ini，
// 与 SessionManager 内 QSettings(IniFormat, UserScope, "qcnote", "qcnote") 一致）
QString sessionFilePath()
{
    return resourceDir().filePath(QStringLiteral("qcnote/qcnote.ini"));
}

// 本用例进程注入的隔离用户名对应的服务名（与 SingleInstance::defaultServerName
// 的拼接规则一致；main 中 qputenv 的 USERNAME 见 main 函数）
QString isolatedServerName()
{
    return QStringLiteral("qcnote-%1").arg(
        QString::fromLocal8Bit(qgetenv("USERNAME")));
}

/// 测试文档集：A 预置会话（含光标），B/C 为启动参数，D 为 IPC 转发
struct TestFiles
{
    QString pathA, pathB, pathC, pathD;
    QUrl urlA, urlB, urlC, urlD;
};

// 以 fromUserInput（与 main.cpp 的位置参数转换一致）构造并写盘测试文件；
// 任一文件创建失败返回 false（QVERIFY 由调用方承担）
bool createTestFiles(TestFiles *out)
{
    TestFiles f;
    f.pathA = resourceDir().filePath(QStringLiteral("会话 A.txt")); // 中文+空格路径
    f.pathB = resourceDir().filePath(QStringLiteral("参数B.txt"));
    f.pathC = resourceDir().filePath(QStringLiteral("参数 C.txt"));
    f.pathD = resourceDir().filePath(QStringLiteral("转发D.txt"));
    const QList<QPair<QString *, const char *>> spec = {
        { &f.pathA, "aaaa\nbbbb" }, { &f.pathB, "b" }, { &f.pathC, "c" }, { &f.pathD, "d" }
    };
    for (const auto &[path, content] : spec) {
        QFile file(*path);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        file.write(content);
    }
    f.urlA = QUrl::fromUserInput(f.pathA); // 与 main.cpp 位置参数同一转换
    f.urlB = QUrl::fromUserInput(f.pathB);
    f.urlC = QUrl::fromUserInput(f.pathC);
    f.urlD = QUrl::fromUserInput(f.pathD);
    *out = f;
    return true;
}

/**
 * @brief 共享引擎上的 Main.qml 测试台（与 tst_sessionrestore.cpp 的 Bench 同构）。
 * Component.onCompleted 在构造期即已执行（恢复会话 → openPendingUrls → 提示
 * 判定），断言在构造后进行。
 */
struct Bench
{
    QQmlEngine *engine = nullptr;
    QQuickWindow *win = nullptr;

    explicit Bench(QQmlEngine *sharedEngine)
        : engine(sharedEngine)
    {
        QQmlComponent c(engine);
        c.setData(R"QML(import QtQuick
import qcnote
Main {}
)QML", QUrl(QStringLiteral("bench.qml")));
        if (c.isError()) {
            for (const QQmlError &e : c.errors())
                qWarning() << "[bench]" << e.toString();
            return;
        }
        win = qobject_cast<QQuickWindow *>(c.create());
    }

    ~Bench() { delete win; }
};

// 当前文档集的本地路径清单（toLocalFile 归一，规避 URL 编码形态差异）
QStringList documentLocalPaths(DocumentManager *dm)
{
    QStringList paths;
    if (!dm)
        return paths;
    for (const Document *doc : dm->documents())
        paths.append(doc->url().toLocalFile());
    return paths;
}

} // namespace

class TestStartupArgs : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // TC-007：恢复会话后叠加打开启动参数并激活 + QML 单例一致性
    void tc007_startupArgsOpenAfterRestore();

    // TC-008：QML 运行中 IPC 转发叠加打开并激活（urlsOpened/activationRequested）
    void tc008_ipcForwardOpensWhileRunning();

    // TC-007：转发已在会话中的 URL → 仅激活不重复
    void tc007_dupForwardOnlyActivates();

private:
    QQmlEngine *m_engine = nullptr;     ///< 全程唯一共享引擎（单例宿主）
    SingleInstance *m_si = nullptr;     ///< main 序列创建的主实例（Primary）
    TestFiles m_files;
    static QQuickWindow *m_benchWindow; ///< 贯穿全部用例的主窗口（主实例运行中）
};

QQuickWindow *TestStartupArgs::m_benchWindow = nullptr;

// 复刻 main.cpp 启动序列：写盘测试文件与预置会话 → create + initialize(
// 启动参数 B、C) → 共享引擎实例化 Main.qml。此后 Component.onCompleted 已
// 执行完毕（restore A → openPendingUrls 刷出 B/C → 首启提示判定 no-op）
void TestStartupArgs::initTestCase()
{
    qRegisterMetaType<QList<QUrl>>(); // QSignalSpy 携带 QList<QUrl> 参数取值

    QVERIFY2(createTestFiles(&m_files), "创建测试文件失败");
    QVERIFY(QFileInfo::exists(m_files.pathA));

    // 预置上次会话（已打开 A、光标 5、active=A）：独立 QSettings 写重定向路径
    {
        QSettings f(sessionFilePath(), QSettings::IniFormat);
        f.setValue(QStringLiteral("documents/size"), 1);
        f.setValue(QStringLiteral("documents/1/url"), m_files.urlA.toString());
        f.setValue(QStringLiteral("documents/1/cursor"), 5);
        f.setValue(QStringLiteral("documents/active"), m_files.urlA.toString());
        f.sync();
    }

    // main.cpp 第一步：引擎加载前创建单实例并携带启动参数初始化。
    // USERNAME 已在 main() 中注入随机值 → 服务名隔离，不会连上真实用户实例
    m_si = SingleInstance::create(nullptr, nullptr);
    QVERIFY(m_si);
    QCOMPARE(m_si->initialize({ m_files.urlB, m_files.urlC }),
             SingleInstance::StartupRole::Primary);

    // main.cpp 第二步：加载主界面（构造期完成恢复会话 → openPendingUrls）
    m_engine = new QQmlEngine;
    Bench b(m_engine);
    QVERIFY2(b.win, "Main.qml 实例化失败");
    // 窗口交给测试类持有（贯穿全部用例，与"主实例持续运行"语义一致）
    m_benchWindow = b.win;
    b.win = nullptr; // 阻止 Bench 析构删除
}

void TestStartupArgs::cleanupTestCase()
{
    delete m_benchWindow;
    delete m_engine;
    m_engine = nullptr;
}

// TC-007（P0）：启动参数文件应在 QML 启动流程（Component.onCompleted 的
// SingleInstance.openPendingUrls()）中被叠加打开：
// 文档集 = [A(会话恢复), B, C(启动参数)]，B、C 顺序保持，active=C（最后
// 打开者）。附带断言 QML 的 SingleInstance 单例与 main 创建的 C++ 实例为
// 同一对象（否则 openPendingUrls 刷的不是 initialize 入队的队列）
void TestStartupArgs::tc007_startupArgsOpenAfterRestore()
{
    // QML 单例一致性：经引擎取 qcnote 模块的 SingleInstance 单例
    SingleInstance *qmlSingleton =
        m_engine->singletonInstance<SingleInstance *>(u"qcnote", u"SingleInstance");
    QVERIFY(qmlSingleton);
    QCOMPARE(qmlSingleton, SingleInstance::instance());
    QCOMPARE(qmlSingleton, m_si);

    DocumentManager *dm = DocumentManager::instance();
    QVERIFY(dm);

    // 启动序列已执行完毕；若有防抖/异步事件未收敛，这里泵事件等待
    QTRY_COMPARE_WITH_TIMEOUT(dm->documents().size(), 3, 5000);

    const QStringList paths = documentLocalPaths(dm);
    QCOMPARE(paths,
             QStringList({ m_files.pathA, m_files.pathB, m_files.pathC })); // 顺序与叠加
    QVERIFY(dm->activeDocument());
    QCOMPARE(dm->activeDocument()->url().toLocalFile(), m_files.pathC); // 最后打开者激活
}

// TC-008（P0）：QML 运行中模拟从实例转发——QLocalSocket 客户端连接主实例
// 监听的隔离服务名，发送单帧 [D]：主实例叠加打开 D 并激活，urlsOpened 与
// activationRequested 各发一次（offscreen 下窗口前置行为不在此断言范围）
void TestStartupArgs::tc008_ipcForwardOpensWhileRunning()
{
    DocumentManager *dm = DocumentManager::instance();
    QVERIFY(dm);
    const int before = dm->documents().size();
    QCOMPARE(before, 3); // 前置：tc007 的三文档仍在（本用例不重开窗口）

    QSignalSpy openedSpy(m_si, &SingleInstance::urlsOpened);
    QSignalSpy activationSpy(m_si, &SingleInstance::activationRequested);

    QLocalSocket client;
    client.connectToServer(isolatedServerName());
    QVERIFY2(client.waitForConnected(5000), "连接主实例 IPC 服务失败");
    client.write(SingleInstance::encodeFrame({ m_files.urlD }));
    // 泵事件直至写缓冲清空（同进程回环写完成通知需事件循环送达）
    QDeadlineTimer flushDeadline(5000);
    while (client.bytesToWrite() > 0 && !flushDeadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    client.disconnectFromServer();

    QTRY_COMPARE_WITH_TIMEOUT(dm->documents().size(), before + 1, 5000);
    QCOMPARE(documentLocalPaths(dm).last(), m_files.pathD);
    QVERIFY(dm->activeDocument());
    QCOMPARE(dm->activeDocument()->url().toLocalFile(), m_files.pathD);
    QTRY_COMPARE(openedSpy.count(), 1);
    QCOMPARE(openedSpy.at(0).at(0).value<QList<QUrl>>(), QList<QUrl>{ m_files.urlD });
    QTRY_COMPARE(activationSpy.count(), 1);
}

// TC-007 去重：转发已在会话中的 A → 不新增文档，仅激活
void TestStartupArgs::tc007_dupForwardOnlyActivates()
{
    DocumentManager *dm = DocumentManager::instance();
    QVERIFY(dm);
    const int before = dm->documents().size();

    QLocalSocket client;
    client.connectToServer(isolatedServerName());
    QVERIFY2(client.waitForConnected(5000), "连接主实例 IPC 服务失败");
    client.write(SingleInstance::encodeFrame({ m_files.urlA }));
    QDeadlineTimer flushDeadline(5000);
    while (client.bytesToWrite() > 0 && !flushDeadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    client.disconnectFromServer();

    QTRY_VERIFY_WITH_TIMEOUT(dm->activeDocument()
                                 && dm->activeDocument()->url().toLocalFile() == m_files.pathA,
                             5000);
    QTest::qWait(100); // 去重路径不新增：给一次事件循环余量后复核数量不变
    QCOMPARE(dm->documents().size(), before);
}

int main(int argc, char *argv[])
{
    // GUI 红线：自动化验证一律 offscreen，全程无可见窗口
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // 隔离首启右键菜单提示（不弹框、不写注册表；须在 QML 引擎/单例创建前设置）
    qputenv("QCNOTE_TEST_SHELL_NOOP", "1");
    // 服务名隔离：SingleInstance 默认服务名 qcnote-<USERNAME> 随机化，
    // 绝不监听/连接真实用户实例的默认管道（本机可能有用户 qcnote 在运行）
    qputenv("USERNAME",
            ("qcnit-qml-" + QUuid::createUuid().toString(QUuid::Id128)).toLocal8Bit());
    // offscreen 平台字体目录（同 tst_sessionrestore）
    static const char kWinFonts[] = "C:/Windows/Fonts";
    if (QDir(QString::fromLatin1(kWinFonts)).exists())
        qputenv("QT_QPA_FONTDIR", kWinFonts);
    QGuiApplication app(argc, argv);
    // 会话重定向：在任何 QSettings 实例（SessionManager 单例）创建之前
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       resourceDir().path());
    // 草稿缓存隔离（审查应改 #2）：Main.qml 的 Component.onCompleted 每次实例化
    // 都会调 DraftCacheManager.restoreDrafts()，其非便携默认目录按测试 exe 名
    // 派生自 %LOCALAPPDATA%（本测试为 %LOCALAPPDATA%\test_startupargs\drafts），
    // 跨运行持久。上一轮若以脏文档收场（QVERIFY 失败 return 跳过清理、或进程
    // 被杀）且防抖在后续事件循环到期写盘，条目残留会让本轮恢复出额外文档，
    // 文档计数类断言偶发失败。启动预清理隔离上一轮残留；结束清理避免测试产物
    // 在用户 %LOCALAPPDATA% 积累。不用 setTestModeEnabled（避免影响既有
    // QSettings 语义），不改应用名（目录不漂移、不积累）。
    const QString draftsDir = QStandardPaths::writableLocation(
                QStandardPaths::AppLocalDataLocation) + QStringLiteral("/drafts");
    QDir(draftsDir).removeRecursively();
    app.setQuitOnLastWindowClosed(false); // 窗口关闭不触发自动退出
    QQuickStyle::setStyle(QStringLiteral("Material"));
    TestStartupArgs t;
    const int testResult = QTest::qExec(&t, argc, argv);
    // 结束清理：删除本轮测试写入的草稿条目，不在 %LOCALAPPDATA% 留测试产物
    QDir(draftsDir).removeRecursively();
    return testResult;
}

#include "tst_startupargs.moc"
