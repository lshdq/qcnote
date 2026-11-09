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
// 另覆盖《测试用例-未保存状态基线比较》TC-010（UI 冒烟）：编辑→撤消
// 回写回到基线后，Tab ● 标记与标题 * 随 modifiedChanged 同步消失、
// 再编辑重现，全程无 "Binding loop" 警告（tc010，复用同一 Bench 测试台）。
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
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariant>

#include "document/document.h"
#include "document/documentmanager.h"

// ==================== 测试台辅助 ====================

namespace {

// 进程级会话重定向目录：main 中 setPath 前创建，进程退出时随 QTemporaryDir 清理
// （与 tst_sessionrestore.cpp 的 sessionRedirectDir 同一模式）
QTemporaryDir &sessionRedirectDir()
{
    static QTemporaryDir dir;
    return dir;
}

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

// ===== TC-010（未保存状态基线比较）专用辅助 =====

// 捕获 QML 绑定循环警告（"Binding loop detected ..."），验证绑定链路健康
QStringList g_bindingLoopWarnings;
void bindingLoopCollector(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Q_UNUSED(type)
    Q_UNUSED(context)
    if (msg.contains(QStringLiteral("Binding loop"), Qt::CaseInsensitive))
        g_bindingLoopWarnings.append(msg);
}

// RAII：用例内临时接管消息 handler（QML 加载与绑定刷新期间捕获绑定循环
// 警告），失败路径也保证恢复，不污染后续用例的 QtTest 默认消息处理
struct MessageHandlerGuard
{
    QtMessageHandler restore;
    explicit MessageHandlerGuard(QtMessageHandler handler)
        : restore(qInstallMessageHandler(handler)) {}
    ~MessageHandlerGuard() { qInstallMessageHandler(restore); }
};

// 定位 DocumentTabs 可视树中的未保存 ● 标记（DocumentTabs.unsavedMark）。
// 基础类型 Text 的 C++ 类名是 QQuickText（原生类型，无 QML 包装后缀，
// qmlTypeNameIs 不适用），以恒定文本 "●" 识别：可视树内唯一
// （markMetrics 是非可视 TextMetrics，不进入可视树）。
// Tab delegate 由 ListView 异步布局实例化：offscreen 下渲染循环不自动
// 推进，调用方须先 win->grabWindow() 同步驱动一帧渲染再定位。
QQuickItem *findUnsavedMark(QQuickWindow *win)
{
    QQuickItem *tabs = findItemByQmlType(win->contentItem(), "DocumentTabs");
    if (!tabs)
        return nullptr;
    QQuickItem *mark = nullptr;
    visitItems(tabs, [&](QQuickItem *it) {
        if (!mark && it->property("text").toString() == QStringLiteral("●"))
            mark = it;
    });
    return mark;
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
    QObject *errDlg = nullptr;        // errorDialog（Main.qml 四对话框经
                                      // MovableDialog 接入，按 dialogTitle 定位）
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
        collectByQmlType(win, "MovableDialog", &dialogs);
        for (QObject *d : dialogs) {
            const QString t = d->property("dialogTitle").toString();
            if (t == QStringLiteral("未保存的修改"))
                unsavedDlg = d;
            else if (t == QStringLiteral("添加右键菜单")
                     || t == QStringLiteral("按编码重新加载"))
                continue; // 非错误对话框（场景 c 只需任一模态 uiLocked 源）
            else
                errDlg = d; // errorDialog（dialogTitle 绑定 currentError，未打开为空）
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

    // TC-010（《测试用例-未保存状态基线比较》）: UI 冒烟——编辑→撤回基线
    // 后 Tab ● 与标题 * 随 modifiedChanged 消失、再编辑重现，无绑定循环
    void tc010_unsavedBaseline_uiMarksFollowModified();
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

// TC-010（《测试用例-未保存状态基线比较》）: UI 冒烟。
// 程序化走 TC-001 场景（经 Document 层模拟 TextArea 撤消回写）：
// 编辑置脏 → Tab ● 出现、标题带 *；撤回基线清脏 → ● 与 * 随
// modifiedChanged 同步消失；再编辑（重做）重新出现。另覆盖 TC-005
// 场景（新建文档输入后全部撤空清 UI）。handler 接管期覆盖 QML 加载
// 与全部绑定刷新，断言无 "Binding loop" 警告。
void TestWindowClose::tc010_unsavedBaseline_uiMarksFollowModified()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // 场景 a：已保存文档 编辑→撤回保存基线（TC-001 场景）
    g_bindingLoopWarnings.clear();
    MessageHandlerGuard guard(bindingLoopCollector); // QML 加载期起捕获
    {
        Bench b;
        QVERIFY2(b.init(), "testbench init failed");
        Document *doc = b.newDoc(QStringLiteral("abc"));
        QVERIFY(b.dm->saveAs(doc, QUrl::fromLocalFile(dir.filePath(QStringLiteral("base.txt")))));
        QVERIFY(!doc->modified()); // 保存成功：基线 = "abc"，干净

        // offscreen 渲染循环不自动推进：同步抓一帧驱动 ListView 布局，
        // 实例化 Tab delegate（此后绑定刷新由 NOTIFY 即时驱动，无需再抓帧）
        QVERIFY(!b.win->grabWindow().isNull());
        QQuickItem *unsavedMark = findUnsavedMark(b.win);
        QVERIFY(unsavedMark != nullptr);
        QVERIFY(!unsavedMark->property("visible").toBool()); // 干净：无 ●
        QVERIFY(!b.win->property("title").toString().contains(QLatin1String(" *"))); // 无 *

        doc->setContent(QStringLiteral("abcX")); // 编辑置脏（模拟 TextArea 回写）
        QVERIFY(doc->modified());
        QTRY_VERIFY(unsavedMark->property("visible").toBool()); // ● 出现
        QTRY_VERIFY(b.win->property("title").toString().contains(QLatin1String(" *")));

        doc->setContent(QStringLiteral("abc")); // 撤消回写回到基线：清脏（缺陷修复点）
        QVERIFY(!doc->modified());
        QTRY_VERIFY(!unsavedMark->property("visible").toBool()); // ● 消失
        QTRY_VERIFY(!b.win->property("title").toString().contains(QLatin1String(" *")));

        doc->setContent(QStringLiteral("abcY")); // 再编辑（重做）：重新置脏
        QVERIFY(doc->modified());
        QTRY_VERIFY(unsavedMark->property("visible").toBool());
        QTRY_VERIFY(b.win->property("title").toString().contains(QLatin1String(" *")));
    }

    // 场景 b：新建文档 输入→全部撤空（TC-005 场景：空基线撤空清 UI）
    {
        Bench b;
        QVERIFY2(b.init(), "testbench init failed");
        Document *fresh = b.newDoc();
        QVERIFY(fresh && !fresh->modified()); // 新文档基线为空串，干净

        QVERIFY(!b.win->grabWindow().isNull()); // 同步驱动 Tab delegate 实例化
        QQuickItem *unsavedMark = findUnsavedMark(b.win);
        QVERIFY(unsavedMark != nullptr);
        QVERIFY(!unsavedMark->property("visible").toBool());

        fresh->setContent(QStringLiteral("x")); // 输入置脏
        QVERIFY(fresh->modified());
        QTRY_VERIFY(unsavedMark->property("visible").toBool());

        fresh->setContent(QString()); // 全部撤空：回到空基线，清脏
        QVERIFY(!fresh->modified());
        QTRY_VERIFY(!unsavedMark->property("visible").toBool());
        QTRY_VERIFY(!b.win->property("title").toString().contains(QLatin1String(" *")));
    }

    QVERIFY2(g_bindingLoopWarnings.isEmpty(),
             qPrintable(QStringLiteral("检测到绑定循环: %1")
                            .arg(g_bindingLoopWarnings.join(u';'))));
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
    // 保证文本渲染路径正常
    static const char kWinFonts[] = "C:/Windows/Fonts";
    if (QDir(QString::fromLatin1(kWinFonts)).exists())
        qputenv("QT_QPA_FONTDIR", kWinFonts);
    QGuiApplication app(argc, argv);
    // 会话重定向：Main.qml 接入会话恢复后，各用例的 Bench 实例化 Main.qml
    // 即创建 SessionManager 单例并在构造期读写 QSettings；必须在任何
    // QSettings 实例创建之前把 IniFormat+UserScope 重定向到临时目录，
    // 否则单例将读写真实用户配置 %APPDATA%/qcnote/qcnote.ini
    //（[documents] 被清零、[window] 写成测试几何），污染跑 ctest 机器
    // 的用户配置。QTemporaryDir 为进程级静态，退出时清理
    sessionRedirectDir();
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       sessionRedirectDir().path());
    // 草稿缓存隔离（审查应改 #2）：Main.qml 的 Component.onCompleted 每次实例化
    // 都会调 DraftCacheManager.restoreDrafts()，其非便携默认目录按测试 exe 名
    // 派生自 %LOCALAPPDATA%（本测试为 %LOCALAPPDATA%\test_window\drafts），
    // 跨运行持久。上一轮若以脏文档收场（QVERIFY 失败 return 跳过清理、或进程
    // 被杀）且防抖在后续事件循环到期写盘，条目残留会让本轮恢复出额外文档，
    // 文档计数类断言偶发失败。启动预清理隔离上一轮残留；结束清理避免测试产物
    // 在用户 %LOCALAPPDATA% 积累。不用 setTestModeEnabled（避免影响既有
    // QSettings 语义），不改应用名（目录不漂移、不积累）。
    const QString draftsDir = QStandardPaths::writableLocation(
                QStandardPaths::AppLocalDataLocation) + QStringLiteral("/drafts");
    QDir(draftsDir).removeRecursively();
    // 窗口关闭（visible = false）不得触发 QGuiApplication 自动退出，
    // 否则退出标志会干扰 QTest 内部事件循环
    app.setQuitOnLastWindowClosed(false);
    // 与主程序一致的控件样式（Main.qml 使用 Material 附加属性）
    QQuickStyle::setStyle(QStringLiteral("Material"));
    TestWindowClose t;
    const int testResult = QTest::qExec(&t, argc, argv);
    // 结束清理：删除本轮测试写入的草稿条目，不在 %LOCALAPPDATA% 留测试产物
    QDir(draftsDir).removeRecursively();
    return testResult;
}

#include "tst_windowclose.moc"
