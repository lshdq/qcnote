#include <QCoreApplication>
#include <QDir>
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

// RAII：便携构造级测试需要临时占用测试进程 exe 目录（build/，与 qcnote.exe
// 同目录）放置 qcnote.portable 标记并接收测试产生的 qcnote.ini。QVERIFY 失败
// 仅 return 出测试函数，局部对象析构照常执行，保证 build 目录恢复原状；
// 目录中已存在任一目标文件（异常残留/他人产物）时不创建也不清理，
// 调用方据此 QSKIP。
class ExeDirPortableView
{
public:
    ExeDirPortableView()
        : m_markerPath(QDir(QCoreApplication::applicationDirPath())
                           .filePath(QStringLiteral("qcnote.portable")))
        , m_iniPath(QDir(QCoreApplication::applicationDirPath())
                        .filePath(QStringLiteral("qcnote.ini")))
        , m_clean(!QFile::exists(m_markerPath) && !QFile::exists(m_iniPath))
    {
    }

    bool dirIsClean() const { return m_clean; }

    bool createMarker()
    {
        if (!m_clean)
            return false;
        QFile marker(m_markerPath);
        if (!marker.open(QIODevice::WriteOnly))
            return false;
        marker.close();
        return true;
    }

    QString iniPath() const { return m_iniPath; }

    ~ExeDirPortableView()
    {
        if (!m_clean)
            return;
        QFile::remove(m_iniPath);    // 便携分支产生的会话文件
        QFile::remove(m_markerPath); // 本视图创建的标记
    }

private:
    QString m_markerPath;
    QString m_iniPath;
    bool m_clean;
};

// 会话恢复核心行为：文档往返、缺失文件静默跳过、active 丢失回退、空会话无副作用、
// 损坏数据容错、光标钳制、防抖写盘与 flush、几何校验纯函数、窗口状态往返、
// 自动换行设置（默认值 / 写盘 / 重启恢复 / 旧会话兼容，TC-001~TC-004）。
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
    void wordWrapDefaultsToFalseWithoutDiskWrite();
    void wordWrapToggleWritesIniAfterFlush();
    void wordWrapRestoredAcrossInstances();
    void legacyIniWithoutEditorSectionIsCompatible();
    void encodingRememberedAndRestoredAcrossSessions();
    void forcedUtf8EncodingRememberedForGbkFile();
    void legacySessionWithoutEncodingFallsBackToAutoDetect();
    void invalidEncodingValueFallsBackToAutoDetect();
    void resolvePortableIniPath_returnsIniWhenMarkerExists();
    void resolvePortableIniPath_returnsEmptyWhenMarkerMissing();
    void resolvePortableIniPath_returnsEmptyWhenAppDirMissing();
    void resolvePortableIniPath_returnsEmptyWhenMarkerIsDirectory();
    void resolvePortableIniPath_returnsEmptyWhenAppDirEmpty();
    void constructWithEmptyPathWritesIniNextToExeWhenMarkerPresent();
    void constructWithEmptyPathKeepsExeDirCleanWhenMarkerMissing();

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

    // 同上，判断 [editor] 节的 wordWrap 行是否为指定布尔值（QSettings IniFormat
    // 以 true/false 落盘；wordWrap 键仅存在于 [editor] 节，全文行匹配无歧义）
    static bool sessionFileHasWordWrap(const QString &path, bool value)
    {
        const QString content = QString::fromUtf8(readRawBytes(path));
        const QRegularExpression re(
            QStringLiteral("^wordWrap=%1\\r?$").arg(value ? QStringLiteral("true")
                                                          : QStringLiteral("false")),
            QRegularExpression::MultilineOption);
        return re.match(content).hasMatch();
    }

    // 同上，判断 [documents] 中第 index 号文档的 encoding 行是否为指定值
    // （键形如 "1\encoding=gbk"）
    static bool sessionFileHasEncoding(const QString &path, int index, const QString &value)
    {
        const QString content = QString::fromUtf8(readRawBytes(path));
        const QString key = QRegularExpression::escape(
            QString::number(index) + QStringLiteral("\\encoding"));
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

// TC-001 默认关闭（无历史设置）：全新临时 ini（不存在）构造实例 → wordWrap 为
// false；构造不触发写盘；同值 setWordWrap(false) 不发信号、不启动防抖（等过
// 防抖窗口后文件仍不存在）；析构兜底 flush 无脏数据，全生命周期零写盘。
void TestSessionManager::wordWrapDefaultsToFalseWithoutDiskWrite()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("session.ini")); // 不存在

    {
        DocumentManager mgr;
        SessionManager session(&mgr, iniPath);
        QCOMPARE(session.wordWrap(), false); // 默认关闭，与现状一致

        QSignalSpy spy(&session, &SessionManager::wordWrapChanged);
        session.setWordWrap(false);       // 同值早退
        QCOMPARE(spy.count(), 0);         // 不发信号
        QTest::qWait(600);                // 若误触发防抖，此刻早已落盘
        QVERIFY(!QFile::exists(iniPath)); // 同值不调度写盘：文件不产生
    } // 析构 flush：无挂起防抖、无脏数据，不落盘

    QVERIFY(!QFile::exists(iniPath)); // 首启无副作用（与 emptySessionIsNoop 同口径）
}

// TC-002 切换写盘（防抖聚合）：false → true 发 1 次 wordWrapChanged，同值重复
// 不多发 → flush() 立即落盘，磁盘原文与 QSettings 双重断言 editor/wordWrap == true。
void TestSessionManager::wordWrapToggleWritesIniAfterFlush()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("session.ini"));

    {
        DocumentManager mgr;
        SessionManager session(&mgr, iniPath);
        QCOMPARE(session.wordWrap(), false);

        QSignalSpy spy(&session, &SessionManager::wordWrapChanged);
        session.setWordWrap(false); // 同值：不发信号
        QCOMPARE(spy.count(), 0);
        session.setWordWrap(true);  // false → true：发 1 次
        QCOMPARE(spy.count(), 1);
        QCOMPARE(session.wordWrap(), true);
        session.setWordWrap(true); // 同值重复：不再多发
        QCOMPARE(spy.count(), 1);

        session.flush(); // 不等防抖：立即落盘
        QVERIFY(sessionFileHasWordWrap(iniPath, true)); // 磁盘原文确证已写出
    }

    QSettings reader(iniPath, QSettings::IniFormat);
    QCOMPARE(reader.value(QStringLiteral("editor/wordWrap")).toBool(), true);
}

// TC-003 重启恢复：true 落盘 → 同一路径新实例（构造预读 [editor]）恢复为 true；
// 再切回 false + flush → 新实例恢复为 false（读盘与实例读取双重断言）。
void TestSessionManager::wordWrapRestoredAcrossInstances()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("session.ini"));

    {
        DocumentManager mgr;
        SessionManager session(&mgr, iniPath);
        session.setWordWrap(true);
        session.flush();
    }

    {
        DocumentManager mgr;
        SessionManager session(&mgr, iniPath); // 构造预读 [editor]
        QCOMPARE(session.wordWrap(), true);    // 与落盘值一致

        session.setWordWrap(false);
        session.flush();
        QVERIFY(sessionFileHasWordWrap(iniPath, false));
    }

    {
        QSettings reader(iniPath, QSettings::IniFormat);
        QCOMPARE(reader.value(QStringLiteral("editor/wordWrap")).toBool(), false);
    }

    {
        DocumentManager mgr;
        SessionManager session(&mgr, iniPath);
        QCOMPARE(session.wordWrap(), false); // 与落盘值一致
    }
}

// TC-004 旧会话文件兼容（无 editor 节）：手写仅含 [documents]/[window] 的旧格式
// ini → 新实例 wordWrap 取默认 false；restoreDocuments 正常恢复文档/光标/active
// 与窗口几何；恢复末尾的对齐写盘补全 [editor] 节且既有节不被破坏。
void TestSessionManager::legacyIniWithoutEditorSectionIsCompatible()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("session.ini"));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("legacy.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("legacy"));

    QString ini;
    ini += QStringLiteral("[documents]\n");
    ini += QStringLiteral("size=1\n");
    ini += QStringLiteral("1\\url=") + url.toString() + QStringLiteral("\n");
    ini += QStringLiteral("1\\cursor=2\n");
    ini += QStringLiteral("active=") + url.toString() + QStringLiteral("\n");
    ini += QStringLiteral("[window]\n");
    ini += QStringLiteral("x=100\n");
    ini += QStringLiteral("y=60\n");
    ini += QStringLiteral("width=1024\n");
    ini += QStringLiteral("height=768\n");
    ini += QStringLiteral("maximized=false\n");
    writeRawBytes(iniPath, ini.toUtf8());

    DocumentManager mgr;
    SessionManager session(&mgr, iniPath);
    QCOMPARE(session.wordWrap(), false); // 旧格式无 [editor]：默认 false

    session.restoreDocuments();
    QCOMPARE(mgr.documents().size(), 1);
    QCOMPARE(mgr.documents().constFirst()->url(), url);
    QCOMPARE(mgr.documents().constFirst()->cursorPosition(), 2);
    QCOMPARE(mgr.activeDocument(), mgr.documents().constFirst());
    // [window] 预读不受影响（测试进程无屏幕：仅下限钳制，1024×768 均高于下限）
    const QVariantMap window = session.windowState();
    QCOMPARE(window.value(QStringLiteral("x")).toInt(), 100);
    QCOMPARE(window.value(QStringLiteral("width")).toInt(), 1024);
    QCOMPARE(window.value(QStringLiteral("maximized")).toBool(), false);

    // 恢复末尾对齐写盘：补全 [editor] 节（wordWrap=false），既有节不被破坏
    QVERIFY(sessionFileHasWordWrap(iniPath, false));
    QSettings reader(iniPath, QSettings::IniFormat);
    QCOMPARE(reader.value(QStringLiteral("editor/wordWrap")).toBool(), false);
    QCOMPARE(reader.value(QStringLiteral("documents/size")).toInt(), 1);
    QCOMPARE(QUrl(reader.value(QStringLiteral("documents/1/url")).toString()), url);
    QCOMPARE(reader.value(QStringLiteral("documents/1/cursor")).toInt(), 2);
    QCOMPARE(reader.value(QStringLiteral("window/x")).toInt(), 100);
    QCOMPARE(reader.value(QStringLiteral("window/width")).toInt(), 1024);
    QCOMPARE(reader.value(QStringLiteral("window/maximized")).toBool(), false);
}

// TC-016：编码记忆与恢复——GBK 文件按指定编码打开后 flush，会话记录 encoding=gbk；
// 新实例恢复按记忆编码打开（doc->encoding() 一致、内容正确）
void TestSessionManager::encodingRememberedAndRestoredAcrossSessions()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("session.ini"));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("memo.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("\xC4\xE3\xBA\xC3")); // GBK：你好

    {
        DocumentManager mgr;
        SessionManager session(&mgr, iniPath);
        mgr.openUrlWithEncoding(url, Document::Encoding::Gbk); // 手动指定编码打开
        session.flush();
    }
    QVERIFY(sessionFileHasEncoding(iniPath, 1, QStringLiteral("gbk")));

    {
        DocumentManager mgr;
        SessionManager session(&mgr, iniPath);
        session.restoreDocuments();
        QCOMPARE(mgr.documents().size(), 1);
        QCOMPARE(mgr.documents().constFirst()->encoding(), Document::Encoding::Gbk); // 按记忆编码
        QCOMPARE(mgr.documents().constFirst()->content(), QStringLiteral("\u4F60\u597D"));
        // 恢复末尾对齐写盘：编码记忆保持
        QVERIFY(sessionFileHasEncoding(iniPath, 1, QStringLiteral("gbk")));
    }
}

// 自动识别结果同样入会话：GBK 文件自动打开（未手动指定）→ 记录 gbk → 恢复仍按 GBK 打开
void TestSessionManager::forcedUtf8EncodingRememberedForGbkFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("session.ini"));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("forced.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("\xC4\xE3\xBA\xC3"));

    {
        DocumentManager mgr;
        SessionManager session(&mgr, iniPath);
        // 用户纠正：GBK 字节强制按 UTF-8 打开（乱码），会话记录该选择
        mgr.openUrlWithEncoding(url, Document::Encoding::Utf8);
        session.flush();
    }
    QVERIFY(sessionFileHasEncoding(iniPath, 1, QStringLiteral("utf8")));

    {
        DocumentManager mgr;
        SessionManager session(&mgr, iniPath);
        session.restoreDocuments();
        // 重启后仍按记忆的 UTF-8 打开（不重新自动识别为 GBK）
        QCOMPARE(mgr.documents().constFirst()->encoding(), Document::Encoding::Utf8);
    }
}

// 旧会话文件（无 encoding 键）兼容：回退自动识别，文档正常恢复
void TestSessionManager::legacySessionWithoutEncodingFallsBackToAutoDetect()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("session.ini"));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("legacy-gbk.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("\xC4\xE3\xBA\xC3"));

    QString ini;
    ini += QStringLiteral("[documents]\n");
    ini += QStringLiteral("size=1\n");
    ini += QStringLiteral("1\\url=") + url.toString() + QStringLiteral("\n");
    ini += QStringLiteral("1\\cursor=0\n");
    ini += QStringLiteral("active=") + url.toString() + QStringLiteral("\n");
    writeRawBytes(iniPath, ini.toUtf8()); // 无 encoding 键（旧格式）

    DocumentManager mgr;
    SessionManager session(&mgr, iniPath);
    QSignalSpy failSpy(&mgr, &DocumentManager::openFailed);
    session.restoreDocuments();
    QCOMPARE(failSpy.count(), 0); // 兼容：不报错
    QCOMPARE(mgr.documents().size(), 1);
    QCOMPARE(mgr.documents().constFirst()->encoding(), Document::Encoding::Gbk); // 自动识别
    QCOMPARE(mgr.documents().constFirst()->content(), QStringLiteral("\u4F60\u597D"));
}

// 非法 encoding 值（损坏数据）静默回退自动识别，不阻断启动
void TestSessionManager::invalidEncodingValueFallsBackToAutoDetect()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString iniPath = dir.filePath(QStringLiteral("session.ini"));
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("bogus.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("plain ascii"));

    QString ini;
    ini += QStringLiteral("[documents]\n");
    ini += QStringLiteral("size=1\n");
    ini += QStringLiteral("1\\url=") + url.toString() + QStringLiteral("\n");
    ini += QStringLiteral("1\\cursor=0\n");
    ini += QStringLiteral("1\\encoding=latin1\n"); // 非法值
    ini += QStringLiteral("active=") + url.toString() + QStringLiteral("\n");
    writeRawBytes(iniPath, ini.toUtf8());

    DocumentManager mgr;
    SessionManager session(&mgr, iniPath);
    QSignalSpy failSpy(&mgr, &DocumentManager::openFailed);
    session.restoreDocuments();
    QCOMPARE(failSpy.count(), 0);
    QCOMPARE(mgr.documents().size(), 1);
    QCOMPARE(mgr.documents().constFirst()->encoding(), Document::Encoding::Utf8); // 回退自动识别
}

// 便携模式：标记文件 qcnote.portable 存在 → 返回 appDir/qcnote.ini 的绝对路径
void TestSessionManager::resolvePortableIniPath_returnsIniWhenMarkerExists()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString appDir = dir.path();
    writeRawBytes(dir.filePath(QStringLiteral("qcnote.portable")), QByteArrayLiteral(""));

    const QString result = SessionManager::resolvePortableIniPath(appDir);

    QCOMPARE(result, QFileInfo(QDir(appDir).filePath(QStringLiteral("qcnote.ini"))).absoluteFilePath());
    QVERIFY(QFileInfo(result).isAbsolute()); // 绝对路径约定
}

// 便携模式：标记文件不存在 → 返回空串（走用户目录默认路径）
void TestSessionManager::resolvePortableIniPath_returnsEmptyWhenMarkerMissing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QVERIFY(SessionManager::resolvePortableIniPath(dir.path()).isEmpty());
}

// 便携模式：appDir 本身不存在 → 返回空串、不崩溃
void TestSessionManager::resolvePortableIniPath_returnsEmptyWhenAppDirMissing()
{
    const QString missing = QDir::temp().absoluteFilePath(
        QStringLiteral("qcnote-nonexistent-") + QString::number(QCoreApplication::applicationPid()));

    QVERIFY(!QDir(missing).exists());
    QVERIFY(SessionManager::resolvePortableIniPath(missing).isEmpty());
}

// 便携模式：标记 qcnote.portable 是目录而非常规文件 → 返回空串（isFile 判定不通过）
void TestSessionManager::resolvePortableIniPath_returnsEmptyWhenMarkerIsDirectory()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("qcnote.portable"))); // 同名目录

    QVERIFY(SessionManager::resolvePortableIniPath(dir.path()).isEmpty());
}

// 便携模式：appDir 为空串 → 必须返回空串，不得以进程 CWD 为基准探测标记。
// 在 exe 目录（ctest 运行时的 CWD）临时放置 qcnote.portable 后传入空串：
// 无空串防御的实现经 QDir("") 会相对 CWD 误判命中并返回 CWD 下 qcnote.ini，
// 本用例锁定"空入参即空结果"契约。标记由 RAII 视图创建并清理。
void TestSessionManager::resolvePortableIniPath_returnsEmptyWhenAppDirEmpty()
{
    ExeDirPortableView exeDir;
    if (!exeDir.dirIsClean())
        QSKIP("build 目录已存在 qcnote.portable/qcnote.ini，跳过以免误动他人产物");

    QVERIFY(exeDir.createMarker()); // CWD 下放置标记：任何基于 CWD 的探测都会命中

    QVERIFY(SessionManager::resolvePortableIniPath(QString()).isEmpty());
}

// 便携构造级：注入路径为空 + exe 目录存在 qcnote.portable 标记 → 会话 ini 落在
// exe 旁（构造函数便携命中分支，需求 F1 写盘行为在单元层的端到端验证）。
// 标记由 RAII 视图创建并清理，不污染 build 目录。
void TestSessionManager::constructWithEmptyPathWritesIniNextToExeWhenMarkerPresent()
{
    ExeDirPortableView exeDir;
    if (!exeDir.dirIsClean())
        QSKIP("build 目录已存在 qcnote.portable/qcnote.ini，跳过以免误动他人产物");

    QVERIFY(exeDir.createMarker());

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QUrl url = QUrl::fromLocalFile(dir.filePath(QStringLiteral("portable.txt")));
    writeRawBytes(url.toLocalFile(), QByteArrayLiteral("P"));

    DocumentManager mgr;
    SessionManager session(&mgr, QString()); // 空注入路径：应命中便携模式
    mgr.openUrls({ url });
    session.flush(); // 不等防抖，立即落盘

    QVERIFY(QFile::exists(exeDir.iniPath())); // ini 生成于 exe 旁
    QSettings reader(exeDir.iniPath(), QSettings::IniFormat);
    QCOMPARE(reader.value(QStringLiteral("documents/size")).toInt(), 1);
    QCOMPARE(QUrl(reader.value(QStringLiteral("documents/1/url")).toString()), url);
}

// 无标记构造级：注入路径为空 + exe 目录无 qcnote.portable → 构造到析构全程不得
// 在 exe 旁产生 qcnote.ini（便携分支不得误触发）。仅做零副作用断言，不验证
// UserScope 实际落点——那指向真实 %APPDATA%\qcnote\qcnote.ini，写盘验证由
// TC-008 集成场景（便携产物临时副本）承担，避免单元测试污染真实用户目录。
// 注意：此处不调用 restoreDocuments——若真实用户目录存在历史会话，
// 恢复路径末尾的 saveNow 会重写该文件。
void TestSessionManager::constructWithEmptyPathKeepsExeDirCleanWhenMarkerMissing()
{
    ExeDirPortableView exeDir;
    if (!exeDir.dirIsClean())
        QSKIP("build 目录已存在 qcnote.portable/qcnote.ini，跳过以免误动他人产物");

    DocumentManager mgr;
    SessionManager session(&mgr, QString()); // 空注入路径 + 无标记：走默认 UserScope
    QCOMPARE(mgr.documents().size(), 0);
    session.flush(); // 无任何写操作：仅 sync，不落盘

    QVERIFY(!QFile::exists(exeDir.iniPath())); // exe 旁零副作用
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
