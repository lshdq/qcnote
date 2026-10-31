// 窗口关闭/退出流程 QML 集成测试（测试用例 TC-001 至 TC-009 的可自动化部分）
//
// 测试方式：offscreen 平台下经 QQmlComponent 实例化完整 Main.qml（真实
// ApplicationWindow + DocumentManager QML 单例 + unsavedDialog/TitleBar 接线），
// 以 QWindow::close() 驱动 QCloseEvent——标题栏关闭图标（X）、Alt+F4、
// 任务栏关闭在 Qt 层汇聚为同一 close 事件并进入同一 onClosing 处理器；
// 用 QMetaObject 调用 unsavedDialog 的 accept()/reject()/discard()，与
// 确认框三按钮（保存/取消/放弃，DialogButtonBox 角色触发）等价。
//
// 覆盖：需求"窗口关闭一次生效"——onClosing 四分支（exiting 放行 /
// uiLocked 拒绝 / 干净直接放行 / 有未保存修改弹确认框）与确认框各分支。
// 无法自动化项（真实鼠标点击关闭图标、真实 Alt+F4 按键、另存为平台
// 对话框内交互）见测试报告"未执行"清单。
//
// 对象定位说明：Qt 6.5+ QtQuick.Controls 控件实例的 metaObject 类名是
// QML 包装类型（如 "Dialog_QMLTYPE_13"、"Menu_QMLTYPE_30"），故按 QML
// 类型短名（首个下划线前段）精确匹配；QtQuick.Dialogs 的 FileDialog
// 实例（QQuickFileDialog）不是窗口的 QObject 子孙（树转储仅见其
// QFileDialogOptions 附属），TC-008 改经 QML 行为断言（saveAsTarget）。
//
// 与 tests/test_core、tests/test_editor 互补：这里验证的是主窗口关闭/退出
// 的完整业务流程（含 exiting 多文档逐个确认链路）。
//
// GUI 红线：main 中强制 QT_QPA_PLATFORM=offscreen，全程无可见窗口。

#include <QTest>
#include <QByteArray>
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
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariant>

#include "document/document.h"
#include "document/documentmanager.h"

// ==================== 测试台辅助 ====================

namespace {

/**
 * @brief QML 类型短名精确匹配。
 *
 * Qt 6.5+ Controls 控件实例类名为 QML 包装类型
 * （"Dialog_QMLTYPE_13"、"Menu_QMLTYPE_30_QML_42" 等），取首个下划线
 * 前段与目标比较；C++ 类名（通常无下划线，如 "QFileDialogOptions"、
 * "DialogButtonBox_QMLTYPE_3" 的短名 "DialogButtonBox"）不等于目标，
 * 天然不误匹配。
 */
bool qmlTypeNameIs(const QObject *obj, const char *name)
{
    const QString cn = QString::fromLatin1(obj->metaObject()->className());
    const int cut = cn.indexOf(u'_');
    return (cut > 0 ? cn.left(cut) : cn) == QLatin1String(name);
}

// 找到 QML 类型短名为 name 的 QObject 子孙（含非可视对象，如 Popup/Dialog）
QObject *findByQmlType(QObject *root, const char *name)
{
    if (!root)
        return nullptr;
    if (qmlTypeNameIs(root, name))
        return root;
    const auto kids = root->children();
    for (QObject *kid : kids) {
        if (QObject *found = findByQmlType(kid, name))
            return found;
    }
    return nullptr;
}

void collectByQmlType(QObject *root, const char *name, QList<QObject *> *out)
{
    if (!root)
        return;
    if (qmlTypeNameIs(root, name))
        out->append(root);
    const auto kids = root->children();
    for (QObject *kid : kids)
        collectByQmlType(kid, name, out);
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

// 找到 QML 类型短名为 name 的 QQuickItem 子孙（仅可视树）
QQuickItem *findItemByQmlType(QQuickItem *root, const char *name)
{
    QQuickItem *found = nullptr;
    visitItems(root, [&](QQuickItem *it) {
        if (!found && qmlTypeNameIs(it, name))
            found = it;
    });
    return found;
}

// 确认框正文（Main.qml 中 contentItem 为 Label，取其 text 属性）
QString dialogText(QObject *dlg)
{
    const QVariant ci = dlg->property("contentItem");
    if (!ci.canConvert<QObject *>())
        return QString();
    QObject *label = ci.value<QObject *>();
    return label ? label->property("text").toString() : QString();
}

// 诊断：转储 QObject 子树（限深度，便于定位实际类名）
void dumpTree(QObject *root, int depth, int maxDepth)
{
    if (!root || depth > maxDepth)
        return;
    qWarning() << "[tree]" << QString(depth * 2, u' ')
                   + QString::fromLatin1(root->metaObject()->className())
                   + (root->objectName().isEmpty() ? QString() : u" #" + root->objectName());
    const auto kids = root->children();
    for (QObject *kid : kids)
        dumpTree(kid, depth + 1, maxDepth);
}

/**
 * @brief 每个测试用例独立的测试台：完整实例化 Main.qml。
 *
 * QML 装载（内联，避免额外模块文件）：
 *   import qcnote
 *   Main {}
 * DocumentManager 为 QML 单例且跨引擎复用同一 C++ 实例（s_instance），
 * 因此每用例开始时清空残余文档，保证前置状态干净。
 */
struct Bench
{
    QQmlEngine engine;
    QQuickWindow *win = nullptr;      // Main.qml 根（ApplicationWindow）
    DocumentManager *dm = nullptr;    // QML 单例的同一 C++ 实例
    QQuickItem *titleBar = nullptr;   // TitleBar 实例
    QObject *unsavedDlg = nullptr;    // unsavedDialog（标题"未保存的修改"）
    QObject *errDlg = nullptr;        // errorDialog
    QObject *mainMenu = nullptr;      // TitleBar.mainMenu（TitleBar 内首个 Menu）

    Bench()
    {
        QQmlComponent c(&engine);
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
        // 从 QML 引擎取实际使用的 DocumentManager 单例（同 test_editor 的
        // manager()：绕开引擎直接 create() 可能得到与 QML 不同的实例）
        dm = engine.singletonInstance<DocumentManager *>(u"qcnote", u"DocumentManager");
        if (!dm)
            return false;
        // 单例可能跨 Bench 复用：清空上一用例残余文档
        const auto docs = dm->documents();
        for (Document *d : docs)
            dm->closeDocument(d);

        QList<QObject *> dialogs;
        collectByQmlType(win, "Dialog", &dialogs);
        for (QObject *d : dialogs) {
            if (d->property("title").toString() == QStringLiteral("未保存的修改"))
                unsavedDlg = d;
            else
                errDlg = d;
        }
        titleBar = findItemByQmlType(win->contentItem(), "TitleBar");
        // TitleBar 内首个 Menu 为 mainMenu（声明在 viewMenu 之前），
        // 用 menuVisible 别名在用例内进一步校验
        mainMenu = titleBar ? findByQmlType(titleBar, "Menu") : nullptr;
        if (!unsavedDlg || !errDlg || !titleBar || !mainMenu) {
            qWarning() << "[bench] missing:" << (!unsavedDlg ? "unsavedDlg" : "")
                       << (!errDlg ? "errDlg" : "")
                       << (!titleBar ? "titleBar" : "")
                       << (!mainMenu ? "mainMenu" : "");
            dumpTree(win, 0, 10);
            return false;
        }
        return true;
    }

    // 新建文档；content 非空时写入内容（setContent 会置 modified = true）
    Document *newDoc(const QString &content = QString())
    {
        dm->newDocument();
        Document *doc = dm->activeDocument();
        if (doc && !content.isEmpty())
            doc->setContent(content);
        return doc;
    }
};

} // namespace

class TestWindowClose : public QObject
{
    Q_OBJECT

private slots:
    // TC-001: 干净状态点击关闭图标一次退出（等价以 close() 驱动 onClosing）
    void tc001_cleanClose_onceCloses();

    // TC-002: 干净状态 Alt+F4 一次退出（close() 等价驱动；真实按键无法 offscreen 合成）
    void tc002_systemCloseEquivalent_onceCloses();

    // TC-003: 干净状态菜单"退出"一次生效（doQuit 为菜单退出入口）
    void tc003_menuQuit_onceCloses();

    // TC-004: 有未保存修改点击关闭 → 弹"未保存的修改"确认框，窗口不关闭
    void tc004_modifiedClose_showsUnsavedDialog();

    // TC-005: 确认框"放弃"→ 关闭文档；无其他脏文档则退出，有则弹下一个
    void tc005_discard_quitsAndIterates();

    // TC-006: 确认框"保存"（有路径）→ 写盘后关闭并退出
    void tc006_accept_savesThenQuits();

    // TC-007: 确认框"取消"→ 中止退出内容保留；再次关闭重新弹框
    void tc007_cancel_abortsAndReconfirms();

    // TC-008（可自动化部分）: 无路径脏文档选"保存" → 转另存为分支且不退出
    void tc008_noUrlAccept_goesSaveAsBranch();

    // TC-009: 模态 UI（主菜单 / 错误框）打开时关闭请求被忽略
    void tc009_modalUI_closeIgnored();
};

// TC-001: 无未保存修改时，一次关闭请求即关闭窗口。
// 旧缺陷实现（close.accepted=false + 嵌套 root.close() 被 QWindow::close()
// 的 inClose 重入保护静默忽略）在此断言下窗口保持可见、需要第二次请求；
// 修复后第一次 close() 即放行。
void TestWindowClose::tc001_cleanClose_onceCloses()
{
    // 场景 a：启动即无文档（欢迎页）
    {
        Bench b;
        QVERIFY2(b.init(), "testbench init failed");
        QVERIFY(!b.dm->hasUnsavedChanges());
        QVERIFY(b.win->isVisible());
        QVERIFY(b.win->close());            // 第一次（也是唯一一次）关闭请求
        QTRY_VERIFY(!b.win->isVisible());   // 窗口已关闭
        QVERIFY(!b.unsavedDlg->property("visible").toBool()); // 不弹确认框
    }
    // 场景 b：存在一个未修改的新文档
    {
        Bench b;
        QVERIFY2(b.init(), "testbench init failed");
        Document *doc = b.newDoc();
        QVERIFY(doc && !doc->modified());
        QVERIFY(b.win->close());
        QTRY_VERIFY(!b.win->isVisible());
        QVERIFY(!b.unsavedDlg->property("visible").toBool());
    }
}

// TC-002: 干净状态系统关闭键（Alt+F4）一次退出。
// 真实 Alt+F4 按键组合依赖平台窗口系统键盘路径（offscreen 无系统窗口），
// 以 QWindow::close() 等价驱动：系统关闭键与 Alt+F4 汇聚为同一
// QCloseEvent → onClosing 处理器。真实按键见报告"未执行"清单。
void TestWindowClose::tc002_systemCloseEquivalent_onceCloses()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // 已保存过的文档（曾写盘、当前无未保存修改）
    Document *doc = b.newDoc(QStringLiteral("saved"));
    QVERIFY(b.dm->saveAs(doc, QUrl::fromLocalFile(dir.filePath(QStringLiteral("a.txt")))));
    QVERIFY(!doc->modified());
    QVERIFY(!b.dm->hasUnsavedChanges());

    QVERIFY(b.win->close());
    QTRY_VERIFY(!b.win->isVisible());
}

// TC-003: 干净状态菜单"退出"一次生效。菜单退出项的处理入口为 doQuit()
// （onRequestQuit → root.doQuit()），真实菜单鼠标点击链路无法 offscreen
// 合成，以直接调用 doQuit() 等价。另验证菜单展开时 doQuit 被防御性屏蔽。
void TestWindowClose::tc003_menuQuit_onceCloses()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc();

    QVERIFY(QMetaObject::invokeMethod(b.win, "doQuit"));
    QTRY_VERIFY(!b.win->isVisible()); // 一次生效

    // 菜单展开（uiLocked）时 doQuit 自身也被屏蔽（维持现状）
    Bench b2;
    QVERIFY2(b2.init(), "testbench init failed");
    b2.newDoc();
    QVERIFY(QMetaObject::invokeMethod(b2.mainMenu, "open"));
    QTRY_VERIFY(b2.mainMenu->property("visible").toBool());
    QVERIFY(b2.titleBar->property("menuVisible").toBool()); // uiLocked 数据源
    QVERIFY(QMetaObject::invokeMethod(b2.win, "doQuit"));
    QTRY_VERIFY(b2.win->isVisible()); // 未关闭
}

// TC-004: 有未保存修改时点击关闭 → 拒绝关闭并弹"未保存的修改"确认框。
void TestWindowClose::tc004_modifiedClose_showsUnsavedDialog()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    Document *doc = b.newDoc(QStringLiteral("modified"));
    QVERIFY(doc->modified());
    QVERIFY(b.dm->hasUnsavedChanges());

    QVERIFY(!b.win->close());                                  // 关闭请求被拒绝
    QTRY_VERIFY(b.win->isVisible());                           // 窗口保留
    QTRY_VERIFY(b.unsavedDlg->property("visible").toBool());   // 确认框弹出
    QVERIFY(dialogText(b.unsavedDlg).contains(doc->displayName()));
}

// TC-005: 确认框"放弃"（DestructiveRole → discarded）→ finishCloseFlow →
// 关闭该文档；无其他脏文档时 continueExit 置 exiting 并放行关闭（该
// root.close() 在对话框按钮事件内调用，不受 close 事件重入保护影响）；
// 还有脏文档时弹下一个确认框。
void TestWindowClose::tc005_discard_quitsAndIterates()
{
    // 场景 a：单个脏文档 → 放弃后直接退出
    {
        Bench b;
        QVERIFY2(b.init(), "testbench init failed");
        Document *doc = b.newDoc(QStringLiteral("dirty"));
        QVERIFY(!b.win->close());
        QTRY_VERIFY(b.unsavedDlg->property("visible").toBool());

        // 等价点"放弃"：Qt 6.11 Controls Dialog 的 QML 包装 metaObject 未暴露
        // discard() 方法（accept()/reject() 暴露），这里直接激活 discarded()
        // 信号——与 DialogButtonBox DestructiveRole 按钮点击后的信号分发
        // 终点等价（Main.qml 的 onDiscarded 处理器被触发）
        QVERIFY(QMetaObject::invokeMethod(b.unsavedDlg, "discarded"));
        QTRY_VERIFY(!b.win->isVisible());               // 应用退出
        QVERIFY(!b.dm->documents().contains(doc));      // 文档已关闭
    }
    // 场景 b：两个脏文档 → 逐个确认后退出（多文档迭代链路）
    {
        Bench b;
        QVERIFY2(b.init(), "testbench init failed");
        Document *docA = b.newDoc(QStringLiteral("A"));
        Document *docB = b.newDoc(QStringLiteral("B"));

        QVERIFY(!b.win->close());
        QTRY_VERIFY(b.unsavedDlg->property("visible").toBool());
        QVERIFY(dialogText(b.unsavedDlg).contains(docA->displayName())); // 先问 A

        QVERIFY(QMetaObject::invokeMethod(b.unsavedDlg, "discarded")); // 放弃 A
        QVERIFY(!b.dm->documents().contains(docA));
        QTRY_VERIFY(b.win->isVisible());                        // 尚未退出
        QTRY_VERIFY(b.unsavedDlg->property("visible").toBool()); // 弹出 B 的确认框
        QVERIFY(dialogText(b.unsavedDlg).contains(docB->displayName()));

        QVERIFY(QMetaObject::invokeMethod(b.unsavedDlg, "discarded")); // 放弃 B
        QTRY_VERIFY(!b.win->isVisible());                       // 全部处理完退出
        QVERIFY(b.dm->documents().isEmpty());
    }
}

// TC-006: 确认框"保存"（文档已有路径）→ 写盘成功 → 关闭文档并退出。
void TestWindowClose::tc006_accept_savesThenQuits()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("t.txt"));

    Document *doc = b.newDoc(QStringLiteral("v1"));
    QVERIFY(b.dm->saveAs(doc, QUrl::fromLocalFile(path))); // 有路径、清脏
    doc->setContent(QStringLiteral("v2"));                 // 重新置脏
    QVERIFY(doc->modified());

    QVERIFY(!b.win->close());
    QTRY_VERIFY(b.unsavedDlg->property("visible").toBool());

    QVERIFY(QMetaObject::invokeMethod(b.unsavedDlg, "accept")); // 等价点"保存"
    QTRY_VERIFY(!b.win->isVisible());                          // 保存后退出

    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArrayLiteral("v2"));            // 磁盘内容为保存值
    QVERIFY(b.dm->documents().isEmpty());
}

// TC-007: 确认框"取消"（RejectRole → rejected）→ resetCloseFlow 中止退出；
// 应用继续运行、内容保留；再次关闭请求重新弹确认框（流程可重入）。
void TestWindowClose::tc007_cancel_abortsAndReconfirms()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    Document *doc = b.newDoc(QStringLiteral("keep"));

    QVERIFY(!b.win->close());
    QTRY_VERIFY(b.unsavedDlg->property("visible").toBool());

    QVERIFY(QMetaObject::invokeMethod(b.unsavedDlg, "reject")); // 等价点"取消"
    QTRY_VERIFY(!b.unsavedDlg->property("visible").toBool());   // 框关闭
    QVERIFY(b.win->isVisible());                                // 应用继续运行
    QCOMPARE(doc->content(), QStringLiteral("keep"));           // 内容保留
    QVERIFY(b.dm->documents().contains(doc));

    // 再次关闭：重新弹确认框
    QVERIFY(!b.win->close());
    QTRY_VERIFY(b.unsavedDlg->property("visible").toBool());
}

// TC-008（可自动化部分）: 无路径脏文档在确认框选"保存" → 进入另存为分支
// （saveAsTarget 置位、确认框关闭、窗口不退出）。另存为 FileDialog 实例
// （QQuickFileDialog）不是窗口的 QObject 子孙，无法直接断言其可见性；
// "另存为对话框选取消 → 中止"链路也无法自动化（取消只能由对话框内部
// 用户交互触发），均见报告"未执行"清单。
void TestWindowClose::tc008_noUrlAccept_goesSaveAsBranch()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    Document *doc = b.newDoc(QStringLiteral("nosave")); // 无 url
    QVERIFY(doc->url().isEmpty());

    QVERIFY(!b.win->close());
    QTRY_VERIFY(b.unsavedDlg->property("visible").toBool());

    QVERIFY(QMetaObject::invokeMethod(b.unsavedDlg, "accept")); // 保存 → 转另存为
    QTRY_VERIFY(!b.unsavedDlg->property("visible").toBool());   // 确认框关闭
    QVERIFY(b.win->isVisible());                                // 流程挂起：等待另存为，未退出

    // 进入另存为分支：saveAsTarget 指向该文档
    const QVariant st = b.win->property("saveAsTarget");
    QVERIFY(st.canConvert<QObject *>());
    QCOMPARE(st.value<QObject *>(), static_cast<QObject *>(doc));
    QCOMPARE(doc->content(), QStringLiteral("nosave")); // 内容保留
}

// TC-009: 模态 UI 打开时关闭请求被忽略（uiLocked 分支，维持现状）。
// 覆盖主菜单（menuVisible）与错误对话框两个模态入口；并验证 uiLocked
// 优先于未保存检查（菜单展开 + 有脏文档时同样忽略且不弹确认框）。
void TestWindowClose::tc009_modalUI_closeIgnored()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");

    // 场景 a：主菜单展开 + 干净状态
    QVERIFY(QMetaObject::invokeMethod(b.mainMenu, "open"));
    QTRY_VERIFY(b.mainMenu->property("visible").toBool());
    QVERIFY(b.titleBar->property("menuVisible").toBool()); // uiLocked 数据源
    QVERIFY(!b.win->close());
    QVERIFY(b.win->isVisible());                             // 关闭被忽略
    QVERIFY(!b.unsavedDlg->property("visible").toBool());    // 不弹确认框
    QMetaObject::invokeMethod(b.mainMenu, "close");

    // 场景 b：主菜单展开 + 有未保存修改 → 同样忽略（uiLocked 分支在前）
    b.newDoc(QStringLiteral("m"));
    QVERIFY(QMetaObject::invokeMethod(b.mainMenu, "open"));
    QTRY_VERIFY(b.mainMenu->property("visible").toBool());
    QVERIFY(!b.win->close());
    QVERIFY(b.win->isVisible());
    QVERIFY(!b.unsavedDlg->property("visible").toBool());
    QMetaObject::invokeMethod(b.mainMenu, "close");

    // 场景 c：错误对话框打开 → 关闭被忽略
    QVERIFY(QMetaObject::invokeMethod(b.errDlg, "open"));
    QTRY_VERIFY(b.errDlg->property("visible").toBool());
    QVERIFY(!b.win->close());
    QVERIFY(b.win->isVisible());
}

int main(int argc, char *argv[])
{
    // GUI 红线：自动化验证一律 offscreen，全程无可见窗口
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // offscreen 平台不带字体目录（Qt 不带内嵌字体）时指定系统字体，
    // 保证文本渲染路径正常
    static const char kWinFonts[] = "C:/Windows/Fonts";
    if (QDir(QString::fromLatin1(kWinFonts)).exists())
        qputenv("QT_QPA_FONTDIR", kWinFonts);
    QGuiApplication app(argc, argv);
    // 窗口关闭（visible = false）不得触发 QGuiApplication 自动退出，
    // 否则退出标志会干扰 QTest 内部事件循环
    app.setQuitOnLastWindowClosed(false);
    // 与主程序一致的控件样式（Main.qml 使用 Material 附加属性）
    QQuickStyle::setStyle(QStringLiteral("Material"));
    TestWindowClose t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_windowclose.moc"
