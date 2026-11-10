// 标签页 Win11 风格改造 QML 集成测试（《测试用例-标签页Win11风格改造》
// TC-002/003/004/005/006/007/008/009/010/011 的可自动化部分；
// TC-001 由构建 + ctest 全量回归承担；纯观感验收（连通无缝的肉眼观感、
// 深浅主题悬停可辨性、菜单按钮与贴底标签的错位观感）由
// tests\verify-win11tabs.ps1 驱动的 e2e_tabshot_driver 截图取证承担）。
//
// 测试方式：offscreen 平台下实例化完整 Main.qml，按 QML 类型短名定位
// TitleBar / DocumentTabs / 其内 ListView（tabList），经 itemAtIndex 取
// delegate，按特征定位标签背景结构（radius 5 主背景 / 底部补角矩形 /
// radius 4 悬停高亮 / border 2 焦点环），断言：
// - TC-002/003：activeTab token 引用 toolbarBackground（浅深两主题）、
//   活动标签背景同工具栏色 + 仅顶圆角 + 无描边 + 补角矩形同色补直底角、
//   标签区贴标题栏底边（y+height == 标题栏高）且高度 34、深色下与
//   标题栏底色可辨（activeTab != titleBarBackground）；
// - TC-004：非活动标签主背景透明、悬停高亮默认不可见；合成鼠标移动
//   （QTest::mouseMove 真实事件路径）到非活动标签后高亮出现
//   （color == AppTheme.hover、radius 4），移开后消失；深浅两主题均验证
//   （DEC-002：深色 #22ffffff 叠加的结构保证）；
// - TC-005：delegate 子树无 Canvas（关闭按钮 × 的绘制载体）、Text 项
//   仅 ● 标记与文件名，DocumentTabs 全树无 Canvas；
// - TC-006：主菜单「关闭标签页」直发 triggered()（tst_searchmenu 同
//   模式）关闭干净文档；Ctrl+W 合成按键关闭干净文档。右键菜单三入口
//   由 test_tabcontextmenu 全量覆盖（含三选确认），未保存 Ctrl+W 三选
//   与其共用 closeDocumentWithConfirm，走查覆盖；
// - TC-007：modified 文档标签出现 visible ● Text，干净文档无；
// - TC-008：自然宽度公式 == 文字 advance + 左右边距（同引擎 TextMetrics
//   实测对照，无关闭按钮常量）；多长名标签等比收缩（100 <= w < 自然宽，
//   总宽不超可用宽）、饱和后退化为 minTabWidth 全员 + 溢出；
// - TC-009：tabsOverflow 与 ListView interactive 联动（溢出可滚动、
//   未溢出事件穿透）；
// - TC-010：forceActiveFocus + 合成方向键循环移动焦点、焦点环 visible
//   （radius 4 适配新形状）、Enter 激活对应文档（Menu/Shift+F10 弹菜单
//   属保留代码，走查 + test_tabcontextmenu 键盘路径共用函数）；
// - TC-011：标签文字右缘距标签右缘 10px（几何断言）；收缩态文字
//   truncated（ElideRight 省略号生效）。
// - TC-012：标签间竖分隔线（DEC-103/104）：3 标签中间活动时首标签 /
//   活动标签 / 活动右邻标签左线均不可见；切换活动文档后可见性即时跟随；
//   相邻两非活动标签之间线可见（贴左缘、宽 1、高 20 垂直居中、
//   色 AppTheme.border）；另断言 titleBarBackground 本轮加深值
//   （浅 #dde2ec / 深 #171c24，较工具栏深一档）。
//
// GUI 红线：main 中强制 QT_QPA_PLATFORM=offscreen，全程无可见窗口。

#include <QColor>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPoint>
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

#include <functional>
#include <memory>

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

// 像素级采样：窗口渲染当前状态后取场景坐标处像素（「无描边」「透明底」
// 等视觉属性在 QQuickPen 属性层不可断言，用渲染结果直接验证）。
// 慢机/高负载下首拍可能是渲染中途帧：传入 expected 时比对失败自动短重试
// （最多 5 次、每次 qWait(40)），仍不符才返回最终值交由 QCOMPARE 报失败。
QColor samplePixel(QQuickWindow *win, const QPointF &scenePos,
                   const QColor &expected = QColor())
{
    win->show();
    QColor sampled;
    const int maxAttempts = expected.isValid() ? 6 : 1;
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        QTest::qWait(attempt == 0 ? 80 : 40); // 渲染安定
        const QImage img = win->grabWindow();
        sampled = img.isNull() ? QColor()
                               : img.pixelColor(qRound(scenePos.x()),
                                                qRound(scenePos.y()));
        if (!expected.isValid() || sampled == expected)
            return sampled;
    }
    return sampled;
}

// 可视树深搜（按 QML 类型短名）
QQuickItem *findItemByQmlType(QQuickItem *root, const char *name)
{
    if (!root)
        return nullptr;
    if (qmlTypeNameIs(root, name))
        return root;
    const auto kids = root->childItems();
    for (QQuickItem *kid : kids) {
        if (QQuickItem *found = findItemByQmlType(kid, name))
            return found;
    }
    return nullptr;
}

// C++ 类名前缀匹配（QQuickRectangle / QQuickListView / QQuickText /
// QQuickCanvas / QQuickMouseArea 等编译期合成类型）
bool classNameStartsWith(const QObject *obj, const char *prefix)
{
    return QString::fromLatin1(obj->metaObject()->className())
        .startsWith(QLatin1String(prefix));
}

QQuickItem *findItemByClassPrefix(QQuickItem *root, const char *prefix)
{
    if (!root)
        return nullptr;
    if (classNameStartsWith(root, prefix))
        return root;
    const auto kids = root->childItems();
    for (QQuickItem *kid : kids) {
        if (QQuickItem *found = findItemByClassPrefix(kid, prefix))
            return found;
    }
    return nullptr;
}

// Rectangle 描边宽（border 分组属性 QQuickPen，tst_windowborder 同模式）。
// 注意：QML 不写 border 时 QQuickPen 默认 width=1 + 默认色，但 valid=false
// （框架不绘制）——「无描边」不可从属性断言，由像素级采样验证。
qreal penWidth(QQuickItem *rect)
{
    const QVariant v = rect->property("border");
    if (!v.isValid() || !v.canConvert<QObject *>())
        return -1;
    const QObject *grp = v.value<QObject *>();
    return grp ? grp->property("width").toReal() : -1;
}

/** 单个标签 delegate 的视觉结构（按特征定位，见文件头注释） */
struct TabVisuals
{
    QQuickItem *mainBg = nullptr;        // radius==5 主背景（活动=工具栏同色）
    QQuickItem *cornerPatch = nullptr;   // 主背景子矩形：底部补直底角（height==radius）
    QQuickItem *hoverHighlight = nullptr;// 主背景子矩形：radius==4 悬停高亮
    QQuickItem *focusRing = nullptr;     // delegate 子矩形：border.width==2
    QQuickItem *separator = nullptr;     // delegate 子矩形：标签间竖分隔线（width==1）
    QQuickItem *mouseArea = nullptr;     // tabMouseArea
    QQuickItem *label = nullptr;         // 文件名 Text
    QQuickItem *unsavedMark = nullptr;   // ● Text
    QList<QQuickItem *> texts;           // delegate 子树全部 Text
    QList<QQuickItem *> canvases;        // delegate 子树全部 Canvas（应恒空）
};

TabVisuals collectTabVisuals(QQuickItem *del, const QString &displayName)
{
    TabVisuals v;
    if (!del)
        return v;
    for (QQuickItem *kid : del->childItems()) {
        if (classNameStartsWith(kid, "QQuickRectangle")) {
            if (qFuzzyCompare(kid->property("radius").toReal(), 5.0))
                v.mainBg = kid;
            else if (qFuzzyCompare(penWidth(kid), 2.0))
                v.focusRing = kid;
            else if (qFuzzyCompare(kid->width(), 1.0))
                v.separator = kid;
        } else if (classNameStartsWith(kid, "QQuickMouseArea")) {
            v.mouseArea = kid;
        }
    }
    if (v.mainBg) {
        for (QQuickItem *kid : v.mainBg->childItems()) {
            if (!classNameStartsWith(kid, "QQuickRectangle"))
                continue;
            if (qFuzzyCompare(kid->property("radius").toReal(), 4.0))
                v.hoverHighlight = kid;
            else if (qFuzzyIsNull(kid->property("radius").toReal()))
                v.cornerPatch = kid;
        }
    }
    // 全子树收集 Text / Canvas（含补角层之外的嵌套）
    const std::function<void(QQuickItem *)> walk = [&](QQuickItem *it) {
        for (QQuickItem *kid : it->childItems()) {
            if (classNameStartsWith(kid, "QQuickText")) {
                v.texts.append(kid);
                const QString t = kid->property("text").toString();
                if (t == QStringLiteral("●"))
                    v.unsavedMark = kid;
                else if (t == displayName)
                    v.label = kid;
            } else if (classNameStartsWith(kid, "QQuickCanvas")) {
                v.canvases.append(kid);
            }
            walk(kid);
        }
    };
    walk(del);
    return v;
}

/**
 * @brief 每个测试用例独立的测试台：完整实例化 Main.qml。
 *
 * DocumentManager 为 QML 单例且跨引擎复用同一 C++ 实例，因此每用例
 * 开始时清空残余文档；AppTheme 为纯 QML 单例（每引擎一份），深色主题
 * 的修改不影响后续用例的新 Bench。
 */
struct Bench
{
    QQmlEngine engine;
    QQuickWindow *win = nullptr;        // Main.qml 根（ApplicationWindow）
    DocumentManager *dm = nullptr;      // QML 单例的同一 C++ 实例
    QObject *theme = nullptr;           // AppTheme QML 单例
    QQuickItem *titleBar = nullptr;     // TitleBar
    QQuickItem *tabs = nullptr;         // DocumentTabs
    QQuickItem *tabList = nullptr;      // DocumentTabs 内 ListView

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
        theme = engine.singletonInstance<QObject *>(u"qcnote", u"AppTheme");
        if (!dm || !theme)
            return false;
        const auto docs = dm->documents();
        for (Document *d : docs)
            dm->closeDocument(d);

        titleBar = findItemByQmlType(win->contentItem(), "TitleBar");
        tabs = findItemByQmlType(win->contentItem(), "DocumentTabs");
        if (!titleBar || !tabs)
            return false;
        tabList = findItemByClassPrefix(tabs, "QQuickListView");
        if (!tabList)
            return false;
        return true;
    }

    void settle() { QTest::qWait(30); }

    // ListView::itemAtIndex（Q_INVOKABLE，与 DocumentTabs.qml 内调用同路径）
    QQuickItem *tabAt(int index)
    {
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(tabList, "itemAtIndex",
                                  Q_RETURN_ARG(QQuickItem *, item),
                                  Q_ARG(int, index));
        return item;
    }

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

    // 在临时目录创建长名文件并打开（displayName 取文件名，制造长标签）
    Document *openLongNameDoc(int seq)
    {
        static const QString kBase =
            QStringLiteral("Win11标签连通式风格改造验收超长文件名文档编号");
        const QString path = sessionRedirectDir().filePath(
            QStringLiteral("%1-%2-very-long-file-name-for-elide.txt").arg(kBase).arg(seq));
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
            return nullptr;
        f.write("x\n");
        f.close();
        dm->openUrls({ QUrl::fromLocalFile(path) });
        settle();
        return dm->activeDocument();
    }

    // 同引擎 TextMetrics 实测文字 advance（与 DocumentTabs.recompute-
    // NaturalWidths 的 labelMetrics 同字体路径：默认字体族 + pixelSize 13）
    qreal measureLabelAdvance(const QString &text)
    {
        QQmlComponent c(&engine);
        c.setData("import QtQuick\nTextMetrics { font.pixelSize: 13 }",
                  QUrl(QStringLiteral("measure.qml")));
        if (c.isError())
            return -1;
        std::unique_ptr<QObject> tm(c.create());
        if (!tm || !tm->setProperty("text", text))
            return -1;
        return tm->property("advanceWidth").toReal();
    }
};

} // namespace

// ==================== 测试类 ====================

class TestWin11Tabs : public QObject
{
    Q_OBJECT

private slots:
    // TC-002: 活动标签连通式（浅色）：token 引用、同色、顶圆角、无描边、
    // 补角矩形、贴底、高度 34
    void tc002_activeConnected_light();

    // TC-003: 活动标签连通式（深色）：同上 + 与标题栏底色可辨
    void tc003_activeConnected_dark();

    // TC-004: 非活动标签透明 + 悬停半透明圆角高亮（真实鼠标事件路径，
    // 深浅两主题；DEC-002 结构保证）
    void tc004_inactiveTransparent_hoverHighlight();

    // TC-005: 每标签关闭按钮已移除（无 Canvas / 无多余 Text）
    void tc005_closeButtonRemoved();

    // TC-006a: 主菜单「文件 → 关闭标签页」仍可关闭
    void tc006a_closeViaMainMenu();

    // TC-006b: Ctrl+W 仍可关闭
    void tc006b_closeViaCtrlW();

    // TC-007: 未保存 ● 标记
    void tc007_unsavedMark();

    // TC-008: 自然宽度公式（无关闭按钮装饰）+ 等比收缩 + 饱和退化
    void tc008_naturalWidth_shrink_saturate();

    // TC-009: 溢出滚动与未溢出事件穿透联动
    void tc009_overflow_interactiveToggle();

    // TC-010: 键盘导航与焦点环
    void tc010_keyboardNav_focusRing();

    // TC-011: 标签文字右边距与省略
    void tc011_labelRightPadding_elide();

    // TC-012: 标签间竖分隔线（DEC-103/104 可见性、几何与颜色）
    void tc012_tabSeparator();
};

// 断言工具：活动标签连通式核心结构（浅/深共用）
static void verifyActiveConnected(Bench &b, Document *activeDoc)
{
    const int idx = b.tabs->property("activeIndex").toInt();
    QQuickItem *del = b.tabAt(idx);
    QVERIFY2(del, "active delegate instantiated");
    QCOMPARE(del->property("document").value<Document *>(), activeDoc);

    const TabVisuals v = collectTabVisuals(del, activeDoc->displayName());
    QVERIFY2(v.mainBg, "main background (radius 5) found");
    QVERIFY2(v.cornerPatch, "bottom corner patch rectangle found");
    QVERIFY2(v.hoverHighlight, "hover highlight rectangle found");
    QVERIFY2(v.focusRing, "focus ring (border 2) found");
    QVERIFY2(v.mouseArea, "tab MouseArea found");
    QVERIFY2(v.label, "label Text found");

    // token 约束：activeTab 引用 toolbarBackground（DEC-001）
    const QColor activeTab = b.theme->property("activeTab").value<QColor>();
    const QColor toolbarBg = b.theme->property("toolbarBackground").value<QColor>();
    QCOMPARE(activeTab, toolbarBg);

    // 活动标签主背景：工具栏同色、仅顶圆角（radius 5）；「无四周描边」
    // 无法从 QQuickPen 属性断言（默认 width=1 且 valid 语义不可见），
    // 由 tc002/tc003 的像素级采样验证（左缘像素 == 工具栏色，旧 1px
    // 描边残留会呈现 AppTheme.border 色而被检出）
    QCOMPARE(v.mainBg->property("color").value<QColor>(), toolbarBg);
    QCOMPARE(v.mainBg->property("radius").toReal(), 5.0);

    // 补角矩形：仅活动可见、同色补直底两角（height == 圆角半径）
    QVERIFY(v.cornerPatch->isVisible());
    QCOMPARE(v.cornerPatch->property("height").toReal(), 5.0);
    QCOMPARE(v.cornerPatch->property("color").value<QColor>(),
             v.mainBg->property("color").value<QColor>());

    // 标签区贴标题栏底边：DocumentTabs y+height == TitleBar 高，高度 34
    // （DEC-004），delegate 底边 == 组件底边 == 标题栏底边（连通前提）
    QCOMPARE(b.tabs->property("height").toReal(), 34.0);
    QCOMPARE(qFuzzyCompare(b.tabs->y() + b.tabs->height(), b.titleBar->height()),
             true);
    QCOMPARE(qFuzzyCompare(del->y() + del->height(), b.tabs->height()), true);

    // 活动标签悬停高亮不出现（悬停高亮仅非活动）
    QVERIFY(!v.hoverHighlight->isVisible());
}

// TC-002: 浅色主题（默认）连通式活动标签
void TestWin11Tabs::tc002_activeConnected_light()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QVERIFY(!b.theme->property("dark").toBool()); // 前置：浅色

    Document *doc = b.newDoc();
    QVERIFY(doc != nullptr);
    b.settle();

    // 浅色工具栏色值（需求 TC-002 预期 #f2f4f8）
    QCOMPARE(b.theme->property("toolbarBackground").value<QColor>(),
             QColor(QStringLiteral("#f2f4f8")));

    // 浅色标题栏底（本轮加深为 #dde2ec，较工具栏深一档）；活动标签
    // （= 工具栏色）与其色差可辨（连通"浮起"效果前提）
    const QColor titleBgLight =
        b.theme->property("titleBarBackground").value<QColor>();
    QCOMPARE(titleBgLight, QColor(QStringLiteral("#dde2ec")));
    QVERIFY(b.theme->property("activeTab").value<QColor>() != titleBgLight);

    verifyActiveConnected(b, doc);

    // 像素级验证无描边：活动标签左缘中点像素 == 工具栏色（填充延续到
    // 边缘）；改造前 1px AppTheme.border 描边会使该像素呈描边色
    QQuickItem *del = b.tabAt(b.tabs->property("activeIndex").toInt());
    QVERIFY(del != nullptr);
    const QColor edge = samplePixel(
        b.win, del->mapToScene(QPointF(0.0, del->height() / 2)),
        b.theme->property("toolbarBackground").value<QColor>());
    QCOMPARE(edge, b.theme->property("toolbarBackground").value<QColor>());
}

// TC-003: 深色主题连通式活动标签（AppTheme.dark = true 等价菜单切换）
void TestWin11Tabs::tc003_activeConnected_dark()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QVERIFY(b.theme->setProperty("dark", true));
    b.settle();

    Document *doc = b.newDoc();
    QVERIFY(doc != nullptr);
    b.settle();

    // 深色工具栏色值（需求 TC-003 预期 #232833）；与标题栏底（本轮加深
    // 为 #171c24，较工具栏深一档）有可辨色差
    QCOMPARE(b.theme->property("toolbarBackground").value<QColor>(),
             QColor(QStringLiteral("#232833")));
    const QColor titleBg = b.theme->property("titleBarBackground").value<QColor>();
    QCOMPARE(titleBg, QColor(QStringLiteral("#171c24")));
    QVERIFY(b.theme->property("activeTab").value<QColor>() != titleBg);

    verifyActiveConnected(b, doc);

    // 像素级验证无描边（深色）：活动标签左缘中点像素 == 深色工具栏色
    QQuickItem *del = b.tabAt(b.tabs->property("activeIndex").toInt());
    QVERIFY(del != nullptr);
    const QColor edge = samplePixel(
        b.win, del->mapToScene(QPointF(0.0, del->height() / 2)),
        b.theme->property("toolbarBackground").value<QColor>());
    QCOMPARE(edge, b.theme->property("toolbarBackground").value<QColor>());
}

// TC-004: 非活动标签透明底；合成鼠标移动后出现半透明圆角高亮
// （AppTheme.hover，radius 4），移开消失；深浅两主题（DEC-002）。
void TestWin11Tabs::tc004_inactiveTransparent_hoverHighlight()
{
    for (int round = 0; round < 2; ++round) {
        Bench b;
        QVERIFY2(b.init(), "testbench init failed");
        if (round == 1) {
            QVERIFY(b.theme->setProperty("dark", true)); // 深色轮
            b.settle();
        }
        Document *d1 = b.newDoc();
        Document *d2 = b.newDoc();
        QVERIFY(d1 && d2);
        b.dm->setActiveDocument(d1); // 活动 = 第 0 个，第 1 个非活动
        b.settle();
        b.win->show();
        b.settle();

        QQuickItem *del = b.tabAt(1);
        QVERIFY2(del, "inactive delegate instantiated");
        const TabVisuals v = collectTabVisuals(del, d2->displayName());
        QVERIFY2(v.mainBg && v.hoverHighlight, "background structure found");

        // 静置：非活动主背景透明（结构）、补角不可见、悬停高亮不可见；
        // 像素级验证透明底：标签顶部空白区（y=6，避开文字与圆角）像素
        // 透出标题栏背景色
        QCOMPARE(v.mainBg->property("color").value<QColor>(),
                 QColor(Qt::transparent));
        QVERIFY(!v.cornerPatch->isVisible());
        QVERIFY(!v.hoverHighlight->isVisible());
        const QColor titleBg =
            b.theme->property("titleBarBackground").value<QColor>();
        const QPointF probe = del->mapToScene(
            QPointF(del->width() / 2, 6));
        QCOMPARE(samplePixel(b.win, probe, titleBg), titleBg);

        // 合成鼠标移动到非活动标签中心（真实事件路径触发 hover）
        const QPointF center = del->mapToScene(
            QPointF(del->width() / 2, del->height() / 2));
        QTest::mouseMove(b.win, center.toPoint());
        QTRY_VERIFY2(v.hoverHighlight->isVisible(),
                     "hover highlight appears on hover");
        QCOMPARE(v.hoverHighlight->property("color").value<QColor>(),
                 b.theme->property("hover").value<QColor>());
        QCOMPARE(v.hoverHighlight->property("radius").toReal(), 4.0);

        // DEC-002 深色档：#22ffffff 叠深色标题栏（结构 + 色值双断言）
        if (round == 1)
            QCOMPARE(b.theme->property("hover").value<QColor>(),
                     QColor(QStringLiteral("#22ffffff")));

        // 非活动标签保持透明（悬停不换实心底）
        QCOMPARE(v.mainBg->property("color").value<QColor>(),
                 QColor(Qt::transparent));

        // 像素级验证悬停高亮可见（DEC-002 可辨性的自动化侧证据）：
        // 同一探测点颜色离开标题栏底色（半透明叠加必然改变像素）
        const QColor hovered = samplePixel(b.win, probe);
        QVERIFY2(hovered != titleBg,
                 qPrintable(QStringLiteral("hover changes pixel: %1 vs bg %2")
                                .arg(hovered.name(), titleBg.name())));

        // 移开：高亮消失，像素恢复标题栏底色
        QTest::mouseMove(b.win, QPoint(5, b.win->height() - 5));
        QTRY_VERIFY2(!v.hoverHighlight->isVisible(),
                     "hover highlight disappears when leaving");
        QCOMPARE(samplePixel(b.win, probe, titleBg), titleBg);
    }
}

// TC-005: 每标签无关闭按钮：delegate 子树无 Canvas（原 × 的绘制载体）、
// Text 项仅 ● 标记与文件名；DocumentTabs 全树无 Canvas。
void TestWin11Tabs::tc005_closeButtonRemoved()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QList<Document *> docs;
    for (int i = 0; i < 3; ++i) {
        Document *d = b.newDoc(i == 1 ? QStringLiteral("dirty") : QString());
        QVERIFY(d != nullptr);
        docs.append(d);
    }
    b.settle();

    for (int i = 0; i < 3; ++i) {
        QQuickItem *del = b.tabAt(i);
        QVERIFY2(del, "delegate instantiated");
        const TabVisuals v = collectTabVisuals(del, docs.at(i)->displayName());
        QCOMPARE(v.canvases.size(), 0); // 无 Canvas = 无关闭按钮 ×
        // Text 白名单：仅文件名与（未保存时）● 标记，无关闭按钮辅助文字
        for (QQuickItem *t : v.texts) {
            const QString text = t->property("text").toString();
            QVERIFY2(text == docs.at(i)->displayName() || text == QStringLiteral("●"),
                     qPrintable(QStringLiteral("unexpected Text in tab: %1")
                                    .arg(text)));
        }
    }

    // 全树兜底：DocumentTabs 子树（含菜单等）无 Canvas
    QList<QObject *> canvases;
    const std::function<void(QQuickItem *)> walk = [&](QQuickItem *it) {
        for (QQuickItem *kid : it->childItems()) {
            if (classNameStartsWith(kid, "QQuickCanvas"))
                canvases.append(kid);
            walk(kid);
        }
    };
    walk(b.tabs);
    QCOMPARE(canvases.size(), 0);
}

// TC-006a: 主菜单「文件 → 关闭标签页」直发 triggered()（tst_searchmenu
// 同模式）——干净文档直接关闭，无确认框。
void TestWin11Tabs::tc006a_closeViaMainMenu()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    Document *d1 = b.newDoc();
    Document *d2 = b.newDoc();
    QVERIFY(d1 && d2);
    b.dm->setActiveDocument(d2); // 主菜单关闭作用于活动文档
    b.settle();

    // 主菜单「关闭标签页」是自绘 KeyMenuItem（TitleBar 组件 KeyMenuItem:
    // MenuItem，短名不同），两类都收集后按文本定位
    QList<QObject *> items;
    collectByQmlType(b.win, "MenuItem", &items);
    collectByQmlType(b.win, "KeyMenuItem", &items);
    QObject *closeItem = nullptr;
    for (QObject *it : items) {
        if (it->property("text").toString() == QStringLiteral("关闭标签页"))
            closeItem = it;
    }
    QVERIFY2(closeItem, "main menu close-tab item found");
    QVERIFY(QMetaObject::invokeMethod(closeItem, "triggered"));
    b.settle();

    QCOMPARE(b.dm->documents().size(), 1);          // 关闭活动文档 d2
    QCOMPARE(b.dm->documents().first(), d1);
}

// TC-006b: Ctrl+W 合成按键关闭活动文档（窗口级 Shortcut 真实按键路径）。
void TestWin11Tabs::tc006b_closeViaCtrlW()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    Document *d1 = b.newDoc();
    Document *d2 = b.newDoc();
    QVERIFY(d1 && d2);
    QCOMPARE(b.dm->activeDocument(), d2);
    b.win->show();
    b.settle();

    QTest::keyClick(b.win, Qt::Key_W, Qt::ControlModifier);
    b.settle();

    QCOMPARE(b.dm->documents().size(), 1);
    QCOMPARE(b.dm->documents().first(), d1);
}

// TC-007: 未保存 ● 标记出现（modified 文档），干净文档无 ●。
void TestWin11Tabs::tc007_unsavedMark()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    Document *clean = b.newDoc();
    Document *dirty = b.newDoc(QStringLiteral("unsaved-content"));
    QVERIFY(clean && dirty);
    b.dm->setActiveDocument(clean);
    b.settle();

    const TabVisuals cleanV = collectTabVisuals(b.tabAt(0), clean->displayName());
    QVERIFY(cleanV.label != nullptr);
    QVERIFY(cleanV.unsavedMark == nullptr || !cleanV.unsavedMark->isVisible());

    const TabVisuals dirtyV = collectTabVisuals(b.tabAt(1), dirty->displayName());
    QVERIFY2(dirtyV.unsavedMark, "unsaved mark Text found");
    QVERIFY(dirtyV.unsavedMark->isVisible());
}

// TC-008: 自然宽度公式（文字 advance + 左右边距，无关闭按钮装饰）；
// 长名标签等比收缩；收缩饱和后退化为 minTabWidth 并溢出。
void TestWin11Tabs::tc008_naturalWidth_shrink_saturate()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");

    // --- 公式：干净文档（leftMarginPlain 10 + rightPadding 10）---
    Document *clean = b.newDoc();
    QVERIFY(clean != nullptr);
    b.settle();
    const qreal labelAdv = b.measureLabelAdvance(clean->displayName());
    QVERIFY(labelAdv >= 0);
    const QVariantList naturalClean = b.tabs->property("naturalWidths").toList();
    QCOMPARE(naturalClean.size(), 1);
    QVERIFY(qFuzzyCompare(naturalClean.at(0).toReal(),
                          labelAdv + 10 + 10));

    // --- 公式：未保存文档（+8 + ●宽 + 5 + 右 10）---
    Document *dirty = b.newDoc(QStringLiteral("x"));
    QVERIFY(dirty != nullptr);
    b.settle();
    QQmlComponent markComp(&b.engine);
    markComp.setData("import QtQuick\nTextMetrics { font.pixelSize: 9 }",
                     QUrl(QStringLiteral("mark.qml")));
    QVERIFY(!markComp.isError());
    std::unique_ptr<QObject> markTm(markComp.create());
    QVERIFY(markTm && markTm->setProperty("text", QStringLiteral("●")));
    const qreal markAdv = markTm->property("advanceWidth").toReal();
    // dirty 文档（"未命名-2"）用其自身 displayName 独立测量，
    // 不复用 clean 文档的 labelAdv（不同文字 advance 可能不同）
    const qreal dirtyLabelAdv = b.measureLabelAdvance(dirty->displayName());
    QVERIFY(dirtyLabelAdv >= 0);
    const QVariantList naturalDirty = b.tabs->property("naturalWidths").toList();
    QVERIFY(qFuzzyCompare(naturalDirty.at(1).toReal(),
                          dirtyLabelAdv + 8 + markAdv + 5 + 10));

    // --- 等比收缩：4 个长名标签在默认宽窗（1000）下部分收缩 ---
    for (int i = 0; i < 2; ++i)
        b.dm->closeDocument(b.dm->documents().last());
    b.settle();
    QList<Document *> longDocs;
    for (int i = 0; i < 4; ++i) {
        Document *d = b.openLongNameDoc(i);
        QVERIFY2(d != nullptr, "long-name doc opened");
        longDocs.append(d);
    }
    b.settle();
    b.win->show();
    b.win->resize(QSize(1000, 640));
    b.settle();

    const QVariantList natural = b.tabs->property("naturalWidths").toList();
    QCOMPARE(natural.size(), 4);
    const QVariantList dist = b.tabs->property("distributedWidths").toList();
    QCOMPARE(dist.size(), 4);
    qreal sum = 0;
    int shrunk = 0;
    for (int i = 0; i < 4; ++i) {
        const qreal w = dist.at(i).toReal();
        const qreal n = natural.at(i).toReal();
        QVERIFY2(w >= b.tabs->property("minTabWidth").toInt(),
                 "shrunk width >= minTabWidth");
        if (w < n - 0.5)
            ++shrunk;
        sum += w;
    }
    QVERIFY2(shrunk > 0, "at least one tab shrunk (deficit distributed)");
    const qreal gaps = b.tabs->property("tabSpacing").toInt() * (4 - 1);
    QVERIFY2(sum + gaps <= b.tabList->width() + 0.5,
             "distributed total fits available width");

    // --- 饱和：8 个长名标签 + 最窄窗（800）→ 全部 minTabWidth + 溢出 ---
    for (int i = 4; i < 8; ++i) {
        Document *d = b.openLongNameDoc(i);
        QVERIFY2(d != nullptr, "long-name doc opened (saturate)");
    }
    b.settle();
    b.win->resize(QSize(800, 600));
    b.settle();

    const QVariantList distSat = b.tabs->property("distributedWidths").toList();
    QCOMPARE(distSat.size(), 8);
    const int minW = b.tabs->property("minTabWidth").toInt();
    for (const QVariant &w : distSat)
        QCOMPARE(w.toInt(), minW);
    QCOMPARE(b.tabs->property("tabsOverflow").toBool(), true);
}

// TC-009: tabsOverflow 与 ListView interactive 联动（溢出可滚动 /
// 未溢出事件穿透，dragArea 拖拽不被吞——结构保证 + 走查）。
void TestWin11Tabs::tc009_overflow_interactiveToggle()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.win->show();
    b.win->resize(QSize(800, 600));

    // 未溢出（2 个短名标签）：不滚动、interactive 关（空白事件穿透）
    QVERIFY(b.newDoc() != nullptr);
    QVERIFY(b.newDoc() != nullptr);
    b.settle();
    QCOMPARE(b.tabs->property("tabsOverflow").toBool(), false);
    QCOMPARE(b.tabList->property("interactive").toBool(), false);

    // 溢出（8 个长名标签）：恢复滚动交互
    QList<Document *> keep;
    for (int i = 0; i < 8; ++i) {
        Document *d = b.openLongNameDoc(i);
        QVERIFY2(d != nullptr, "long-name doc opened");
        keep.append(d);
    }
    b.settle();
    QCOMPARE(b.tabs->property("tabsOverflow").toBool(), true);
    QCOMPARE(b.tabList->property("interactive").toBool(), true);
}

// TC-010: 键盘导航（左右循环）与焦点环（radius 4、活动/非活动底色上
// 均以 border 2 表达，不依赖底色）；Enter 激活对应文档。
void TestWin11Tabs::tc010_keyboardNav_focusRing()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QList<Document *> docs;
    for (int i = 0; i < 3; ++i)
        docs.append(b.newDoc());
    b.dm->setActiveDocument(docs.at(0));
    b.settle();
    b.win->show();

    // 聚焦标签区：focusIndex 对齐 activeIndex（0）
    QVERIFY(QMetaObject::invokeMethod(b.tabs, "forceActiveFocus"));
    b.settle();
    QCOMPARE(b.tabs->property("activeFocus").toBool(), true);
    QCOMPARE(b.tabs->property("focusIndex").toInt(), 0);

    // 右移两次：0 → 1 → 2；焦点环跟随且可见（radius 4 适配新形状）
    QTest::keyClick(b.win, Qt::Key_Right);
    b.settle();
    QCOMPARE(b.tabs->property("focusIndex").toInt(), 1);
    QQuickItem *del = b.tabAt(1);
    QVERIFY(del != nullptr);
    const TabVisuals v1 = collectTabVisuals(del, docs.at(1)->displayName());
    QVERIFY2(v1.focusRing, "focus ring found");
    QVERIFY(v1.focusRing->isVisible());
    QCOMPARE(v1.focusRing->property("radius").toReal(), 4.0);
    QCOMPARE(v1.focusRing->property("color").value<QColor>(),
             QColor(Qt::transparent)); // 仅描边框

    QTest::keyClick(b.win, Qt::Key_Right);
    b.settle();
    QCOMPARE(b.tabs->property("focusIndex").toInt(), 2);

    // 左移一次：2 → 1
    QTest::keyClick(b.win, Qt::Key_Left);
    b.settle();
    QCOMPARE(b.tabs->property("focusIndex").toInt(), 1);

    // 在焦点 0 时左移：循环到末尾（n-1）
    QTest::keyClick(b.win, Qt::Key_Left);
    QTest::keyClick(b.win, Qt::Key_Left);
    b.settle();
    QCOMPARE(b.tabs->property("focusIndex").toInt(), 2);

    // Enter 激活焦点文档（不改变顺序键路径结论）
    QTest::keyClick(b.win, Qt::Key_Return);
    b.settle();
    QCOMPARE(b.dm->activeDocument(), docs.at(2));
}

// TC-011: 标签文字右边距（几何：文字右缘距标签右缘 10px）；收缩态
// ElideRight 省略生效（truncated）。
void TestWin11Tabs::tc011_labelRightPadding_elide()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.win->show();

    // 完整显示态：未命名短名文档（低于单标签动态上限 W/4，不触发省略），
    // 文字右缘距标签右缘 == rightPadding(10)
    Document *doc = b.newDoc();
    QVERIFY(doc != nullptr);
    b.settle();

    QQuickItem *del = b.tabAt(0);
    QVERIFY(del != nullptr);
    const TabVisuals v = collectTabVisuals(del, doc->displayName());
    QVERIFY2(v.label, "label Text found");
    QCOMPARE(v.label->property("truncated").toBool(), false);
    const qreal labelRight =
        v.label->mapToItem(del, QPointF(v.label->width(), 0)).x();
    QVERIFY2(qFuzzyCompare(del->width() - labelRight, 10.0),
             qPrintable(QStringLiteral("right padding = %1 (expect 10)")
                            .arg(del->width() - labelRight)));

    // 收缩态（8 长名标签 + 最窄窗）：ElideRight 生效（省略号不贴右缘的
    // 间距由同一边距保证）。tabAt(0) 为未命名短名标签（可能不截断），
    // 省略断言取第一个长名标签（index 1，位于可视区内）
    for (int i = 0; i < 8; ++i)
        QVERIFY(b.openLongNameDoc(i) != nullptr);
    b.win->resize(QSize(800, 600));
    b.settle();
    del = b.tabAt(1);
    QVERIFY(del != nullptr);
    const QString longName = b.dm->documents().at(1)->displayName();
    const TabVisuals vShrunk = collectTabVisuals(del, longName);
    QVERIFY2(vShrunk.label, "label Text found (shrunk)");
    QCOMPARE(vShrunk.label->property("truncated").toBool(), true);
}

// TC-012: 标签间竖分隔线（DEC-103：复用 AppTheme.border；DEC-104：
// visible = index>0 && 非活动 && 左邻非活动，at() 无绑定依赖由 count
// 双依赖重算覆盖，活动切换经 isActive 依赖自动重算）。3 标签中间活动时
// 首标签 / 活动标签 / 活动右邻标签左线均不可见；切换活动后可见性即时
// 跟随；相邻两非活动标签之间线可见（贴左缘、宽 1、高 20 垂直居中、
// 色 AppTheme.border，线在标签内部不占宽度）。
void TestWin11Tabs::tc012_tabSeparator()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QList<Document *> docs;
    for (int i = 0; i < 3; ++i)
        docs.append(b.newDoc());
    b.dm->setActiveDocument(docs.at(1)); // 中间标签活动
    b.settle();

    // 结构前置：每个 delegate 都能定位到分隔线矩形（width==1）
    for (int i = 0; i < 3; ++i) {
        QQuickItem *del = b.tabAt(i);
        QVERIFY2(del, "delegate instantiated");
        const TabVisuals v =
            collectTabVisuals(del, docs.at(i)->displayName());
        QVERIFY2(v.separator, "tab separator rectangle found");
    }

    // 第 i 个标签左缘分隔线是否可见（displayName 仅影响 label 定位，
    // 分隔线定位不依赖，传空即可）
    const auto sepVisible = [&b](int i) -> bool {
        QQuickItem *del = b.tabAt(i);
        if (!del)
            return false;
        const TabVisuals v = collectTabVisuals(del, QString());
        return v.separator && v.separator->isVisible();
    };

    // 中间活动：首标签（index 0）无线；活动标签（凸起卡片）左线不画；
    // 右邻标签的左邻是活动 → 其左线不画（活动标签两侧均无线）
    QVERIFY2(!sepVisible(0), "first tab has no separator on its left");
    QVERIFY2(!sepVisible(1), "active tab has no separator");
    QVERIFY2(!sepVisible(2), "tab right of active has no separator");

    // 切活动到首标签 d0：唯一相邻非活动对 (d1,d2) 之间线出现（贴 d2 左缘）；
    // d1 左邻为活动 → 仍无线（活动标签右侧不画线）
    b.dm->setActiveDocument(docs.at(0));
    b.settle();
    QVERIFY2(!sepVisible(0), "first tab still has no separator");
    QVERIFY2(!sepVisible(1), "tab right of active still has no separator");
    QVERIFY2(sepVisible(2), "separator between inactive pair (1,2) visible");

    // 线的几何与颜色（取 tab2，当前唯一可见线）：贴标签左缘、宽 1、高 20
    // （≈标签高 34 的 60%）垂直居中、色 AppTheme.border（DEC-103）；
    // 线在标签内部左缘、不占宽度（宽度公式不动）
    QQuickItem *del = b.tabAt(2);
    QVERIFY(del != nullptr);
    const TabVisuals v2 = collectTabVisuals(del, docs.at(2)->displayName());
    QQuickItem *sep = v2.separator;
    QVERIFY(sep != nullptr);
    QCOMPARE(sep->property("width").toReal(), 1.0);
    QCOMPARE(b.tabs->property("tabSeparatorHeight").toInt(), 20);
    QCOMPARE(sep->property("height").toReal(), 20.0);
    QVERIFY2(sep->height() >= del->height() * 0.6 - 1.5
                 && sep->height() <= del->height() * 0.6 + 0.5,
             qPrintable(QStringLiteral("separator height %1 ≈ 60%% of %2")
                            .arg(sep->height()).arg(del->height())));
    QCOMPARE(sep->property("color").value<QColor>(),
             b.theme->property("border").value<QColor>());
    QCOMPARE(sep->mapToItem(del, QPointF(0.0, 0.0)).x(), 0.0); // 贴左缘
    QVERIFY2(qFuzzyCompare(sep->y() + sep->height() / 2.0,
                           del->height() / 2.0),
             "separator vertically centered in tab");

    // 切活动到末标签 d2：d2 左线即时消失（活动），d1 左线出现（自身与
    // 左邻 d0 均非活动）——可见性跟随活动切换
    b.dm->setActiveDocument(docs.at(2));
    b.settle();
    QVERIFY2(sepVisible(1),
             "separator appears when self and left neighbor both inactive");
    QVERIFY2(!sepVisible(2), "newly active tab's separator disappears");

    // 切回中间 d1：d1 线消失（活动）、d2 线消失（左邻为活动）
    b.dm->setActiveDocument(docs.at(1));
    b.settle();
    QVERIFY2(!sepVisible(1), "separator hidden when tab becomes active");
    QVERIFY2(!sepVisible(2), "separator hidden when left neighbor is active");
}

int main(int argc, char *argv[])
{
    // GUI 红线：自动化验证一律 offscreen，全程无可见窗口
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // 隔离首启右键菜单提示（与 tst_dialogstyle 同一模式）
    qputenv("QCNOTE_TEST_SHELL_NOOP", "1");
    // offscreen 平台指定系统字体
    static const char kWinFonts[] = "C:/Windows/Fonts";
    if (QDir(QString::fromLatin1(kWinFonts)).exists())
        qputenv("QT_QPA_FONTDIR", kWinFonts);
    QGuiApplication app(argc, argv);
    // 会话重定向到临时目录（不污染真实用户配置）
    sessionRedirectDir();
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       sessionRedirectDir().path());
    // 草稿缓存目录按测试 exe 名派生，预清理/结束清理
    const QString draftsDir = QStandardPaths::writableLocation(
                QStandardPaths::AppLocalDataLocation) + QStringLiteral("/drafts");
    QDir(draftsDir).removeRecursively();
    app.setQuitOnLastWindowClosed(false);
    // 与主程序一致的控件样式
    QQuickStyle::setStyle(QStringLiteral("Material"));
    TestWin11Tabs t;
    const int testResult = QTest::qExec(&t, argc, argv);
    QDir(draftsDir).removeRecursively();
    return testResult;
}

#include "tst_win11tabs.moc"
