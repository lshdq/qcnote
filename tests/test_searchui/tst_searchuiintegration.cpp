// 正则搜索与替换 QML 集成测试（offscreen）：
// - 控制器层 TC-101 至 TC-103（真实 SearchController + Main.qml 全链路，
//   补足 test_core/tst_searchengine 的引擎等效覆盖——此处控制器真实实例化，
//   target/textDocument 绑定 EditorArea 活动编辑器）
// - 人工交互用例 TC-201 至 TC-206 的可自动化部分（对话框生命周期、
//   高亮层数据、预填与状态保留、错误提示与置灰、多光标互斥与 uiLocked、
//   回归：全部替换单步撤销 + 关闭后多光标编辑）
//
// 测试方式：offscreen 平台下实例化完整 Main.qml（ApplicationWindow +
// EditorArea + SearchDialog + SearchController + SearchHighlightLayer 完整
// 接线），经 QMetaObject 调用 Main 的 QML function（doFind/uiLocked/
// searchJumpGlobal 等）与合成键盘事件（Enter/Shift+Enter/Esc/Ctrl+Z/
// Ctrl+D）驱动与人工交互等价的链路。
//
// 与 tests/test_core 的引擎单测互补：这里验证 QML 集成层（对话框交互、
// 预填逻辑、uiLocked 聚合、高亮层段数据、撤销链路、多光标互斥）。
//
// GUI 红线：main 中强制 QT_QPA_PLATFORM=offscreen，全程无可见窗口。
// 无法自动化的人工项（高亮渲染视觉效果、描边观感、滚动跟随、DPI）见
// 测试报告“人工待验”清单。

#include <QDir>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRectF>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>
// qpa 公共接口（Qt 安装的版本化 include 路径）：走窗口系统真实按键链路，
// 使 QML Shortcut 的 ShortcutOverride→激活路径生效；普通 include 路径
// 无此头，只能用版本化全路径
#include <QtGui/6.11.2/QtGui/qpa/qwindowsysteminterface.h>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>

#include "document/document.h"
#include "document/documentmanager.h"
#include "editor/multicursorcontroller.h"
#include "editor/searchcontroller.h"
#include "editor/searchengine.h"

// Document/DocumentManager 为全局命名空间类；editor 下类在 qcnote 内
using qcnote::MultiCursorController;
using qcnote::SearchController;
using qcnote::SearchEngine;

// ==================== 测试台辅助 ====================

namespace {

// 进程级会话重定向目录：main 中 setPath 前创建，进程退出时随 QTemporaryDir
// 清理（与 tst_dialogstyle.cpp 同一模式，避免污染真实用户配置）
QTemporaryDir &sessionRedirectDir()
{
    static QTemporaryDir dir;
    return dir;
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

// 类名包含匹配（QML 动态类型带 _QMLTYPE_N 后缀，统一包含匹配）
bool classNameMatches(const QObject *obj, const char *needle)
{
    const QString name = QString::fromLatin1(obj->metaObject()->className());
    return name.contains(QLatin1String(needle));
}

// 深度遍历 QObject 子孙（含非可视对象）
template <typename F>
void visitObjects(QObject *root, const F &fn)
{
    if (!root)
        return;
    fn(root);
    const auto kids = root->children();
    for (QObject *kid : kids)
        visitObjects(kid, fn);
}

// 按标题定位 Dialog（Main.qml 内弹窗经 MovableDialog 接入：标题在
// dialogTitle 属性，Dialog.title 保持空；SearchDialog 实例类名含
// "SearchDialog"，同样满足包含匹配）
QObject *findDialogByTitle(QQuickWindow *win, const QString &title)
{
    QObject *found = nullptr;
    visitObjects(win, [&](QObject *o) {
        if (found || !classNameMatches(o, "Dialog"))
            return;
        // QQuickDialog 自身或 QML Dialog 包装均可：按 dialogTitle 属性匹配
        if (o->property("dialogTitle").toString() == title)
            found = o;
    });
    return found;
}

/**
 * @brief 每个测试用例独立的测试台：完整实例化 Main.qml。
 *
 * DocumentManager 为 QML 单例且跨引擎复用同一 C++ 实例，因此每用例
 * 开始时清空残余文档，保证前置状态干净（与 tst_dialogstyle 同模式）。
 */
struct Bench
{
    QQmlEngine engine;
    QQuickWindow *win = nullptr;        // Main.qml 根（ApplicationWindow）
    DocumentManager *dm = nullptr;      // QML 单例的同一 C++ 实例
    QQuickItem *editorArea = nullptr;   // EditorArea（查找集成状态持有者）
    QObject *searchDlg = nullptr;       // SearchDialog（标题「查找与替换」）

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

        visitItems(win->contentItem(), [&](QQuickItem *it) {
            if (!editorArea && classNameMatches(it, "EditorArea"))
                editorArea = it;
        });
        if (!editorArea)
            return false;

        searchDlg = findDialogByTitle(win, QStringLiteral("查找与替换"));
        if (!searchDlg)
            return false;
        return true;
    }

    // 查找控制器（EditorArea 会话级单实例，readonly alias）
    SearchController *ctl() const
    {
        return qobject_cast<SearchController *>(
                    editorArea->property("searchController").value<QObject *>());
    }

    // 活动编辑器（TextArea，随活动文档变化，动态读取）
    QQuickItem *activeEditor() const
    {
        return editorArea->property("activeEditor").value<QQuickItem *>();
    }

    // 活动编辑器的多光标控制器
    MultiCursorController *activeMc() const
    {
        return qobject_cast<MultiCursorController *>(
                    editorArea->property("activeMc").value<QObject *>());
    }

    // TextArea.positionToRectangle（Q_INVOKABLE；QQuickTextEdit 为 Qt 私有
    // 头禁用，经 QMetaObject 按名称调用——与 tst_editorintegration 的
    // rectOf 同模式）：文档位置 → TextArea 本地坐标光标矩形
    QRectF rectOf(QQuickItem *editor, int pos) const
    {
        QRectF ret;
        QMetaObject::invokeMethod(editor, "positionToRectangle",
                                  Q_RETURN_ARG(QRectF, ret), Q_ARG(int, pos));
        return ret;
    }

    // 新建文档并写入初始内容（setContent 会置 modified = true）
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

    void settle() { QTest::qWait(30); }

    // 合成键盘事件直发窗口（QQuickWindow 分发至 activeFocusItem，
    // 与真实按键链路一致；与 tst_editorintegration 的 sendKey 同模式）
    void sendKey(int key, Qt::KeyboardModifiers mods, const QString &text = QString())
    {
        QKeyEvent press(QEvent::KeyPress, key, mods, text);
        QCoreApplication::sendEvent(win, &press);
        QKeyEvent release(QEvent::KeyRelease, key, mods, text);
        QCoreApplication::sendEvent(win, &release);
        settle();
    }

    void keyReturn(bool shift = false)
    {
        sendKey(Qt::Key_Return,
                shift ? Qt::KeyboardModifiers(Qt::ShiftModifier)
                      : Qt::KeyboardModifiers(Qt::NoModifier));
    }

    void esc() { sendKey(Qt::Key_Escape, Qt::NoModifier); }

    // 合成鼠标点击（press + release 直投窗口，走 QQuickPopup/控件的真实
    // 派发路径；复选框等控件的状态信号（toggled 等）只在交互路径发射，
    // 程序化 toggle()/setProperty 不触发 QML onToggled 处理器）
    void clickItem(QQuickItem *item)
    {
        const QPointF center = item->mapToScene(
                    QPointF(item->width() / 2.0, item->height() / 2.0));
        const QPointF global = QPointF(win->mapToGlobal(center.toPoint()));
        QMouseEvent press(QEvent::MouseButtonPress, center, global,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, center, global,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(win, &press);
        QCoreApplication::sendEvent(win, &release);
        settle();
    }

    // 经元对象调用 Main 的 QML function（无返回值）
    bool invokeMain(const char *name, const QVariant &arg1 = QVariant())
    {
        if (arg1.isValid())
            return QMetaObject::invokeMethod(win, name, Q_ARG(QVariant, arg1));
        return QMetaObject::invokeMethod(win, name);
    }

    // 调用返回 bool 的 Main QML function（uiLocked）
    bool mainBool(const char *name)
    {
        QVariant ret;
        if (!QMetaObject::invokeMethod(win, name, Q_RETURN_ARG(QVariant, ret)))
            return false;
        return ret.toBool();
    }

    // 调用对话框 QML function（doFindNext / doReplaceCurrent / doReplaceAll）
    bool invokeDialog(const char *name, QVariant *ret = nullptr)
    {
        if (ret)
            return QMetaObject::invokeMethod(searchDlg, name,
                                             Q_RETURN_ARG(QVariant, *ret));
        return QMetaObject::invokeMethod(searchDlg, name);
    }

    // ---- 对话框内部控件定位 ----

    // 查找/替换输入框（contentItem 子树中的 TextField，按场景 y 序）
    QList<QQuickItem *> textFields()
    {
        QList<QQuickItem *> out;
        QObject *ci = searchDlg->property("contentItem").value<QObject *>();
        QQuickItem *col = qobject_cast<QQuickItem *>(ci);
        if (!col)
            return out;
        visitItems(col, [&](QQuickItem *it) {
            if (classNameMatches(it, "TextField"))
                out.append(it);
        });
        std::sort(out.begin(), out.end(), [](QQuickItem *a, QQuickItem *b) {
            return a->mapToScene(QPointF()).y() < b->mapToScene(QPointF()).y();
        });
        return out;
    }

    QQuickItem *findField() { return textFields().value(0); }
    QQuickItem *replaceField() { return textFields().value(1); }

    // 按文本定位复选框（「全字匹配」等）
    QQuickItem *checkBox(const QString &text)
    {
        QQuickItem *found = nullptr;
        QObject *ci = searchDlg->property("contentItem").value<QObject *>();
        QQuickItem *col = qobject_cast<QQuickItem *>(ci);
        if (!col)
            return nullptr;
        visitItems(col, [&](QQuickItem *it) {
            if (!found && classNameMatches(it, "CheckBox")
                    && it->property("text").toString() == text)
                found = it;
        });
        return found;
    }

    // 状态行 Label：contentItem（ColumnLayout）的直接子项中唯一的
    // Text/Label 类型项（另三个直接子项是 RowLayout；行首「查找：」「替换：」
    // 与复选框文本都在 RowLayout 子树内，不在此列）
    QQuickItem *statusLabel()
    {
        QObject *ci = searchDlg->property("contentItem").value<QObject *>();
        QQuickItem *col = qobject_cast<QQuickItem *>(ci);
        if (!col)
            return nullptr;
        for (QQuickItem *child : col->childItems()) {
            if (classNameMatches(child, "Text") || classNameMatches(child, "Label"))
                return child;
        }
        return nullptr;
    }

    QString statusText()
    {
        QQuickItem *label = statusLabel();
        return label ? label->property("text").toString() : QString();
    }

    // footer 按钮盒中按文本取按钮（三个动作按钮 + 关闭）
    QQuickItem *button(const QString &text)
    {
        QObject *box = searchDlg->property("footer").value<QObject *>();
        if (!box)
            return nullptr;
        QQuickItem *found = nullptr;
        visitObjects(box, [&](QObject *o) {
            QQuickItem *it = qobject_cast<QQuickItem *>(o);
            if (!found && it && classNameMatches(it, "DialogButton")
                    && it->property("text").toString() == text)
                found = it;
        });
        return found;
    }

    // 活动编辑器的查找高亮层（TextArea 子树内）
    QQuickItem *highlightLayer()
    {
        QQuickItem *found = nullptr;
        QQuickItem *ed = activeEditor();
        if (!ed)
            return nullptr;
        visitItems(ed, [&](QQuickItem *it) {
            if (!found && classNameMatches(it, "SearchHighlightLayer"))
                found = it;
        });
        return found;
    }

    // ---- 多光标选区数据（控制器状态；接管期原生选区被清空，见
    // EditorArea 注释，与 tst_editorintegration 的 selectionPairs 同兼容解析） ----
    // QVariantList 经 QML 边界可能展平为 [s1,e1,s2,e2,...]，兼容嵌套形态
    static QStringList mcSelectionPairs(MultiCursorController *mc)
    {
        QStringList out;
        const QVariantList raw = mc->selectionRanges();
        if (raw.isEmpty())
            return out;
        if (raw.at(0).canConvert<QVariantList>()) {
            for (const QVariant &v : raw) {
                const QVariantList pair = v.toList();
                if (pair.size() == 2)
                    out << QStringLiteral("%1:%2")
                               .arg(pair.at(0).toInt()).arg(pair.at(1).toInt());
            }
        } else {
            for (int i = 0; i + 1 < raw.size(); i += 2)
                out << QStringLiteral("%1:%2")
                           .arg(raw.at(i).toInt()).arg(raw.at(i + 1).toInt());
        }
        return out;
    }

    // 活动编辑器文本 / 选区便捷读取
    QString editorText() const
    {
        QQuickItem *ed = activeEditor();
        return ed ? ed->property("text").toString() : QString();
    }
    int selStart() const { return activeEditor()->property("selectionStart").toInt(); }
    int selEnd() const { return activeEditor()->property("selectionEnd").toInt(); }
};

} // namespace

// ==================== 测试类 ====================

class TestSearchUiIntegration : public QObject
{
    Q_OBJECT

private slots:
    // ---- 控制器层（真实 SearchController + 完整 Main 接线） ----

    // TC-101：全部替换单次撤销（replaceAll → 编辑器 Ctrl+Z 一步完整回滚）
    void tc101_replaceAll_singleUndo();

    // TC-102：替换当前后跳下一个（连续替换直至清空，环绕定位）
    void tc102_replaceCurrent_thenNext();

    // TC-103：内容变更后重算（外部 setContent → 匹配重算、索引钳制）
    void tc103_recomputeAfterContentChange();

    // ---- 人工交互用例的可自动化部分 ----

    // TC-201：Ctrl+F/Enter/Shift+Enter/Esc 全链路（顶部对齐、计数文本、
    // 环绕跳转、关闭后高亮清除、关闭态 F3 跳转）
    void tc201_dialog_lifecycle();

    // TC-201a：对话框已打开时重复 Ctrl+F → 聚焦查找框并全选、不重置状态
    void tc201a_repeatCtrlF_refocus();

    // TC-201b：关闭按钮路径（非 Esc）→ 关闭、高亮隐藏、焦点回编辑器
    void tc201b_closeButton_path();

    // TC-201c：对话框打开态按真实 F3 → 转发查找下一个（与 Enter 等效）
    void tc201c_f3_whileOpen();
    void tc201d_f3_onFooterButton();

    // TC-202：高亮层段数据（全部匹配逐段、跨可视行拆段、当前匹配段、
    // 溢出时停止绘制）；视觉观感部分人工待验
    void tc202_highlight_layer();

    // TC-203：预填与状态保留（无换行选区预填并全选、含换行不预填、
    // 查找串与选项会话内保留）
    void tc203_prefill_and_retention();

    // TC-204：无效正则错误文本（非仅颜色线索）与按钮禁用；
    // 正则开启时全字开关禁用置灰
    void tc204_invalidRegex_and_wholeWordDisabled();

    // TC-205：多光标互斥（打开即退出）与 uiLocked（Ctrl+Tab 被阻止）
    void tc205_multicursorMutex_and_uiLocked();

    // TC-206：回归——全部替换后 Ctrl+Z 一步回滚且 modified 正确；
    // 关闭对话框后 Ctrl+D 多光标编辑零回归
    void tc206_regression_undoAndMultiCursor();
};

// ==================== 控制器层 ====================

// TC-101：经对话框「全部替换」→ 编辑器一次 Ctrl+Z 完整回滚（真实键盘
// 路径：关闭对话框回焦编辑器 → 窗口级 Ctrl+Z → TextArea 原生 undo）
void TestSearchUiIntegration::tc101_replaceAll_singleUndo()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("foo bar foo baz"));
    QVERIFY(b.ctl() != nullptr);

    // 打开对话框并驱动全部替换（真实链路：输入框 → 控制器 → doReplaceAll）
    QVERIFY(b.invokeMain("doFind"));
    QTRY_VERIFY(b.searchDlg->property("visible").toBool());
    b.findField()->setProperty("text", QStringLiteral("foo"));
    b.replaceField()->setProperty("text", QStringLiteral("X"));
    b.settle();
    QCOMPARE(b.ctl()->matchCount(), 2);

    QVariant replaced;
    QVERIFY(b.invokeDialog("doReplaceAll", &replaced));
    // doReplaceAll 为 QML function 无返回值（undefined），处数记录在
    // lastReplacedCount 属性（状态行「已替换 N 处」的数据源）
    QCOMPARE(b.searchDlg->property("lastReplacedCount").toInt(), 2);
    QCOMPARE(b.editorText(), QStringLiteral("X bar X baz"));
    // 「已替换 2 处」保留到下一次查找/替换操作
    QCOMPARE(b.statusText(), QStringLiteral("已替换 2 处"));

    // Esc 关闭 → 焦点回编辑器 → 单次 Ctrl+Z 整批回滚
    b.esc();
    QTRY_VERIFY(!b.searchDlg->property("visible").toBool());
    QCOMPARE(b.editorArea->property("searchActive").toBool(), false);
    QVERIFY(b.activeEditor()->property("activeFocus").toBool());
    b.sendKey(Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(b.editorText(), QStringLiteral("foo bar foo baz"));
    // 替换改写文档：modified 为真
    QVERIFY(b.dm->activeDocument()->modified());
}

// TC-102：替换当前匹配后自动指向下一个；连续替换直至全部清空（环绕）
void TestSearchUiIntegration::tc102_replaceCurrent_thenNext()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("foo a foo b foo"));

    QVERIFY(b.invokeMain("doFind"));
    QTRY_VERIFY(b.searchDlg->property("visible").toBool());
    b.findField()->setProperty("text", QStringLiteral("foo"));
    b.replaceField()->setProperty("text", QStringLiteral("QUX"));
    b.settle();
    QCOMPARE(b.ctl()->matchCount(), 3);

    // 首跳定位第 1 个（[0,3)），再跳到第 2 个（[6,9)）
    QVERIFY(b.invokeDialog("doFindNext"));
    QCOMPARE(b.ctl()->currentIndex(), 0);
    QCOMPARE(b.selStart(), 0);
    QCOMPARE(b.selEnd(), 3);
    QVERIFY(b.invokeDialog("doFindNext"));
    QCOMPARE(b.ctl()->currentIndex(), 1);
    QCOMPARE(b.selStart(), 6);
    QCOMPARE(b.selEnd(), 9);

    // 替换第 2 个：文本更新、索引指向「替换文本之后」的下一个匹配 [12,15)
    QVERIFY(b.invokeDialog("doReplaceCurrent"));
    QCOMPARE(b.editorText(), QStringLiteral("foo a QUX b foo"));
    QCOMPARE(b.ctl()->matchCount(), 2);
    QCOMPARE(b.ctl()->currentIndex(), 1);
    QVariantList cur = b.ctl()->property("currentRange").toList();
    QCOMPARE(cur.size(), 2);
    QCOMPARE(cur.at(0).toInt(), 12);
    QCOMPARE(cur.at(1).toInt(), 15);

    // 连续替换剩余两处：替换 [12,15) 后环绕定位 [0,3)；再替换后清空
    QVERIFY(b.invokeDialog("doReplaceCurrent"));
    QCOMPARE(b.editorText(), QStringLiteral("foo a QUX b QUX"));
    QCOMPARE(b.ctl()->currentIndex(), 0);
    QVERIFY(b.invokeDialog("doReplaceCurrent"));
    QCOMPARE(b.editorText(), QStringLiteral("QUX a QUX b QUX"));
    QCOMPARE(b.ctl()->matchCount(), 0);
    QCOMPARE(b.ctl()->currentIndex(), -1);
    QCOMPARE(b.statusText(), QStringLiteral("无匹配"));
}

// TC-103：文档内容被外部重置（重新加载/编码重载同链路）→ 匹配按新内容
// 重算、当前索引钳制合法；重置为无匹配时索引归 -1
void TestSearchUiIntegration::tc103_recomputeAfterContentChange()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    Document *doc = b.newDoc(QStringLiteral("foo foo foo"));

    QVERIFY(b.invokeMain("doFind"));
    QTRY_VERIFY(b.searchDlg->property("visible").toBool());
    b.findField()->setProperty("text", QStringLiteral("foo"));
    b.settle();
    QCOMPARE(b.ctl()->matchCount(), 3);

    // 两次跳转后当前索引 2
    QVERIFY(b.invokeDialog("doFindNext"));
    QVERIFY(b.invokeDialog("doFindNext"));
    QVERIFY(b.invokeDialog("doFindNext"));
    QCOMPARE(b.ctl()->currentIndex(), 2);

    // 外部整体重置为 2 个匹配。经 EditorArea 的 doc.content → editor.text
    // 同步链，QTextDocument 收到两阶段 contentsChange（先清空后写入）：
    // 第一阶段重算后无匹配（索引钳制为 -1），第二阶段重算出 2 个匹配时
    // 「原本无当前匹配保持不变」的钳制语义使索引停在 -1（合法、无越界），
    // 对话框回到「共 2 个」未跳转态，下一次 Enter 从第 1 个重新定位——
    // 与引擎等效单测（单阶段重算钳到末位下标）的语义差异记录于测试报告。
    doc->setContent(QStringLiteral("foo foo"));
    b.settle();
    QCOMPARE(b.ctl()->matchCount(), 2);
    QCOMPARE(b.ctl()->currentIndex(), -1);
    QCOMPARE(b.statusText(), QStringLiteral("共 2 个"));

    // 重置后跳转正常：Enter 从第 1 个匹配开始
    QVERIFY(b.invokeDialog("doFindNext"));
    QCOMPARE(b.ctl()->currentIndex(), 0);
    QCOMPARE(b.selStart(), 0);
    QCOMPARE(b.selEnd(), 3);

    // 再重置为无匹配：索引 -1、状态「无匹配」
    doc->setContent(QStringLiteral("bar baz"));
    b.settle();
    QCOMPARE(b.ctl()->matchCount(), 0);
    QCOMPARE(b.ctl()->currentIndex(), -1);
    QCOMPARE(b.statusText(), QStringLiteral("无匹配"));
}

// ==================== 人工交互用例的可自动化部分 ====================

// TC-201：Ctrl+F 打开（顶部对齐不遮挡编辑区）、实时计数、Enter/Shift+Enter
// 环绕跳转、Esc 关闭清除高亮、关闭态 F3 沿用上次参数继续跳转（DEC-009：
// 不恢复全部高亮）
void TestSearchUiIntegration::tc201_dialog_lifecycle()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("hello world hello"));

    // Ctrl+F（Main.doFind 经窗口 Shortcut 触发的同一函数）
    QVERIFY(b.invokeMain("doFind"));
    QTRY_VERIFY(b.searchDlg->property("visible").toBool());
    QVERIFY(b.editorArea->property("searchActive").toBool());

    // 模态：进入 uiLocked
    QVERIFY(b.mainBool("uiLocked"));

    // 顶部对齐（DEC-002）：y == 96（标题栏+工具栏下方），远小于垂直居中位
    QTest::qWait(50); // 等布局 polish
    const qreal dlgY = b.searchDlg->property("y").toReal();
    const qreal dlgH = b.searchDlg->property("height").toReal();
    const qreal centeredY = (b.win->height() - dlgH) / 2.0;
    QVERIFY2(dlgY < centeredY - 50,
             qPrintable(QStringLiteral("y=%1 must be top-aligned (centered would be %2)")
                            .arg(dlgY).arg(centeredY)));

    // 查找框聚焦；输入即实时查找与计数
    QVERIFY(b.findField()->property("activeFocus").toBool());
    b.findField()->setProperty("text", QStringLiteral("hello"));
    b.settle();
    QCOMPARE(b.ctl()->matchCount(), 2);
    QCOMPARE(b.statusText(), QStringLiteral("第 1 个 / 共 2 个"));

    // Enter：首跳定位当前匹配（原生选区，DEC-007）
    b.keyReturn();
    QCOMPARE(b.ctl()->currentIndex(), 0);
    QCOMPARE(b.selStart(), 0);
    QCOMPARE(b.selEnd(), 5);
    QCOMPARE(b.statusText(), QStringLiteral("第 1 个 / 共 2 个"));

    // Enter：第 2 个；再 Enter：环绕回第 1 个
    b.keyReturn();
    QCOMPARE(b.ctl()->currentIndex(), 1);
    QCOMPARE(b.selStart(), 12);
    QCOMPARE(b.selEnd(), 17);
    b.keyReturn();
    QCOMPARE(b.ctl()->currentIndex(), 0);

    // Shift+Enter：反向回第 2 个
    b.keyReturn(true);
    QCOMPARE(b.ctl()->currentIndex(), 1);

    // 高亮层打开期间可见
    QQuickItem *layer = b.highlightLayer();
    QVERIFY(layer != nullptr);
    QVERIFY(layer->property("visible").toBool());

    // Esc：关闭、高亮清除（searchActive=false）、退出 uiLocked
    b.esc();
    QTRY_VERIFY(!b.searchDlg->property("visible").toBool());
    QCOMPARE(b.editorArea->property("searchActive").toBool(), false);
    QVERIFY(layer->property("visible").toBool() == false);
    QVERIFY(!b.mainBool("uiLocked"));

    // 关闭态 F3（searchJumpGlobal）：沿用上次参数继续跳转，不恢复高亮。
    // 当前索引 1（[12,17)），F3 前进环绕回索引 0（[0,5)）
    QVERIFY(b.invokeMain("searchJumpGlobal", QVariant(true)));
    QCOMPARE(b.ctl()->currentIndex(), 0); // 从 1 环绕回 0
    QCOMPARE(b.selStart(), 0);
    QCOMPARE(b.selEnd(), 5);
    QCOMPARE(b.editorArea->property("searchActive").toBool(), false);
    QVERIFY(layer->property("visible").toBool() == false);
}

// TC-201a：对话框已打开时重复 Ctrl+F：不重开、不重置匹配状态，改为聚焦
// 查找框并全选（便于直接重新输入）——openSearchDialog 的已打开分支
void TestSearchUiIntegration::tc201a_repeatCtrlF_refocus()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("hello world hello"));

    QVERIFY(b.invokeMain("doFind"));
    QTRY_VERIFY(b.searchDlg->property("visible").toBool());
    b.findField()->setProperty("text", QStringLiteral("hello"));
    b.settle();
    QCOMPARE(b.ctl()->matchCount(), 2);

    // 跳转一次后当前索引 0；焦点移到替换框（模拟用户点过选项/替换框）
    QVERIFY(b.invokeDialog("doFindNext"));
    QCOMPARE(b.ctl()->currentIndex(), 0);
    // 焦点移到替换框（模拟用户点过替换框/选项）
    b.replaceField()->setProperty("text", QStringLiteral("X"));
    QVERIFY(QMetaObject::invokeMethod(b.replaceField(), "forceActiveFocus"));
    b.settle();

    // 重复 Ctrl+F：对话框保持打开、查找框聚焦并全选、匹配状态不重置
    QVERIFY(b.invokeMain("doFind"));
    QVERIFY(b.searchDlg->property("visible").toBool());
    QVERIFY2(b.findField()->property("activeFocus").toBool(),
             "repeat Ctrl+F must refocus the find field");
    const QString findText = b.findField()->property("text").toString();
    QCOMPARE(b.findField()->property("selectionStart").toInt(), 0);
    QCOMPARE(b.findField()->property("selectionEnd").toInt(), findText.size());
    QCOMPARE(b.ctl()->matchCount(), 2);
    QCOMPARE(b.ctl()->currentIndex(), 0);
}

// TC-201b：关闭按钮路径（RejectRole 与 Esc 汇入同一 rejected 信号）：
// 关闭、高亮隐藏（searchActive=false）、焦点回编辑器
void TestSearchUiIntegration::tc201b_closeButton_path()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("hello world hello"));

    QVERIFY(b.invokeMain("doFind"));
    QTRY_VERIFY(b.searchDlg->property("visible").toBool());
    b.findField()->setProperty("text", QStringLiteral("hello"));
    b.settle();
    QVERIFY(b.invokeDialog("doFindNext"));
    QCOMPARE(b.ctl()->currentIndex(), 0);

    QQuickItem *layer = b.highlightLayer();
    QVERIFY(layer != nullptr);
    QVERIFY(layer->property("visible").toBool());

    // 点「关闭」按钮（真实鼠标事件，非 Esc）
    QQuickItem *closeBtn = b.button(QStringLiteral("关闭"));
    QVERIFY2(closeBtn != nullptr, "close button not found");
    b.clickItem(closeBtn);
    QTRY_VERIFY(!b.searchDlg->property("visible").toBool());
    QCOMPARE(b.editorArea->property("searchActive").toBool(), false);
    QVERIFY2(layer->property("visible").toBool() == false,
             "highlight must hide after close button");
    QVERIFY2(b.activeEditor()->property("activeFocus").toBool(),
             "focus must return to editor after close button");
}

// TC-201c：对话框打开态按真实 F3 键：窗口级 Shortcut 转发为查找下一个，
// 与输入框 Enter 等效（单次执行，不重复跳转）
void TestSearchUiIntegration::tc201c_f3_whileOpen()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("hello world hello"));

    QVERIFY(b.invokeMain("doFind"));
    QTRY_VERIFY(b.searchDlg->property("visible").toBool());
    b.findField()->setProperty("text", QStringLiteral("hello"));
    b.settle();
    QCOMPARE(b.ctl()->matchCount(), 2);

    // 首跳定位第 1 个（[0,5)）
    QVERIFY(b.invokeDialog("doFindNext"));
    QCOMPARE(b.ctl()->currentIndex(), 0);
    QCOMPARE(b.selStart(), 0);
    QCOMPARE(b.selEnd(), 5);

    // 真实 F3：前进到第 2 个（[12,17)）——恰好一次，不重复执行。
    // 经 QWindowSystemInterface 注入窗口系统按键（与真实键入同链路）；
    // 对话框打开期间窗口级 Shortcut 不激活（模态弹层重定向键派发），
    // 由 SearchDialog 内容区的 Keys 转发执行，语义与输入框 Enter 一致
    QWindowSystemInterface::handleKeyEvent(b.win, QEvent::KeyPress,
                                           Qt::Key_F3, Qt::NoModifier);
    QWindowSystemInterface::handleKeyEvent(b.win, QEvent::KeyRelease,
                                           Qt::Key_F3, Qt::NoModifier);
    b.settle();
    QTest::qWait(50); // 等转发与跳转链路完成
    QCOMPARE(b.ctl()->currentIndex(), 1);
    QCOMPARE(b.selStart(), 12);
    QCOMPARE(b.selEnd(), 17);
    QCOMPARE(b.searchDlg->property("visible").toBool(), true); // 对话框保持打开

    // Shift+F3：后退一步（索引 1 → 0），与 forwardSearchKey 的后退分支一致
    QWindowSystemInterface::handleKeyEvent(b.win, QEvent::KeyPress,
                                            Qt::Key_F3, Qt::ShiftModifier);
    QWindowSystemInterface::handleKeyEvent(b.win, QEvent::KeyRelease,
                                            Qt::Key_F3, Qt::ShiftModifier);
    b.settle();
    QTest::qWait(50);
    QCOMPARE(b.ctl()->currentIndex(), 0);
    QCOMPARE(b.selStart(), 0);
    QCOMPARE(b.selEnd(), 5);
    QCOMPARE(b.searchDlg->property("visible").toBool(), true);
}

// TC-201d：焦点落在 footer 按钮上时按 F3 仍前进（DEC-013 补 footer 焦点
// 路径：footer 的 DialogButtonBox 不在 contentItem 子树内，键事件父链为
// 按钮→DialogButtonBox→弹层面板，需 footer 侧同款转发）；Ctrl+F3 被修饰
// 键过滤不触发跳转
void TestSearchUiIntegration::tc201d_f3_onFooterButton()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("hello world hello"));

    QVERIFY(b.invokeMain("doFind"));
    QTRY_VERIFY(b.searchDlg->property("visible").toBool());
    b.findField()->setProperty("text", QStringLiteral("hello"));
    b.settle();
    QCOMPARE(b.ctl()->matchCount(), 2);

    // 首跳定位第 1 个（[0,5)）
    QVERIFY(b.invokeDialog("doFindNext"));
    QCOMPARE(b.ctl()->currentIndex(), 0);
    QCOMPARE(b.selStart(), 0);
    QCOMPARE(b.selEnd(), 5);

    // 真实点击 footer「查找下一个」按钮：按钮执行一次前进（索引 1），
    // 且焦点落在按钮上（DialogButton 默认 StrongFocus）
    QQuickItem *nextBtn = b.button(QStringLiteral("查找下一个"));
    QVERIFY2(nextBtn != nullptr, "find-next button not found");
    b.clickItem(nextBtn);
    QCOMPARE(b.ctl()->currentIndex(), 1);
    QVERIFY2(nextBtn->property("activeFocus").toBool(),
             "focus must be on the footer button after click");

    // 焦点在 footer 按钮上按真实 F3：再前进恰一次（环绕回索引 0），
    // 对话框保持打开
    QWindowSystemInterface::handleKeyEvent(b.win, QEvent::KeyPress,
                                           Qt::Key_F3, Qt::NoModifier);
    QWindowSystemInterface::handleKeyEvent(b.win, QEvent::KeyRelease,
                                           Qt::Key_F3, Qt::NoModifier);
    b.settle();
    QTest::qWait(50); // 等转发与跳转链路完成
    QCOMPARE(b.ctl()->currentIndex(), 0); // 从 1 环绕回 0
    QCOMPARE(b.selStart(), 0);
    QCOMPARE(b.selEnd(), 5);
    QCOMPARE(b.searchDlg->property("visible").toBool(), true);

    // Ctrl+F3：修饰键过滤不转发，索引保持不变（KeypadModifier 容忍，
    // Ctrl/Alt/Meta 组合不触发跳转）
    QWindowSystemInterface::handleKeyEvent(b.win, QEvent::KeyPress,
                                           Qt::Key_F3, Qt::ControlModifier);
    QWindowSystemInterface::handleKeyEvent(b.win, QEvent::KeyRelease,
                                           Qt::Key_F3, Qt::ControlModifier);
    b.settle();
    QTest::qWait(50);
    QCOMPARE(b.ctl()->currentIndex(), 0);
}


// TC-202（可自动化部分）：全部匹配高亮逐段（每匹配每可视行一段）、跨
// 可视行匹配拆多段、当前匹配有独立段数据（描边数据源）、关闭换行回到
// 单段；匹配数 >10000（DEC-004）时高亮停止绘制。像素级视觉观感人工待验。
void TestSearchUiIntegration::tc202_highlight_layer()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    // 600 个 a + 行尾无 a 的短词：长匹配在窄视口 wrap 下跨多个可视行，
    // 且 a+ 仅命中连续 a 段（尾部词不含 a，避免产生第二个匹配）
    b.newDoc(QString(600, QLatin1Char('a')) + QStringLiteral("\nxyz"));

    QVERIFY(b.invokeMain("doFind"));
    QTRY_VERIFY(b.searchDlg->property("visible").toBool());

    // 正则模式匹配整段 a+（单匹配跨可视行）；显式开 wrap 解耦用户配置
    b.findField()->setProperty("text", QStringLiteral("a+"));
    b.ctl()->setProperty("useRegex", true);
    b.settle();
    QCOMPARE(b.ctl()->matchCount(), 1);
    b.activeEditor()->setProperty("wrapMode", 4); // WrapAtWordBoundaryOrAnywhere
    b.settle();

    QQuickItem *layer = b.highlightLayer();
    QVERIFY2(layer, "SearchHighlightLayer not found");
    QVERIFY(layer->property("visible").toBool());

    // 全部匹配段：600 个 a 在 ~1000px 视口折成多行 → 拆成多段，
    // 每段 w>0、y 严格递增（逐可视行拆段）
    const QVariantList segs = layer->property("matchSegments").toList();
    QVERIFY2(segs.size() >= 2,
             qPrintable(QStringLiteral("wrapped match must split into 2+ segments, got %1")
                            .arg(segs.size())));
    qreal prevY = -1;
    for (int i = 0; i < segs.size(); ++i) {
        const QVariantMap seg = segs.at(i).toMap();
        QVERIFY2(seg.value("w").toDouble() > 0,
                 qPrintable(QStringLiteral("segment %1 width must be positive").arg(i)));
        QVERIFY(seg.value("h").toDouble() > 0);
        QVERIFY2(seg.value("y").toDouble() > prevY,
                 qPrintable(QStringLiteral("segment %1 y must strictly increase").arg(i)));
        prevY = seg.value("y").toDouble();
    }

    // 跳转后当前匹配有独立段数据（描边矩形的模型）
    QVERIFY(b.invokeDialog("doFindNext"));
    const QVariantList cur = layer->property("currentSegments").toList();
    QCOMPARE(cur.size(), segs.size()); // 同一跨行匹配拆同数段

    // 关闭换行（NoWrap）：同一匹配回到单段
    b.activeEditor()->setProperty("wrapMode", 0);
    b.settle();
    QCOMPARE(layer->property("matchSegments").toList().size(), 1);

    // 溢出防护（DEC-004）：>10000 匹配 → 高亮层停止绘制（matchRanges 传空）
    b.activeEditor()->setProperty("wrapMode", 4);
    Document *doc = b.dm->activeDocument();
    doc->setContent(QString(10001, QLatin1Char('a')));
    b.settle();
    b.ctl()->setProperty("useRegex", false);
    b.findField()->setProperty("text", QStringLiteral("a"));
    b.settle();
    QCOMPARE(b.ctl()->matchCount(), SearchEngine::kMaxMatchCount);
    QVERIFY(b.ctl()->overflowed());
    QVERIFY(layer->property("matchSegments").toList().isEmpty());
    QCOMPARE(b.statusText(), QStringLiteral(">10000"));
}

// TC-203：选区预填（不含换行 → 填入查找框并全选；含换行 → 不预填沿用
// 上次查找串）；查找串与选项开关会话内保留（重开对话框状态一致）
void TestSearchUiIntegration::tc203_prefill_and_retention()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("foo bar\nbaz foo"));

    // 选中不含换行的 "baz"（[8,11)）→ 打开即预填并全选
    QQuickItem *ed = b.activeEditor();
    QVERIFY(QMetaObject::invokeMethod(ed, "select", Q_ARG(int, 8), Q_ARG(int, 11)));
    b.settle();
    QVERIFY(b.invokeMain("doFind"));
    QTRY_VERIFY(b.searchDlg->property("visible").toBool());
    QCOMPARE(b.ctl()->pattern(), QStringLiteral("baz"));
    QCOMPARE(b.findField()->property("text").toString(), QStringLiteral("baz"));
    QCOMPARE(b.findField()->property("selectionStart").toInt(), 0);
    QCOMPARE(b.findField()->property("selectionEnd").toInt(), 3);

    // 开启「区分大小写」后关闭（真实 Esc 路径；复选框经真实点击驱动，
    // onToggled → 控制器链路同步生效）
    QQuickItem *caseBox = b.checkBox(QStringLiteral("区分大小写"));
    QVERIFY(caseBox != nullptr);
    b.clickItem(caseBox);
    QCOMPARE(caseBox->property("checked").toBool(), true);
    QCOMPARE(b.ctl()->caseSensitive(), true);
    b.esc();
    QTRY_VERIFY(!b.searchDlg->property("visible").toBool());

    // 含换行选区（"bar\nbaz" [4,18)）→ 不预填，沿用上次查找串与选项
    QVERIFY(QMetaObject::invokeMethod(ed, "select", Q_ARG(int, 4), Q_ARG(int, 18)));
    b.settle();
    QVERIFY(b.invokeMain("doFind"));
    QTRY_VERIFY(b.searchDlg->property("visible").toBool());
    QCOMPARE(b.ctl()->pattern(), QStringLiteral("baz")); // 未被换行选区覆盖
    QCOMPARE(b.findField()->property("text").toString(), QStringLiteral("baz"));
    QCOMPARE(b.checkBox(QStringLiteral("区分大小写"))->property("checked").toBool(), true);
    // 大小写敏感下 "baz" 仍 1 处匹配
    QCOMPARE(b.ctl()->matchCount(), 1);
}

// TC-204：无效正则 → 错误文本展示（非仅颜色线索）+ 动作按钮禁用；
// 正则开启时「全字匹配」禁用置灰、关闭恢复
void TestSearchUiIntegration::tc204_invalidRegex_and_wholeWordDisabled()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("abc"));

    QVERIFY(b.invokeMain("doFind"));
    QTRY_VERIFY(b.searchDlg->property("visible").toBool());

    QQuickItem *wordBox = b.checkBox(QStringLiteral("全字匹配"));
    QQuickItem *regexBox = b.checkBox(QStringLiteral("正则表达式"));
    QVERIFY(wordBox != nullptr);
    QVERIFY(regexBox != nullptr);

    // 正则开：全字匹配禁用置灰（enabled=false）；正则关：恢复可用。
    // 复选框经真实鼠标点击驱动（toggled 信号只在交互路径发射，程序化
    // toggle() 不触发 QML onToggled 处理器，无法同步控制器）
    QVERIFY(wordBox->property("enabled").toBool());
    b.clickItem(regexBox);
    b.settle();
    QCOMPARE(regexBox->property("checked").toBool(), true);
    QCOMPARE(b.ctl()->useRegex(), true);
    QVERIFY2(!wordBox->property("enabled").toBool(),
             "whole-word checkbox must be disabled when regex is on");
    b.clickItem(regexBox);
    b.settle();
    QCOMPARE(b.ctl()->useRegex(), false);
    QVERIFY(wordBox->property("enabled").toBool());

    // 无效正则：错误文本非空（Label 直接显示 errorMessage，非仅颜色变化），
    // 匹配清零，三个动作按钮禁用、关闭恒可用
    b.clickItem(regexBox);
    QCOMPARE(b.ctl()->useRegex(), true);
    b.findField()->setProperty("text", QStringLiteral("[unclosed"));
    b.settle();
    QVERIFY(!b.ctl()->errorMessage().isEmpty());
    QCOMPARE(b.ctl()->matchCount(), 0);
    QCOMPARE(b.statusText(), b.ctl()->errorMessage());
    QVERIFY(!b.button(QStringLiteral("查找下一个"))->property("enabled").toBool());
    QVERIFY(!b.button(QStringLiteral("替换"))->property("enabled").toBool());
    QVERIFY(!b.button(QStringLiteral("全部替换"))->property("enabled").toBool());
    QVERIFY(b.button(QStringLiteral("关闭"))->property("enabled").toBool());

    // 修正为合法模式：错误清除、按钮恢复
    b.findField()->setProperty("text", QStringLiteral("b"));
    b.settle();
    QVERIFY(b.ctl()->errorMessage().isEmpty());
    QCOMPARE(b.ctl()->matchCount(), 1);
    QVERIFY(b.button(QStringLiteral("查找下一个"))->property("enabled").toBool());
    QVERIFY(b.button(QStringLiteral("替换"))->property("enabled").toBool());
    QVERIFY(b.button(QStringLiteral("全部替换"))->property("enabled").toBool());
}

// TC-205：多光标激活时 Ctrl+F 自动退出多光标（DEC-008）；对话框打开期间
// uiLocked 生效（Ctrl+Tab 切换标签被阻止），关闭后恢复
void TestSearchUiIntegration::tc205_multicursorMutex_and_uiLocked()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("aaaa\nbbbb"));
    b.newDoc(QStringLiteral("second doc"));
    b.dm->setActiveDocument(b.dm->documents().constFirst());
    b.settle();

    // 激活多光标（视觉列选，与 test_editor 的坐标驱动同法：rect(0) 中心
    // 按下 → rect(8) 中心拖动 → 结束。等长 ASCII 上与旧位置驱动
    // columnDragBegin(0, 5) 等价——两行各一对选区；该步骤只用于激活
    // 多光标状态）
    MultiCursorController *mc = b.activeMc();
    QVERIFY(mc != nullptr);
    QQuickItem *ed = b.activeEditor();
    QVERIFY(ed != nullptr);
    const QRectF r0 = b.rectOf(ed, 0);
    const QRectF r8 = b.rectOf(ed, 8);
    mc->columnDragBeginAt(r0.x(), r0.y() + r0.height() / 2);
    mc->columnDragUpdateAt(r8.x(), r8.y() + r8.height() / 2);
    mc->columnDragEnd();
    b.settle();
    QVERIFY(mc->active());
    QCOMPARE(mc->cursorCount(), 2);

    // Ctrl+F：对话框打开 + 多光标自动退出 + uiLocked
    QVERIFY(b.invokeMain("doFind"));
    QTRY_VERIFY(b.searchDlg->property("visible").toBool());
    QVERIFY(!mc->active());
    QCOMPARE(mc->cursorCount(), 1);
    QVERIFY(b.mainBool("uiLocked"));

    // 对话框打开期间 Ctrl+Tab（cycleTab）被 uiLocked 阻止：活动文档不变
    Document *before = b.dm->activeDocument();
    QVERIFY(b.invokeMain("cycleTab", QVariant(1)));
    b.settle();
    QCOMPARE(b.dm->activeDocument(), before);

    // Esc 关闭：uiLocked 解除，Ctrl+Tab 恢复生效（切换到第二文档）
    b.esc();
    QTRY_VERIFY(!b.searchDlg->property("visible").toBool());
    QVERIFY(!b.mainBool("uiLocked"));
    QVERIFY(b.invokeMain("cycleTab", QVariant(1)));
    b.settle();
    QVERIFY(b.dm->activeDocument() != before);
}

// TC-206：全部替换后 Ctrl+Z 一步回滚、modified 正确；关闭对话框后
// Ctrl+D 多光标编辑零回归（既有行为不受查找功能影响）
void TestSearchUiIntegration::tc206_regression_undoAndMultiCursor()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("foo bar foo"));

    // 全部替换
    QVERIFY(b.invokeMain("doFind"));
    QTRY_VERIFY(b.searchDlg->property("visible").toBool());
    b.findField()->setProperty("text", QStringLiteral("foo"));
    b.replaceField()->setProperty("text", QStringLiteral("X"));
    b.settle();
    QVariant replaced;
    QVERIFY(b.invokeDialog("doReplaceAll", &replaced));
    // doReplaceAll 为 QML function 无返回值（undefined），处数记录在
    // lastReplacedCount 属性（状态行「已替换 N 处」的数据源）
    QCOMPARE(b.searchDlg->property("lastReplacedCount").toInt(), 2);
    QCOMPARE(b.editorText(), QStringLiteral("X bar X"));
    QVERIFY(b.dm->activeDocument()->modified());

    // Esc 关闭 → 焦点回编辑器
    b.esc();
    QTRY_VERIFY(!b.searchDlg->property("visible").toBool());
    QVERIFY(b.activeEditor()->property("activeFocus").toBool());

    // Ctrl+D 多光标（关闭对话框后既有行为零回归）：光标置于词内（replaceAll
    // 后光标停在替换文本末尾的词边界，词边界无词可选是既有语义），
    // 两个 X 逐个追加成选区
    b.activeEditor()->setProperty("cursorPosition", 0);
    b.settle();
    b.sendKey(Qt::Key_D, Qt::ControlModifier);
    MultiCursorController *mc = b.activeMc();
    QVERIFY(mc->active());
    QCOMPARE(mc->cursorCount(), 1); // 首次 Ctrl+D：选中当前词（尚为单选区）
    QCOMPARE(Bench::mcSelectionPairs(mc),
             QStringList() << QStringLiteral("0:1"));
    b.sendKey(Qt::Key_D, Qt::ControlModifier);
    QCOMPARE(mc->cursorCount(), 2); // 追加相同词：两个 X 各成选区
    QCOMPARE(Bench::mcSelectionPairs(mc),
             QStringList() << QStringLiteral("0:1") << QStringLiteral("6:7"));

    // 批量输入 Y（多光标路径）
    b.sendKey(Qt::Key_Y, Qt::NoModifier, QStringLiteral("Y"));
    QCOMPARE(b.editorText(), QStringLiteral("Y bar Y"));

    // Ctrl+Z 第 1 步：回滚多光标批量输入（一步）
    b.sendKey(Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(b.editorText(), QStringLiteral("X bar X"));
    // Ctrl+Z 第 2 步：回滚「全部替换」整批（单 editBlock，一步完整回滚）
    b.sendKey(Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(b.editorText(), QStringLiteral("foo bar foo"));
    QVERIFY(b.dm->activeDocument()->modified()); // 撤销不改保存状态
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
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       sessionRedirectDir().path());
    // 草稿缓存隔离（审查应改 #2）：Main.qml 的 Component.onCompleted 每次实例化
    // 都会调 DraftCacheManager.restoreDrafts()，其非便携默认目录按测试 exe 名
    // 派生自 %LOCALAPPDATA%（本测试为 %LOCALAPPDATA%\test_searchui\drafts），
    // 跨运行持久。上一轮若以脏文档收场（QVERIFY 失败 return 跳过清理、或进程
    // 被杀）且防抖在后续事件循环到期写盘，条目残留会让本轮恢复出额外文档，
    // 文档计数类断言偶发失败。启动预清理隔离上一轮残留；结束清理避免测试产物
    // 在用户 %LOCALAPPDATA% 积累。不用 setTestModeEnabled（避免影响既有
    // QSettings 语义），不改应用名（目录不漂移、不积累）。
    const QString draftsDir = QStandardPaths::writableLocation(
                QStandardPaths::AppLocalDataLocation) + QStringLiteral("/drafts");
    QDir(draftsDir).removeRecursively();
    // 窗口关闭（visible = false）不得触发自动退出，避免干扰 QTest 事件循环
    app.setQuitOnLastWindowClosed(false);
    // 与主程序一致的控件样式（Main.qml 使用 Material 附加属性）
    QQuickStyle::setStyle(QStringLiteral("Material"));
    TestSearchUiIntegration t;
    const int testResult = QTest::qExec(&t, argc, argv);
    // 结束清理：删除本轮测试写入的草稿条目，不在 %LOCALAPPDATA% 留测试产物
    QDir(draftsDir).removeRecursively();
    return testResult;
}

#include "tst_searchuiintegration.moc"
