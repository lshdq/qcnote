// 对话框样式与弹窗拖动 QML 集成测试（覆盖两份测试用例文档的可自动化
// 部分：《测试用例-对话框居中与按钮Windows化》TC-001/002/004/005/006/
// 007/008，弹窗拖动需求引入的《测试用例-弹窗拖动》TC-002/003/004/006/
// 008/009）。
//
// 测试方式：offscreen 平台下实例化完整 Main.qml，按 QML 类型短名
// MovableDialog + dialogTitle 属性定位五个对话框（unsavedDialog /
// errorDialog / ctxMenuPromptDialog / reloadConfirmDialog /
// draftConflictDialog），断言：
// - 默认位置：MovableDialog 的 x/y 绑定居中（与窗口中心差 ≤1px），
//   未拖动时 resize 后跟随；
// - 拖动（弹窗拖动需求）：合成鼠标（左键 press → 分步 move → release，
//   直投窗口走 DragHandler 真实抓取路径）拖 header 把手，x/y 精确位移；
//   四方向超界拖动被 clamp（把手不丢失）；关闭重开复位默认位置；拖动后
//   窗口 resize 重新 clamp；SearchDialog 拖动后查找/替换功能正常；
// - footer：DialogButtonBox 为 WinLayout、右对齐，按钮自左向右为
//   保存/不保存/取消（unsaved）、确定（error）、添加/不添加（ctxPrompt），
//   primary 标记正确，「放弃」不再出现；
// - closePolicy == Popup.CloseOnEscape（属性断言 + 合成鼠标点击
//   对话框外部遮罩不关闭的行为断言）；
// - 键盘路径：Esc 取消中止；Enter（保存按钮持有焦点）触发保存分支；
// - 正文文案精确为「是否要将更改保存到 <displayName>？」；
// - 打开失败真实链路：errorDialog 弹出居中，队列逐个展示。
//
// 用例分派：弹窗拖动 TC-001（默认位置）由本文件 tc001 覆盖；TC-005
// （未拖动 resize 跟随）由 tc001 的 resize 段覆盖；TC-007（模态回归）
// 由既有 tc004/tc002/tc005 覆盖；TC-008（键盘路径）的 Enter/Esc 分支
// 由既有 tc005 覆盖、"拖拽把手不进入焦点链"由 drag008 覆盖；TC-010
// 由 ctest 总体覆盖；
// TC-011（人工视觉验收）无法 offscreen 覆盖，见测试报告"未执行"清单。
// 弹窗拖动的 TC-012（skill 文档验证）不属本目标。
//
// 对象定位说明：与 tst_windowclose 相同，按 QML 类型短名匹配；Main.qml
// 五对话框经 MovableDialog（继承 Dialog，自绘 header 把手）接入，实例
// 类型短名为 "MovableDialog"，标题属性为 dialogTitle（Dialog.title 保持
// 空）；全部按固定标题精确匹配定位，其中 errorDialog 的 dialogTitle
// 绑定 currentError、未打开时为空串，按空串标题显式识别。SearchDialog.qml 根
// 类型同为 MovableDialog，但实例类型短名仍为文件组件名 "SearchDialog"。
// ctxMenuPromptDialog 的首启真实链路被 QCNOTE_TEST_SHELL_NOOP 隔离
// （contextMenuPromptNeeded 恒 false），采用直接 open() 的等价模式。
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

#include <algorithm>

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

// ---- Qt 枚举数值常量（避免依赖 Qt Quick Controls 非公开头文件） ----

// QQuickPopup::ClosePolicy：Popup.CloseOnEscape == 0x10。
// 仅 Esc 关闭：不含 CloseOnPressOutside（0x01）/ CloseOnReleaseOutside（0x04）
// 等任何"点击对话框外部即关闭"的位
constexpr int kCloseOnEscape = 0x10;
// QQuickDialogButtonBox::DialogButtonLayout 首个枚举值 WinLayout == 0
constexpr int kWinLayout = 0;
// Qt::AlignRight == 0x2
constexpr int kAlignRight = 0x2;

/**
 * @brief QML 类型短名精确匹配（与 tst_windowclose 同一模式）。
 *
 * Qt 6.5+ Controls 控件实例类名为 QML 包装类型（如 "Dialog_QMLTYPE_13"），
 * 取首个下划线前段与目标比较。
 */
bool qmlTypeNameIs(const QObject *obj, const char *name)
{
    const QString cn = QString::fromLatin1(obj->metaObject()->className());
    const int cut = cn.indexOf(u'_');
    return (cut > 0 ? cn.left(cut) : cn) == QLatin1String(name);
}

// 找到 QML 类型短名为 name 的全部 QObject 子孙（含 Popup/Dialog 非可视对象）
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

// 确认框正文（Main.qml 中 contentItem 为 Label，取其 text 属性）
QString dialogText(QObject *dlg)
{
    const QVariant ci = dlg->property("contentItem");
    if (!ci.canConvert<QObject *>())
        return QString();
    QObject *label = ci.value<QObject *>();
    return label ? label->property("text").toString() : QString();
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
    QObject *unsavedDlg = nullptr;      // unsavedDialog（标题「未保存的修改」）
    QObject *errDlg = nullptr;          // errorDialog（dialogTitle 绑定 currentError，未打开为空）
    QObject *ctxPromptDlg = nullptr;    // ctxMenuPromptDialog（标题「添加右键菜单」）
    QObject *reloadDlg = nullptr;       // reloadConfirmDialog（标题「按编码重新加载」）
    QObject *draftDlg = nullptr;        // draftConflictDialog（标题「恢复未保存的修改」）
    QObject *searchDlg = nullptr;       // SearchDialog（DEC-002 顶部对齐，drag009 用）
    QQuickItem *editorArea = nullptr;   // EditorArea（查找控制器持有者，drag009 用）

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

        // 五个对话框按 QML 类型短名 MovableDialog + dialogTitle 属性定位；
        // 四个按固定标题精确匹配，errorDialog 的 dialogTitle 绑定
        // currentError、未打开时为空串，按空串标题显式识别
        QList<QObject *> dialogs;
        collectByQmlType(win, "MovableDialog", &dialogs);
        for (QObject *d : dialogs) {
            const QString t = d->property("dialogTitle").toString();
            if (t == QStringLiteral("未保存的修改"))
                unsavedDlg = d;
            else if (t == QStringLiteral("添加右键菜单"))
                ctxPromptDlg = d;
            else if (t == QStringLiteral("按编码重新加载"))
                reloadDlg = d;
            else if (t == QStringLiteral("恢复未保存的修改"))
                draftDlg = d;
            else if (t.isEmpty())
                errDlg = d;
        }
        if (dialogs.size() != 5 || !unsavedDlg || !errDlg || !ctxPromptDlg
            || !reloadDlg || !draftDlg) {
            qWarning() << "[bench] dialogs found:" << dialogs.size()
                       << "unsaved:" << (unsavedDlg != nullptr)
                       << "err:" << (errDlg != nullptr)
                       << "ctxPrompt:" << (ctxPromptDlg != nullptr)
                       << "reload:" << (reloadDlg != nullptr)
                       << "draft:" << (draftDlg != nullptr);
            return false;
        }

        // SearchDialog（独立文件组件，类型短名 "SearchDialog"）与 EditorArea
        QList<QObject *> sd;
        collectByQmlType(win, "SearchDialog", &sd);
        searchDlg = sd.isEmpty() ? nullptr : sd.first();
        QList<QObject *> ea;
        collectByQmlType(win, "EditorArea", &ea);
        editorArea = ea.isEmpty() ? nullptr : qobject_cast<QQuickItem *>(ea.first());
        if (!searchDlg || !editorArea) {
            qWarning() << "[bench] missing:" << (!searchDlg ? "searchDlg" : "")
                       << (!editorArea ? "editorArea" : "");
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

// 对话框 footer 的 DialogButtonBox（Main.qml 三对话框的 footer 均为该类型）
QObject *footerBox(QObject *dlg)
{
    const QVariant f = dlg->property("footer");
    return f.canConvert<QObject *>() ? f.value<QObject *>() : nullptr;
}

// 按钮盒内全部 DialogButton（QML 声明树序）
QList<QQuickItem *> boxButtons(QObject *box)
{
    QList<QObject *> objs;
    collectByQmlType(box, "DialogButton", &objs);
    QList<QQuickItem *> items;
    for (QObject *o : objs)
        if (auto *it = qobject_cast<QQuickItem *>(o))
            items.append(it);
    return items;
}

// 按文本取按钮盒内首个匹配按钮
QQuickItem *buttonByText(QObject *box, const QString &text)
{
    for (QQuickItem *it : boxButtons(box)) {
        if (it->property("text").toString() == text)
            return it;
    }
    return nullptr;
}

// 按钮自左向右的文本（按场景坐标 x 排序，验证 WinLayout 下的实际视觉顺序）
QStringList buttonsLeftToRight(QObject *box)
{
    QList<QQuickItem *> bs = boxButtons(box);
    std::sort(bs.begin(), bs.end(), [](QQuickItem *a, QQuickItem *b) {
        return a->mapToScene(QPointF()).x() < b->mapToScene(QPointF()).x();
    });
    QStringList out;
    for (QQuickItem *b : bs)
        out << b->property("text").toString();
    return out;
}

/**
 * @brief 居中断言：|dlg.x - (win.width-dlg.width)/2| ≤ 1 且 y 同理。
 *
 * Popup 的 x/y 相对其 parentItem；Dialog 直接作为 ApplicationWindow 子项
 * 时 parent 解析为窗口 contentItem（与窗口同尺寸），故与窗口尺寸直接换算。
 */
bool isCentered(QQuickWindow *win, QObject *dlg, QString *why = nullptr)
{
    const qreal x = dlg->property("x").toReal();
    const qreal y = dlg->property("y").toReal();
    const qreal w = dlg->property("width").toReal();
    const qreal h = dlg->property("height").toReal();
    const qreal dx = x - (win->width() - w) / 2.0;
    const qreal dy = y - (win->height() - h) / 2.0;
    if (qFabs(dx) > 1.0 || qFabs(dy) > 1.0) {
        if (why)
            *why = QString("dx=%1 dy=%2 (win=%3x%4 dlg@(%5,%6) %7x%8)")
                       .arg(dx).arg(dy)
                       .arg(win->width()).arg(win->height())
                       .arg(x).arg(y).arg(w).arg(h);
        return false;
    }
    return true;
}

// 合成鼠标点击窗口内对话框外部坐标（QTest::mouseClick 经
// QWindowSystemInterface::handleMouseEvent 模拟真实窗口系统投递，
// 走 QQuickPopup 对遮罩点击的真实派发路径，验证 closePolicy 行为）
void clickOutside(QQuickWindow *win, qreal x, qreal y)
{
    QTest::mouseClick(win, Qt::LeftButton, Qt::NoModifier, QPoint(qRound(x), qRound(y)));
}

// ---- 弹窗拖动（《测试用例-弹窗拖动》）辅助 ----

// MovableDialog 自绘 header 把手（Dialog.header 属性，QQuickItem）
QQuickItem *dialogHeader(QObject *dlg)
{
    const QVariant h = dlg->property("header");
    return h.canConvert<QQuickItem *>() ? h.value<QQuickItem *>() : nullptr;
}

// header 把手中心的窗口坐标（mapToScene：Popup 内容项以窗口为场景），
// 钳制在窗口内：弹窗右/下越出窗口的部分不可接收事件，取把手在窗口内
// 的可见部分（弹窗被 clamp 后把手中心可能位于窗口外）
QPointF headerGrabPoint(QQuickWindow *win, QObject *dlg)
{
    QQuickItem *hdr = dialogHeader(dlg);
    if (!hdr || hdr->width() <= 0 || hdr->height() <= 0)
        return QPointF();
    QPointF c = hdr->mapToScene(QPointF(hdr->width() / 2.0, hdr->height() / 2.0));
    c.setX(qBound(10.0, c.x(), win->width() - 10.0));
    c.setY(qBound(10.0, c.y(), win->height() - 10.0));
    return c;
}

// 合成鼠标拖动（左键）：press 于 from → 分步 move 至 to → release。
// 必须经 QTest::mouse*（内部走 QWindowSystemInterface::handleMouseEvent，
// 模拟窗口系统投递且 mouseMove 携带 mousePress 登记的按键状态）：
// DragHandler 的 move 送达依赖该路径的点状态跟踪（pressPosition /
// 抓取簿记），直接 sendEvent 自构 QMouseEvent 无法驱动 move 送达
void mouseDrag(QQuickWindow *win, const QPointF &from, const QPointF &to, int steps = 8)
{
    QTest::mousePress(win, Qt::LeftButton, Qt::NoModifier, from.toPoint());
    for (int i = 1; i <= steps; ++i) {
        const QPointF p = from + (to - from) * (i / qreal(steps));
        QTest::mouseMove(win, p.toPoint());
        QTest::qWait(10);
    }
    QTest::mouseRelease(win, Qt::LeftButton, Qt::NoModifier, to.toPoint());
    QTest::qWait(10);
}

} // namespace

class TestDialogStyle : public QObject
{
    Q_OBJECT

private slots:
    // TC-001: 未保存确认框居中显示（初始几何与 resize 后均居中）
    void tc001_unsavedDialog_centered();

    // TC-002: 按钮顺序、命名与对齐符合 Windows 惯例
    void tc002_footerButtons_layoutNamingAlignment();

    // TC-004: 点击对话框外部不关闭（closePolicy 属性 + 行为断言）
    void tc004_closePolicy_escapeOnly();

    // TC-005: Esc 取消 / Enter 保存（真实键盘路径，两个子场景拆分为
    //         两个用例：tc005_escCancel / tc005_enterSave）
    void tc005_escCancel();

    void tc005_enterSave();

    // TC-006: 正文文案为「是否要将更改保存到 <displayName>？」
    void tc006_bodyTextFormat();

    // TC-007: 错误提示框居中与「确定」按钮（真实打开失败链路 + 排队展示）
    void tc007_errorDialog_centeredOkQueue();

    // TC-008: 右键菜单首启提示居中与按钮（NOOP 隔离下直接 open 等价驱动）
    void tc008_ctxPrompt_centeredButtons();

    // GBK 编码支持 TC-018 关联：按编码重载确认框的样式惯例
    // （居中、WinLayout、按钮 重新加载(primary)/取消）与正文文案
    void tc010_reloadConfirm_layoutText();

    // 「未保存修改临时缓存」需求：草稿冲突汇总对话框（draftConflictDialog）
    // 的基本风格用例：居中、modal、宽 440、closePolicy 仅 Esc、footer
    // WinLayout、按钮 恢复（primary）/丢弃
    void tc011_draftConflict_layoutButtons();

    // ---- 弹窗拖动（《测试用例-弹窗拖动》可自动化部分）----

    // TC-002: header 把手合成鼠标拖动 → x/y 精确位移（±1px）
    void drag002_headerDrag_movesDialog();

    // TC-003: 四方向超界拖动被 clamp（把手不丢失，不等式成立）
    void drag003_clampFourDirections();

    // TC-004: 拖离默认位置后关闭重开 → 复位默认位置（居中）
    void drag004_reopen_resetsPosition();

    // TC-006: 拖到边界后窗口 resize → 重新 clamp（不等式仍成立）
    void drag006_resizeAfterDrag_reclamps();

    // TC-008: 拖拽把手不进入焦点链：Tab/Shift+Tab 遍历中 header 始终
    //         不持有焦点，模态焦点不逃逸出弹窗
    void drag008_focusChain_handleNotInTabChain();

    // TC-009: SearchDialog 拖动后查找/替换功能正常（输入实时查找、Enter
    //         跳转、Esc 关闭、重开复位 y=96）
    void drag009_searchDialog_afterDrag();
};

// TC-001: 有未保存修改触发关闭 → 确认框弹出且居中；窗口 resize 后保持居中。
void TestDialogStyle::tc001_unsavedDialog_centered()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("dirty"));

    QVERIFY(!b.win->close());                                  // 关闭被拒，弹确认框
    QTRY_VERIFY(b.unsavedDlg->property("visible").toBool());

    // 初始几何（1000x640，Windowed 默认）下居中（等一轮布局 polish）
    QString why;
    QTest::qWait(50);
    QVERIFY2(isCentered(b.win, b.unsavedDlg, &why), qPrintable(why));

    // resize 后仍保持居中（未拖动：x/y 绑定跟随窗口尺寸变化）
    b.win->resize(QSize(1200, 760));
    QTRY_VERIFY2(isCentered(b.win, b.unsavedDlg, &why), qPrintable(why));

    b.win->resize(QSize(1000, 640));
    QTRY_VERIFY2(isCentered(b.win, b.unsavedDlg, &why), qPrintable(why));
}

// TC-002: footer 按钮自左向右为 保存/不保存/取消，右对齐，WinLayout；
// 保存为 primary 默认按钮；「放弃」不再出现。
void TestDialogStyle::tc002_footerButtons_layoutNamingAlignment()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("dirty"));

    QVERIFY(!b.win->close());
    QTRY_VERIFY(b.unsavedDlg->property("visible").toBool());
    QTest::qWait(50); // 等按钮布局 polish，取视觉坐标

    QObject *box = footerBox(b.unsavedDlg);
    QVERIFY2(box, "footer DialogButtonBox not found");

    // 显式 WinLayout（覆盖 Material 风格默认的 AndroidLayout）
    QCOMPARE(box->property("buttonLayout").toInt(), kWinLayout);
    // 按钮组右对齐（Windows 惯例紧凑右对齐，而非拉伸填满整行）
    QCOMPARE(box->property("alignment").toInt(), kAlignRight);

    // 自左向右：保存 / 不保存 / 取消；「放弃」不再出现
    const QStringList names = buttonsLeftToRight(box);
    QCOMPARE(names, QStringList({ QStringLiteral("保存"),
                                  QStringLiteral("不保存"),
                                  QStringLiteral("取消") }));
    QVERIFY(!names.contains(QStringLiteral("放弃")));

    // primary 标记：仅保存为 accent 填充默认按钮
    QVERIFY(buttonByText(box, QStringLiteral("保存"))->property("primary").toBool());
    QVERIFY(!buttonByText(box, QStringLiteral("不保存"))->property("primary").toBool());
    QVERIFY(!buttonByText(box, QStringLiteral("取消"))->property("primary").toBool());
}

// TC-004: 五个对话框 closePolicy 均为 Popup.CloseOnEscape；
// 合成鼠标点击对话框外部遮罩 → 不关闭、退出流程未中止、内容保留。
void TestDialogStyle::tc004_closePolicy_escapeOnly()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");

    // 属性断言：仅 Esc 关闭，不含任何点击外部关闭的位
    for (QObject *dlg : { b.unsavedDlg, b.errDlg, b.ctxPromptDlg, b.reloadDlg,
                          b.draftDlg }) {
        QVERIFY(dlg);
        QCOMPARE(dlg->property("closePolicy").toInt(), kCloseOnEscape);
    }

    // 行为断言：点击对话框外部（遮罩区域）不关闭
    Document *doc = b.newDoc(QStringLiteral("dirty"));
    QVERIFY(!b.win->close());
    QTRY_VERIFY(b.unsavedDlg->property("visible").toBool());
    QTest::qWait(50); // 等布局就位，确保点击坐标在对话框边界外

    // 窗口顶部中央：对话框（宽 380、垂直居中）之外，且避开 12px 边缘缩放命中区
    clickOutside(b.win, b.win->width() / 2.0, 20);
    QTest::qWait(150);
    QVERIFY(b.unsavedDlg->property("visible").toBool()); // 不关闭
    QVERIFY(b.win->isVisible());                         // 退出流程未中止
    QVERIFY(b.dm->documents().contains(doc));            // 内容保留
    QCOMPARE(doc->content(), QStringLiteral("dirty"));
}

// TC-005（Esc 部分）: Esc（closePolicy CloseOnEscape 的键盘路径）→ 取消：
// 框关、流程中止、内容保留；再次关闭重新弹确认框（流程可重入）。
void TestDialogStyle::tc005_escCancel()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("esc.txt"));

    Document *doc = b.newDoc(QStringLiteral("v1"));
    QVERIFY(b.dm->saveAs(doc, QUrl::fromLocalFile(path))); // 有路径、清脏
    doc->setContent(QStringLiteral("v2"));                 // 重新置脏
    QVERIFY(doc->modified());

    QVERIFY(!b.win->close());
    QTRY_VERIFY(b.unsavedDlg->property("visible").toBool());

    QTest::keyClick(b.win, Qt::Key_Escape);
    QTRY_VERIFY(!b.unsavedDlg->property("visible").toBool()); // 框关闭
    QVERIFY(b.win->isVisible());                              // 应用继续运行
    QCOMPARE(doc->content(), QStringLiteral("v2"));           // 内容保留
    QVERIFY(b.dm->documents().contains(doc));

    // 流程可重入：再次关闭重新弹确认框
    QVERIFY(!b.win->close());
    QTRY_VERIFY(b.unsavedDlg->property("visible").toBool());
}

// TC-005（Enter 部分）: Enter（「保存」按钮持有焦点）→ 保存分支：写盘后
// 关闭退出。
// 历史缺陷（Qt 源码级根因，已修复）：QQuickAbstractButton 的键盘激活键
// 来自平台主题 ButtonPressKeys（QPlatformTheme 默认仅 Space/Select，
// QWindowsTheme::themeHint 未覆盖该提示），Enter/Return 不在激活键内——
// 按钮持有焦点按 Enter 无效果（对照实验：同一焦点下按 Space 可正常触发
// 保存写盘退出）。已由 DialogButton.qml 组件内显式处理 Return/Enter
// （触发 click()，补齐 Windows 对话框回车惯例）修复，本用例现为常绿
// 回归保护。
void TestDialogStyle::tc005_enterSave()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("enter.txt"));

    Document *doc = b.newDoc(QStringLiteral("v1"));
    QVERIFY(b.dm->saveAs(doc, QUrl::fromLocalFile(path))); // 有路径、清脏
    doc->setContent(QStringLiteral("v2"));                 // 重新置脏
    QVERIFY(doc->modified());

    QVERIFY(!b.win->close());
    QTRY_VERIFY(b.unsavedDlg->property("visible").toBool());

    QQuickItem *saveBtn = buttonByText(footerBox(b.unsavedDlg), QStringLiteral("保存"));
    QVERIFY(saveBtn);
    QVERIFY(QMetaObject::invokeMethod(saveBtn, "forceActiveFocus"));
    QTest::keyClick(b.win, Qt::Key_Return);
    QTRY_VERIFY(!b.win->isVisible()); // 保存成功 → 无其他脏文档 → 退出

    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArrayLiteral("v2")); // 磁盘内容为保存值
}

// TC-006: 正文精确为「是否要将更改保存到 <displayName>？」（含文档名）。
void TestDialogStyle::tc006_bodyTextFormat()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    Document *doc = b.newDoc(QStringLiteral("dirty"));

    QVERIFY(!b.win->close());
    QTRY_VERIFY(b.unsavedDlg->property("visible").toBool());

    QCOMPARE(dialogText(b.unsavedDlg),
             QStringLiteral("是否要将更改保存到 %1？").arg(doc->displayName()));
}

// TC-007: 打开不存在文件（真实 openFailed 链路）→ 错误框弹出且居中、
// 单个「确定」primary 按钮、WinLayout；错误框可见时后续错误入队，
// 「确定」后逐个展示，队列清空后关闭。
void TestDialogStyle::tc007_errorDialog_centeredOkQueue()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath(QStringLiteral("not-exist-a.txt"));
    const QString c = dir.filePath(QStringLiteral("not-exist-c.txt"));
    QVERIFY(!QFile::exists(a));
    QVERIFY(!QFile::exists(c));

    // 真实链路：openUrls 失败 → openFailed → queueError → 错误框弹出
    b.dm->openUrls({ QUrl::fromLocalFile(a) });
    QTRY_VERIFY(b.errDlg->property("visible").toBool());
    QCOMPARE(b.errDlg->property("dialogTitle").toString(), QStringLiteral("打开失败"));
    QVERIFY(dialogText(b.errDlg).contains(a));

    // 居中 + footer 单「确定」primary 按钮 + WinLayout
    QTest::qWait(50);
    QString why;
    QVERIFY2(isCentered(b.win, b.errDlg, &why), qPrintable(why));
    QObject *box = footerBox(b.errDlg);
    QVERIFY2(box, "errorDialog footer DialogButtonBox not found");
    QCOMPARE(box->property("buttonLayout").toInt(), kWinLayout);
    QCOMPARE(buttonsLeftToRight(box), QStringList({ QStringLiteral("确定") }));
    QVERIFY(buttonByText(box, QStringLiteral("确定"))->property("primary").toBool());

    // 错误框可见时第二个错误入队（不覆盖当前展示）
    b.dm->openUrls({ QUrl::fromLocalFile(c) });
    QVERIFY(dialogText(b.errDlg).contains(a)); // 当前仍展示第一个错误

    // 「确定」→ 继续展示队列中的下一个错误
    QVERIFY(QMetaObject::invokeMethod(b.errDlg, "accept"));
    QTRY_VERIFY(b.errDlg->property("visible").toBool());
    QVERIFY(dialogText(b.errDlg).contains(c));

    // 队列清空后再「确定」→ 框关闭
    QVERIFY(QMetaObject::invokeMethod(b.errDlg, "accept"));
    QTRY_VERIFY(!b.errDlg->property("visible").toBool());
}

// TC-008: 右键菜单首启提示居中；按钮为 添加（primary）/ 不添加；
// 点击外部不关闭。首启真实链路被 QCNOTE_TEST_SHELL_NOOP 隔离
// （contextMenuPromptNeeded 恒 false），按 tst_windowclose 对 errDlg 的
// 同模式直接 open() 等价驱动。
void TestDialogStyle::tc008_ctxPrompt_centeredButtons()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");

    QVERIFY(QMetaObject::invokeMethod(b.ctxPromptDlg, "open"));
    QTRY_VERIFY(b.ctxPromptDlg->property("visible").toBool());
    QTest::qWait(50);

    // 居中
    QString why;
    QVERIFY2(isCentered(b.win, b.ctxPromptDlg, &why), qPrintable(why));

    // 按钮：添加（primary）/ 不添加；WinLayout
    QObject *box = footerBox(b.ctxPromptDlg);
    QVERIFY2(box, "ctxMenuPromptDialog footer DialogButtonBox not found");
    QCOMPARE(box->property("buttonLayout").toInt(), kWinLayout);
    QCOMPARE(buttonsLeftToRight(box),
             QStringList({ QStringLiteral("添加"), QStringLiteral("不添加") }));
    QVERIFY(buttonByText(box, QStringLiteral("添加"))->property("primary").toBool());
    QVERIFY(!buttonByText(box, QStringLiteral("不添加"))->property("primary").toBool());

    // 点击外部不关闭（closePolicy 行为断言）
    clickOutside(b.win, b.win->width() / 2.0, 20);
    QTest::qWait(150);
    QVERIFY(b.ctxPromptDlg->property("visible").toBool());
}

// GBK 编码支持（《测试用例-GBK与UTF8编码支持》TC-018 关联）：按编码
// 重载确认框样式与 Windows 按钮惯例一致：居中、WinLayout、自左向右
// 重新加载（primary）/取消，正文文案含编码名与「丢弃未保存的修改」。
// 直接触发重载入口（doReloadWithEncoding）而非真实菜单鼠标链路，
// 与 tc008 的 open() 等价驱动模式一致；完整确认/取消行为链路由
// test_encodingmenu 覆盖。
void TestDialogStyle::tc010_reloadConfirm_layoutText()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("gbk.txt"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(QByteArray::fromHex("c4e3bac3")); // "你好" 的 GBK 字节
    f.close();
    b.dm->openUrls({ QUrl::fromLocalFile(path) });
    Document *doc = b.dm->activeDocument();
    QVERIFY(doc && doc->encoding() == Document::Encoding::Gbk);
    doc->setContent(QStringLiteral("dirty")); // 置脏以触发确认框

    QVERIFY(QMetaObject::invokeMethod(
        b.win, "doReloadWithEncoding",
        Q_ARG(QVariant, static_cast<int>(Document::Encoding::Gbk))));
    QTRY_VERIFY(b.reloadDlg->property("visible").toBool());
    QTest::qWait(50);

    // 居中 + footer WinLayout + 按钮 重新加载（primary）/取消
    QString why;
    QVERIFY2(isCentered(b.win, b.reloadDlg, &why), qPrintable(why));
    QObject *box = footerBox(b.reloadDlg);
    QVERIFY2(box, "reloadConfirmDialog footer DialogButtonBox not found");
    QCOMPARE(box->property("buttonLayout").toInt(), kWinLayout);
    QCOMPARE(buttonsLeftToRight(box),
             QStringList({ QStringLiteral("重新加载"), QStringLiteral("取消") }));
    QVERIFY(buttonByText(box, QStringLiteral("重新加载"))->property("primary").toBool());
    QVERIFY(!buttonByText(box, QStringLiteral("取消"))->property("primary").toBool());

    // 正文文案（按触发编码 GBK 呈现）
    QCOMPARE(dialogText(b.reloadDlg),
             QStringLiteral("通过 GBK 重新加载将丢弃未保存的修改"));
}

// 「未保存修改临时缓存」需求：草稿冲突汇总对话框（draftConflictDialog，
// DEC-006「恢复」全量应用/「丢弃」全量丢弃）基本风格与其他对话框的
// Windows 惯例一致：居中、modal、宽 440、WinLayout、自左向右
// 恢复（primary）/丢弃。真实链路依赖启动期草稿冲突检测，此处按
// tc008 的 open() 等价驱动模式做风格断言；恢复/丢弃的行为链路由
// test_draftcache 覆盖。
void TestDialogStyle::tc011_draftConflict_layoutButtons()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");

    QVERIFY(QMetaObject::invokeMethod(b.draftDlg, "open"));
    QTRY_VERIFY(b.draftDlg->property("visible").toBool());
    QTest::qWait(50);

    // 居中 + 基本几何/模态（与 unsavedDialog 等同维度）
    QString why;
    QVERIFY2(isCentered(b.win, b.draftDlg, &why), qPrintable(why));
    QCOMPARE(b.draftDlg->property("width").toReal(), 440.0);
    QVERIFY(b.draftDlg->property("modal").toBool());
    QCOMPARE(b.draftDlg->property("closePolicy").toInt(), kCloseOnEscape);

    // footer：DialogButtonBox 为 WinLayout、按钮 恢复（primary）/丢弃
    QObject *box = footerBox(b.draftDlg);
    QVERIFY2(box, "draftConflictDialog footer DialogButtonBox not found");
    QCOMPARE(box->property("buttonLayout").toInt(), kWinLayout);
    QCOMPARE(buttonsLeftToRight(box),
             QStringList({ QStringLiteral("恢复"), QStringLiteral("丢弃") }));
    QVERIFY(buttonByText(box, QStringLiteral("恢复"))->property("primary").toBool());
    QVERIFY(!buttonByText(box, QStringLiteral("丢弃"))->property("primary").toBool());
}

// ==================== 弹窗拖动（《测试用例-弹窗拖动》） ====================
// 弹窗拖动 TC-002: 打开未保存确认框，合成鼠标左键拖 header 把手中心
// 移动 (60, -30) → 弹窗 x/y 精确增加同位移（±1px 容差），拖动不触发
// 关闭（模态行为不变）。
void TestDialogStyle::drag002_headerDrag_movesDialog()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("dirty"));

    QVERIFY(!b.win->close());
    QTRY_VERIFY(b.unsavedDlg->property("visible").toBool());
    QTest::qWait(50); // 等布局 polish（header 几何就位）

    const qreal x0 = b.unsavedDlg->property("x").toReal();
    const qreal y0 = b.unsavedDlg->property("y").toReal();
    const QPointF start = headerGrabPoint(b.win, b.unsavedDlg);
    QVERIFY2(!start.isNull(), "header handle not found or not polished");

    const QPointF delta(60, -30); // 普通窗口内位移（不触边界）
    mouseDrag(b.win, start, start + delta);
    QTest::qWait(50);

    const qreal x1 = b.unsavedDlg->property("x").toReal();
    const qreal y1 = b.unsavedDlg->property("y").toReal();
    QVERIFY2(qAbs(x1 - (x0 + delta.x())) <= 1.0,
             qPrintable(QString("dx moved %1, expected %2 (x0=%3 x1=%4)")
                            .arg(x1 - x0).arg(delta.x()).arg(x0).arg(x1)));
    QVERIFY2(qAbs(y1 - (y0 + delta.y())) <= 1.0,
             qPrintable(QString("dy moved %1, expected %2 (y0=%3 y1=%4)")
                            .arg(y1 - y0).arg(delta.y()).arg(y0).arg(y1)));
    // 拖动不改变模态行为：对话框保持打开
    QVERIFY(b.unsavedDlg->property("visible").toBool());
}

// 弹窗拖动 TC-003: 分别向左 / 右 / 上 / 下大幅超界拖动：位置被 clamp
// 在把手不丢失的范围内。Qt 对 Item 型 Popup 强制 effective x/y ≥ 0
// （左/上不可越出窗口，MovableDialog.clampX/Y 下界已对齐取 0）；右/下
// 允许身体部分移出窗口：
//   x ∈ [0, parent.width - keepWidth]、y ∈ [0, parent.height - headerHeight]
// （keepWidth = 80）；不等式成立且把手始终在窗口内可抓回。
void TestDialogStyle::drag003_clampFourDirections()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("dirty"));

    QVERIFY(!b.win->close());
    QTRY_VERIFY(b.unsavedDlg->property("visible").toBool());
    // 几何取确定值：进程内会话文件可能携带此前用例写入的窗口几何
    //（SessionManager 防抖落盘，Component.onCompleted 会恢复），
    // 统一复位到默认 1000x640 再断言
    b.win->resize(QSize(1000, 640));
    QTest::qWait(50);

    QObject *dlg = b.unsavedDlg;
    const qreal w = dlg->property("width").toReal(); // 380
    const int keep = dlg->property("keepWidth").toInt();
    QCOMPARE(keep, 80);
    QQuickItem *hdr = dialogHeader(dlg);
    QVERIFY2(hdr && hdr->height() > 0, "header handle not found");
    const qreal hdrH = hdr->height();

    const auto x = [dlg]() { return dlg->property("x").toReal(); };
    const auto y = [dlg]() { return dlg->property("y").toReal(); };

    // 左：x 下限 = 0（Qt 对 Item 型 Popup 的框架级强制，见 MovableDialog
    // clampX 注释；原设计的 -(width-keepWidth) 负值不可达）
    QPointF c = headerGrabPoint(b.win, dlg);
    QVERIFY(!c.isNull());
    mouseDrag(b.win, c, c + QPointF(-2000, 0));
    QTest::qWait(30);
    QVERIFY2(qAbs(x() - 0.0) <= 1.0,
             qPrintable(QString("left clamp: x=%1 expected 0").arg(x())));

    // 右：x 上限 = parent.width - keepWidth（身体允许部分移出右边界）
    c = headerGrabPoint(b.win, dlg);
    mouseDrag(b.win, c, c + QPointF(2000, 0));
    QTest::qWait(30);
    QVERIFY2(qAbs(x() - (b.win->width() - keep)) <= 1.0,
             qPrintable(QString("right clamp: x=%1 expected %2")
                            .arg(x()).arg(b.win->width() - keep)));

    // 上：y 下限 = 0
    c = headerGrabPoint(b.win, dlg);
    mouseDrag(b.win, c, c + QPointF(0, -2000));
    QTest::qWait(30);
    QVERIFY2(qAbs(y() - 0.0) <= 1.0,
             qPrintable(QString("top clamp: y=%1 expected 0").arg(y())));

    // 下：y 上限 = parent.height - headerHeight（身体允许部分移出下边界）
    c = headerGrabPoint(b.win, dlg);
    mouseDrag(b.win, c, c + QPointF(0, 2000));
    QTest::qWait(30);
    QVERIFY2(qAbs(y() - (b.win->height() - hdrH)) <= 1.0,
             qPrintable(QString("bottom clamp: y=%1 expected %2 (winH=%3 hdrH=%4)")
                            .arg(y()).arg(b.win->height() - hdrH)
                            .arg(b.win->height()).arg(hdrH)));

    // 全部不等式仍成立（把手在窗口内；含测试用例文档 TC-003 的
    // x ≥ -(width - keepWidth) 下界，框架 0 下界强于文档下界）
    QVERIFY(x() >= -(w - keep) - 1.0 && x() <= b.win->width() - keep + 1.0);
    QVERIFY(x() >= -1.0);
    QVERIFY(y() >= -1.0 && y() <= b.win->height() - hdrH + 1.0);
    QVERIFY(dlg->property("visible").toBool());
}

// 弹窗拖动 TC-004: 拖离默认位置 → Esc 关闭 → 再次触发打开：位置复位
// 默认（居中，TC-001 断言重新成立）。拖动仅当次打开期间生效。
void TestDialogStyle::drag004_reopen_resetsPosition()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("dirty"));

    // 打开并拖离默认位置
    QVERIFY(!b.win->close());
    QTRY_VERIFY(b.unsavedDlg->property("visible").toBool());
    QTest::qWait(50);
    const QPointF c = headerGrabPoint(b.win, b.unsavedDlg);
    QVERIFY(!c.isNull());
    mouseDrag(b.win, c, c + QPointF(120, 80));
    QTest::qWait(30);
    QString why;
    QVERIFY2(!isCentered(b.win, b.unsavedDlg, &why),
             qPrintable("precondition: dialog should be off-center, " + why));

    // Esc 关闭（文档仍为脏）→ 再次关闭窗口重新弹确认框
    QTest::keyClick(b.win, Qt::Key_Escape);
    QTRY_VERIFY(!b.unsavedDlg->property("visible").toBool());
    QVERIFY(!b.win->close());
    QTRY_VERIFY(b.unsavedDlg->property("visible").toBool());
    QTest::qWait(50);

    // 重开复位默认位置（居中）
    QVERIFY2(isCentered(b.win, b.unsavedDlg, &why), qPrintable(why));
}

// 弹窗拖动 TC-006: 拖到右下边界后缩小窗口 → 已拖动的可见弹窗被重新
// clamp，把手仍在窗口内（TC-003 不等式全部成立）。
void TestDialogStyle::drag006_resizeAfterDrag_reclamps()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("dirty"));

    QVERIFY(!b.win->close());
    QTRY_VERIFY(b.unsavedDlg->property("visible").toBool());
    QTest::qWait(50);

    QObject *dlg = b.unsavedDlg;
    const qreal w = dlg->property("width").toReal();
    const int keep = dlg->property("keepWidth").toInt();
    QQuickItem *hdr = dialogHeader(dlg);
    QVERIFY2(hdr && hdr->height() > 0, "header handle not found");
    const qreal hdrH = hdr->height();

    // 拖到右下边界（1000x640 窗口下的 clamp 上限）
    const QPointF c = headerGrabPoint(b.win, dlg);
    QVERIFY(!c.isNull());
    mouseDrag(b.win, c, c + QPointF(2000, 2000));
    QTest::qWait(30);
    QVERIFY2(qAbs(dlg->property("x").toReal() - (b.win->width() - keep)) <= 1.0,
             "precondition: dragged to right bound");
    QVERIFY2(qAbs(dlg->property("y").toReal() - (b.win->height() - hdrH)) <= 1.0,
             "precondition: dragged to bottom bound");

    // 缩窗至最小尺寸 800x500：原 clamp 上限越界 → 重新 clamp
    b.win->resize(QSize(800, 500));
    QTest::qWait(50);
    QVERIFY2(qAbs(dlg->property("x").toReal() - (800 - keep)) <= 1.0,
             qPrintable(QString("reclamp x=%1 expected %2")
                            .arg(dlg->property("x").toReal()).arg(800 - keep)));
    QVERIFY2(qAbs(dlg->property("y").toReal() - (500 - hdrH)) <= 1.0,
             qPrintable(QString("reclamp y=%1 expected %2")
                            .arg(dlg->property("y").toReal()).arg(500 - hdrH)));

    // 不等式全部成立（把手在窗口内）
    const qreal x = dlg->property("x").toReal();
    const qreal y = dlg->property("y").toReal();
    QVERIFY(x >= -(w - keep) - 1.0 && x <= 800 - keep + 1.0);
    QVERIFY(y >= -1.0 && y <= 500 - hdrH + 1.0);
    QVERIFY(dlg->property("visible").toBool());
}

// 弹窗拖动 TC-008: 拖拽把手不进入焦点链：MovableDialog 自绘 header 为
// Label（非可聚焦控件），弹窗打开后反复 Tab / Shift+Tab 遍历，header
// 始终不持有焦点；模态（modal: true）焦点不逃逸出弹窗（activeFocusItem
// 沿 QObject parent 链可达弹窗）。Enter/Esc 键盘分支由 tc005 覆盖。
void TestDialogStyle::drag008_focusChain_handleNotInTabChain()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("dirty"));

    QVERIFY(!b.win->close());
    QTRY_VERIFY(b.unsavedDlg->property("visible").toBool());
    QTest::qWait(50); // 等布局 polish 与初始焦点落位（footer 主按钮）

    QQuickItem *hdr = dialogHeader(b.unsavedDlg);
    QVERIFY2(hdr && hdr->height() > 0, "header handle not found");
    QVERIFY(b.win->activeFocusItem() != nullptr); // 模态弹窗打开即有初始焦点

    // 把手从不持有焦点（打开瞬间）
    QVERIFY2(!hdr->property("activeFocus").toBool(),
             "drag handle must not take focus on open");

    // 焦点项是否属于弹窗（沿 QObject parent 链向上查找；模态焦点约束）
    const auto focusInsideDialog = [&b]() {
        QObject *it = b.win->activeFocusItem();
        while (it) {
            if (it == b.unsavedDlg)
                return true;
            it = it->parent();
        }
        return false;
    };

    // 正向 Tab 遍历若干轮（footer 3 按钮循环，contentItem Label 不可聚焦）
    for (int i = 0; i < 8; ++i) {
        QTest::keyClick(b.win, Qt::Key_Tab);
        QTest::qWait(10);
        QVERIFY2(!hdr->property("activeFocus").toBool(),
                 "drag handle must never gain focus in Tab chain");
        QVERIFY2(focusInsideDialog(),
                 qPrintable(QString("focus escaped modal dialog at Tab #%1: %2")
                                .arg(i)
                                .arg(QString::fromLatin1(b.win->activeFocusItem()
                                        ? b.win->activeFocusItem()->metaObject()->className()
                                        : "<null>"))));
    }

    // 反向 Shift+Tab 同样成立
    for (int i = 0; i < 4; ++i) {
        QTest::keyClick(b.win, Qt::Key_Tab, Qt::ShiftModifier);
        QTest::qWait(10);
        QVERIFY(!hdr->property("activeFocus").toBool());
        QVERIFY(focusInsideDialog());
    }

    // 遍历不触发关闭或默认按钮（弹窗保持打开，流程未中止）
    QVERIFY(b.unsavedDlg->property("visible").toBool());
    QVERIFY(b.win->isVisible());
}

// 弹窗拖动 TC-009: SearchDialog 拖动到新位置后查找/替换功能正常：
// 输入即实时查找计数、Enter 跳转（原生选区）、Esc 关闭并交还焦点/
// 关高亮、重开复位默认位置（水平居中、y=96，DEC-002）。与 tst_searchui
// 的 TC-201 有重叠，此处从简（拖动 + Enter/Esc 主路径）。
void TestDialogStyle::drag009_searchDialog_afterDrag()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");
    b.newDoc(QStringLiteral("hello world hello"));

    QVERIFY(QMetaObject::invokeMethod(b.win, "doFind"));
    QTRY_VERIFY(b.searchDlg->property("visible").toBool());
    QTest::qWait(50);

    // 初始默认位置：水平居中、y=96（DEC-002 顶部对齐）
    const qreal x0 = b.searchDlg->property("x").toReal();
    const qreal y0 = b.searchDlg->property("y").toReal();
    const qreal w0 = b.searchDlg->property("width").toReal();
    QVERIFY2(qAbs(x0 - (b.win->width() - w0) / 2.0) <= 1.0, "initial x centered");
    QVERIFY2(qAbs(y0 - 96.0) <= 1.0,
             qPrintable(QString("initial y=%1 expected 96").arg(y0)));

    // 拖动到新位置（+120, +60）：位置精确跟随
    const QPointF c = headerGrabPoint(b.win, b.searchDlg);
    QVERIFY2(!c.isNull(), "search dialog header not found");
    mouseDrag(b.win, c, c + QPointF(120, 60));
    QTest::qWait(30);
    QVERIFY2(qAbs(b.searchDlg->property("x").toReal() - (x0 + 120)) <= 1.0, "dragged x");
    QVERIFY2(qAbs(b.searchDlg->property("y").toReal() - (y0 + 60)) <= 1.0, "dragged y");
    QVERIFY(b.searchDlg->property("visible").toBool());

    // 拖动后功能正常：查找框持有焦点，输入即实时查找（2 处匹配）。
    // 文本输入沿用 tst_searchui 的模式：直接设查找框 text 属性
    // （onTextChanged → 控制器 pattern，真实接线）
    QObject *ctl = b.editorArea->property("searchController").value<QObject *>();
    QVERIFY(ctl);
    QList<QObject *> fields;
    collectByQmlType(b.searchDlg, "TextField", &fields);
    QVERIFY2(!fields.isEmpty(), "find field not found");
    QVERIFY(fields.first()->property("activeFocus").toBool()); // 打开即聚焦查找框
    QVERIFY(QMetaObject::invokeMethod(fields.first(), "forceActiveFocus"));
    QVERIFY(fields.first()->setProperty("text", QStringLiteral("hello")));
    QTest::qWait(50);
    QCOMPARE(ctl->property("matchCount").toInt(), 2);

    // Enter：首跳定位当前匹配（索引 0，无崩溃、状态正常）
    QTest::keyClick(b.win, Qt::Key_Return);
    QTRY_COMPARE(ctl->property("currentIndex").toInt(), 0);
    QVERIFY(b.searchDlg->property("visible").toBool()); // 查找类惯例：不关闭

    // Esc：关闭，高亮层收起（searchActive false）
    QTest::keyClick(b.win, Qt::Key_Escape);
    QTRY_VERIFY(!b.searchDlg->property("visible").toBool());
    QVERIFY(!b.editorArea->property("searchActive").toBool());

    // 重开：复位默认位置（水平居中、y=96）
    QVERIFY(QMetaObject::invokeMethod(b.win, "doFind"));
    QTRY_VERIFY(b.searchDlg->property("visible").toBool());
    QTest::qWait(50);
    QVERIFY2(qAbs(b.searchDlg->property("x").toReal() - (b.win->width() - w0) / 2.0) <= 1.0,
             "reopened x centered");
    QVERIFY2(qAbs(b.searchDlg->property("y").toReal() - 96.0) <= 1.0,
             qPrintable(QString("reopened y=%1 expected 96")
                            .arg(b.searchDlg->property("y").toReal())));
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
    // 派生自 %LOCALAPPDATA%（本测试为 %LOCALAPPDATA%\test_dialogstyle\drafts），
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
    TestDialogStyle t;
    const int testResult = QTest::qExec(&t, argc, argv);
    // 结束清理：删除本轮测试写入的草稿条目，不在 %LOCALAPPDATA% 留测试产物
    QDir(draftsDir).removeRecursively();
    return testResult;
}

#include "tst_dialogstyle.moc"
