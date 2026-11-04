// GBK 与 UTF-8 编码支持 QML 集成测试（测试用例文档
// 《测试用例-GBK与UTF8编码支持》TC-017/TC-018 的可自动化部分）。
//
// 测试方式：offscreen 平台下实例化完整 Main.qml，按标题定位「编码」
// 子菜单（通过编码重新加载 / 通过编码保存）与按编码重载确认框，断言：
// - TC-017 菜单入口：子菜单结构存在；「通过编码重新加载」对无文档与
//   未命名文档（url 空）禁用、已打开文件启用；「通过编码保存」当前
//   编码项打勾（checkable）；状态栏编码文本随文档元数据变化
//   （GBK / UTF-8 / UTF-8 BOM）；
// - TC-018 确认框：脏文档触发重载弹确认框（uiLocked 屏蔽快捷键），
//   取消内容保留、确认以磁盘内容重载且不置脏；干净文档直接重载不弹框；
// - 「通过编码保存」真实转换链路（磁盘字节 GBK→UTF-8、状态栏与打勾
//   状态更新）；未命名文档转既有另存为分支（saveAsTarget + 编码已
//   保留）；GBK 不可编码内容走既有 saveFailed → errorDialog 流。
//
// 触发方式说明：真实菜单鼠标链路无法 offscreen 合成，与
// tst_windowclose 的 discarded() 直发模式一致，对菜单项直发 triggered()
// 信号——与真实点击后的信号分发终点等价（TitleBar onTriggered →
// requestReload/SaveWithEncoding → Main 处理函数 → DocumentManager）。
// GBK 测试文件手工构造字节（"你好" = C4 E3 BA C3），不依赖系统 locale。
//
// 无法自动化项（真实鼠标点开三级菜单的视觉呈现、系统另存为对话框内
// 交互）见测试报告"未执行"清单。
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
// （与 tst_windowclose.cpp 的 sessionRedirectDir 同一模式）
QTemporaryDir &sessionRedirectDir()
{
    static QTemporaryDir dir;
    return dir;
}

/**
 * @brief QML 类型短名精确匹配（与 tst_windowclose 同一模式）。
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

// 深度遍历可视树（StatusBar 在可视树中）
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

QQuickItem *findItemByQmlType(QQuickItem *root, const char *name)
{
    QQuickItem *found = nullptr;
    visitItems(root, [&](QQuickItem *it) {
        if (!found && qmlTypeNameIs(it, name))
            found = it;
    });
    return found;
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

// 在 Menu 子树中按 text 定位 MenuItem（「编码」子菜单内均为普通 MenuItem）
QObject *findMenuItemByText(QObject *menu, const QString &text)
{
    QList<QObject *> items;
    collectByQmlType(menu, "MenuItem", &items);
    for (QObject *it : items) {
        if (it->property("text").toString() == text)
            return it;
    }
    return nullptr;
}

// 状态栏全部 Label 文本（编码显示为其中之一，按精确文本断言）
QStringList statusBarTexts(QQuickItem *statusBar)
{
    QStringList out;
    if (!statusBar)
        return out;
    QList<QObject *> labels;
    collectByQmlType(statusBar, "Label", &labels);
    for (QObject *l : labels)
        out << l->property("text").toString();
    return out;
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

// "你好" 的 GBK（CP936）字节：C4 E3 BA C3（与系统 locale 无关）
QByteArray gbkHelloBytes()
{
    return QByteArray::fromHex("c4e3bac3");
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
    QObject *encodingMenu = nullptr;    // TitleBar「编码」子菜单
    QObject *reloadMenu = nullptr;      // 「通过编码重新加载」三级菜单
    QObject *saveMenu = nullptr;        // 「通过编码保存」三级菜单
    QObject *reloadDlg = nullptr;       // reloadConfirmDialog（标题「按编码重新加载」）
    QObject *errDlg = nullptr;          // errorDialog（title 绑定 currentError，未打开为空）
    QQuickItem *statusBar = nullptr;    // StatusBar 实例（可视树）

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

        // 菜单按 title 定位（TitleBar 内唯一）
        encodingMenu = findMenuByTitle(win, QStringLiteral("编码"));
        reloadMenu = findMenuByTitle(win, QStringLiteral("通过编码重新加载"));
        saveMenu = findMenuByTitle(win, QStringLiteral("通过编码保存"));

        // 对话框按标题定位；errorDialog 未打开时 title 为空串走排除法
        QList<QObject *> dialogs;
        collectByQmlType(win, "Dialog", &dialogs);
        for (QObject *d : dialogs) {
            const QString t = d->property("title").toString();
            if (t == QStringLiteral("按编码重新加载"))
                reloadDlg = d;
            else if (t.isEmpty())
                errDlg = d;
        }

        statusBar = findItemByQmlType(win->contentItem(), "StatusBar");
        if (!encodingMenu || !reloadMenu || !saveMenu || !reloadDlg
            || !errDlg || !statusBar) {
            qWarning() << "[bench] missing:" << (!encodingMenu ? "encodingMenu" : "")
                       << (!reloadMenu ? "reloadMenu" : "")
                       << (!saveMenu ? "saveMenu" : "")
                       << (!reloadDlg ? "reloadDlg" : "")
                       << (!errDlg ? "errDlg" : "")
                       << (!statusBar ? "statusBar" : "");
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

    // 打开给定字节的文件并返回文档（自动识别编码）
    Document *openBytes(QTemporaryDir &dir, const QString &name,
                        const QByteArray &bytes)
    {
        const QString path = writeTempFile(dir, name, bytes);
        dm->openUrls({ QUrl::fromLocalFile(path) });
        return dm->activeDocument();
    }

    // 直发菜单项 triggered()（与真实点击的信号分发终点等价；
    // 与 tst_windowclose 直发 discarded() 的模式一致）
    bool triggerItem(QObject *menu, const QString &text)
    {
        QObject *item = findMenuItemByText(menu, text);
        return item && QMetaObject::invokeMethod(item, "triggered");
    }
};

} // namespace

class TestEncodingMenu : public QObject
{
    Q_OBJECT

private slots:
    // TC-017: 菜单结构、启用状态（无文档/未命名/已打开文件）与打勾状态
    void tc017_menuStructure_enablementAndCheckState();

    // TC-017: 状态栏编码文本（GBK / UTF-8 BOM / UTF-8 / 新建默认）
    void tc017_statusBar_encodingText();

    // TC-017 关联: 「通过编码保存」真实转换链路（GBK→UTF-8 磁盘字节、
    // 状态栏与打勾状态更新、不置脏）
    void tc017_saveWithEncoding_uiConversion();

    // TC-018: 脏文档触发重载 → 弹确认框（uiLocked 屏蔽）；取消内容保留
    void tc018_reload_unsaved_confirmCancelKeeps();

    // TC-018: 确认后以磁盘内容重载、不置脏、编码更新
    void tc018_reload_unsaved_confirmAcceptReloads();

    // TC-018 关联: 干净文档直接重载，不弹确认框
    void tc018_reload_clean_noDialog();

    // 未命名文档「通过编码保存」→ 转既有另存为分支（编码已保留）
    void tc0xx_saveUnnamed_redirectsSaveAs();

    // GBK 不可编码内容「通过编码保存」→ 既有 saveFailed → errorDialog
    void tc0xx_saveGbkUnencodable_errorDialog();
};

// TC-017: 「编码」子菜单存在两组操作；「通过编码重新加载」仅在活动文档
// 存在且 url 非空（非未命名）时启用；「通过编码保存」项 checkable 且当前
// 文档编码对应项打勾。
void TestEncodingMenu::tc017_menuStructure_enablementAndCheckState()
{
    // 场景 a：无文档（清空后无活动文档）→ 两组操作均禁用
    {
        Bench b;
        QVERIFY2(b.init(), "testbench init failed");
        QVERIFY(b.dm->activeDocument() == nullptr);
        QCOMPARE(b.reloadMenu->property("enabled").toBool(), false);
        QCOMPARE(b.saveMenu->property("enabled").toBool(), false);
    }

    // 场景 b：未命名文档（url 空）→ 重载禁用；保存可用，UTF-8 默认打勾
    {
        Bench b;
        QVERIFY2(b.init(), "testbench init failed");
        Document *doc = b.newDoc();
        QVERIFY(doc);
        QVERIFY(doc->url().isEmpty());
        QCOMPARE(b.reloadMenu->property("enabled").toBool(), false);
        QCOMPARE(b.saveMenu->property("enabled").toBool(), true);

        QObject *utf8Item = findMenuItemByText(b.saveMenu, QStringLiteral("UTF-8"));
        QObject *gbkItem = findMenuItemByText(b.saveMenu, QStringLiteral("GBK"));
        QVERIFY(utf8Item && gbkItem);
        QCOMPARE(utf8Item->property("checkable").toBool(), true);
        QCOMPARE(gbkItem->property("checkable").toBool(), true);
        QCOMPARE(utf8Item->property("checked").toBool(), true);  // 新建默认 UTF-8
        QCOMPARE(gbkItem->property("checked").toBool(), false);
    }

    // 场景 c：已打开 GBK 文件 → 重载启用；保存 GBK 项打勾
    {
        Bench b;
        QVERIFY2(b.init(), "testbench init failed");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        Document *doc = b.openBytes(dir, QStringLiteral("a.txt"), gbkHelloBytes());
        QVERIFY(doc);
        QVERIFY(!doc->url().isEmpty());
        QCOMPARE(doc->encoding(), Document::Encoding::Gbk); // C++ 自动识别前置
        QCOMPARE(b.reloadMenu->property("enabled").toBool(), true);

        QObject *utf8Item = findMenuItemByText(b.saveMenu, QStringLiteral("UTF-8"));
        QObject *gbkItem = findMenuItemByText(b.saveMenu, QStringLiteral("GBK"));
        QVERIFY(utf8Item && gbkItem);
        QCOMPARE(gbkItem->property("checked").toBool(), true);   // 当前编码打勾
        QCOMPARE(utf8Item->property("checked").toBool(), false);

        // 「通过编码重新加载」子项存在（UTF-8 / GBK）
        QVERIFY(findMenuItemByText(b.reloadMenu, QStringLiteral("UTF-8")));
        QVERIFY(findMenuItemByText(b.reloadMenu, QStringLiteral("GBK")));
    }
}

// TC-017: 状态栏编码显示绑定文档元数据：GBK → "GBK"；带 BOM UTF-8 →
// "UTF-8 BOM"；无 BOM UTF-8 → "UTF-8"；未命名新文档默认 "UTF-8"。
// 按精确文本匹配（"UTF-8 BOM" 含 "UTF-8" 子串，须整串相等）。
void TestEncodingMenu::tc017_statusBar_encodingText()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // GBK 文件（自动识别）
    Document *gbkDoc = b.openBytes(dir, QStringLiteral("gbk.txt"), gbkHelloBytes());
    QVERIFY(gbkDoc && gbkDoc->encoding() == Document::Encoding::Gbk);
    const QStringList gbkTexts = statusBarTexts(b.statusBar);
    QVERIFY2(gbkTexts.contains(QStringLiteral("GBK")),
             "status bar should show exactly 'GBK'");
    QVERIFY(!gbkTexts.contains(QStringLiteral("UTF-8")));

    // 带 BOM 的 UTF-8 文件
    Document *bomDoc = b.openBytes(
        dir, QStringLiteral("bom.txt"),
        QByteArray("\xEF\xBB\xBF") + QStringLiteral("你好").toUtf8());
    QVERIFY(bomDoc && bomDoc->encoding() == Document::Encoding::Utf8
            && bomDoc->hasBom());
    QVERIFY2(statusBarTexts(b.statusBar).contains(QStringLiteral("UTF-8 BOM")),
             "status bar should show exactly 'UTF-8 BOM'");

    // 无 BOM 的 UTF-8 文件（多字节中文，非纯 ASCII 路径）
    Document *utf8Doc = b.openBytes(
        dir, QStringLiteral("utf8.txt"), QStringLiteral("你好").toUtf8());
    QVERIFY(utf8Doc && utf8Doc->encoding() == Document::Encoding::Utf8
            && !utf8Doc->hasBom());
    const QStringList utf8Texts = statusBarTexts(b.statusBar);
    QVERIFY2(utf8Texts.contains(QStringLiteral("UTF-8")),
             "status bar should show exactly 'UTF-8'");
    QVERIFY(!utf8Texts.contains(QStringLiteral("UTF-8 BOM")));
    QVERIFY(!utf8Texts.contains(QStringLiteral("GBK")));

    // 未命名新文档：默认 UTF-8
    QVERIFY(b.newDoc());
    QVERIFY2(statusBarTexts(b.statusBar).contains(QStringLiteral("UTF-8")),
             "untitled document defaults to UTF-8");
}

// TC-017 关联: 「通过编码保存 → UTF-8」真实链路：GBK 文件转存为 UTF-8
// 磁盘字节（无 BOM）、编码元数据与打勾状态更新、状态栏跟随、不置脏。
void TestEncodingMenu::tc017_saveWithEncoding_uiConversion()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Document *doc = b.openBytes(dir, QStringLiteral("conv.txt"), gbkHelloBytes());
    QVERIFY(doc && doc->encoding() == Document::Encoding::Gbk);
    const QString path = doc->url().toLocalFile();

    // 菜单项触发：TitleBar requestSaveWithEncoding → Main → saveWithEncoding
    QVERIFY2(b.triggerItem(b.saveMenu, QStringLiteral("UTF-8")),
             "failed to trigger save-as-UTF-8 menu item");

    // 磁盘字节为无 BOM UTF-8；文档编码与修改标记更新
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QStringLiteral("你好").toUtf8());
    f.close();
    QCOMPARE(doc->encoding(), Document::Encoding::Utf8);
    QCOMPARE(doc->modified(), false);

    // 打勾状态切换到 UTF-8；状态栏跟随
    QCOMPARE(findMenuItemByText(b.saveMenu, QStringLiteral("UTF-8"))
                 ->property("checked").toBool(), true);
    QCOMPARE(findMenuItemByText(b.saveMenu, QStringLiteral("GBK"))
                 ->property("checked").toBool(), false);
    QVERIFY(statusBarTexts(b.statusBar).contains(QStringLiteral("UTF-8")));
    QVERIFY(!statusBarTexts(b.statusBar).contains(QStringLiteral("GBK")));
}

// TC-018: 打开文件修改未保存 → 触发「通过编码重新加载」弹确认框
// （含 uiLocked 快捷键屏蔽聚合）；取消 → 框关、内容与脏状态保留。
void TestEncodingMenu::tc018_reload_unsaved_confirmCancelKeeps()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Document *doc = b.openBytes(dir, QStringLiteral("r.txt"), gbkHelloBytes());
    QVERIFY(doc && doc->encoding() == Document::Encoding::Gbk);
    doc->setContent(QStringLiteral("dirty"));
    QVERIFY(doc->modified());

    QVERIFY2(b.triggerItem(b.reloadMenu, QStringLiteral("GBK")),
             "failed to trigger reload-as-GBK menu item");
    QTRY_VERIFY(b.reloadDlg->property("visible").toBool());
    QCOMPARE(dialogText(b.reloadDlg),
             QStringLiteral("通过 GBK 重新加载将丢弃未保存的修改"));

    // 确认框打开期间进入 uiLocked（快捷键/关闭请求屏蔽聚合）
    QVariant uiLockedResult;
    QVERIFY(QMetaObject::invokeMethod(b.win, "uiLocked",
                                      Q_RETURN_ARG(QVariant, uiLockedResult)));
    QVERIFY(uiLockedResult.toBool());

    // 取消：内容保留、仍为脏、框关闭、解除锁定
    QVERIFY(QMetaObject::invokeMethod(b.reloadDlg, "reject"));
    QTRY_VERIFY(!b.reloadDlg->property("visible").toBool());
    QCOMPARE(doc->content(), QStringLiteral("dirty"));
    QCOMPARE(doc->modified(), true);
    QVariant unlockedResult;
    QVERIFY(QMetaObject::invokeMethod(b.win, "uiLocked",
                                      Q_RETURN_ARG(QVariant, unlockedResult)));
    QVERIFY(!unlockedResult.toBool());
}

// TC-018: 确认「重新加载」→ 以磁盘内容覆盖内存修改（不置脏），编码更新。
void TestEncodingMenu::tc018_reload_unsaved_confirmAcceptReloads()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Document *doc = b.openBytes(dir, QStringLiteral("r2.txt"), gbkHelloBytes());
    QVERIFY(doc && doc->encoding() == Document::Encoding::Gbk);
    doc->setContent(QStringLiteral("dirty"));
    QVERIFY(doc->modified());

    QVERIFY(b.triggerItem(b.reloadMenu, QStringLiteral("GBK")));
    QTRY_VERIFY(b.reloadDlg->property("visible").toBool());

    QVERIFY(QMetaObject::invokeMethod(b.reloadDlg, "accept"));
    QTRY_VERIFY(!b.reloadDlg->property("visible").toBool());
    // 磁盘 GBK 内容重新解码：未保存修改被丢弃、不置脏
    QCOMPARE(doc->content(), QStringLiteral("你好"));
    QCOMPARE(doc->modified(), false);
    QCOMPARE(doc->encoding(), Document::Encoding::Gbk);
    // 状态栏仍显示 GBK
    QVERIFY(statusBarTexts(b.statusBar).contains(QStringLiteral("GBK")));
}

// TC-018 关联: 干净文档（无未保存修改）触发重载不弹确认框，直接执行
// （DEC-008：确认框只在有未保存修改时出现）。
void TestEncodingMenu::tc018_reload_clean_noDialog()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Document *doc = b.openBytes(dir, QStringLiteral("c.txt"), gbkHelloBytes());
    QVERIFY(doc && doc->encoding() == Document::Encoding::Gbk);
    QVERIFY(!doc->modified());

    // 以 UTF-8 强制重载 GBK 字节：不弹框、编码切换（内容呈替换字符）
    QVERIFY(b.triggerItem(b.reloadMenu, QStringLiteral("UTF-8")));
    QTest::qWait(100);
    QCOMPARE(b.reloadDlg->property("visible").toBool(), false);
    QCOMPARE(doc->encoding(), Document::Encoding::Utf8);
    QVERIFY(doc->content() != QStringLiteral("你好"));
    QCOMPARE(doc->modified(), false);
    // 状态栏跟随新编码
    QVERIFY(statusBarTexts(b.statusBar).contains(QStringLiteral("UTF-8")));
}

// 未命名文档「通过编码保存」：saveWithEncoding 返回 false（C++ 已保留
// 编码设置）→ 走既有另存为分支（saveAsTarget 置位、另存为对话框打开；
// QQuickFileDialog 非窗口 QObject 子孙，按 tst_windowclose TC-008 模式经
// saveAsTarget 断言）。
void TestEncodingMenu::tc0xx_saveUnnamed_redirectsSaveAs()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    Document *doc = b.newDoc(QStringLiteral("内容"));
    QVERIFY(doc && doc->url().isEmpty());
    QVERIFY(doc->encoding() == Document::Encoding::Utf8);

    QVERIFY2(b.triggerItem(b.saveMenu, QStringLiteral("GBK")),
             "failed to trigger save-as-GBK menu item");
    QTest::qWait(50);

    // 转另存为分支：目标指向该文档；C++ 已保留 GBK 编码设置（另存为按该编码写出）
    const QVariant st = b.win->property("saveAsTarget");
    QVERIFY(st.canConvert<QObject *>());
    QCOMPARE(st.value<QObject *>(), static_cast<QObject *>(doc));
    QCOMPARE(doc->encoding(), Document::Encoding::Gbk);
    QCOMPARE(doc->content(), QStringLiteral("内容")); // 内容保留待另存为
    QVERIFY(b.win->isVisible());                      // 应用继续运行
}

// GBK 不可编码内容「通过编码保存 → GBK」：saveWithEncoding 失败发
// saveFailed → 既有 queueError → errorDialog（保存失败提示），编码回滚，
// 不误入另存为分支。
void TestEncodingMenu::tc0xx_saveGbkUnencodable_errorDialog()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // 未命名文档（含 emoji，GBK 不可编码）→ 先另存为 UTF-8 文件取得 url
    Document *doc = b.newDoc(QStringLiteral("a😀b"));
    QVERIFY(doc);
    const QString path = dir.filePath(QStringLiteral("emoji.txt"));
    QVERIFY(b.dm->saveAs(doc, QUrl::fromLocalFile(path))); // UTF-8 写出成功
    QVERIFY(!doc->url().isEmpty());
    QVERIFY(!doc->modified());

    // 触发「通过编码保存 → GBK」：转换失败 → saveFailed → errorDialog
    QVERIFY2(b.triggerItem(b.saveMenu, QStringLiteral("GBK")),
             "failed to trigger save-as-GBK menu item");
    QTRY_VERIFY(b.errDlg->property("visible").toBool());
    QCOMPARE(b.errDlg->property("title").toString(), QStringLiteral("保存失败"));
    QVERIFY(dialogText(b.errDlg).contains(path)); // 错误目标为该文件路径

    // 有 url：不误入另存为分支（saveAsTarget 保持空）
    const QVariant st = b.win->property("saveAsTarget");
    QVERIFY(st.isNull() || !st.canConvert<QObject *>()
            || st.value<QObject *>() == nullptr);

    // 编码设置回滚（失败不产生误导性标记，DEC-006）；磁盘保持 UTF-8 未破坏
    QCOMPARE(doc->encoding(), Document::Encoding::Utf8);
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QStringLiteral("a😀b").toUtf8());
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
    // 窗口关闭（visible = false）不得触发自动退出，避免干扰 QTest 事件循环
    app.setQuitOnLastWindowClosed(false);
    // 与主程序一致的控件样式（Main.qml 使用 Material 附加属性）
    QQuickStyle::setStyle(QStringLiteral("Material"));
    TestEncodingMenu t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_encodingmenu.moc"
