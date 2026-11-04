// 对话框居中与按钮 Windows 化 QML 集成测试（测试用例文档
// 《测试用例-对话框居中与按钮Windows化》TC-001/002/004/005/006/007/008
// 的可自动化部分）。
//
// 测试方式：offscreen 平台下实例化完整 Main.qml，按标题定位四个对话框
// （unsavedDialog / errorDialog / ctxMenuPromptDialog / reloadConfirmDialog），
// 断言：
// - 居中：anchors.centerIn 生效，x/y 与窗口中心差 ≤1px，resize 后跟随；
// - footer：DialogButtonBox 为 WinLayout、右对齐，按钮自左向右为
//   保存/不保存/取消（unsaved）、确定（error）、添加/不添加（ctxPrompt），
//   primary 标记正确，「放弃」不再出现；
// - closePolicy == Popup.CloseOnEscape（属性断言 + 合成鼠标点击
//   对话框外部遮罩不关闭的行为断言）；
// - 键盘路径：Esc 取消中止；Enter（保存按钮持有焦点）触发保存分支；
// - 正文文案精确为「是否要将更改保存到 <displayName>？」；
// - 打开失败真实链路：errorDialog 弹出居中，队列逐个展示。
//
// 用例分派：TC-003（三按钮角色接线）由 test_window（tst_windowclose 的
// TC-005~008 accept/discarded/reject 分支）回归覆盖；TC-009（构建与全量
// 测试）由 ctest 总体覆盖；TC-010（人工视觉验收）无法 offscreen 覆盖，
// 见测试报告"未执行"清单。
//
// 对象定位说明：与 tst_windowclose 相同，按 QML 类型短名匹配
// （"Dialog"、"DialogButton"）；errorDialog 的 title 绑定 currentError，
// 未打开时为空串，按排除法定位（另两个对话框按固定标题精确匹配）。
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
#include <QMouseEvent>
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
    QObject *errDlg = nullptr;          // errorDialog（title 绑定 currentError，未打开为空）
    QObject *ctxPromptDlg = nullptr;    // ctxMenuPromptDialog（标题「添加右键菜单」）
    QObject *reloadDlg = nullptr;       // reloadConfirmDialog（标题「按编码重新加载」）

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

        // 四个对话框按标题定位；errorDialog 未打开时 title 为空串走排除法
        QList<QObject *> dialogs;
        collectByQmlType(win, "Dialog", &dialogs);
        for (QObject *d : dialogs) {
            const QString t = d->property("title").toString();
            if (t == QStringLiteral("未保存的修改"))
                unsavedDlg = d;
            else if (t == QStringLiteral("添加右键菜单"))
                ctxPromptDlg = d;
            else if (t == QStringLiteral("按编码重新加载"))
                reloadDlg = d;
            else
                errDlg = d;
        }
        if (dialogs.size() != 4 || !unsavedDlg || !errDlg || !ctxPromptDlg || !reloadDlg) {
            qWarning() << "[bench] dialogs found:" << dialogs.size()
                       << "unsaved:" << (unsavedDlg != nullptr)
                       << "err:" << (errDlg != nullptr)
                       << "ctxPrompt:" << (ctxPromptDlg != nullptr)
                       << "reload:" << (reloadDlg != nullptr);
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

// 合成鼠标点击窗口内对话框外部坐标（press + release 直投窗口，
// 走 QQuickPopup 对遮罩点击的真实派发路径，验证 closePolicy 行为）
void clickOutside(QQuickWindow *win, qreal x, qreal y)
{
    const QPointF local(x, y);
    const QPointF global = win->mapToGlobal(local.toPoint());
    QMouseEvent press(QEvent::MouseButtonPress, local, global,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, local, global,
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QGuiApplication::sendEvent(win, &press);
    QGuiApplication::sendEvent(win, &release);
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

    // resize 后仍保持居中（anchors.centerIn 跟随窗口尺寸变化）
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

// TC-004: 四个对话框 closePolicy 均为 Popup.CloseOnEscape；
// 合成鼠标点击对话框外部遮罩 → 不关闭、退出流程未中止、内容保留。
void TestDialogStyle::tc004_closePolicy_escapeOnly()
{
    Bench b;
    QVERIFY2(b.init(), "testbench init failed");

    // 属性断言：仅 Esc 关闭，不含任何点击外部关闭的位
    for (QObject *dlg : { b.unsavedDlg, b.errDlg, b.ctxPromptDlg, b.reloadDlg }) {
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
// ⚠ 已知缺陷（Qt 源码级根因，详见测试报告）：QQuickAbstractButton 的
// 键盘激活键来自平台主题 ButtonPressKeys（QPlatformTheme 默认仅
// Space/Select，QWindowsTheme::themeHint 未覆盖该提示），Enter/Return 不
// 在激活键内——按钮持有焦点按 Enter 无效果（对照实验：同一焦点下按
// Space 可正常触发保存写盘退出）。本断言按测试用例 TC-005 预期
// （Enter=触发保存分支）保留，待主流程修复（如 DialogButton 内显式
// 处理 Return/Enter 键）后转绿。
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
    QCOMPARE(b.errDlg->property("title").toString(), QStringLiteral("打开失败"));
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
    TestDialogStyle t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_dialogstyle.moc"
