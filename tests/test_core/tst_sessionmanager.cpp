#include <QCoreApplication>
#include <QFile>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include "document/document.h"
#include "document/documentmanager.h"
#include "document/sessionmanager.h"

// 会话恢复核心行为：文档往返、缺失文件静默跳过、active 丢失回退、空会话无副作用、
// 损坏数据容错、光标钳制、防抖写盘与 flush、几何校验纯函数、窗口状态往返。
// 均为 C++ 直接 new（注入 DocumentManager 与临时会话路径），不经 QML 单例工厂。
class TestSessionManager : public QObject
{
    Q_OBJECT

private slots:
    void sessionRoundTrip();
    void unnamedDocumentsAreExcludedFromSession();
    void missingFilesAreSkippedSilently();
    void lostActiveFallsBackToLastOpened();
    void emptySessionIsNoop();
    void corruptSessionEntriesAreSkippedOrClamped();
    void cursorPositionIsClampedToContentLength();
    void debouncedSaveAndFlush();
    void aboutToQuitFlushesPendingDebounce();
    void validateWindowStateClampsAndRejectsOffscreen();
    void windowStateRoundTrip();

private:
    static void writeRawBytes(const QString &path, const QByteArray &bytes)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(file.write(bytes) == bytes.size());
        file.close();
    }

    static QByteArray readRawBytes(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return QByteArray();
        return file.readAll();
    }

    // 直接读磁盘原文判断 [documents] 的 size 行（绕开 QSettings 进程内共享缓存，
    // 真实检验"是否已落盘"，供防抖测试使用）。文件不存在视为无该行。
    static bool sessionFileHasSize(const QString &path, int size)
    {
        const QString content = QString::fromUtf8(readRawBytes(path));
        const QRegularExpression re(QStringLiteral("^size=%1\\r?$").arg(size),
                                    QRegularExpression::MultilineOption);
        return re.match(content).hasMatch();
    }

    // 同上，判断 [documents] 中第 index 号文档的 cursor 行是否为指定值
    // （键形如 "1\cursor=3"，反斜杠经 escape 处理）。数值行无 URL 转义
    // 不确定性，适合做原始字节断言。
    static bool sessionFileHasCursor(const QString &path, int index, int value)
    {
        const QString content = QString::fromUtf8(readRawBytes(path));
        const QString key = QRegularExpression::escape(
            QString::number(index) + QStringLiteral("\\cursor"));
        const QRegularExpression re(QStringLiteral("^") + key + QStringLiteral("=%1\\r?$").arg(value),
                                    QRegularExpression::MultilineOption);
        return re.match(content).hasMatch();
    }
};

// 会话往返：2 个文件 + 光标 + active → flush → 新管理器/新会话管理器恢复，
// 断言文档数量、顺序（Tab 顺序 = 上次顺序）、active、各光标
void TestSessionManager::sessionRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("session.ini"));
    const QUrl urlA = QUrl::fromLocalFile(dir.filePath(QStringLiteral("a.txt")));
    const QUrl urlB = QUrl::fromLocalFile(dir.filePath(QStringLiteral("b.txt")));
    writeRawBytes(urlA.toLocalFile(), QByteArrayLiteral("aaaa"));
    writeRawBytes(urlB.toLocalFile(), QByteArrayLiteral("bbbbbb"));

    {
        DocumentManager mgr;
        SessionManager session(&mgr, iniPath); // 先于恢复目标构造，建立信号连接
        mgr.openUrls({ urlA, urlB });
        mgr.documents().at(0)->setCursorPosition(2);
        mgr.documents().at(1)->setCursorPosition(5);
        mgr.setActiveDocument(mgr.documents().at(0)); // active 记录为第一个（非常见的最后一个）
        session.flush(); // 立即落盘（不等防抖）
    } // 声明顺序保证 SessionManager 先于 DocumentManager 析构（manager 生命周期覆盖）

    {
        DocumentManager mgr;
        SessionManager session(&mgr, iniPath);
        session.restoreDocuments();

        QCOMPARE(mgr.documents().size(), 2);
        QCOMPARE(mgr.documents().at(0)->url(), urlA);
        QCOMPARE(mgr.documents().at(1)->url(), urlB);
        QCOMPARE(mgr.documents().at(0)->cursorPosition(), 2);
        QCOMPARE(mgr.documents().at(1)->cursorPosition(), 5);
        QCOMPARE(mgr.activeDocument(), mgr.documents().at(0));
    }
}

// 未命名文档不入会话：newDocument()（不 saveAs，url 为空）+ 打开 1 个有
// url 文件 → flush → 会话 size == 1，唯一条目即有 url 文档（未命名文档
// 无 url，不占编号不入会话）
void TestSessionManager::unnamedDocumentsAreExcludedFromSession()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("session.ini"));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("saved.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("content"));

    {
        DocumentManager mgr;
        SessionManager session(&mgr, iniPath);
        mgr.newDocument();     // 未命名文档：url 为空
        mgr.openUrls({ url }); // 有 url 文档（自动激活）
        session.flush();
    }

    QSettings reader(iniPath, QSettings::IniFormat);
    QCOMPARE(reader.value(QStringLiteral("documents/size")).toInt(), 1);
    // 唯一条目是有 url 文档：不含未命名文档
    QCOMPARE(QUrl(reader.value(QStringLiteral("documents/1/url")).toString()), url);
    QVERIFY(!reader.value(QStringLiteral("documents/1/url")).toString().isEmpty());
    // 未命名文档未占据第 2 个编号
    QVERIFY(!reader.contains(QStringLiteral("documents/2/url")));
}

// 缺失文件静默跳过：3 个文件删中间 1 个 → 恢复 2 个；无 openFailed；顺序保持
void TestSessionManager::missingFilesAreSkippedSilently()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("session.ini"));
    const QUrl urlA = QUrl::fromLocalFile(dir.filePath(QStringLiteral("a.txt")));
    const QUrl urlB = QUrl::fromLocalFile(dir.filePath(QStringLiteral("b.txt")));
    const QUrl urlC = QUrl::fromLocalFile(dir.filePath(QStringLiteral("c.txt")));
    writeRawBytes(urlA.toLocalFile(), QByteArrayLiteral("A"));
    writeRawBytes(urlB.toLocalFile(), QByteArrayLiteral("B"));
    writeRawBytes(urlC.toLocalFile(), QByteArrayLiteral("C"));

    {
        DocumentManager mgr;
        SessionManager session(&mgr, iniPath);
        mgr.openUrls({ urlA, urlB, urlC });
        session.flush();
    }

    QVERIFY(QFile::remove(urlB.toLocalFile())); // 模拟文件被删除/移动

    {
        DocumentManager mgr;
        SessionManager session(&mgr, iniPath);
        QSignalSpy failSpy(&mgr, &DocumentManager::openFailed);
        session.restoreDocuments();

        QCOMPARE(failSpy.count(), 0); // 静默跳过：不发 openFailed、不弹错
        QCOMPARE(mgr.documents().size(), 2);
        QCOMPARE(mgr.documents().at(0)->url(), urlA); // 顺序保持（缺失者被剔除）
        QCOMPARE(mgr.documents().at(1)->url(), urlC);
    }
}

// active 丢失回退：active 指向已删除文件 → 恢复后 activeDocument == 最后一个成功打开的文档
void TestSessionManager::lostActiveFallsBackToLastOpened()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("session.ini"));
    const QUrl urlA = QUrl::fromLocalFile(dir.filePath(QStringLiteral("a.txt")));
    const QUrl urlB = QUrl::fromLocalFile(dir.filePath(QStringLiteral("b.txt")));
    writeRawBytes(urlA.toLocalFile(), QByteArrayLiteral("A"));
    writeRawBytes(urlB.toLocalFile(), QByteArrayLiteral("B"));

    {
        DocumentManager mgr;
        SessionManager session(&mgr, iniPath);
        mgr.openUrls({ urlA, urlB }); // 最后打开的 B 被自动激活并记录
        session.flush();
    }

    QVERIFY(QFile::remove(urlB.toLocalFile())); // 删除 active 指向的文件

    {
        DocumentManager mgr;
        SessionManager session(&mgr, iniPath);
        session.restoreDocuments();

        QCOMPARE(mgr.documents().size(), 1);
        QCOMPARE(mgr.activeDocument(), mgr.documents().constFirst()); // 回退到最后成功打开者
    }
}

// 空会话/无会话文件：restore 不崩溃、不打开文档、不写盘（首启无副作用）
void TestSessionManager::emptySessionIsNoop()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("session.ini")); // 不存在

    DocumentManager mgr;
    SessionManager session(&mgr, iniPath);
    session.restoreDocuments();

    QCOMPARE(mgr.documents().size(), 0);
    QCOMPARE(mgr.activeDocument(), nullptr);
    QVERIFY(!QFile::exists(iniPath)); // 不写盘：首次启动行为与现状一致
}

// 损坏数据容错：手工写坏 ini（size 虚大、非法 url、缺失文件、负 cursor）→
// 不崩溃、坏条目跳过/钳制、无 openFailed，恢复后对齐写盘剔除坏条目
void TestSessionManager::corruptSessionEntriesAreSkippedOrClamped()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("session.ini"));
    const QUrl goodUrl = QUrl::fromLocalFile(dir.filePath(QStringLiteral("good.txt")));
    const QUrl goneUrl = QUrl::fromLocalFile(dir.filePath(QStringLiteral("gone.txt")));
    writeRawBytes(goodUrl.toLocalFile(), QByteArrayLiteral("abc")); // 3 字符

    QString ini;
    ini += QStringLiteral("[documents]\n");
    ini += QStringLiteral("size=100000\n"); // size 虚大：仅 3 条实际数据
    ini += QStringLiteral("1\\url=") + goodUrl.toString() + QStringLiteral("\n");
    ini += QStringLiteral("1\\cursor=-50\n"); // 负 cursor：钳到 0
    ini += QStringLiteral("2\\url=!!!损坏的地址\n"); // 非法 url：跳过
    ini += QStringLiteral("2\\cursor=7\n");
    ini += QStringLiteral("3\\url=") + goneUrl.toString() + QStringLiteral("\n"); // 缺失文件：跳过
    ini += QStringLiteral("3\\cursor=1\n");
    ini += QStringLiteral("active=") + goneUrl.toString() + QStringLiteral("\n"); // active 丢失
    writeRawBytes(iniPath, ini.toUtf8());

    DocumentManager mgr;
    SessionManager session(&mgr, iniPath);
    QSignalSpy failSpy(&mgr, &DocumentManager::openFailed);
    session.restoreDocuments(); // 不崩溃

    QCOMPARE(failSpy.count(), 0); // 坏条目在预检阶段丢弃，不发 openFailed
    QCOMPARE(mgr.documents().size(), 1);
    QCOMPARE(mgr.documents().constFirst()->url(), goodUrl);
    QCOMPARE(mgr.documents().constFirst()->cursorPosition(), 0);           // 负 cursor 钳到 0
    QCOMPARE(mgr.activeDocument(), mgr.documents().constFirst());          // active 丢失回退

    // 恢复后的对齐写盘剔除坏条目：会话只剩有效文档
    QSettings reader(iniPath, QSettings::IniFormat);
    QCOMPARE(reader.value(QStringLiteral("documents/size")).toInt(), 1);
}

// 光标钳制：记录 cursor 超内容长度 → 恢复后 == content.length()
void TestSessionManager::cursorPositionIsClampedToContentLength()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("session.ini"));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("short.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("hello")); // 5 字符

    {
        DocumentManager mgr;
        SessionManager session(&mgr, iniPath);
        mgr.openUrls({ url });
        mgr.documents().constFirst()->setCursorPosition(100); // 超出内容长度
        session.flush(); // 会话记录 cursor=100
    }

    {
        DocumentManager mgr;
        SessionManager session(&mgr, iniPath);
        session.restoreDocuments();
        QCOMPARE(mgr.documents().constFirst()->cursorPosition(), 5); // 钳到内容长度
    }
}

// 防抖与 flush：改动后立即读磁盘原文无变化 → 600ms 后防抖落盘；再改动 → flush() 立即落盘
void TestSessionManager::debouncedSaveAndFlush()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("session.ini"));
    const QUrl urlA = QUrl::fromLocalFile(dir.filePath(QStringLiteral("a.txt")));
    const QUrl urlB = QUrl::fromLocalFile(dir.filePath(QStringLiteral("b.txt")));
    writeRawBytes(urlA.toLocalFile(), QByteArrayLiteral("A"));
    writeRawBytes(urlB.toLocalFile(), QByteArrayLiteral("B"));

    DocumentManager mgr;
    SessionManager session(&mgr, iniPath);

    mgr.openUrls({ urlA }); // documentsChanged → 500ms 防抖启动
    QVERIFY(!sessionFileHasSize(iniPath, 1)); // 防抖未到期：磁盘无变化

    QTest::qWait(600); // 超过防抖窗口（500ms ± 5% 精度余量）
    QVERIFY(sessionFileHasSize(iniPath, 1)); // 防抖超时自动落盘

    mgr.openUrls({ urlB }); // 再次变化：防抖重新计时
    QVERIFY(!sessionFileHasSize(iniPath, 2));
    session.flush(); // 不等防抖：立即落盘
    QVERIFY(sessionFileHasSize(iniPath, 2));
}

// TC-007 退出路径：改动后防抖挂起 → 直接触发 aboutToQuit（生产接线
// qApp->aboutToQuit → flush，见 SessionManager 构造）→ 挂起状态立即落盘。
// 以光标 + active 变化（非文档集合变化）驱动防抖，补齐用例中
// "改变 active/光标 → 直接触发 aboutToQuit"的触发路径；
// 经 QMetaObject 按名调用信号，与真实退出走同一 aboutToQuit 信号路径。
void TestSessionManager::aboutToQuitFlushesPendingDebounce()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("session.ini"));
    const QUrl urlA = QUrl::fromLocalFile(dir.filePath(QStringLiteral("a.txt")));
    const QUrl urlB = QUrl::fromLocalFile(dir.filePath(QStringLiteral("b.txt")));
    writeRawBytes(urlA.toLocalFile(), QByteArrayLiteral("A"));
    writeRawBytes(urlB.toLocalFile(), QByteArrayLiteral("B"));

    DocumentManager mgr;
    SessionManager session(&mgr, iniPath);
    mgr.openUrls({ urlA, urlB });
    session.flush(); // 基线落盘：两个文档 cursor 均为 0
    QVERIFY(sessionFileHasCursor(iniPath, 1, 0));

    // 光标 + active 变化：防抖重新计时（挂起），磁盘保持基线
    mgr.documents().at(0)->setCursorPosition(3);
    mgr.setActiveDocument(mgr.documents().at(0));
    QVERIFY(!sessionFileHasCursor(iniPath, 1, 3)); // 防抖未到期：未落盘

    // 触发退出信号（不真正退出事件循环）：aboutToQuit → flush 立即落盘
    QVERIFY(QMetaObject::invokeMethod(qApp, "aboutToQuit", Qt::DirectConnection));
    QVERIFY(sessionFileHasCursor(iniPath, 1, 3)); // 挂起的防抖已落盘
}

// 几何校验纯函数：合法值原样返回、超小/超大宽高钳制、离屏返回空、空屏幕列表仅下限钳制
void TestSessionManager::validateWindowStateClampsAndRejectsOffscreen()
{
    const QList<QRect> screens = { QRect(0, 0, 1920, 1040), QRect(1920, 0, 1280, 720) };

    // 合法值原样返回（含 maximized 标志），键集合完整
    const QVariantMap ok = SessionManager::validateWindowState(120, 80, 1000, 640, true, screens);
    QCOMPARE(ok.size(), 5);
    QCOMPARE(ok.value(QStringLiteral("x")).toInt(), 120);
    QCOMPARE(ok.value(QStringLiteral("y")).toInt(), 80);
    QCOMPARE(ok.value(QStringLiteral("width")).toInt(), 1000);
    QCOMPARE(ok.value(QStringLiteral("height")).toInt(), 640);
    QCOMPARE(ok.value(QStringLiteral("maximized")).toBool(), true);

    // 落在第二屏的合法值也通过
    const QVariantMap secondScreen =
        SessionManager::validateWindowState(2000, 100, 900, 600, false, screens);
    QCOMPARE(secondScreen.value(QStringLiteral("x")).toInt(), 2000);
    QCOMPARE(secondScreen.value(QStringLiteral("width")).toInt(), 900);

    // 超小宽高：钳到下限 800×500
    const QVariantMap tooSmall =
        SessionManager::validateWindowState(120, 80, 100, 400, false, screens);
    QCOMPARE(tooSmall.value(QStringLiteral("width")).toInt(), 800);
    QCOMPARE(tooSmall.value(QStringLiteral("height")).toInt(), 500);

    // 超大宽高：钳到所有屏幕最大可用 1920×1040
    const QVariantMap tooLarge =
        SessionManager::validateWindowState(120, 80, 5000, 9999, false, screens);
    QCOMPARE(tooLarge.value(QStringLiteral("width")).toInt(), 1920);
    QCOMPARE(tooLarge.value(QStringLiteral("height")).toInt(), 1040);

    // 位置离屏（与任一屏幕均不相交，拔掉副屏场景）：返回空 map
    QVERIFY(SessionManager::validateWindowState(9000, 9000, 1000, 640, false, screens).isEmpty());

    // 空屏幕列表：仅下限钳制（无上限、不做位置校验），原样返回位置
    const QVariantMap noScreens =
        SessionManager::validateWindowState(-100, 50, 300, 400, false, {});
    QCOMPARE(noScreens.size(), 5);
    QCOMPARE(noScreens.value(QStringLiteral("x")).toInt(), -100);
    QCOMPARE(noScreens.value(QStringLiteral("y")).toInt(), 50);
    QCOMPARE(noScreens.value(QStringLiteral("width")).toInt(), 800);
    QCOMPARE(noScreens.value(QStringLiteral("height")).toInt(), 500);

    const QVariantMap noScreensBig =
        SessionManager::validateWindowState(10, 10, 100000, 99999, false, {});
    QCOMPARE(noScreensBig.value(QStringLiteral("width")).toInt(), 100000); // 无上限：原样保留
    QCOMPARE(noScreensBig.value(QStringLiteral("height")).toInt(), 99999);
}

// 窗口状态往返：saveWindowState → flush → 新 SessionManager → windowState() 一致
// （测试进程仅 QCoreApplication：屏幕列表为空，走"仅下限钳制"，取下限之上的值保证确定性）
void TestSessionManager::windowStateRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("session.ini"));

    {
        DocumentManager mgr;
        SessionManager session(&mgr, iniPath);
        session.saveWindowState(120, 80, 1000, 640, true);
        session.flush(); // 立即落盘
    }

    {
        DocumentManager mgr;
        SessionManager session(&mgr, iniPath);
        const QVariantMap restored = session.windowState();
        QCOMPARE(restored.value(QStringLiteral("x")).toInt(), 120);
        QCOMPARE(restored.value(QStringLiteral("y")).toInt(), 80);
        QCOMPARE(restored.value(QStringLiteral("width")).toInt(), 1000);
        QCOMPARE(restored.value(QStringLiteral("height")).toInt(), 640);
        QCOMPARE(restored.value(QStringLiteral("maximized")).toBool(), true);
    }
}

int runSessionManagerTests(int argc, char *argv[])
{
    // SessionManager 的防抖 QTimer 与 QTest::qWait 依赖事件循环，需要应用实例
    // （无 QGuiApplication 时屏幕列表为空，几何校验走"仅下限钳制"分支，不依赖平台）
    QCoreApplication app(argc, argv);
    TestSessionManager t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_sessionmanager.moc"
