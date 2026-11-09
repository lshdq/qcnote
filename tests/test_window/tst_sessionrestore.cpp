// 会话恢复 QML 集成测试（需求"会话恢复"TC-011 / TC-012 及退出路径加分项；
// tc014 为审查修复回归：窗口几何持久化往返（P0）与恢复光标滚动到位（P1）；
// tc015 为审查修复回归：最小化期间 maximized 标志保持不被覆盖（P2），
// 并含 maximized=true 的 QML 恢复分支断言（二轮审查可选-2））
//
// 测试方式：offscreen 平台下经 QQmlComponent 实例化完整 Main.qml，覆盖
// 启动接线全链路——Component.onCompleted → SessionManager.restoreDocuments()
// （文档集合 / Tab 顺序 / 活动标签 / 各文档光标）→ 窗口几何应用 → visible；
// 以及 EditorArea 的光标双向同步（doc → TextArea 恢复、TextArea → doc 回写）。
//
// 会话重定向：main 中在任何 QSettings 实例（SessionManager 单例）创建之前
// 调用 QSettings::setPath(IniFormat, UserScope, <临时目录>)，单例的会话
// 文件落位于 <临时目录>/qcnote/qcnote.ini，不污染真实用户配置。
//
// 共享引擎：全程唯一一个 QQmlEngine（initTestCase 创建、cleanupTestCase
// 删除），与生产形态一致——生产只有一个引擎，两个单例只创建一次、从不
// 跨引擎复用。tc011 为首个用例，预写会话（独立 QSettings + sync）时尚无
// 任何单例；共享引擎首次实例化 Main.qml 时单例才首次创建（构造时读盘，
// 读到预写会话），与生产冷启动一致。不得在用例中直接或经临时引擎请求
// 任何 qcnote 单例——一切经共享引擎的 Main.qml 实例化路径。
//
// 单例残留：DocumentManager / SessionManager 为 CppOwnership 单例、跨用例
// 复用同一 C++ 实例，因此每用例开始前关闭全部文档、结束前同样清理，并等待
// 防抖（500ms）落盘收敛——会话文件是单例自身写出的，文件收敛即单例缓存
// 收敛，下一用例的 restoreDocuments 从确定状态出发。
//
// GUI 红线：main 中强制 QT_QPA_PLATFORM=offscreen，全程无可见窗口。
// 与 tst_windowclose.cpp 互补：那里验证关闭/退出流程，这里验证启动恢复。

#include <QTest>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRect>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariant>

#include "document/document.h"
#include "document/documentmanager.h"
#include "document/sessionmanager.h"

// ==================== 测试台辅助 ====================

namespace {

// 进程级会话重定向目录：main 中 setPath 前创建，进程退出时随 QTemporaryDir 清理
QTemporaryDir &sessionRedirectDir()
{
    static QTemporaryDir dir;
    return dir;
}

// 重定向后的会话文件完整路径：setPath 基目录 + <组织名>/<应用名>.ini
// （与 SessionManager 内 QSettings(IniFormat, UserScope, "qcnote", "qcnote") 一致）
QString sessionFilePath()
{
    return sessionRedirectDir().filePath(QStringLiteral("qcnote/qcnote.ini"));
}

// QML 类型短名精确匹配（Qt 6.5+ 控件实例类名为 QML 包装类型，如
// "TextArea_QMLTYPE_13"，取首个下划线前段比较；说明详见 tst_windowclose.cpp）
bool qmlTypeNameIs(const QObject *obj, const char *name)
{
    const QString cn = QString::fromLatin1(obj->metaObject()->className());
    const int cut = cn.indexOf(u'_');
    return (cut > 0 ? cn.left(cut) : cn) == QLatin1String(name);
}

// 深度遍历可视树
template <typename F>
void visitItems(QQuickItem *item, const F &fn)
{
    if (!item)
        return;
    fn(item);
    const auto children = item->childItems();
    for (QQuickItem *child : children)
        visitItems(child, fn);
}

// 找当前可见的 TextArea（EditorArea 仅活动文档的 delegate 可见，
// 因此它即活动文档的编辑器；无文档/不可见时返回 nullptr）
QQuickItem *findVisibleTextArea(QQuickWindow *win)
{
    QQuickItem *found = nullptr;
    visitItems(win->contentItem(), [&](QQuickItem *it) {
        if (found || !it->isVisible() || !qmlTypeNameIs(it, "TextArea"))
            return;
        found = it;
    });
    return found;
}

// 关闭全部文档：DocumentManager 单例跨用例复用，下一用例的 restore 在
// Bench 构造期执行，必须从空文档列表出发（restore 对已打开同 url 文档
// 是去重激活而非重开，脏列表会破坏"新启动"语义）
void closeAllDocuments()
{
    DocumentManager *dm = DocumentManager::instance();
    if (!dm)
        return;
    const auto docs = dm->documents();
    for (Document *d : docs)
        dm->closeDocument(d);
}

// 等待防抖（500ms 单触发）落盘，直到会话文件 [documents] size 收敛为期望值。
// 每次迭代新建 QSettings 实例（构造时重读磁盘），避免实例缓存陈旧
bool waitForSessionDocCount(int expected)
{
    for (int i = 0; i < 100; ++i) { // 最多约 5s（100 × 50ms）
        {
            QSettings f(sessionFilePath(), QSettings::IniFormat);
            if (f.value(QStringLiteral("documents/size")).toInt() == expected)
                return true;
        }
        QTest::qWait(50);
    }
    return false;
}

// 同 waitForSessionDocCount 模式：等待 [window] 五键收敛为期望几何
bool waitForWindowRect(int x, int y, int width, int height, bool maximized)
{
    for (int i = 0; i < 100; ++i) { // 最多约 5s（100 × 50ms）
        {
            QSettings f(sessionFilePath(), QSettings::IniFormat);
            if (f.value(QStringLiteral("window/x")).toInt() == x
                    && f.value(QStringLiteral("window/y")).toInt() == y
                    && f.value(QStringLiteral("window/width")).toInt() == width
                    && f.value(QStringLiteral("window/height")).toInt() == height
                    && f.value(QStringLiteral("window/maximized")).toBool() == maximized)
                return true;
        }
        QTest::qWait(50);
    }
    return false;
}

// 同上模式：仅等待 [window]/maximized 收敛为期望值（几何值由上一状态
// 决定、对 P2-1 断言不关键）
bool waitForWindowMaximized(bool expected)
{
    for (int i = 0; i < 100; ++i) { // 最多约 5s（100 × 50ms）
        {
            QSettings f(sessionFilePath(), QSettings::IniFormat);
            if (f.value(QStringLiteral("window/maximized")).toBool() == expected)
                return true;
        }
        QTest::qWait(50);
    }
    return false;
}

// 取编辑器所在 ScrollView 的滚动载体：ScrollView 无 flickable 属性（运行时
// 取值 undefined——这正是 P1-1 缺陷根因），其 contentItem 即内部 Flickable，
// 与 EditorArea 修复后 scrollCursorIntoView 的取法一致；从编辑器沿父链向上
// 找 ScrollView（中间隔 contentItem Flickable 一层）
QQuickItem *flickableOf(QQuickItem *editor)
{
    QQuickItem *p = editor ? editor->parentItem() : nullptr;
    while (p && !qmlTypeNameIs(p, "ScrollView"))
        p = p->parentItem();
    if (!p)
        return nullptr;
    return qobject_cast<QQuickItem *>(p->property("contentItem").value<QObject *>());
}

/**
 * @brief 每个测试用例独立的测试台：在共享引擎上完整实例化 Main.qml。
 *
 * 引擎为 TestSessionRestore 全程持有的共享引擎，Bench 不拥有、不析构
 * （~Bench 只 delete win）；与 tst_windowclose.cpp 的 Bench 同构
 * （QML 装载内联，避免额外模块文件）。
 * 注意：restoreDocuments 在构造期（Component.onCompleted）即已执行，
 * init() 只取单例引用与做断言前置检查，不做文档清理——清理必须在
 * 构造前完成（见 closeAllDocuments）。
 *
 * 单例创建时序：Main.qml 的 title 绑定先创建 DocumentManager 单例，
 * Component.onCompleted 再创建 SessionManager 单例（其 create() 依赖
 * DocumentManager::instance()），与真实应用一致；不得经无 QML 上下文的
 * 引擎单独请求 SessionManager 单例——那会绕过上述时序。
 */
struct Bench
{
    QQmlEngine *engine = nullptr;  // 共享引擎（TestSessionRestore 持有，Bench 不拥有）
    QQuickWindow *win = nullptr;   // Main.qml 根（ApplicationWindow）
    DocumentManager *dm = nullptr; // QML 单例的同一 C++ 实例

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

    bool init()
    {
        if (!win)
            return false;
        dm = engine->singletonInstance<DocumentManager *>(u"qcnote", u"DocumentManager");
        return dm != nullptr;
    }
};

} // namespace

class TestSessionRestore : public QObject
{
    Q_OBJECT

private slots:
    // 共享引擎创建/销毁：全程唯一 QQmlEngine，与生产形态一致（单引擎、
    // 单例只创建一次、不跨引擎复用）；此时会话重定向 setPath 已在 main 完成
    void initTestCase();
    void cleanupTestCase();

    // TC-011: 启动恢复链路——文档集合/顺序、活动标签、各文档光标、
    // 编辑器光标双向同步、窗口按恢复时序显示
    void tc011_startupRestore_documentsCursorActive();

    // TC-012: 无会话回归——空会话启动显示欢迎页（无文档），新建文档正常
    void tc012_noSession_welcomeAndNewDocument();

    // 加分项：干净状态下驱动退出路径（close → onClosing 放行），
    // 防抖落盘后会话文件与当前文档集一致（数量/URL/光标/窗口节）
    void tc013_closePath_writesSession();

    // tc014: 窗口几何持久化往返（P0-1：恢复后不动窗口不得以默认值
    // (0,0,1000,640) 覆盖 [window]）+ 恢复光标后滚动到位
    //（P1-1：scrollCursorIntoView 经 contentItem 实际滚动）
    void tc014_windowGeometryAndScrollRestore();

    // tc015: 最小化期间 maximized 标志保持（P2-1：Minimized 迁移时
    // QML visibility 丢失"此前最大化"信息，上报不得把缓存覆盖为 false）
    void tc015_minimizedKeepsMaximizedFlag();

private:
    QQmlEngine *m_engine = nullptr; ///< 全程唯一共享引擎：单例宿主，跨用例复用
};

// 共享引擎创建：此后 Main.qml 的首次实例化（tc011 的 Bench）才触发两个
// 单例的创建，与生产“单引擎、单例只创建一次”一致
void TestSessionRestore::initTestCase()
{
    m_engine = new QQmlEngine;
}

// 引擎删除会一并销毁其实例化的 DocumentManager 单例；SessionManager 为
// CppOwnership 仍存活，其 m_manager 为 QPointer、销毁自动置空，后续判空
// 安全跳过——不崩溃
void TestSessionRestore::cleanupTestCase()
{
    delete m_engine;
    m_engine = nullptr;
}

// TC-011: 预写会话（2 个存在的临时文件 + 光标 + active）后实例化 Main.qml，
// 断言恢复结果。文档 a 非活动、b 活动，光标值均小于内容长度（钳制路径由
// test_core 的 SessionManager 单元测试覆盖，这里取原样恢复值）。
void TestSessionRestore::tc011_startupRestore_documentsCursorActive()
{
    closeAllDocuments();

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pathA = dir.filePath(QStringLiteral("a.txt"));
    const QString pathB = dir.filePath(QStringLiteral("b.txt"));
    {
        QFile f(pathA);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("aaaa\nbbbb\ncccc"); // 长度 15
    }
    {
        QFile f(pathB);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("0123456789"); // 长度 10
    }
    const QUrl urlA = QUrl::fromLocalFile(pathA);
    const QUrl urlB = QUrl::fromLocalFile(pathB);
    const int cursorA = 6;
    const int cursorB = 9;

    // 按 SessionManager 格式预写会话：2 个文档（a 在前 = 上次 Tab 顺序）、
    // 各自光标、active = b
    {
        QSettings s(sessionFilePath(), QSettings::IniFormat);
        s.setValue(QStringLiteral("documents/size"), 2);
        s.setValue(QStringLiteral("documents/1/url"), urlA.toString());
        s.setValue(QStringLiteral("documents/1/cursor"), cursorA);
        s.setValue(QStringLiteral("documents/2/url"), urlB.toString());
        s.setValue(QStringLiteral("documents/2/cursor"), cursorB);
        s.setValue(QStringLiteral("documents/active"), urlB.toString());
        s.sync();
    }
    // 首个用例：共享引擎下这是 Main.qml 的首次实例化，两个单例此时才首次
    // 创建——构造时读盘，读到上面的预写会话，与生产冷启动一致
    Bench b(m_engine);
    QVERIFY2(b.init(), "testbench init failed");

    // 文档集合与 Tab 顺序（= 上次顺序）
    QCOMPARE(b.dm->documents().size(), 2);
    QCOMPARE(b.dm->documents().at(0)->url(), urlA);
    QCOMPARE(b.dm->documents().at(1)->url(), urlB);
    // 活动标签恢复为会话记录的 b
    QCOMPARE(b.dm->activeDocument(), b.dm->documents().at(1));
    // 各文档光标恢复
    QCOMPARE(b.dm->documents().at(0)->cursorPosition(), cursorA);
    QCOMPARE(b.dm->documents().at(1)->cursorPosition(), cursorB);

    // 窗口已按恢复时序显示（几何应用后才 visible）；offscreen 屏幕几何
    // 不确定，宽松断言：宽度不小于产品最小宽
    QVERIFY(b.win->isVisible());
    QVERIFY(b.win->width() >= b.win->minimumWidth());

    // 光标下行同步（doc → TextArea）：活动文档（b）的编辑器光标已恢复
    QQuickItem *editorB = findVisibleTextArea(b.win);
    QVERIFY(editorB != nullptr);
    QCOMPARE(editorB->property("cursorPosition").toInt(), cursorB);
    QCOMPARE(editorB->property("length").toInt(), 10);

    // 光标上行同步（TextArea → doc）：属性写入与真实光标移动走同一
    // cursorPositionChanged 信号路径，触发 EditorArea 的用户回写分支
    QVERIFY(editorB->setProperty("cursorPosition", 4));
    QCOMPARE(b.dm->documents().at(1)->cursorPosition(), 4);

    // 切换活动文档后：a 的编辑器（现可见）光标为其恢复值——
    // 各 delegate 的光标状态独立保留
    b.dm->setActiveDocument(b.dm->documents().at(0));
    QQuickItem *editorA = findVisibleTextArea(b.win);
    QVERIFY(editorA != nullptr);
    QVERIFY(editorA != editorB);
    QCOMPARE(editorA->property("cursorPosition").toInt(), cursorA);
    QVERIFY(editorA->setProperty("cursorPosition", 2));
    QCOMPARE(b.dm->documents().at(0)->cursorPosition(), 2);

    closeAllDocuments();
}

// TC-012: 无会话回归——上一用例清理后等待会话文件收敛为空（size=0），
// 再实例化 Main.qml：无文档（欢迎页）、active 为空、新建文档正常工作。
// 回归红线：无会话文件时行为与现状完全一致。
void TestSessionRestore::tc012_noSession_welcomeAndNewDocument()
{
    closeAllDocuments();
    // 等待上一用例清理触发的防抖写盘收敛：文件 size=0 由单例自身写出，
    // 亦即单例的 QSettings 缓存已为 0，Bench 构造期 restore 确定读到空会话
    QVERIFY2(waitForSessionDocCount(0), "session file should settle to empty");

    Bench b(m_engine);
    QVERIFY2(b.init(), "testbench init failed");

    QCOMPARE(b.dm->documents().size(), 0);
    QVERIFY(b.dm->activeDocument() == nullptr);
    QVERIFY(b.win->isVisible());

    // 新建文档正常工作
    b.dm->newDocument();
    QCOMPARE(b.dm->documents().size(), 1);
    QVERIFY(b.dm->activeDocument() != nullptr);
    QCOMPARE(b.dm->activeDocument()->content(), QString());

    closeAllDocuments();
}

// 加分项：干净状态下驱动退出路径——空会话启动后打开并保存两个文档、
// 移动光标，QWindow::close() → onClosing 干净放行；防抖落盘后以新
// QSettings 实例读会话文件，断言与当前文档集一致。
void TestSessionRestore::tc013_closePath_writesSession()
{
    closeAllDocuments();
    QVERIFY2(waitForSessionDocCount(0), "session file should settle to empty");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    Bench b(m_engine);
    QVERIFY2(b.init(), "testbench init failed");
    QVERIFY(b.dm->documents().isEmpty()); // 前置：空会话启动

    // 两个已保存到磁盘的文档（会话只记录有 URL 的文档；未命名不记录）
    const QUrl urlX = QUrl::fromLocalFile(dir.filePath(QStringLiteral("x.txt")));
    const QUrl urlY = QUrl::fromLocalFile(dir.filePath(QStringLiteral("y.txt")));
    b.dm->newDocument();
    Document *docX = b.dm->activeDocument();
    docX->setContent(QStringLiteral("alpha"));
    QVERIFY(b.dm->saveAs(docX, urlX));
    b.dm->newDocument();
    Document *docY = b.dm->activeDocument();
    docY->setContent(QStringLiteral("beta"));
    QVERIFY(b.dm->saveAs(docY, urlY));
    docY->setCursorPosition(4); // 光标变化进入防抖（会话恢复数据源）

    // 干净状态关闭窗口：onClosing 放行（无未保存修改，现有退出流程）
    QVERIFY(!b.dm->hasUnsavedChanges());
    QVERIFY(b.win->close());
    QTRY_VERIFY(!b.win->isVisible());

    // 防抖落盘后：会话文件与当前文档集一致
    QVERIFY2(waitForSessionDocCount(2), "session file should record both documents");
    QSettings f(sessionFilePath(), QSettings::IniFormat);
    QCOMPARE(f.value(QStringLiteral("documents/size")).toInt(), 2);
    QCOMPARE(QUrl(f.value(QStringLiteral("documents/1/url")).toString()), urlX);
    QCOMPARE(QUrl(f.value(QStringLiteral("documents/2/url")).toString()), urlY);
    QCOMPARE(f.value(QStringLiteral("documents/2/cursor")).toInt(), 4);
    // 窗口节已写入（Component.onCompleted 的首次上报保证首启也写 [window]）
    QVERIFY(f.contains(QStringLiteral("window/width")));

    closeAllDocuments();
}

// tc014: 窗口几何持久化往返（P0-1 回归）+ 恢复光标后滚动到位（P1-1 回归）。
//
// 几何：SessionManager 单例跨用例复用、windowState() 读构造期缓存而非重读
// 磁盘，直接预写 [window] 文件对本用例无效——改经窗口 API 驱动（x/y/宽高
// 变化 → onXChanged → lastNormal* → 防抖写盘）把单例缓存与会话文件同步带入
// 目标几何，等价于冷启动读到预置 [window]。随后开新窗口且全程不动：新 Main
// 根的 lastNormal* 从默认值 (0,0,1000,640) 出发，若 onCompleted 未在应用
// 恢复几何前先行更新数据源，visible=true 同步触发的首次上报即以默认值覆盖
// [window]（P0-1 回归即在此失败）。
//
// 滚动：长文档（80 行，内容高度远超可视区）光标设在最后一行行首并入会话 →
// 新窗口 delegate 重建走"创建期应用光标并滚动"的会话恢复同款路径。注意 Qt 6
// 的 TextArea 置于 ScrollView 内会原生自动滚动光标（attachFlickable 后
// cursorRectangleChanged → ensureCursorVisible），仅断言 contentY > 0 无法
// 区分实现来源——故在行为断言之外，重置滚动后直接调用 scrollCursorIntoView
// 精确断言自定义实现生效：取不存在的 flickable 属性恒早退时 contentY 保持 0
//（P1-1 回归即在此失败）。
void TestSessionRestore::tc014_windowGeometryAndScrollRestore()
{
    closeAllDocuments();
    QVERIFY2(waitForSessionDocCount(0), "session file should settle to empty");

    // ---- Part 1: 窗口几何持久化往返（P0-1 断言）----
    // offscreen 屏幕尺寸不确定：基于主屏可用区构造目标几何，再经恢复时
    // windowState() 使用的同一校验纯函数"合法化"，保证 ≥ 800×500、屏内、
    // 恢复后逐值相等可精确断言
    const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
    QList<QRect> screenGeoms;
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (const QScreen *s : screens)
        screenGeoms.append(s->availableGeometry());
    const QVariantMap legalized = SessionManager::validateWindowState(
            avail.x() + 10, avail.y() + 10,
            qMin(900, avail.width() - 20), qMin(550, avail.height() - 20),
            false, screenGeoms);
    QVERIFY(!legalized.isEmpty());
    const int targetX = legalized.value(QStringLiteral("x")).toInt();
    const int targetY = legalized.value(QStringLiteral("y")).toInt();
    const int targetW = legalized.value(QStringLiteral("width")).toInt();
    const int targetH = legalized.value(QStringLiteral("height")).toInt();

    // ---- Part 2 的长文档素材（函数级作用域：QTemporaryDir 须存活到 b3
    // 构造——restore 的本地文件存在性预检会再次访问它）----
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QStringList lines;
    for (int i = 1; i <= 80; ++i)
        lines << QStringLiteral("line%1").arg(i);
    const QString longContent = lines.join(u'\n');
    const QString longPath = dir.filePath(QStringLiteral("long.txt"));
    {
        QFile f(longPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        const QByteArray longBytes = longContent.toUtf8();
        QVERIFY(f.write(longBytes) == longBytes.size());
    }
    const QUrl longUrl = QUrl::fromLocalFile(longPath);
    const int lastLineStart = longContent.length() - QStringLiteral("line80").length();

    {
        // 驱动单例缓存与会话文件进入目标几何（"上次会话以此几何退出"）
        Bench b1(m_engine);
        QVERIFY2(b1.init(), "testbench init failed");
        b1.win->setX(targetX);
        b1.win->setY(targetY);
        b1.win->setWidth(targetW);
        b1.win->setHeight(targetH);
        QVERIFY2(waitForWindowRect(targetX, targetY, targetW, targetH, false),
                 "window geometry should settle to target");
    }

    {
        // 新窗口：恢复几何已应用 + 全程不动窗口
        Bench b2(m_engine);
        QVERIFY2(b2.init(), "testbench init failed");
        // 恢复生效：窗口实际几何与目标一致
        QCOMPARE(b2.win->x(), targetX);
        QCOMPARE(b2.win->y(), targetY);
        QCOMPARE(b2.win->width(), targetW);
        QCOMPARE(b2.win->height(), targetH);
        // 跨过 500ms 防抖窗口后读盘：[window] 仍为目标值，未被默认值
        // (0,0,1000,640) 覆盖（P0-1 回归在此失败）
        QTest::qWait(800);
        QSettings f(sessionFilePath(), QSettings::IniFormat);
        QCOMPARE(f.value(QStringLiteral("window/x")).toInt(), targetX);
        QCOMPARE(f.value(QStringLiteral("window/y")).toInt(), targetY);
        QCOMPARE(f.value(QStringLiteral("window/width")).toInt(), targetW);
        QCOMPARE(f.value(QStringLiteral("window/height")).toInt(), targetH);
        QCOMPARE(f.value(QStringLiteral("window/maximized")).toBool(), false);

        // ---- Part 2: 恢复光标后滚动到位（P1-1 断言）----
        b2.dm->openUrls({ longUrl });
        Document *doc = b2.dm->documentForUrl(longUrl);
        QVERIFY(doc != nullptr);
        QTest::qWait(50); // 等一拍让 delegate 完成布局（光标矩形可测）
        doc->setCursorPosition(lastLineStart); // C++ 侧写光标 → 下行应用 + 滚动
        QQuickItem *editor2 = findVisibleTextArea(b2.win);
        QVERIFY(editor2 != nullptr);
        QCOMPARE(editor2->property("cursorPosition").toInt(), lastLineStart);
        QQuickItem *flick2 = flickableOf(editor2);
        QVERIFY(flick2 != nullptr);
        // 行为断言：光标在文档末尾时已滚动进可视区。注意 Qt 6 的 TextArea
        // 置于 ScrollView 内会原生 attach 到内部 Flickable 并在
        // cursorRectangleChanged 时自动 ensureCursorVisible，本断言钉住的
        // 是端到端效果（原生 + 自定义实现共同保证）
        QTRY_VERIFY2(flick2->property("contentY").toReal() > 0.0,
                     "cursor at document end should be scrolled into view");
        // P1-1 精确断言：重置滚动后直接调用 scrollCursorIntoView，隔离原生
        // 滚动的贡献——函数若取不存在的 flickable 属性恒早退，contentY
        // 将保持 0，即在此失败
        QVERIFY(flick2->setProperty("contentY", 0.0));
        QVERIFY(QMetaObject::invokeMethod(editor2, "scrollCursorIntoView"));
        QTRY_VERIFY2(flick2->property("contentY").toReal() > 0.0,
                     "scrollCursorIntoView should scroll by itself (P1-1)");
        QVERIFY2(waitForSessionDocCount(1), "session should record the long document");
    } // b2 析构：文档保持打开、光标已入会话（delegate 随窗口销毁重建）

    {
        // 新窗口：delegate 重建即"创建期应用光标并滚动"（会话恢复同款路径）
        Bench b3(m_engine);
        QVERIFY2(b3.init(), "testbench init failed");
        QQuickItem *editor3 = findVisibleTextArea(b3.win);
        QVERIFY(editor3 != nullptr);
        QCOMPARE(editor3->property("cursorPosition").toInt(), lastLineStart);
        QQuickItem *flick3 = flickableOf(editor3);
        QVERIFY(flick3 != nullptr);
        // 同 b2：先行为断言（端到端滚动已发生），再重置滚动直接调用函数
        // 精确断言自定义实现本身生效（P1-1）
        QTRY_VERIFY2(flick3->property("contentY").toReal() > 0.0,
                     "restored cursor should be scrolled into view");
        QVERIFY(flick3->setProperty("contentY", 0.0));
        QVERIFY(QMetaObject::invokeMethod(editor3, "scrollCursorIntoView"));
        QTRY_VERIFY2(flick3->property("contentY").toReal() > 0.0,
                     "scrollCursorIntoView should scroll by itself (P1-1)");
    }

    closeAllDocuments();
}

// tc015: 最小化期间 maximized 标志保持（P2-1 回归）。
//
// 场景：最大化状态下最小化窗口（如任务栏最小化）→ QML visibility 报告
// Minimized（丢失"此前最大化"信息）→ onVisibilityChanged 触发防抖上报。
// 若上报时实时取 visibility === Window.Maximized，会把缓存的 maximized
// 覆盖为 false（P2-1 缺陷：最大化+最小化退出后重启丢失最大化状态）；
// 修复为 lastMaximized 仅在 Maximized/Windowed 迁移时更新，Minimized
// 迁移保持缓存值不变。断言最小化触发的写盘仍记录 maximized=true。
//
// offscreen 平台驱动方式：QQuickWindow 的 visibility 属性写入走
// setWindowStates（offscreen 平台窗口状态纯记录、必发 windowStateChanged
// → visibilityChanged），与真实窗口状态迁移同一信号路径。
void TestSessionRestore::tc015_minimizedKeepsMaximizedFlag()
{
    closeAllDocuments();
    QVERIFY2(waitForSessionDocCount(0), "session file should settle to empty");

    Bench b(m_engine);
    QVERIFY2(b.init(), "testbench init failed");

    // 前置：从 Windowed 出发（Bench 构造应用 [window] 后恢复为普通窗口）
    QCOMPARE(b.win->visibility(), QQuickWindow::Windowed);

    // 1) 最大化：lastMaximized 置 true → 防抖写盘 maximized=true
    QVERIFY(b.win->setProperty("visibility", QQuickWindow::Maximized));
    QCOMPARE(b.win->visibility(), QQuickWindow::Maximized); // 驱动生效（否则用例无效）
    QVERIFY2(waitForWindowMaximized(true), "maximized=true should be persisted");

    // 2) 最小化（最大化窗口最小化）：onVisibilityChanged 以 Minimized
    //    迁移上报——缺陷实现会把 maximized 覆盖为 false（P2-1 回归在此失败）
    QVERIFY(b.win->setProperty("visibility", QQuickWindow::Minimized));
    QCOMPARE(b.win->visibility(), QQuickWindow::Minimized);

    // 3) 跨过防抖窗口后读盘：最小化触发的写盘仍记录 maximized=true
    QTest::qWait(800);
    {
        QSettings f(sessionFilePath(), QSettings::IniFormat);
        QCOMPARE(f.value(QStringLiteral("window/maximized")).toBool(), true);
    }

    // ---- Part 2: maximized=true 的 QML 恢复分支（可选-2 回归）----
    // 此前该分支（onCompleted 中 if (g.maximized === true)
    // visibility = Window.Maximized）从未被断言（未测试代码）。文档收敛后
    // 开新窗口（单例缓存与盘上 [window] maximized 均已为 true），断言
    // 恢复分支生效：新窗口以 Maximized 状态显示。offscreen 平台窗口状态
    // 驱动可行性已由 Part 1 的 1) 自证（QML visibility 属性写入走
    // setWindowStates 纯记录、必发 visibilityChanged，与恢复分支同一信号路径）
    closeAllDocuments();
    QVERIFY2(waitForSessionDocCount(0), "session file should settle to empty");
    {
        Bench b2(m_engine);
        QVERIFY2(b2.init(), "testbench init failed");
        QCOMPARE(b2.win->visibility(), QQuickWindow::Maximized);
        // lastNormal 语义从简不断言：恢复分支先写 lastNormal* 再切
        // Maximized，最大化几何不会污染"还原"基准（正常几何仍为会话记录值）
    }

    closeAllDocuments();
}

int main(int argc, char *argv[])
{
    // GUI 红线：自动化验证一律 offscreen，全程无可见窗口
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // 隔离首启右键菜单提示：无标记机器上 Main.qml 启动会弹模态提示框并
    // 锁定 uiLocked、拦截窗口关闭路径导致用例超时；置 no-op 钩子使
    // ShellIntegration 不提示、不写注册表（须在 QML 引擎/单例创建前设置）
    qputenv("QCNOTE_TEST_SHELL_NOOP", "1");
    // offscreen 平台不带字体目录（Qt 不带内嵌字体）时指定系统字体，
    // 保证 TextArea 文本渲染/光标测量路径正常
    static const char kWinFonts[] = "C:/Windows/Fonts";
    if (QDir(QString::fromLatin1(kWinFonts)).exists())
        qputenv("QT_QPA_FONTDIR", kWinFonts);
    QGuiApplication app(argc, argv);
    // 会话重定向：必须在任何 QSettings 实例（首个 Bench 构造的
    // SessionManager 单例）创建之前调用；落位于 <临时目录>/qcnote/qcnote.ini，
    // 不污染真实用户配置。QTemporaryDir 为进程级静态，退出时清理
    sessionRedirectDir();
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       sessionRedirectDir().path());
    // 草稿缓存隔离（审查应改 #2）：Main.qml 的 Component.onCompleted 每次实例化
    // 都会调 DraftCacheManager.restoreDrafts()，其非便携默认目录按测试 exe 名
    // 派生自 %LOCALAPPDATA%（本测试为 %LOCALAPPDATA%\test_sessionrestore\drafts），
    // 跨运行持久。上一轮若以脏文档收场（QVERIFY 失败 return 跳过清理、或进程
    // 被杀）且防抖在后续事件循环到期写盘，条目残留会让本轮恢复出额外文档，
    // 文档计数类断言（tc011/tc012 等 waitForSessionDocCount）偶发失败。启动
    // 预清理隔离上一轮残留；结束清理避免测试产物在用户 %LOCALAPPDATA% 积累。
    // 不用 setTestModeEnabled（避免影响既有 QSettings 语义），不改应用名
    // （目录不漂移、不积累）。
    const QString draftsDir = QStandardPaths::writableLocation(
                QStandardPaths::AppLocalDataLocation) + QStringLiteral("/drafts");
    QDir(draftsDir).removeRecursively();
    // 窗口关闭（visible = false）不得触发 QGuiApplication 自动退出，
    // 否则退出标志会干扰 QTest 内部事件循环
    app.setQuitOnLastWindowClosed(false);
    // 与主程序一致的控件样式（Main.qml 使用 Material 附加属性）
    QQuickStyle::setStyle(QStringLiteral("Material"));
    TestSessionRestore t;
    const int testResult = QTest::qExec(&t, argc, argv);
    // 结束清理：删除本轮测试写入的草稿条目，不在 %LOCALAPPDATA% 留测试产物
    QDir(draftsDir).removeRecursively();
    return testResult;
}

#include "tst_sessionrestore.moc"
