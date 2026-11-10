// 标签页右键菜单 QML 集成测试（测试用例文档《测试用例-标签页右键菜单》
// TC-001 至 TC-011 的可自动化部分；TC-013/TC-014 由构建、ctest 全量与
// offscreen 冒烟承担）。
//
// 测试方式：offscreen 平台下实例化完整 Main.qml，按 QML 类型短名定位
// DocumentTabs 与其根级右键菜单 tabContextMenu（经「关闭右边所有」等
// 文本反查 MenuItem 的 menu 属性），程序化设置 contextIndex 后 open()
// 菜单，对菜单项发送**真实鼠标事件**（QTest::mouseClick 合成，与用户
// 点击的事件分发链一致，区别于 tst_searchmenu/tst_encodingmenu 的直发
// triggered() 模式——右键菜单批量关闭入口 closeRangeWithConfirm 带
// uiLocked() 防重入检查，其结果与菜单可见性时序相关，必须走真实事件
// 路径才能覆盖）。未保存三选框经 accept()/reject()/discarded() 程序化
// 驱动（与 tst_windowclose 同一模式）。
//
// 覆盖：
// - TC-002 置灰条件：最左/中间/最右/单标签时 4 个菜单项 enabled；
// - TC-003 关闭当前文件（无修改，真实点击）；
// - TC-005 关闭右边所有（真实点击 + 锚点激活）；
// - TC-006 关闭其他文件逐个确认顺序（从左到右）与不保存链路；
// - TC-007 取消中止剩余批量关闭；
// - TC-010 关闭左边所有（真实点击 + 锚点激活）；
// - 对照用例：直发 triggered()（菜单未打开）的批量关闭——若直发成功
//   而真实点击失败，即证明 closeRangeWithConfirm 的 uiLocked() 检查被
//   菜单项触发瞬间的菜单自身可见性误杀（时序缺陷定位依据）。
//
// 无法自动化项（真实右键坐标命中、键盘 Menu/Shift+F10 弹出、系统另存
// 为对话框内交互、快捷键真实按键）见测试报告"未执行"清单。
//
// GUI 红线：main 中强制 QT_QPA_PLATFORM=offscreen，全程无可见窗口。

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QVariant>

#include "document/document.h"
#include "document/documentmanager.h"

// ==================== 测试台辅助 ====================

namespace {

// 进程级会话重定向目录：main 中 setPath 前创建（与 tst_searchmenu 同一模式）
QTemporaryDir &sessionRedirectDir()
{
    static QTemporaryDir dir;
    return dir;
}

bool qmlTypeNameIs(const QObject *obj, const char *name)
{
    const QString cn = QString::fromLatin1(obj->metaObject()->className());
    const int cut = cn.indexOf(u'_');
    return (cut > 0 ? cn.left(cut) : cn) == QLatin1String(name);
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

// 在菜单子树中按文本定位普通 MenuItem（KeyMenuItem 短名不同，不误匹配）
QObject *findMenuItem(QObject *menu, const QString &text)
{
    QList<QObject *> items;
    collectByQmlType(menu, "MenuItem", &items);
    for (QObject *it : items) {
        if (it->property("text").toString() == text)
            return it;
    }
    return nullptr;
}

/**
 * @brief 每个测试用例独立的测试台：完整实例化 Main.qml。
 *
 * DocumentManager 为 QML 单例且跨引擎复用同一 C++ 实例，因此每用例
 * 开始时清空残余文档，保证前置状态干净。
 */
struct Bench
{
    QQmlEngine engine;
    QQuickWindow *win = nullptr;        // Main.qml 根（ApplicationWindow）
    DocumentManager *dm = nullptr;      // QML 单例的同一 C++ 实例
    QQuickItem *documentTabs = nullptr; // DocumentTabs（右键菜单持有者）
    QObject *tabContextMenu = nullptr;  // DocumentTabs 根级右键菜单

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
        dm = engine.singletonInstance<DocumentManager *>(u"qcnote", u"DocumentManager");
        if (!dm)
            return false;
        const auto docs = dm->documents();
        for (Document *d : docs)
            dm->closeDocument(d);

        // DocumentTabs 在可视树中
        QQuickItem *tabs = nullptr;
        visitItems(win->contentItem(), [&](QQuickItem *it) {
            if (!tabs && qmlTypeNameIs(it, "DocumentTabs"))
                tabs = it;
        });
        documentTabs = tabs;

        // 右键菜单：按「关闭右边所有」文本反查 MenuItem，经 menu 属性取所属 Menu
        QList<QObject *> allItems;
        collectByQmlType(win, "MenuItem", &allItems);
        for (QObject *it : allItems) {
            if (it->property("text").toString() == QStringLiteral("关闭右边所有")) {
                tabContextMenu = it->property("menu").value<QObject *>();
                break;
            }
        }

        if (!documentTabs || !tabContextMenu) {
            qWarning() << "[bench] missing:" << (!documentTabs ? "documentTabs" : "")
                       << (!tabContextMenu ? "tabContextMenu" : "");
            return false;
        }

        // 4 个菜单项全部可定位
        for (const QString &t : { QStringLiteral("关闭当前文件"),
                                  QStringLiteral("关闭右边所有"),
                                  QStringLiteral("关闭左边所有"),
                                  QStringLiteral("关闭其他文件") }) {
            if (!findMenuItem(tabContextMenu, t)) {
                qWarning() << "[bench] menu item missing:" << t;
                return false;
            }
        }
        return true;
    }

    void settle() { QTest::qWait(30); }

    // 新建文档并可选写入内容（setContent 置 modified = true）
    Document *newDoc(const QString &content = QString())
    {
        dm->newDocument();
        settle();
        Document *doc = dm->activeDocument();
        if (doc && !content.isEmpty()) {
            doc->setContent(content);
            settle();
        }
        return doc;
    }

    // 打开右键菜单于指定标签索引（等价真实右键：设置 contextIndex 后 open）
    bool openContextMenuAt(int index)
    {
        if (!documentTabs->setProperty("contextIndex", index))
            return false;
        return QMetaObject::invokeMethod(tabContextMenu, "open");
    }

    // 对菜单项发送真实鼠标点击（合成 QMouseEvent 经窗口事件分发，
    // 与用户点击等价；Window 型 popup 经 item 所在 window 自动定位）
    bool realClick(const QString &itemText)
    {
        QObject *item = findMenuItem(tabContextMenu, itemText);
        if (!item)
            return false;
        QQuickItem *mi = qobject_cast<QQuickItem *>(item);
        if (!mi || !mi->window())
            return false;
        // 等菜单项布局就绪（QTRY_VERIFY 宏含裸 return，不能用于本函数）
        for (int i = 0; i < 100 && (mi->width() <= 0 || mi->height() <= 0); ++i)
            QTest::qWait(20);
        if (mi->width() <= 0 || mi->height() <= 0)
            return false;
        const QPointF center = mi->mapToScene(QPointF(mi->width() / 2, mi->height() / 2));
        QTest::mouseClick(mi->window(), Qt::LeftButton, Qt::NoModifier,
                          center.toPoint());
        return true;
    }

    // 直发菜单项 triggered()（菜单未打开；tst_searchmenu 同模式，对照组）
    bool triggerDirect(const QString &itemText)
    {
        QObject *item = findMenuItem(tabContextMenu, itemText);
        return item && QMetaObject::invokeMethod(item, "triggered");
    }

    // 未保存三选框（dialogTitle「未保存的修改」）
    QObject *unsavedDialog() const
    {
        QList<QObject *> dialogs;
        collectByQmlType(win, "MovableDialog", &dialogs);
        for (QObject *d : dialogs) {
            if (d->property("dialogTitle").toString() == QStringLiteral("未保存的修改"))
                return d;
        }
        return nullptr;
    }
};

} // namespace

// ==================== 测试类 ====================

class TestTabContextMenu : public QObject
{
    Q_OBJECT

private slots:
    // TC-002: 菜单项置灰条件（位置×数量的全部组合）
    void tc002_enablement();

    // TC-003: 关闭当前文件（无修改，真实点击，直接关闭无确认框）
    void tc003_closeCurrent_clean_realClick();

    // TC-005: 关闭右边所有（真实点击；全部干净 → 关闭右侧并激活锚点）
    void tc005_closeRight_realClick();

    // TC-010: 关闭左边所有（真实点击；关闭左侧并激活锚点）
    void tc010_closeLeft_realClick();

    // TC-006: 关闭其他文件逐个三选确认（从左到右，全部「不保存」）
    void tc006_closeOthers_discardAll_realClick();

    // TC-007: 第一个确认框「取消」中止剩余批量关闭
    void tc007_closeOthers_cancelAborts_realClick();

    // 对照：直发 triggered()（菜单未打开）的关闭右边所有——定位真实
    // 点击路径与直发路径的行为差异（uiLocked 时序缺陷诊断依据）
    void ctrl_closeRight_directSignal();
};

// TC-002: 最左标签「关闭左边所有」禁用；最右「关闭右边所有」禁用；
// 单标签「关闭其他文件」禁用（左右均禁用、当前可用）；中间 4 项全可用。
void TestTabContextMenu::tc002_enablement()
{
    // 前置：4 个干净文档 T1..T4
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    for (int i = 0; i < 4; ++i)
        QVERIFY(b.newDoc() != nullptr);
    QCOMPARE(b.dm->documents().size(), 4);

    const auto checkEnablement = [&](Bench &bench, int ctxIndex,
                                     bool cur, bool right, bool left, bool others) {
        QVERIFY2(bench.openContextMenuAt(ctxIndex),
                 qPrintable(QStringLiteral("open menu at %1 failed").arg(ctxIndex)));
        QTest::qWait(50);
        QObject *mCur = findMenuItem(bench.tabContextMenu, QStringLiteral("关闭当前文件"));
        QObject *mRight = findMenuItem(bench.tabContextMenu, QStringLiteral("关闭右边所有"));
        QObject *mLeft = findMenuItem(bench.tabContextMenu, QStringLiteral("关闭左边所有"));
        QObject *mOthers = findMenuItem(bench.tabContextMenu, QStringLiteral("关闭其他文件"));
        QVERIFY(mCur && mRight && mLeft && mOthers);
        QCOMPARE(mCur->property("enabled").toBool(), cur);
        QCOMPARE(mRight->property("enabled").toBool(), right);
        QCOMPARE(mLeft->property("enabled").toBool(), left);
        QCOMPARE(mOthers->property("enabled").toBool(), others);
        QVERIFY(QMetaObject::invokeMethod(bench.tabContextMenu, "close"));
        QTest::qWait(30);
    };

    checkEnablement(b, 0, true, true, false, true);   // 最左：左禁用
    checkEnablement(b, 1, true, true, true, true);    // 中间：4 项全可用
    checkEnablement(b, 3, true, false, true, true);   // 最右：右禁用

    // 关闭到只剩 1 个：仅「关闭当前文件」可用
    for (int i = 0; i < 3; ++i)
        b.dm->closeDocument(b.dm->documents().last());
    b.settle();
    QCOMPARE(b.dm->documents().size(), 1);
    checkEnablement(b, 0, true, false, false, false); // 单标签：右/左/其他禁用
}

// TC-003: 真实点击「关闭当前文件」（干净文档）：无确认框直接关闭。
// closeDocumentWithConfirm 无 uiLocked 检查——本用例同时是 tc005 的对照组：
// 若本用例通过而 tc005 失败，差异只能在 closeRangeWithConfirm 的 uiLocked。
void TestTabContextMenu::tc003_closeCurrent_clean_realClick()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    Document *d1 = b.newDoc();
    Document *d2 = b.newDoc();
    QVERIFY(d1 && d2);
    b.dm->setActiveDocument(d1);
    b.settle();

    QVERIFY2(b.openContextMenuAt(0), "open context menu failed");
    QTest::qWait(50);
    QVERIFY2(b.realClick(QStringLiteral("关闭当前文件")), "click item failed");
    b.settle();

    QCOMPARE(b.dm->documents().size(), 1);
    QCOMPARE(b.dm->documents().first(), d2);
    QVERIFY(!b.unsavedDialog() || !b.unsavedDialog()->property("visible").toBool());
}

// TC-005: T1..T5 干净，右键 T3 → 关闭右边所有：T4、T5 关闭，激活 T3。
void TestTabContextMenu::tc005_closeRight_realClick()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QList<Document *> docs;
    for (int i = 0; i < 5; ++i)
        docs.append(b.newDoc());
    QCOMPARE(b.dm->documents().size(), 5);

    QVERIFY2(b.openContextMenuAt(2), "open context menu failed");
    QTest::qWait(50);
    QVERIFY2(b.realClick(QStringLiteral("关闭右边所有")), "click item failed");
    b.settle();

    QCOMPARE(b.dm->documents().size(), 3); // T1 T2 T3 保留
    QCOMPARE(b.dm->documents().first(), docs.at(0));
    QCOMPARE(b.dm->documents().last(), docs.at(2));
    QCOMPARE(b.dm->activeDocument(), docs.at(2)); // 锚点激活
    QVERIFY(!b.tabContextMenu->property("visible").toBool()); // 菜单已关闭
}

// TC-010: T1..T4 干净，右键 T3 → 关闭左边所有：T1、T2 关闭，激活 T3。
void TestTabContextMenu::tc010_closeLeft_realClick()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QList<Document *> docs;
    for (int i = 0; i < 4; ++i)
        docs.append(b.newDoc());

    QVERIFY2(b.openContextMenuAt(2), "open context menu failed");
    QTest::qWait(50);
    QVERIFY2(b.realClick(QStringLiteral("关闭左边所有")), "click item failed");
    b.settle();

    QCOMPARE(b.dm->documents().size(), 2); // T3 T4 保留
    QCOMPARE(b.dm->documents().first(), docs.at(2));
    QCOMPARE(b.dm->documents().last(), docs.at(3));
    QCOMPARE(b.dm->activeDocument(), docs.at(2)); // 锚点激活
}

// TC-006: T1..T4，T1..T3 均有修改，右键 T4 → 关闭其他文件：
// 确认框按 T1 → T2 → T3 顺序弹出，全部「不保存」后仅剩 T4 且激活。
void TestTabContextMenu::tc006_closeOthers_discardAll_realClick()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QList<Document *> docs;
    for (int i = 0; i < 4; ++i)
        docs.append(b.newDoc(QStringLiteral("content-%1").arg(i)));
    QCOMPARE(b.dm->documents().size(), 4);

    QVERIFY2(b.openContextMenuAt(3), "open context menu failed");
    QTest::qWait(50);
    QVERIFY2(b.realClick(QStringLiteral("关闭其他文件")), "click item failed");

    QObject *dlg = b.unsavedDialog();
    QVERIFY(dlg != nullptr);

    // 从左到右逐个「不保存」：每步断言当前挂起文档与剩余数
    for (int i = 0; i < 3; ++i) {
        QTRY_VERIFY(dlg->property("visible").toBool());
        const QVariant flowDoc = b.win->property("closeFlowDoc");
        QVERIFY(flowDoc.canConvert<QObject *>());
        QCOMPARE(flowDoc.value<QObject *>(), static_cast<QObject *>(docs.at(i)));
        QVERIFY(QMetaObject::invokeMethod(dlg, "discarded")); // 不保存
        b.settle();
        QCOMPARE(b.dm->documents().size(), 3 - i); // 逐个减少
    }

    QVERIFY(!dlg->property("visible").toBool());
    QCOMPARE(b.dm->documents().size(), 1); // 仅剩 T4
    QCOMPARE(b.dm->documents().first(), docs.at(3));
    QCOMPARE(b.dm->activeDocument(), docs.at(3)); // 锚点激活
}

// TC-007: 同 TC-006 前置，第一个确认框「取消」：无文档被关闭、
// 剩余全部保留、不再弹框。
void TestTabContextMenu::tc007_closeOthers_cancelAborts_realClick()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QList<Document *> docs;
    for (int i = 0; i < 4; ++i)
        docs.append(b.newDoc(QStringLiteral("content-%1").arg(i)));

    QVERIFY2(b.openContextMenuAt(3), "open context menu failed");
    QTest::qWait(50);
    QVERIFY2(b.realClick(QStringLiteral("关闭其他文件")), "click item failed");

    QObject *dlg = b.unsavedDialog();
    QVERIFY(dlg != nullptr);
    QTRY_VERIFY(dlg->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(dlg, "reject")); // 取消
    b.settle();

    QCOMPARE(b.dm->documents().size(), 4);          // 一个都没关
    QVERIFY(!dlg->property("visible").toBool());    // 对话框关闭
    QCOMPARE(b.dm->activeDocument(), docs.at(3));   // 仍是原活动文档
    for (Document *d : docs)
        QVERIFY(b.dm->documents().contains(d));     // 全部保留
}

// 对照组：菜单未打开时直发「关闭右边所有」triggered()（tst_searchmenu
// 同模式）。与 tc005（真实点击）的差值即 uiLocked 时序问题的直接证据。
void TestTabContextMenu::ctrl_closeRight_directSignal()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QList<Document *> docs;
    for (int i = 0; i < 5; ++i)
        docs.append(b.newDoc());
    QCOMPARE(b.dm->documents().size(), 5);

    // 菜单未打开（contextIndex 已设置，visible = false）
    QVERIFY(b.documentTabs->setProperty("contextIndex", 2));
    QVERIFY(!b.tabContextMenu->property("visible").toBool());
    QVERIFY2(b.triggerDirect(QStringLiteral("关闭右边所有")),
             "direct trigger failed");
    b.settle();

    QCOMPARE(b.dm->documents().size(), 3);
    QCOMPARE(b.dm->activeDocument(), docs.at(2));
}

int main(int argc, char *argv[])
{
    // GUI 红线：自动化验证一律 offscreen，全程无可见窗口
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // 隔离首启右键菜单提示：避免真实注册表读写与首启弹框干扰用例
    qputenv("QCNOTE_TEST_SHELL_NOOP", "1");
    // offscreen 平台不带字体目录时指定系统字体，保证文本渲染路径正常
    static const char kWinFonts[] = "C:/Windows/Fonts";
    if (QDir(QString::fromLatin1(kWinFonts)).exists())
        qputenv("QT_QPA_FONTDIR", kWinFonts);
    QGuiApplication app(argc, argv);
    // 会话重定向到临时目录（必须在任何 QSettings 实例创建前 setPath），
    // 避免污染真实用户配置 %APPDATA%/qcnote/qcnote.ini
    sessionRedirectDir();
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       sessionRedirectDir().path());
    // 草稿缓存隔离（与 tst_searchmenu 同一模式）：目录按测试 exe 名派生，
    // 启动预清理上一轮残留，结束清理本轮产物
    const QString draftsDir = QStandardPaths::writableLocation(
                QStandardPaths::AppLocalDataLocation) + QStringLiteral("/drafts");
    QDir(draftsDir).removeRecursively();
    // 窗口关闭（visible = false）不得触发自动退出，避免干扰 QTest 事件循环
    app.setQuitOnLastWindowClosed(false);
    // 与主程序一致的控件样式（Main.qml 使用 Material 附加属性）
    QQuickStyle::setStyle(QStringLiteral("Material"));
    TestTabContextMenu t;
    const int testResult = QTest::qExec(&t, argc, argv);
    QDir(draftsDir).removeRecursively();
    return testResult;
}

#include "tst_tabcontextmenu.moc"
