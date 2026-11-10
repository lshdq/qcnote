// 窗口边界线 QML 集成测试（覆盖《测试用例-窗口边界线》TC-003/TC-004/
// TC-005 的可自动化部分；纯视觉观感（四边与桌面可辨、深浅主题观感）
// 与 TC-006 交互手感属肉眼验收，见测试报告"待用户肉眼验证"清单）。
//
// 测试方式：offscreen 平台下实例化完整 Main.qml，在 ApplicationWindow
// contentItem 子项中按特征（QQuickRectangle + z==999 + border.width==1
// + 透明填充）定位窗口边界描边层，断言：
// - TC-003（几何部分）：anchors.fill 窗口（宽高等于窗口尺寸）、
//   border.width == 1、填充透明、z == 999；AppTheme 的 windowBorder /
//   windowBorderInactive 两个 token 均为有效颜色且互不相同（深浅主题
//   观感本身需肉眼）；
// - TC-004：setVisibility(Maximized) 后描边层隐藏，还原 Windowed 后恢复；
// - TC-005（绑定一致性）：border.color 与 active 状态严格对应——
//   active 时等于 AppTheme.windowBorder，非 active 时等于
//   AppTheme.windowBorderInactive（offscreen 下无法合成窗口系统激活/
//   失焦，按当前 isActive() 分支断言绑定正确，两个 token 已由
//   tc003 验证可区分）；
// - TC-006（结构部分）：描边层不接收任何鼠标输入（acceptedMouseButtons
//   为空、无键盘焦点能力），且 z 低于全部 8 个边缘缩放命中区（z:1000）
//   ——命中区始终位于描边层之上，命中优先级不受影响。
//
// GUI 红线：main 中强制 QT_QPA_PLATFORM=offscreen，全程无可见窗口。

#include <QTest>
#include <QColor>
#include <QDir>
#include <QGuiApplication>
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

#include <functional>

// ==================== 测试台辅助 ====================

namespace {

// 进程级会话重定向目录（与 tst_dialogstyle 同一模式，避免污染真实配置）
QTemporaryDir &sessionRedirectDir()
{
    static QTemporaryDir dir;
    return dir;
}

/**
 * @brief 每个测试用例独立的测试台：完整实例化 Main.qml。
 */
struct Bench
{
    QQmlEngine engine;
    QQuickWindow *win = nullptr;        // Main.qml 根（ApplicationWindow）
    QQuickItem *border = nullptr;       // 窗口边界描边层（Rectangle, z:999）
    QObject *theme = nullptr;           // AppTheme QML 单例
    QList<QQuickItem *> resizeHandles;  // 边缘缩放命中区（z:1000）

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
        theme = engine.singletonInstance<QObject *>(u"qcnote", u"AppTheme");
        if (!theme)
            return false;

        // 边界描边层：全树递归查找 z==999、border.width==1 的 QQuickRectangle
        //（Main.qml 中唯一满足该特征的项；位于 ApplicationWindow 内容控件之下，
        // 故从窗口 contentItem 递归搜索）
        QList<QQuickItem *> candidates;
        const std::function<void(QQuickItem *)> collect = [&](QQuickItem *it) {
            if (QString::fromLatin1(it->metaObject()->className())
                    .startsWith(QStringLiteral("QQuickRectangle"))
                && qFuzzyCompare(it->z(), 999.0)) {
                const QVariant bw = it->property("border");
                if (bw.isValid() && bw.canConvert<QObject *>()) {
                    // border 分组属性（QQuickPen，QObject 派生）取宽度
                    const QObject *grp = bw.value<QObject *>();
                    if (grp && qFuzzyCompare(grp->property("width").toReal(), 1.0))
                        candidates.append(it);
                }
            }
            for (QQuickItem *k : it->childItems())
                collect(k);
        };
        collect(win->contentItem());
        if (candidates.size() != 1) {
            qWarning() << "[bench] border candidates:" << candidates.size();
            return false;
        }
        border = candidates.first();

        // 边缘缩放命中区：递归查找 QML 类型短名 ResizeHandle 且 z==1000
        // 的项（四边 + 四角共 8 个，与描边层同层级）
        const std::function<void(QObject *)> collectHandles = [&](QObject *o) {
            const QString cn = QString::fromLatin1(o->metaObject()->className());
            const int cut = cn.indexOf(u'_');
            const QString shortName = cut > 0 ? cn.left(cut) : cn;
            if (shortName == QLatin1String("ResizeHandle")) {
                if (auto *it = qobject_cast<QQuickItem *>(o);
                        it && qFuzzyCompare(it->z(), 1000.0))
                    resizeHandles.append(it);
            }
            for (QObject *k : o->children())
                collectHandles(k);
        };
        collectHandles(win);
        return true;
    }
};

} // namespace

class TestWindowBorder : public QObject
{
    Q_OBJECT

private slots:
    // TC-003（几何部分）: 描边层铺满窗口、1px、透明填充、z:999；
    // 主题 token 有效且互不相同
    void tc003_borderGeometry_tokens();

    // TC-004: 最大化隐藏边线，还原 Windowed 恢复
    void tc004_maximizedHides_border();

    // TC-005（绑定一致性）: border.color 与 active 状态严格对应
    void tc005_colorFollows_activeState();

    // TC-006（结构部分）: 描边层无输入接收、低于全部缩放命中区
    void tc006_noInputInterception_belowResizeHandles();
};

// TC-003: anchors.fill: parent → 尺寸等于窗口；border.width 1、填充
// 透明、z 999；windowBorder / windowBorderInactive 有效且互不相同。
void TestWindowBorder::tc003_borderGeometry_tokens()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");

    // 铺满窗口（anchors.fill: parent，等一轮布局）
    QTest::qWait(50);
    QCOMPARE(b.border->width(), b.win->width());
    QCOMPARE(b.border->height(), b.win->height());

    // 1px 描边、透明填充、z 999
    const QObject *grp = b.border->property("border").value<QObject *>();
    QVERIFY(grp);
    QCOMPARE(grp->property("width").toReal(), 1.0);
    QCOMPARE(b.border->property("color").value<QColor>(), QColor(Qt::transparent));

    // 主题 token：两个语义色均为有效颜色且互不相同（深浅主题观感需肉眼）
    const QColor active = b.theme->property("windowBorder").value<QColor>();
    const QColor inactive = b.theme->property("windowBorderInactive").value<QColor>();
    QVERIFY(active.isValid());
    QVERIFY(inactive.isValid());
    QVERIFY(active != inactive);
}

// TC-004: visibility → Maximized 描边层隐藏；还原 Windowed 恢复显示。
void TestWindowBorder::tc004_maximizedHides_border()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");

    b.win->show();
    b.win->setVisibility(QWindow::Windowed);
    QTRY_COMPARE(b.win->visibility(), QWindow::Windowed);
    QVERIFY2(b.border->isVisible(), "border must be visible in Windowed state");

    b.win->setVisibility(QWindow::Maximized);
    QTRY_COMPARE(b.win->visibility(), QWindow::Maximized);
    QTRY_VERIFY2(!b.border->isVisible(),
                 "border must hide when maximized");

    b.win->setVisibility(QWindow::Windowed);
    QTRY_COMPARE(b.win->visibility(), QWindow::Windowed);
    QTRY_VERIFY2(b.border->isVisible(), "border must restore when back to Windowed");
}

// TC-005: border.color 与 active 状态严格对应（offscreen 无法合成窗口
// 激活/失焦，按当前 isActive() 分支断言绑定一致性；两个 token 的区分
// 已由 tc003 保证）。
void TestWindowBorder::tc005_colorFollows_activeState()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QTest::qWait(50);

    const QColor actual = b.border->property("border").value<QObject *>()
                              ->property("color").value<QColor>();
    const QColor expected = b.win->isActive()
        ? b.theme->property("windowBorder").value<QColor>()
        : b.theme->property("windowBorderInactive").value<QColor>();
    QCOMPARE(actual, expected);
}

// TC-006（结构部分）: 描边层无输入接收（不接受任何鼠标按键、无键盘
// 焦点能力），且 z 低于全部 8 个边缘缩放命中区——命中区位于描边层
// 之上，边缘缩放命中优先级不受影响（拖拽手感需肉眼）。
void TestWindowBorder::tc006_noInputInterception_belowResizeHandles()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");

    // 无输入接收：不接受任何鼠标按键、不可获键盘焦点
    QCOMPARE(b.border->acceptedMouseButtons(), Qt::NoButton);
    QVERIFY(!b.border->property("activeFocus").toBool());
    QVERIFY(!(b.border->flags() & QQuickItem::ItemAcceptsInputMethod));

    // 缩放命中区：四边 + 四角共 8 个，全部 z:1000 > 描边层 z:999
    QCOMPARE(b.resizeHandles.size(), 8);
    for (QQuickItem *h : b.resizeHandles)
        QVERIFY2(h->z() > b.border->z(),
                 qPrintable(QString("resize handle z=%1 must exceed border z=%2")
                                .arg(h->z()).arg(b.border->z())));
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
    // 会话重定向到临时目录，避免污染真实用户配置
    sessionRedirectDir();
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       sessionRedirectDir().path());
    // 草稿缓存目录按测试 exe 名派生，预清理/结束清理（与 tst_dialogstyle
    // 同一模式，避免跨运行残留干扰及在 %LOCALAPPDATA% 积累测试产物）
    const QString draftsDir = QStandardPaths::writableLocation(
                QStandardPaths::AppLocalDataLocation) + QStringLiteral("/drafts");
    QDir(draftsDir).removeRecursively();
    app.setQuitOnLastWindowClosed(false);
    // 与主程序一致的控件样式
    QQuickStyle::setStyle(QStringLiteral("Material"));
    TestWindowBorder t;
    const int testResult = QTest::qExec(&t, argc, argv);
    QDir(draftsDir).removeRecursively();
    return testResult;
}

#include "tst_windowborder.moc"
