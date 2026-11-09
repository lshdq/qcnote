// 查找替换菜单入口 QML 集成测试（测试用例文档
// 《测试用例-查找替换菜单入口》TC-001/002/003/004/005/006/008 的
// 可自动化部分；TC-010/TC-011 由构建、ctest 全量与 offscreen 冒烟承担）。
//
// 测试方式：offscreen 平台下实例化完整 Main.qml，按标题定位「文件」「搜索」
// 「视图」子菜单与「查找与替换」对话框，对菜单项直发 triggered() 信号
// （与 tst_encodingmenu 同一模式）——与真实点击后的信号分发终点等价
// （TitleBar onTriggered → requestFind/Replace/FindNext/FindPrev →
// Main.qml doFind/doReplace/searchJumpGlobal → SearchDialog/SearchController）。
// 断言：
// - TC-001 主菜单重组结构：文件（新建/打开…/保存/另存为…/关闭标签页 +
//   编码三级级联）→ 搜索（查找…/替换…/下一个/上一个）→ 视图（原「查看」
//   更名、深色主题/自动换行保留）→ 顶层「退出」；KeyMenuItem 快捷键文本
//   与窗口级 Shortcut 序列一致（TC-009 可自动化部分）；
// - TC-002 无文档时搜索四项与文件内文档相关项置灰（新建/打开… 可用），
//   新建文档后恢复（置灰语义与重组前一致）；
// - TC-003/004 菜单查找…/替换…入口：对话框打开、选区预填并全选、焦点
//   分别落查找框/替换框、匹配高亮激活、进入 uiLocked；
// - TC-005 下一个/上一个：对话框关闭态跳转（原生选区随 currentIndex 移动、
//   环绕、不恢复全部高亮 DEC-009）；
// - TC-006 从未查找或上次查找串无匹配时无操作、不崩溃；
// - TC-008 代表性回归：文件→新建/保存（未命名转既有另存为分支）、
//   编码→通过编码保存（GBK→UTF-8 真实转换）、视图→自动换行（全局设置
//   与编辑器 wrapMode 联动）、顶层退出（一次生效）。
//
// 无法自动化项（真实鼠标点开多级菜单的视觉呈现、快捷键文本右对齐的像素
// 观感、TC-007 模态遮罩对真实鼠标点击的阻断、系统另存为对话框内交互、
// 真实按键合成）见测试报告"未执行"清单；真实按键链路已由
// tst_searchuiintegration（Ctrl+F/F3/Shift+F3）覆盖，此处不重复建设。
//
// GUI 红线：main 中强制 QT_QPA_PLATFORM=offscreen，全程无可见窗口。

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QList>
#include <QMetaObject>
#include <QObject>
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
#include "editor/searchcontroller.h"

using qcnote::SearchController;

// ==================== 测试台辅助 ====================

namespace {

// 进程级会话重定向目录：main 中 setPath 前创建，进程退出时随 QTemporaryDir
// 清理（与 tst_encodingmenu.cpp 的 sessionRedirectDir 同一模式）
QTemporaryDir &sessionRedirectDir()
{
    static QTemporaryDir dir;
    return dir;
}

/**
 * @brief QML 类型短名精确匹配（与 tst_encodingmenu 同一模式）。
 *
 * Qt 6.5+ Controls 控件实例类名为 QML 包装类型（如 "Menu_QMLTYPE_30"），
 * 取首个下划线前段与目标比较；TitleBar 内联组件 KeyMenuItem 短名不同，
 * 天然不与 "MenuItem" 误匹配。
 */
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

// 深度遍历可视树（EditorArea、对话框输入框在可视树中）
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

// 在对象子树中按 title 定位 Menu（title 在窗口内唯一）
QObject *findMenuByTitle(QObject *root, const QString &title)
{
    QList<QObject *> menus;
    collectByQmlType(root, "Menu", &menus);
    for (QObject *m : menus) {
        if (m->property("title").toString() == title)
            return m;
    }
    return nullptr;
}

// 在菜单子树中按文本 + QML 类型短名定位条目：KeyMenuItem（TitleBar 自绘
// 快捷键文本项）与 MenuItem（普通项）分别匹配，短名不同互不误匹配
QObject *findItemTyped(QObject *menu, const QString &text, const char *type)
{
    QList<QObject *> items;
    collectByQmlType(menu, type, &items);
    for (QObject *it : items) {
        if (it->property("text").toString() == text)
            return it;
    }
    return nullptr;
}

// 写临时测试文件（返回本地路径；失败返回空串）
QString writeTempFile(const QTemporaryDir &dir, const QString &name,
                      const QByteArray &bytes)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return QString();
    f.write(bytes);
    f.close();
    return path;
}

// 期望的菜单条目文本与快捷键文本（KeyMenuItem.keyText）
struct KeyExpect
{
    const char *text;
    const char *key;
};

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
    QQuickItem *editorArea = nullptr;   // EditorArea（查找集成状态持有者）
    QObject *mainMenu = nullptr;        // 主菜单（顶层「退出」所在）
    QObject *fileMenu = nullptr;        // 「文件」子菜单
    QObject *searchMenu = nullptr;      // 「搜索」子菜单
    QObject *viewMenu = nullptr;        // 「视图」子菜单（原「查看」）
    QObject *reloadEncMenu = nullptr;   // 编码 ▸ 通过编码重新加载（三级）
    QObject *saveEncMenu = nullptr;     // 编码 ▸ 通过编码保存（三级）
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
        // 单例可能跨 Bench 复用：清空上一用例残余文档
        const auto docs = dm->documents();
        for (Document *d : docs)
            dm->closeDocument(d);

        // 菜单按 title 定位（窗口内唯一）
        mainMenu = findMenuByTitle(win, QStringLiteral("主菜单"));
        fileMenu = findMenuByTitle(win, QStringLiteral("文件"));
        searchMenu = findMenuByTitle(win, QStringLiteral("搜索"));
        viewMenu = findMenuByTitle(win, QStringLiteral("视图"));
        reloadEncMenu = findMenuByTitle(win, QStringLiteral("通过编码重新加载"));
        saveEncMenu = findMenuByTitle(win, QStringLiteral("通过编码保存"));

        // 查找与替换对话框定位：SearchDialog.qml 为独立文件组件，实例的
        // QML 类型短名是 "SearchDialog"（非 Controls 的 "Dialog" 包装名）
        QList<QObject *> dialogs;
        collectByQmlType(win, "SearchDialog", &dialogs);
        searchDlg = dialogs.isEmpty() ? nullptr : dialogs.first();

        // EditorArea 在可视树中（查找控制器 / 活动编辑器持有者）
        QQuickItem *area = nullptr;
        visitItems(win->contentItem(), [&](QQuickItem *it) {
            if (!area && qmlTypeNameIs(it, "EditorArea"))
                area = it;
        });
        editorArea = area;

        if (!mainMenu || !fileMenu || !searchMenu || !viewMenu
                || !reloadEncMenu || !saveEncMenu || !searchDlg || !editorArea) {
            qWarning() << "[bench] missing:" << (!mainMenu ? "mainMenu" : "")
                       << (!fileMenu ? "fileMenu" : "")
                       << (!searchMenu ? "searchMenu" : "")
                       << (!viewMenu ? "viewMenu" : "")
                       << (!reloadEncMenu ? "reloadEncMenu" : "")
                       << (!saveEncMenu ? "saveEncMenu" : "")
                       << (!searchDlg ? "searchDlg" : "")
                       << (!editorArea ? "editorArea" : "");
            return false;
        }
        return true;
    }

    void settle() { QTest::qWait(30); }

    // ---- 菜单条目定位与直发 ----

    QObject *keyItem(QObject *menu, const QString &text)
    {
        return findItemTyped(menu, text, "KeyMenuItem");
    }

    QObject *menuItem(QObject *menu, const QString &text)
    {
        return findItemTyped(menu, text, "MenuItem");
    }

    // 直发菜单项 triggered()（真实点击的信号分发终点等价；
    // 与 tst_encodingmenu 直发 triggered() 的模式一致）
    bool triggerKeyItem(QObject *menu, const QString &text)
    {
        QObject *item = keyItem(menu, text);
        return item && QMetaObject::invokeMethod(item, "triggered");
    }

    bool triggerMenuItem(QObject *menu, const QString &text)
    {
        QObject *item = menuItem(menu, text);
        return item && QMetaObject::invokeMethod(item, "triggered");
    }

    // ---- 查找集成访问 ----

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

    int selStart() const
    {
        QQuickItem *ed = activeEditor();
        return ed ? ed->property("selectionStart").toInt() : -1;
    }

    int selEnd() const
    {
        QQuickItem *ed = activeEditor();
        return ed ? ed->property("selectionEnd").toInt() : -1;
    }

    // 对话框内查找/替换输入框：查找框无 placeholderText，替换框带捕获组
    // 提示文本（含 "$1"）——按此区分（不依赖布局 polish 时序）
    QList<QQuickItem *> dialogTextFields() const
    {
        QList<QQuickItem *> out;
        if (!searchDlg)
            return out;
        QObject *ci = searchDlg->property("contentItem").value<QObject *>();
        visitItems(qobject_cast<QQuickItem *>(ci), [&](QQuickItem *it) {
            if (qmlTypeNameIs(it, "TextField"))
                out.append(it);
        });
        return out;
    }

    QQuickItem *findField() const
    {
        for (QQuickItem *it : dialogTextFields()) {
            if (it->property("placeholderText").toString().isEmpty())
                return it;
        }
        return nullptr;
    }

    QQuickItem *replaceField() const
    {
        for (QQuickItem *it : dialogTextFields()) {
            if (it->property("placeholderText").toString().contains(QStringLiteral("$1")))
                return it;
        }
        return nullptr;
    }

    // 活动编辑器的查找高亮层（TextArea 子树内；visible 绑 active）
    QQuickItem *highlightLayer() const
    {
        QQuickItem *found = nullptr;
        QQuickItem *ed = activeEditor();
        if (!ed)
            return nullptr;
        visitItems(ed, [&](QQuickItem *it) {
            if (!found && qmlTypeNameIs(it, "SearchHighlightLayer"))
                found = it;
        });
        return found;
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

    // 打开给定字节的文件并返回文档（自动识别编码）
    Document *openBytes(QTemporaryDir &dir, const QString &name,
                        const QByteArray &bytes)
    {
        const QString path = writeTempFile(dir, name, bytes);
        dm->openUrls({ QUrl::fromLocalFile(path) });
        settle();
        return dm->activeDocument();
    }
};

} // namespace

// ==================== 测试类 ====================

class TestSearchMenu : public QObject
{
    Q_OBJECT

private slots:
    // TC-001: 主菜单重组结构（文件/搜索/视图/退出、快捷键文本、编码三级）
    void tc001_menuStructure();

    // TC-002: 无文档时搜索四项置灰；新建文档后全部可用
    void tc002_searchItems_enablement();

    // TC-003: 查找…入口：对话框打开、选区预填全选、焦点在查找框、高亮激活
    void tc003_findEntry_prefillAndHighlight();

    // TC-004: 替换…入口：同一对话框、焦点在替换输入框
    void tc004_replaceEntry_focusReplaceField();

    // TC-005: 下一个/上一个：关闭态跳转、选区随移动、环绕、不恢复高亮
    void tc005_nextPrev_jumpMovesSelection();

    // TC-006: 从未查找 / 上次查找无匹配时无操作不崩溃
    void tc006_noSearchState_noop();

    // TC-008 回归: 文件→新建（新增文档）、文件→保存（未命名转另存为分支）
    void tc007_regression_fileNewAndSave();

    // TC-008 回归: 编码→通过编码保存（GBK→UTF-8）、视图→自动换行联动
    void tc008_regression_encodingSaveAndWordWrap();

    // TC-008 回归: 顶层退出一次生效（干净文档无确认流程）
    void tc009_regression_quitEntry();
};

// TC-001: 主菜单重组为 文件（含编码三级级联）→ 搜索 → 视图 → 顶层退出。
// 「查看」必须已更名「视图」；KeyMenuItem 快捷键文本与窗口级 Shortcut
// 序列一致（TC-009 可自动化部分：菜单显示文本正确性）。
void TestSearchMenu::tc001_menuStructure()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");

    // 「编码」子菜单仍收纳在 文件 ▸ 内（重组仅层级变化）；旧标题「查看」不存在
    QVERIFY2(findMenuByTitle(b.fileMenu, QStringLiteral("编码")) != nullptr,
             "编码子菜单必须位于 文件 ▸ 内");
    QVERIFY2(findMenuByTitle(b.win, QStringLiteral("查看")) == nullptr,
             "旧子菜单标题「查看」应已更名为「视图」");

    // 文件 ▸：五项 KeyMenuItem 的条目与快捷键文本
    const KeyExpect fileKeys[] = {
        { "新建", "Ctrl+N" },
        { "打开…", "Ctrl+O" },
        { "保存", "Ctrl+S" },
        { "另存为…", "Ctrl+Shift+S" },
        { "关闭标签页", "Ctrl+W" },
    };
    for (const KeyExpect &k : fileKeys) {
        const QString text = QString::fromUtf8(k.text);
        QObject *item = b.keyItem(b.fileMenu, text);
        QVERIFY2(item != nullptr,
                 qPrintable(QStringLiteral("文件菜单缺少条目: %1").arg(text)));
        QCOMPARE(item->property("keyText").toString(), QLatin1String(k.key));
    }

    // 文件 ▸ 编码 ▸ 通过编码重新加载 / 通过编码保存（各含 UTF-8、GBK；
    // 保存组 checkable——当前编码打勾，详见 tst_encodingmenu）
    QVERIFY(b.menuItem(b.reloadEncMenu, QStringLiteral("UTF-8")) != nullptr);
    QVERIFY(b.menuItem(b.reloadEncMenu, QStringLiteral("GBK")) != nullptr);
    QObject *saveUtf8 = b.menuItem(b.saveEncMenu, QStringLiteral("UTF-8"));
    QObject *saveGbk = b.menuItem(b.saveEncMenu, QStringLiteral("GBK"));
    QVERIFY(saveUtf8 && saveGbk);
    QCOMPARE(saveUtf8->property("checkable").toBool(), true);
    QCOMPARE(saveGbk->property("checkable").toBool(), true);

    // 搜索 ▸：四项 KeyMenuItem 的条目与快捷键文本
    const KeyExpect searchKeys[] = {
        { "查找…", "Ctrl+F" },
        { "替换…", "Ctrl+H" },
        { "下一个", "F3" },
        { "上一个", "Shift+F3" },
    };
    for (const KeyExpect &k : searchKeys) {
        const QString text = QString::fromUtf8(k.text);
        QObject *item = b.keyItem(b.searchMenu, text);
        QVERIFY2(item != nullptr,
                 qPrintable(QStringLiteral("搜索菜单缺少条目: %1").arg(text)));
        QCOMPARE(item->property("keyText").toString(), QLatin1String(k.key));
    }

    // 视图 ▸：深色主题 / 自动换行（内容不变，checkable）
    QObject *darkItem = b.menuItem(b.viewMenu, QStringLiteral("深色主题"));
    QObject *wrapItem = b.menuItem(b.viewMenu, QStringLiteral("自动换行"));
    QVERIFY(darkItem && wrapItem);
    QCOMPARE(darkItem->property("checkable").toBool(), true);
    QCOMPARE(wrapItem->property("checkable").toBool(), true);

    // 顶层「退出」保留（普通 MenuItem，恒可用）
    QObject *quitItem = b.menuItem(b.mainMenu, QStringLiteral("退出"));
    QVERIFY2(quitItem != nullptr, "顶层必须保留「退出」条目");
    QCOMPARE(quitItem->property("enabled").toBool(), true);
}

// TC-002: 欢迎页（无文档）时搜索四项全部禁用置灰，文件内 保存/另存为…/
// 关闭标签页 及 编码两组操作 置灰（与重组前语义一致），新建/打开… 可用；
// 新建文档后搜索四项与文档相关项恢复可用。
void TestSearchMenu::tc002_searchItems_enablement()
{
    // 场景 a：无文档（欢迎页）
    {
        Bench b;
        QVERIFY2(b.init(), "testbench init failed");
        QVERIFY(b.dm->activeDocument() == nullptr);

        for (const char *t : { "查找…", "替换…", "下一个", "上一个" }) {
            const QString text = QString::fromUtf8(t);
            QObject *item = b.keyItem(b.searchMenu, text);
            QVERIFY2(item != nullptr,
                     qPrintable(QStringLiteral("搜索菜单缺少条目: %1").arg(text)));
            QVERIFY2(!item->property("enabled").toBool(),
                     qPrintable(QStringLiteral("无文档时 %1 必须置灰").arg(text)));
        }

        // 文件内文档相关项置灰、新建/打开… 可用（重组前同语义）
        QVERIFY(!b.keyItem(b.fileMenu, QStringLiteral("保存"))->property("enabled").toBool());
        QVERIFY(!b.keyItem(b.fileMenu, QStringLiteral("另存为…"))->property("enabled").toBool());
        QVERIFY(!b.keyItem(b.fileMenu, QStringLiteral("关闭标签页"))->property("enabled").toBool());
        QVERIFY(b.keyItem(b.fileMenu, QStringLiteral("新建"))->property("enabled").toBool());
        QVERIFY(b.keyItem(b.fileMenu, QStringLiteral("打开…"))->property("enabled").toBool());
        QVERIFY(!b.reloadEncMenu->property("enabled").toBool());
        QVERIFY(!b.saveEncMenu->property("enabled").toBool());
    }

    // 场景 b：新建文档后搜索四项全部可用
    {
        Bench b;
        QVERIFY2(b.init(), "testbench init failed");
        QVERIFY(b.newDoc() != nullptr);

        for (const char *t : { "查找…", "替换…", "下一个", "上一个" }) {
            const QString text = QString::fromUtf8(t);
            QObject *item = b.keyItem(b.searchMenu, text);
            QVERIFY(item != nullptr);
            QVERIFY2(item->property("enabled").toBool(),
                     qPrintable(QStringLiteral("有活动文档时 %1 必须可用").arg(text)));
        }
        QVERIFY(b.keyItem(b.fileMenu, QStringLiteral("保存"))->property("enabled").toBool());
        QVERIFY(b.keyItem(b.fileMenu, QStringLiteral("另存为…"))->property("enabled").toBool());
        QVERIFY(b.keyItem(b.fileMenu, QStringLiteral("关闭标签页"))->property("enabled").toBool());
        QVERIFY(b.saveEncMenu->property("enabled").toBool());
        // 未命名文档无磁盘内容可重载：通过编码重新加载仍置灰（既有语义）
        QVERIFY(!b.reloadEncMenu->property("enabled").toBool());
    }
}

// TC-003: 主菜单 → 搜索 → 查找…（同 Ctrl+F / doFind 链路）：对话框打开、
// 查找框内容为选区文本且全选、焦点在查找框、匹配高亮激活、进入 uiLocked。
void TestSearchMenu::tc003_findEntry_prefillAndHighlight()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("foo bar foo baz"));
    QQuickItem *ed = b.activeEditor();
    QVERIFY(ed != nullptr);

    // 选中首个 "foo"（[0,3)，不含换行）作为预填源
    QVERIFY(QMetaObject::invokeMethod(ed, "select", Q_ARG(int, 0), Q_ARG(int, 3)));
    b.settle();

    QVERIFY2(b.triggerKeyItem(b.searchMenu, QStringLiteral("查找…")),
             "failed to trigger find menu item");
    QTRY_VERIFY(b.searchDlg->property("visible").toBool());

    // 对话框打开：查找框聚焦、内容为选区文本且全选
    QTest::qWait(50); // 等布局 polish 与焦点稳定
    QQuickItem *findField = b.findField();
    QQuickItem *replaceField = b.replaceField();
    QVERIFY2(findField != nullptr, "find field not found");
    QVERIFY2(replaceField != nullptr, "replace field not found");
    QVERIFY2(findField->property("activeFocus").toBool(),
             "focus must be on the find field");
    QCOMPARE(findField->property("text").toString(), QStringLiteral("foo"));
    QCOMPARE(findField->property("selectionStart").toInt(), 0);
    QCOMPARE(findField->property("selectionEnd").toInt(), 3);

    // 预填即实时查找：2 处匹配；高亮层激活（背后全部匹配高亮）
    QCOMPARE(b.ctl()->pattern(), QStringLiteral("foo"));
    QCOMPARE(b.ctl()->matchCount(), 2);
    QVERIFY(b.editorArea->property("searchActive").toBool());
    QQuickItem *layer = b.highlightLayer();
    QVERIFY(layer != nullptr);
    QVERIFY(layer->property("visible").toBool());

    // 模态：对话框打开期间进入 uiLocked（菜单入口不可达，TC-007 数据源）
    QVariant locked;
    QVERIFY(QMetaObject::invokeMethod(b.win, "uiLocked", Q_RETURN_ARG(QVariant, locked)));
    QVERIFY(locked.toBool());

    // 关闭（rejected → 清高亮、焦点回编辑器）
    QVERIFY(QMetaObject::invokeMethod(b.searchDlg, "reject"));
    QTRY_VERIFY(!b.searchDlg->property("visible").toBool());
    QCOMPARE(b.editorArea->property("searchActive").toBool(), false);
    QVERIFY(ed->property("activeFocus").toBool());
}

// TC-004: 主菜单 → 搜索 → 替换…（同 Ctrl+H / doReplace 链路）：同一对话框
// 打开，焦点在替换输入框（不在查找框）；选区预填链路与查找入口一致。
void TestSearchMenu::tc004_replaceEntry_focusReplaceField()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("alpha beta alpha"));
    QQuickItem *ed = b.activeEditor();
    QVERIFY(ed != nullptr);

    QVERIFY(QMetaObject::invokeMethod(ed, "select", Q_ARG(int, 0), Q_ARG(int, 5)));
    b.settle();

    QVERIFY2(b.triggerKeyItem(b.searchMenu, QStringLiteral("替换…")),
             "failed to trigger replace menu item");
    QTRY_VERIFY(b.searchDlg->property("visible").toBool());
    QTest::qWait(50);

    QVERIFY2(b.replaceField()->property("activeFocus").toBool(),
             "focus must be on the replace field");
    QVERIFY2(!b.findField()->property("activeFocus").toBool(),
             "focus must not stay on the find field");

    // 预填链路一致：选区文本入查找串（openReplace 前同一次预填）
    QCOMPARE(b.ctl()->pattern(), QStringLiteral("alpha"));
    QCOMPARE(b.ctl()->matchCount(), 2);

    QVERIFY(QMetaObject::invokeMethod(b.searchDlg, "reject"));
    QTRY_VERIFY(!b.searchDlg->property("visible").toBool());
}

// TC-005: 执行过一次查找并 Esc 关闭对话框后，菜单 下一个/上一个 交替触发：
// 原生选区随 currentIndex 移动、环绕导航，与 F3/Shift+F3 关闭态语义一致；
// 不恢复全部高亮（DEC-009）、不重开对话框。
void TestSearchMenu::tc005_nextPrev_jumpMovesSelection()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("foo a foo b foo")); // 3 处 "foo"：[0,3) [6,9) [12,15)
    QVERIFY(b.activeEditor() != nullptr);

    // 前置：菜单 查找… 打开对话框，经真实输入链路置入查找串（有多个匹配）
    QVERIFY(b.triggerKeyItem(b.searchMenu, QStringLiteral("查找…")));
    QTRY_VERIFY(b.searchDlg->property("visible").toBool());
    b.findField()->setProperty("text", QStringLiteral("foo"));
    b.settle();
    QCOMPARE(b.ctl()->matchCount(), 3);
    QCOMPARE(b.ctl()->currentIndex(), 0); // restartSearch 归零（尚未跳转）

    // Esc 关闭对话框（前置条件完成）：匹配状态保留（DEC-009）
    QVERIFY(QMetaObject::invokeMethod(b.searchDlg, "reject"));
    QTRY_VERIFY(!b.searchDlg->property("visible").toBool());
    QCOMPARE(b.editorArea->property("searchActive").toBool(), false);

    // 菜单 下一个（首跳）：全新会话首跳先定位当前匹配（SearchDialog.jump
    // 的 hasJumped 语义，与对话框内 Enter 首跳一致），原生选区落到 [0,3)
    QVERIFY2(b.triggerKeyItem(b.searchMenu, QStringLiteral("下一个")),
             "failed to trigger find-next menu item");
    b.settle();
    QCOMPARE(b.ctl()->currentIndex(), 0);
    QCOMPARE(b.selStart(), 0);
    QCOMPARE(b.selEnd(), 3);
    QCOMPARE(b.searchDlg->property("visible").toBool(), false); // 不重开对话框

    // 下一个：0 → 1；再下一个：1 → 2（选区随 currentRange 移动）
    QVERIFY(b.triggerKeyItem(b.searchMenu, QStringLiteral("下一个")));
    b.settle();
    QCOMPARE(b.ctl()->currentIndex(), 1);
    QCOMPARE(b.selStart(), 6);
    QCOMPARE(b.selEnd(), 9);
    QVERIFY(b.triggerKeyItem(b.searchMenu, QStringLiteral("下一个")));
    b.settle();
    QCOMPARE(b.ctl()->currentIndex(), 2);
    QCOMPARE(b.selStart(), 12);
    QCOMPARE(b.selEnd(), 15);

    // 上一个：2 → 1
    QVERIFY(b.triggerKeyItem(b.searchMenu, QStringLiteral("上一个")));
    b.settle();
    QCOMPARE(b.ctl()->currentIndex(), 1);
    QCOMPARE(b.selStart(), 6);
    QCOMPARE(b.selEnd(), 9);

    // 上一个：1 → 0；再上一个：环绕回 2（与 Shift+F3 一致）
    QVERIFY(b.triggerKeyItem(b.searchMenu, QStringLiteral("上一个")));
    b.settle();
    QCOMPARE(b.ctl()->currentIndex(), 0);
    QVERIFY(b.triggerKeyItem(b.searchMenu, QStringLiteral("上一个")));
    b.settle();
    QCOMPARE(b.ctl()->currentIndex(), 2);
    QCOMPARE(b.selStart(), 12);
    QCOMPARE(b.selEnd(), 15);

    // DEC-009：关闭态跳转不恢复全部高亮
    QCOMPARE(b.editorArea->property("searchActive").toBool(), false);
    QQuickItem *layer = b.highlightLayer();
    QVERIFY(layer != nullptr);
    QVERIFY(!layer->property("visible").toBool());
}

// TC-006: 打开文档但从未查找（pattern 空），或上次查找串无匹配时，
// 菜单 下一个/上一个 无任何变化、不崩溃、不弹窗。
void TestSearchMenu::tc006_noSearchState_noop()
{
    // 场景 a：从未查找（全新会话 pattern 为空）
    {
        Bench b;
        QVERIFY2(b.init(), "testbench init failed");
        b.newDoc(QStringLiteral("abc"));
        QCOMPARE(b.ctl()->pattern(), QString());
        QCOMPARE(b.ctl()->matchCount(), 0);

        const int selBefore = b.selStart();
        const int cursorBefore = b.activeEditor()->property("cursorPosition").toInt();
        QVERIFY(b.triggerKeyItem(b.searchMenu, QStringLiteral("下一个")));
        QVERIFY(b.triggerKeyItem(b.searchMenu, QStringLiteral("上一个")));
        b.settle();
        QCOMPARE(b.ctl()->currentIndex(), -1);
        QCOMPARE(b.selStart(), selBefore);
        QCOMPARE(b.activeEditor()->property("cursorPosition").toInt(), cursorBefore);
        QCOMPARE(b.searchDlg->property("visible").toBool(), false);
        QVERIFY(b.win->isVisible()); // 应用继续运行，未崩溃
    }

    // 场景 b：上次查找串无匹配（经真实预填链路置入）
    {
        Bench b;
        QVERIFY2(b.init(), "testbench init failed");
        b.newDoc(QStringLiteral("abc"));
        QVERIFY(b.triggerKeyItem(b.searchMenu, QStringLiteral("查找…")));
        QTRY_VERIFY(b.searchDlg->property("visible").toBool());
        b.findField()->setProperty("text", QStringLiteral("zzz"));
        b.settle();
        QCOMPARE(b.ctl()->matchCount(), 0);
        QVERIFY(QMetaObject::invokeMethod(b.searchDlg, "reject"));
        QTRY_VERIFY(!b.searchDlg->property("visible").toBool());

        const int cursorBefore = b.activeEditor()->property("cursorPosition").toInt();
        QVERIFY(b.triggerKeyItem(b.searchMenu, QStringLiteral("下一个")));
        QVERIFY(b.triggerKeyItem(b.searchMenu, QStringLiteral("上一个")));
        b.settle();
        QCOMPARE(b.ctl()->currentIndex(), -1);
        QCOMPARE(b.activeEditor()->property("cursorPosition").toInt(), cursorBefore);
        QVERIFY(b.win->isVisible());
    }
}

// TC-008 回归: 文件 → 新建（经重组后子菜单触发，新增文档）；文件 → 保存
// （未命名脏文档转既有另存为分支：saveAsTarget 置位、内容保留、不退出；
// 系统另存为对话框内交互无法自动化，按 tst_windowclose TC-008 模式断言）。
void TestSearchMenu::tc007_regression_fileNewAndSave()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");

    // 文件 → 新建
    const int before = b.dm->documents().size();
    QVERIFY2(b.triggerKeyItem(b.fileMenu, QStringLiteral("新建")),
             "failed to trigger new menu item");
    b.settle();
    QCOMPARE(b.dm->documents().size(), before + 1);
    QVERIFY(b.dm->activeDocument() != nullptr);

    // 文件 → 保存（未命名脏文档）
    Document *doc = b.dm->activeDocument();
    doc->setContent(QStringLiteral("menu save"));
    QVERIFY(doc->modified());
    QVERIFY2(b.triggerKeyItem(b.fileMenu, QStringLiteral("保存")),
             "failed to trigger save menu item");
    b.settle();
    const QVariant st = b.win->property("saveAsTarget");
    QVERIFY(st.canConvert<QObject *>());
    QCOMPARE(st.value<QObject *>(), static_cast<QObject *>(doc));
    QCOMPARE(doc->content(), QStringLiteral("menu save")); // 内容保留待另存为
    QVERIFY(b.win->isVisible());                           // 应用继续运行
}

// TC-008 回归: 编码 ▸ 通过编码保存 ▸ UTF-8（GBK 文件真实转换：磁盘字节、
// 编码元数据、不置脏；完整链路由 tst_encodingmenu 覆盖，此处为重组后
// 代表性回归）；视图 → 自动换行（SessionManager 全局设置翻转，菜单项
// 打勾与编辑器 wrapMode 同源联动）。
void TestSearchMenu::tc008_regression_encodingSaveAndWordWrap()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // 编码 → 通过编码保存 → UTF-8
    // "你好" 的 GBK（CP936）字节：C4 E3 BA C3（与系统 locale 无关）
    Document *doc = b.openBytes(dir, QStringLiteral("a.txt"),
                                QByteArray::fromHex("c4e3bac3"));
    QVERIFY(doc && doc->encoding() == Document::Encoding::Gbk);
    QVERIFY2(b.triggerMenuItem(b.saveEncMenu, QStringLiteral("UTF-8")),
             "failed to trigger save-as-UTF-8 item");
    b.settle();
    QFile f(dir.filePath(QStringLiteral("a.txt")));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QStringLiteral("你好").toUtf8());
    f.close();
    QCOMPARE(doc->encoding(), Document::Encoding::Utf8);
    QCOMPARE(doc->modified(), false);

    // 视图 → 自动换行：checked 与编辑器 wrapMode 共用 SessionManager 数据源
    QObject *wrapItem = b.menuItem(b.viewMenu, QStringLiteral("自动换行"));
    QVERIFY(wrapItem != nullptr);
    QQuickItem *ed = b.activeEditor();
    QVERIFY(ed != nullptr);
    const bool wrapBefore = wrapItem->property("checked").toBool();
    // wrapMode 联动基线：NoWrap(0) / WrapAtWordBoundaryOrAnywhere(4)
    const int wrapModeBefore = ed->property("wrapMode").toInt();
    QCOMPARE(wrapModeBefore, wrapBefore ? 4 : 0);

    QVERIFY(b.triggerMenuItem(b.viewMenu, QStringLiteral("自动换行")));
    b.settle();
    QCOMPARE(wrapItem->property("checked").toBool(), !wrapBefore);
    QCOMPARE(ed->property("wrapMode").toInt(), wrapBefore ? 0 : 4);

    // 触发回原值（不污染后续用例与会话）
    QVERIFY(b.triggerMenuItem(b.viewMenu, QStringLiteral("自动换行")));
    b.settle();
    QCOMPARE(wrapItem->property("checked").toBool(), wrapBefore);
    QCOMPARE(ed->property("wrapMode").toInt(), wrapModeBefore);
}

// TC-008 回归: 顶层「退出」对干净文档一次生效（无确认流程；与
// tst_windowclose TC-003 的 doQuit 语义一致，此处验证重组后菜单接线）。
void TestSearchMenu::tc009_regression_quitEntry()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(); // 干净文档：不弹未保存确认框

    QVERIFY2(b.triggerMenuItem(b.mainMenu, QStringLiteral("退出")),
             "failed to trigger quit menu item");
    QTRY_VERIFY(!b.win->isVisible()); // 一次生效
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
    // 草稿缓存隔离（审查应改 #2）：Main.qml 的 Component.onCompleted 每次实例化
    // 都会调 DraftCacheManager.restoreDrafts()，其非便携默认目录按测试 exe 名
    // 派生自 %LOCALAPPDATA%（本测试为 %LOCALAPPDATA%\test_searchmenu\drafts），
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
    TestSearchMenu t;
    const int testResult = QTest::qExec(&t, argc, argv);
    // 结束清理：删除本轮测试写入的草稿条目，不在 %LOCALAPPDATA% 留测试产物
    QDir(draftsDir).removeRecursively();
    return testResult;
}

#include "tst_searchmenu.moc"
