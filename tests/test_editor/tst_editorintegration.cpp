// 列式编辑 QML 集成测试（测试用例 TC-101 至 TC-113 的可自动化部分）
//
// 测试方式：offscreen 平台下经 QQuickView 实例化真实 EditorArea 组件
// （内含 TextArea + MultiCursorController + MultiCursorLayer 的完整接线），
// 以 QTest 合成键盘/鼠标事件与 QInputMethodEvent 驱动与人工交互等价的
// 事件链路（窗口 → Keys.onPressed/MouseArea → 控制器 → QTextDocument）。
//
// 与 tests/test_core 的引擎单测互补：这里验证的是 QML 集成层
// （事件拦截、状态同步、覆盖层数据、剪贴板、文档切换/外部重置）。
//
// GUI 红线：main 中强制 QT_QPA_PLATFORM=offscreen，全程无可见窗口。
// 无法自动化的人工项（DPI 视觉、闪烁节奏、真实 IME 组字窗口）见
// 测试报告"人工待验"清单。

#include <QTest>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPoint>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickView>
#include <QStringList>
#include <QTextLayout>
#include <QVariant>

#include "document/document.h"
#include "document/documentmanager.h"
#include "editor/multicursorcontroller.h"

using qcnote::MultiCursorController;

// ==================== 测试台辅助 ====================

namespace {

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

// 类名包含匹配：类名可能带命名空间前缀（qcnote::Foo）或 QML 动态
// 类型后缀（Foo_QMLTYPE_N），统一用包含匹配；exclude 用于排除干扰项
bool classNameMatches(const QObject *obj, const char *needle, const char *exclude = nullptr)
{
    const QString name = QString::fromLatin1(obj->metaObject()->className());
    return name.contains(QLatin1String(needle))
           && (!exclude || !name.contains(QLatin1String(exclude)));
}

// 找到类名包含 needle 的 QObject 子孙（含非可视对象）
QObject *findObject(QObject *root, const char *needle)
{
    if (!root)
        return nullptr;
    if (classNameMatches(root, needle))
        return root;
    const auto kids = root->children();
    for (QObject *kid : kids) {
        if (QObject *found = findObject(kid, needle))
            return found;
    }
    return nullptr;
}

// 找到类名包含 needle 的 QQuickItem 子孙（仅可视树）
QQuickItem *findItem(QQuickItem *root, const char *needle)
{
    QQuickItem *found = nullptr;
    visitItems(root, [&](QQuickItem *it) {
        if (!found && classNameMatches(it, needle))
            found = it;
    });
    return found;
}

/**
 * @brief 每个测试用例独立的测试台：真实 EditorArea + 一份新文档。
 *
 * QML 装载（内联，避免额外模块文件）：
 *   EditorArea { Component.onCompleted: DocumentManager.newDocument() }
 * EditorArea 内部由 Repeater 为每文档创建 TextArea + 控制器 + 覆盖层，
 * 与主程序 Main.qml 的接线完全一致（含 Keys/MouseArea 拦截逻辑）。
 */
struct Bench
{
    QQuickView view;
    QQuickItem *root = nullptr;                        // EditorArea
    QQuickItem *editor = nullptr;                      // 活动文档 TextArea
    MultiCursorController *mc = nullptr;               // 其控制器
    QQuickItem *layer = nullptr;                       // MultiCursorLayer
    DocumentManager *dm = nullptr;
    Document *doc = nullptr;                           // 打开时的活动文档

    // TextArea 经 QMetaObject 访问（QQuickTextEdit 为 Qt 私有头，禁用）
    QString text() const { return editor->property("text").toString(); }
    int cursorPosition() const { return editor->property("cursorPosition").toInt(); }
    void setCursorPosition(int pos) { editor->setProperty("cursorPosition", pos); }
    int selectionStart() const { return editor->property("selectionStart").toInt(); }
    int selectionEnd() const { return editor->property("selectionEnd").toInt(); }
    // TextEdit.positionToRectangle(pos)（Q_INVOKABLE，签名 int，返回 QRectF）
    QRectF rectOf(int pos) const
    {
        QRectF ret;
        QMetaObject::invokeMethod(const_cast<QQuickItem *>(editor),
                                  "positionToRectangle",
                                  Q_RETURN_ARG(QRectF, ret), Q_ARG(int, pos));
        return ret;
    }

    // 从当前 view 的 QML 引擎取 DocumentManager 单例：必须与 QML 侧实际
    // 使用的实例一致（EditorArea 的 Repeater 绑定其模型），不能绕开引擎
    // 直接 create()，否则可能得到与 QML 不同的实例
    DocumentManager *manager()
    {
        if (!dm)
            dm = view.engine()->singletonInstance<DocumentManager *>(
                         u"qcnote", u"DocumentManager");
        return dm;
    }

    // 类名匹配见上方 classNameMatches 注释（TextArea 可能被包装为
    // "TextArea_QMLTYPE_N"；控制器类名带 qcnote:: 命名空间前缀）

    bool open(const QString &initialText = QString())
    {
        static const QByteArray qml = R"QML(
import QtQuick
import qcnote
EditorArea {
    width: 520; height: 400
    // 诊断：打印冒泡到根的键（编辑器层未消费的事件）
    Keys.onPressed: (event) => console.warn("BENCH-KEY", event.key, event.modifiers, event.text)
}
)QML";
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.resize(520, 400);
        QQmlComponent component(view.engine());
        component.setData(qml, QUrl());
        if (component.isError()) {
            qWarning("testbench QML errors: %s",
                     qPrintable(component.errorString()));
            return false;
        }
        root = qobject_cast<QQuickItem *>(component.create());
        if (!root) {
            qWarning("testbench root creation failed");
            return false;
        }
        view.setContent(QUrl(), &component, root);
        view.show();
        if (!QTest::qWaitForWindowExposed(&view)) {
            qWarning("testbench window not exposed");
            return false;
        }
        settle();
        // 从 QML 引擎取实际使用的 DocumentManager 单例；清理上一用例
        // 残留文档（每引擎单例时无残留，全局工厂单例时保证用例隔离），
        // 再新建文档：EditorArea 的 Repeater 绑定 documentsModel，
        // 模型 rowsInserted 驱动 delegate（TextArea + 控制器 + 覆盖层）创建
        DocumentManager *managerInstance = manager();
        if (!managerInstance) {
            qWarning("DocumentManager singleton unavailable");
            return false;
        }
        const auto stale = managerInstance->documents();
        for (Document *d : stale)
            managerInstance->closeDocument(d);
        managerInstance->newDocument();
        settle();
        doc = managerInstance->activeDocument();
        if (!doc) {
            qWarning("no active document after newDocument (docs=%d)",
                     int(managerInstance->documents().size()));
            return false;
        }
        // TextArea 在模块作用域中可能被包装为 "TextArea_QMLTYPE_N"
        editor = findItem(root, "TextArea");
        if (!editor) {
            QStringList names;
            visitItems(root, [&](QQuickItem *it) {
                names << QString::fromLatin1(it->metaObject()->className());
            });
            qWarning("TextArea not found; items: %s",
                     qPrintable(names.join(QLatin1Char(','))));
            return false;
        }
        mc = qobject_cast<MultiCursorController *>(
                findObject(editor, "MultiCursorController"));
        if (!mc) {
            qWarning("MultiCursorController not found");
            return false;
        }
        layer = findItem(root, "MultiCursorLayer");
        if (!layer) {
            qWarning("MultiCursorLayer not found");
            return false;
        }
        editor->forceActiveFocus();
        settle();
        if (!initialText.isEmpty()) {
            // 经 Document.content 外部写入（与打开文件同链路），
            // EditorArea 的 onContentChanged 会同步 TextArea.text
            doc->setContent(initialText);
            settle();
        }
        if (text() != initialText) {
            qWarning("initial text not synced, got '%s'", qPrintable(text()));
            return false;
        }
        return true;
    }

    // 等待 QML 绑定/布局/信号传递
    void settle() { QTest::qWait(30); }

    // 文档位置 → 窗口坐标（字符左缘中点；positionAt 与之互逆）
    QPoint pointOf(int docPos) const
    {
        const QRectF r = rectOf(docPos);
        const QPointF scene = editor->mapToScene(QPointF(r.x() + 0.5, r.y() + r.height() / 2));
        return scene.toPoint();
    }

    // 合成鼠标事件：手动构造 QMouseEvent 发送（QTest::mouseMove 生成的
    // move 事件 buttons 状态不符合按下拖拽语义，导致原生/MouseArea 拖拽
    // 选择失效；此处完全控制 button/buttons/modifiers）
    void sendMouse(QEvent::Type type, const QPointF &local, Qt::MouseButton button,
                   Qt::MouseButtons buttons, Qt::KeyboardModifiers mods)
    {
        const QPointF global = QPointF(view.mapToGlobal(local.toPoint()));
        QMouseEvent ev(type, local, global, button, buttons, mods);
        QCoreApplication::sendEvent(&view, &ev);
    }

    // Alt+单击：在文档位置处添加光标
    void altClick(int docPos)
    {
        const QPoint p = pointOf(docPos);
        sendMouse(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton,
                  Qt::AltModifier);
        sendMouse(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton,
                  Qt::AltModifier);
        settle();
    }

    // 普通鼠标拖拽选择（无修饰键，原生路径；MouseArea 放行 TextArea）
    void nativeDragSelect(int from, int to)
    {
        const QPoint p0 = pointOf(from);
        const QPoint p1 = pointOf(to);
        sendMouse(QEvent::MouseButtonPress, p0, Qt::LeftButton, Qt::LeftButton,
                  Qt::NoModifier);
        sendMouse(QEvent::MouseMove, p1, Qt::NoButton, Qt::LeftButton,
                  Qt::NoModifier);
        sendMouse(QEvent::MouseButtonRelease, p1, Qt::LeftButton, Qt::NoButton,
                  Qt::NoModifier);
        settle();
    }

    // Alt+Shift 拖拽列选
    void columnDrag(int from, int to)
    {
        const auto mods = Qt::AltModifier | Qt::ShiftModifier;
        const QPoint p0 = pointOf(from);
        const QPoint p1 = pointOf(to);
        sendMouse(QEvent::MouseButtonPress, p0, Qt::LeftButton, Qt::LeftButton, mods);
        sendMouse(QEvent::MouseMove, p1, Qt::NoButton, Qt::LeftButton, mods);
        sendMouse(QEvent::MouseButtonRelease, p1, Qt::LeftButton, Qt::NoButton, mods);
        settle();
    }

    // 普通单击（无修饰键：多光标时退出并回到单击处）
    void plainClick(int docPos)
    {
        const QPoint p = pointOf(docPos);
        sendMouse(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton,
                  Qt::NoModifier);
        sendMouse(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton,
                  Qt::NoModifier);
        settle();
    }

    // 合成键盘事件：直接构造 QKeyEvent 发送到窗口（QQuickWindow 分发至
    // activeFocusItem，与真实按键链路一致）。不用 QTest::keyClick：
    // Qt 6.11 QTest 对 Ctrl+Alt+Shift 组合的 ascii 映射存在断言崩溃，
    // 且其自动生成的字母文本恒为小写。修饰键组合的 text 与真实平台
    // 一致取空，可打印字符经 type() 显式指定
    void sendKey(int key, Qt::KeyboardModifiers mods, const QString &text)
    {
        QKeyEvent press(QEvent::KeyPress, key, mods, text);
        QCoreApplication::sendEvent(&view, &press);
        QKeyEvent release(QEvent::KeyRelease, key, mods, text);
        QCoreApplication::sendEvent(&view, &release);
        settle();
    }

    void key(int k, Qt::KeyboardModifiers m = Qt::NoModifier)
    {
        sendKey(k, m, QString());
    }

    void type(const QString &s)
    {
        for (const QChar &ch : s)
            sendKey(ch.unicode(), Qt::NoModifier, QString(ch));
        settle();
    }

    // 控制器 selectionRanges → "start:end" 列表（便于 QCOMPARE 输出）。
    // 注意：controller 经 QML 引擎边界（VMEMetaObject）时内嵌 [start,end]
    // 对可能被展平为 [s1,e1,s2,e2,...]（MultiCursorLayer.selectionPairs
    // 对同一现象做兼容），此处同样兼容两种形态
    QStringList selectionPairs() const
    {
        QStringList out;
        const QVariantList raw = mc->selectionRanges();
        if (raw.isEmpty())
            return out;
        if (raw.at(0).canConvert<QVariantList>()) {
            // 嵌套形态：[[s1,e1],[s2,e2],...]
            for (const QVariant &v : raw) {
                const QVariantList pair = v.toList();
                if (pair.size() == 2)
                    out << QStringLiteral("%1:%2")
                               .arg(pair.at(0).toInt())
                               .arg(pair.at(1).toInt());
            }
        } else {
            // 展平形态：[s1,e1,s2,e2,...]
            for (int i = 0; i + 1 < raw.size(); i += 2)
                out << QStringLiteral("%1:%2")
                           .arg(raw.at(i).toInt())
                           .arg(raw.at(i + 1).toInt());
        }
        return out;
    }

    // 覆盖层选区高亮段数（每行一段；可视化数据的代理断言）
    int segmentCount() const
    {
        return layer->property("selectionSegments").toList().size();
    }
};

} // namespace

// ==================== 测试类 ====================

// 列式编辑 T3 验收：QML 集成层（TC-101 至 TC-113 可自动化项）
class TestEditorIntegration : public QObject
{
    Q_OBJECT

private slots:
    // 残留文档清理在各用例 Bench::open 内完成（manager 为 QML 引擎单例）
    void init() {}

    // TC-101：Alt+Click 添加光标与同步输入/Backspace
    void tc101_altClick_syncInput();

    // TC-102：Ctrl+Alt+Down 连续加光标后同步输入
    void tc102_ctrlAltUpDown_cursors();

    // TC-103：Alt+Shift 拖拽列选区，逐行替换；覆盖层高亮段数
    void tc103_altShiftDrag_columnSelect();

    // TC-104：Ctrl+Alt+Shift+方向键键盘列选，与鼠标列选等价
    void tc104_keyboardColumnSelect();

    // TC-105：原生拖选多行后 Shift+Alt+I 行尾光标批量输入
    void tc105_shiftAltI_endOfLines();

    // TC-106：Ctrl+D 逐个追加与 Ctrl+Shift+L 全部选中并替换
    void tc106_ctrlD_ctrlShiftL();

    // TC-107：列复制/剪切/粘贴（行数匹配分发、不匹配整份插入）
    void tc107_columnCopyCutPaste();

    // TC-108：多光标一次输入后单步撤销/重做
    void tc108_singleUndoRedo();

    // TC-109：Esc 退出与普通单击退出多光标
    void tc109_esc_and_click_exit();

    // TC-110：输入法预编辑不进文档、提交分发到所有光标（合成事件）
    void tc110_ime_preedit_and_commit();

    // TC-111：EditorArea.cursorCount 状态与状态栏"光标 ×N"文本
    void tc111_statusbar_cursorCount();

    // TC-112：单光标回归：编辑/选择/复制/粘贴/撤销全走原生，零拦截
    void tc112_singleCursor_regression();

    // TC-113：标签页切换保留多光标；外部内容重置退回单光标
    void tc113_tabSwitch_externalReset();
};

// TC-101：Alt+单击两处加光标（初始光标 + 2 = 3 光标），输入与 Backspace
// 在所有光标同步生效
void TestEditorIntegration::tc101_altClick_syncInput()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("aaaa\nbbbb\ncccc")), "testbench open failed");

    QCOMPARE(b.mc->cursorCount(), 1);
    QVERIFY(!b.mc->active());

    b.altClick(1); // 第 1 行 'a' 前
    QCOMPARE(b.mc->cursorCount(), 2);
    QVERIFY(b.mc->active());

    b.altClick(7); // 第 2 行 'b' 前
    QCOMPARE(b.mc->cursorCount(), 3);

    // 光标位于 0、1、7：三处同步插入 X（位置 7 = 第 2 行第 3 个 b 前）
    b.type(QStringLiteral("X"));
    QCOMPARE(b.text(), QStringLiteral("XaXaaa\nbbXbb\ncccc"));
    QCOMPARE(b.mc->cursorCount(), 3);

    // Backspace 同步删除刚插入的 X
    b.key(Qt::Key_Backspace);
    QCOMPARE(b.text(), QStringLiteral("aaaa\nbbbb\ncccc"));

    // 覆盖层在接管期可见（额外光标渲染数据的代理断言）
    QVERIFY(b.layer->property("visible").toBool());
}

// TC-102：Ctrl+Alt+Down 三次 → 4 行同列光标（短行/空行钳制行末），同步输入
void TestEditorIntegration::tc102_ctrlAltUpDown_cursors()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("aaaa\nbbbb\ncccc\n")), "testbench open failed");

    b.setCursorPosition(2);
    b.settle();
    QCOMPARE(b.mc->cursorCount(), 1);

    const auto mods = Qt::ControlModifier | Qt::AltModifier;
    b.key(Qt::Key_Down, mods); // → 行 2 列 2
    QCOMPARE(b.mc->cursorCount(), 2);
    b.key(Qt::Key_Down, mods); // → 行 3 列 2
    b.key(Qt::Key_Down, mods); // → 行 4（空行）钳制行末
    QCOMPARE(b.mc->cursorCount(), 4);

    b.type(QStringLiteral("X"));
    QCOMPARE(b.text(), QStringLiteral("aaXaa\nbbXbb\nccXcc\nX"));

    // 状态栏数据源：EditorArea.cursorCount == 4
    QCOMPARE(b.root->property("cursorCount").toInt(), 4);
}

// TC-103：Alt+Shift 拖拽矩形列选（锚点第 1 行列 2、游标第 3 行列 4），
// 每行生成列区间选区；输入逐行替换；Backspace 恢复
void TestEditorIntegration::tc103_altShiftDrag_columnSelect()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("aaaa\nbbbb\ncccc")), "testbench open failed");

    b.columnDrag(1, 13);
    QVERIFY(b.mc->active());
    QVERIFY(!b.mc->columnSelecting()); // 松开后定格为普通多选区

    // 三行各覆盖列 [1,3)（文档坐标区间）
    QCOMPARE(b.selectionPairs(),
             QStringList() << QStringLiteral("1:3") << QStringLiteral("6:8")
                           << QStringLiteral("11:13"));
    // 覆盖层选区高亮：每行一段
    QCOMPARE(b.segmentCount(), 3);
    // 接管期状态栏“已选 N 字符”：原生选区已被清空，取各选区之和 3×2=6
    QCOMPARE(b.root->property("selectionLength").toInt(), 6);

    b.type(QStringLiteral("="));
    QCOMPARE(b.text(), QStringLiteral("a=a\nb=b\nc=c"));
    // 选区被替换后无剩余选区，已选字符数归 0
    QCOMPARE(b.selectionPairs().size(), 0);
    QCOMPARE(b.root->property("selectionLength").toInt(), 0);

    b.key(Qt::Key_Backspace);
    // 列选替换已消耗原文中间 2 字符，Backspace 仅删除替换字符
    QCOMPARE(b.text(), QStringLiteral("aa\nbb\ncc"));

    // ---- 修复验证：applyBatch 后 active 随光标汇聚回落 ----
    // 单选区被输入替换 → 光标汇聚为 1 个空光标：active 应回落为 false
    b.key(Qt::Key_Escape);
    b.setCursorPosition(1);
    b.settle();
    b.key(Qt::Key_D, Qt::ControlModifier); // 选中单词 "aa" → [0,2)
    QVERIFY(b.mc->active());
    b.type(QStringLiteral("X"));
    QCOMPARE(b.text(), QStringLiteral("X\nbb\ncc")); // 全词替换
    QVERIFY(!b.mc->active());              // 汇聚为 1：回落
    QCOMPARE(b.mc->cursorCount(), 1);

    // 纯移动类汇聚（同行双光标 Home 同到行首）同样回落，
    // 且原生光标同步到汇聚点（不残留汇聚前位置）
    //（"X\nbb\ncc"：行 2 为 [2,4)，光标 3、4 同行，Home 同到 2）
    b.setCursorPosition(3);
    b.settle();
    b.altClick(4);                         // 同行双光标 3、4
    QVERIFY(b.mc->active());
    b.key(Qt::Key_Home);                   // 汇聚到行首 2
    QVERIFY(!b.mc->active());
    QCOMPARE(b.mc->cursorCount(), 1);
    QCOMPARE(b.cursorPosition(), 2);
}

// TC-104：Ctrl+Alt+Shift+Right/Down 键盘列选，与鼠标列选等价
void TestEditorIntegration::tc104_keyboardColumnSelect()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("aaaa\nbbbb\ncccc")), "testbench open failed");

    b.setCursorPosition(1);
    b.settle();

    const auto mods = Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier;
    b.key(Qt::Key_Right, mods); // 单行 [1,2)
    QCOMPARE(b.selectionPairs(), QStringList() << QStringLiteral("1:2"));
    b.key(Qt::Key_Right, mods); // 单行 [1,3)
    b.key(Qt::Key_Down, mods);  // 行 1-2
    b.key(Qt::Key_Down, mods);  // 行 1-3
    QCOMPARE(b.selectionPairs(),
             QStringList() << QStringLiteral("1:3") << QStringLiteral("6:8")
                           << QStringLiteral("11:13"));
    QCOMPARE(b.segmentCount(), 3);

    // 键盘列选与鼠标列选（TC-103）结果一致，输入逐行替换
    b.type(QStringLiteral("+"));
    QCOMPARE(b.text(), QStringLiteral("a+a\nb+b\nc+c"));
}

// TC-105：原生拖选跨行 → Shift+Alt+I 在覆盖行行尾生成光标，批量输入 ';'
void TestEditorIntegration::tc105_shiftAltI_endOfLines()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("ab\ncdef\ngh")), "testbench open failed");

    // 普通拖选（原生路径，不经控制器；MouseArea 放行）：
    // 位置 2 拖至 8，跨第 1~3 行部分选中（避免起点 0 的矩形恰落在
    // TextArea 左 padding 边缘造成的命中差异）
    b.nativeDragSelect(2, 8);
    QCOMPARE(b.selectionStart(), 2);
    QCOMPARE(b.selectionEnd(), 8);
    QVERIFY(!b.mc->active()); // 原生选择本身不构成接管

    b.key(Qt::Key_I, Qt::ShiftModifier | Qt::AltModifier);
    QCOMPARE(b.mc->cursorCount(), 3);
    QVERIFY(b.mc->active());
    QCOMPARE(b.selectionPairs().size(), 0); // 行尾空选区光标

    b.type(QStringLiteral(";"));
    QCOMPARE(b.text(), QStringLiteral("ab;\ncdef;\ngh;"));
}

// TC-106：Ctrl+D 依次选中相同词（含环绕），Ctrl+Shift+L 全选后同步替换
void TestEditorIntegration::tc106_ctrlD_ctrlShiftL()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("foo bar foo baz foo")), "testbench open failed");

    b.setCursorPosition(1); // 光标置于第一个 "foo" 内
    b.settle();

    b.key(Qt::Key_D, Qt::ControlModifier);
    QCOMPARE(b.selectionPairs(), QStringList() << QStringLiteral("0:3"));
    b.key(Qt::Key_D, Qt::ControlModifier);
    QCOMPARE(b.selectionPairs(),
             QStringList() << QStringLiteral("0:3") << QStringLiteral("8:11"));
    b.key(Qt::Key_D, Qt::ControlModifier);
    QCOMPARE(b.selectionPairs(),
             QStringList() << QStringLiteral("0:3") << QStringLiteral("8:11")
                           << QStringLiteral("16:19"));
    b.key(Qt::Key_D, Qt::ControlModifier); // 全部已选中：无操作
    QCOMPARE(b.mc->cursorCount(), 3);

    b.type(QStringLiteral("X"));
    QCOMPARE(b.text(), QStringLiteral("X bar X baz X"));

    // Ctrl+Shift+L：光标置于某 "X" 内 → 全部相同词各自成选区
    b.key(Qt::Key_Escape);
    QCOMPARE(b.mc->cursorCount(), 1);
    QVERIFY(!b.mc->active());

    b.setCursorPosition(0); // "X bar X baz X"：光标置于 "X" 词内
    b.settle();
    b.key(Qt::Key_L, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(b.selectionPairs(),
             QStringList() << QStringLiteral("0:1") << QStringLiteral("6:7")
                           << QStringLiteral("12:13"));
    b.type(QStringLiteral("Y"));
    QCOMPARE(b.text(), QStringLiteral("Y bar Y baz Y"));
}

// TC-107：列选后 Ctrl+C 按行拼接；Ctrl+X 删除原选区；Ctrl+V 行数匹配
// 逐行分发、不匹配整份插入
void TestEditorIntegration::tc107_columnCopyCutPaste()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("aaaa\nbbbb\ncccc")), "testbench open failed");

    // 键盘列选列 [1,3) 三行
    b.setCursorPosition(1);
    b.settle();
    const auto sel = Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier;
    b.key(Qt::Key_Right, sel);
    b.key(Qt::Key_Right, sel);
    b.key(Qt::Key_Down, sel);
    b.key(Qt::Key_Down, sel);
    QCOMPARE(b.mc->cursorCount(), 3);

    // 复制：各行选区文本按行拼接
    QGuiApplication::clipboard()->clear();
    b.key(Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("aa\nbb\ncc"));
    QCOMPARE(b.text(), QStringLiteral("aaaa\nbbbb\ncccc")); // 复制不改内容

    // 剪切：同复制并删除所有选区（光标汇聚为 3 个行内位置）
    b.key(Qt::Key_X, Qt::ControlModifier);
    QCOMPARE(b.text(), QStringLiteral("aa\nbb\ncc"));
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("aa\nbb\ncc"));
    QCOMPARE(b.mc->cursorCount(), 3);

    // 粘贴（行数 3 == 光标数 3）：逐行分发，恢复原文
    b.key(Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(b.text(), QStringLiteral("aaaa\nbbbb\ncccc"));

    // 粘贴（行数 2 != 光标数 3）：每个光标插入整份文本
    QGuiApplication::clipboard()->setText(QStringLiteral("P\nQ"));
    // 退出多光标（粘贴后引擎仍处于 3 光标状态，直接再列选会以最末光标为锚）
    b.key(Qt::Key_Escape);
    b.setCursorPosition(1);
    b.settle();
    // 重新列选
    b.key(Qt::Key_Right, sel);
    b.key(Qt::Key_Right, sel);
    b.key(Qt::Key_Down, sel);
    b.key(Qt::Key_Down, sel);
    QCOMPARE(b.mc->cursorCount(), 3);
    b.key(Qt::Key_V, Qt::ControlModifier);
    // 行数不匹配：每个光标把整份文本插入/替换（此处为列选区替换）
    QCOMPARE(b.text(), QStringLiteral("aP\nQa\nbP\nQb\ncP\nQc"));
}

// TC-108：多光标一次输入（单批 editBlock）→ 一次 Ctrl+Z 整批回滚；
// 一次重做整批恢复
void TestEditorIntegration::tc108_singleUndoRedo()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("ab\ncd")), "testbench open failed");

    b.altClick(3); // 光标 0、3
    QCOMPARE(b.mc->cursorCount(), 2);

    b.type(QStringLiteral("Z"));
    QCOMPARE(b.text(), QStringLiteral("Zab\nZcd"));

    // 一次撤销：两处插入同时回滚，并退回单光标
    b.key(Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(b.text(), QStringLiteral("ab\ncd"));
    QCOMPARE(b.mc->cursorCount(), 1);
    QVERIFY(!b.mc->active());

    // 一次重做：两处插入同时恢复（原生 redo）
    b.key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(b.text(), QStringLiteral("Zab\nZcd"));
}

// TC-109：Esc 退出保留末光标；普通单击退出并回到单击处
void TestEditorIntegration::tc109_esc_and_click_exit()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("aaaa\nbbbb")), "testbench open failed");

    b.altClick(2);
    QCOMPARE(b.mc->cursorCount(), 2);

    // Esc：保留排序最末光标
    b.key(Qt::Key_Escape);
    QCOMPARE(b.mc->cursorCount(), 1);
    QVERIFY(!b.mc->active());

    // 再次多光标（Alt+Click 到与保留光标不同的位置）后普通单击：
    // 退出并定位到单击处（原生语义）
    b.altClick(6);
    QCOMPARE(b.mc->cursorCount(), 2);
    b.plainClick(6);
    QCOMPARE(b.mc->cursorCount(), 1);
    QVERIFY(!b.mc->active());
    QCOMPARE(b.cursorPosition(), 6);

    // 退出后输入为单点生效
    b.type(QStringLiteral("Z"));
    QCOMPARE(b.text(), QStringLiteral("aaaa\nbZbbb"));
}

// TC-110：预编辑（组字）不进文档；提交字符串分发到所有光标。
// 真实 IME 组字窗口的视觉表现（预编辑仅在主光标处）无法离屏断言，人工待验；
// 此处以合成 QInputMethodEvent 验证控制器事件过滤器路径。
void TestEditorIntegration::tc110_ime_preedit_and_commit()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("aa\nbb")), "testbench open failed");

    b.altClick(3); // 光标 0、3
    QCOMPARE(b.mc->cursorCount(), 2);

    // 预编辑：放行原生（仅主光标显示，组字窗口），不进文档、不分发
    {
        QInputMethodEvent preedit(QStringLiteral("ni"),
                                  QList<QInputMethodEvent::Attribute>());
        QCoreApplication::sendEvent(b.editor, &preedit);
    }
    b.settle();
    QCOMPARE(b.text(), QStringLiteral("aa\nbb"));
    QCOMPARE(b.mc->cursorCount(), 2);

    // 提交：分发到所有光标（并阻止原生再次插入）
    {
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("你"));
        QCoreApplication::sendEvent(b.editor, &commit);
    }
    b.settle();
    QCOMPARE(b.text(), QStringLiteral("你aa\n你bb"));
    QCOMPARE(b.mc->cursorCount(), 2);
}

// TC-111：EditorArea.cursorCount 随多光标进入/退出更新；
// StatusBar 在 cursorCount > 1 时显示"光标 ×N"
void TestEditorIntegration::tc111_statusbar_cursorCount()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("aaaa\nbbbb\ncccc")), "testbench open failed");

    QCOMPARE(b.root->property("cursorCount").toInt(), 1);
    b.altClick(1);
    QCOMPARE(b.root->property("cursorCount").toInt(), 2);
    b.altClick(7);
    QCOMPARE(b.root->property("cursorCount").toInt(), 3);
    b.key(Qt::Key_Escape);
    QCOMPARE(b.root->property("cursorCount").toInt(), 1);

    // StatusBar 组件（真实组件实例化）：cursorCount=3 时出现"光标 ×3"
    // doc 经上下文属性注入（独立引擎的单例与主测试台不是同一实例）
    static const QByteArray statusQml = R"QML(
import QtQuick
import qcnote
StatusBar {
    width: 320; height: 28
    doc: testDoc
    cursorCount: 3
}
)QML";
    QQuickView statusView;
    statusView.engine()->rootContext()->setContextProperty(
                QStringLiteral("testDoc"), QVariant::fromValue(b.doc));
    QQmlComponent component(statusView.engine());
    component.setData(statusQml, QUrl());
    QVERIFY2(!component.isError(), qPrintable(component.errorString()));
    QQuickItem *statusRoot = qobject_cast<QQuickItem *>(component.create());
    QVERIFY(statusRoot != nullptr);
    statusView.setContent(QUrl(), &component, statusRoot);
    statusView.resize(320, 28);
    statusView.show();
    QVERIFY(QTest::qWaitForWindowExposed(&statusView));
    QTest::qWait(30);

    QStringList labels;
    visitItems(statusRoot, [&](QQuickItem *it) {
        const QString name = QString::fromLatin1(it->metaObject()->className());
        // Basic 样式 Label 实现为 QQuickText；Material 等样式为 QQuickLabel
        if (name.contains(QLatin1String("Text")) || name.contains(QLatin1String("Label")))
            labels << it->property("text").toString();
    });
    QVERIFY2(labels.contains(QStringLiteral("光标 ×3")),
             qPrintable(QStringLiteral("labels: %1").arg(labels.join(QLatin1Char('|')))));
    statusView.close();
}

// TC-112：单光标回归：不加多光标时输入/选择/复制/粘贴/撤销全部原生，
// 控制器全程不接管、不拦截
void TestEditorIntegration::tc112_singleCursor_regression()
{
    Bench b;
    QVERIFY2(b.open(QString()), "testbench open failed");

    b.type(QStringLiteral("hello"));
    QCOMPARE(b.text(), QStringLiteral("hello"));
    QCOMPARE(b.cursorPosition(), 5);

    // Shift+Left：原生选择（不拦截；光标 5 在文末，向左选 'o'）
    b.key(Qt::Key_Left, Qt::ShiftModifier);
    QCOMPARE(b.selectionStart(), 4);
    QCOMPARE(b.selectionEnd(), 5);
    QVERIFY(!b.mc->active());

    // Ctrl+C：原生复制
    QGuiApplication::clipboard()->clear();
    b.key(Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("o"));

    // End + Ctrl+V：原生粘贴
    b.key(Qt::Key_End);
    QCOMPARE(b.cursorPosition(), 5);
    b.key(Qt::Key_V, Qt::ControlModifier);
    QCOMPARE(b.text(), QStringLiteral("helloo"));

    // Ctrl+Z：原生撤销
    b.key(Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(b.text(), QStringLiteral("hello"));

    // 全程未接管、单光标
    QVERIFY(!b.mc->active());
    QCOMPARE(b.mc->cursorCount(), 1);
    QVERIFY(!b.layer->property("visible").toBool()); // 覆盖层隐藏

    // 普通单击（无 Alt）不激活多光标，仅原生定位
    b.plainClick(2);
    QVERIFY(!b.mc->active());
    QCOMPARE(b.cursorPosition(), 2);
}

// TC-113：标签页切换保留多光标（状态按文档独立）；外部内容重置退回单光标
void TestEditorIntegration::tc113_tabSwitch_externalReset()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("aaaa\nbbbb")), "testbench open failed"); // 文档 A
    Document *docA = b.doc;
    QQuickItem *editorA = b.editor;
    MultiCursorController *mcA = b.mc;

    b.altClick(5); // 第 2 行行首：光标 0、5
    QCOMPARE(mcA->cursorCount(), 2);
    QVERIFY(mcA->active());

    // 新建文档 B：A 失活（delegate 保留，状态不销毁）
    b.manager()->newDocument();
    b.settle();
    QVERIFY(b.manager()->activeDocument() != docA);

    // 切回 A：多光标保留，编辑继续生效
    b.manager()->setActiveDocument(docA);
    b.settle();
    QCOMPARE(mcA->cursorCount(), 2);
    QVERIFY(mcA->active());
    QCOMPARE(b.mc, mcA); // 活动编辑器即 A

    b.type(QStringLiteral("Z"));
    QCOMPARE(editorA->property("text").toString(), QStringLiteral("Zaaaa\nZbbbb"));

    // 外部重置（重新加载/另存覆盖链路）：退回单光标且不崩溃
    docA->setContent(QStringLiteral("reset"));
    b.settle();
    QCOMPARE(editorA->property("text").toString(), QStringLiteral("reset"));
    QCOMPARE(mcA->cursorCount(), 1);
    QVERIFY(!mcA->active());
    QCOMPARE(b.root->property("cursorCount").toInt(), 1);
    QCOMPARE(b.cursorPosition(), 0);
}

int main(int argc, char *argv[])
{
    // GUI 红线：自动化验证一律 offscreen，禁止可见窗口
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // offscreen 平台字体数据库需显式字体目录（Qt 不再自带字体），
    // 指向 Windows 系统字体，保证文本布局（positionToRectangle）有效
    static const char kWinFonts[] = "C:/Windows/Fonts";
    if (QDir(QString::fromLatin1(kWinFonts)).exists())
        qputenv("QT_QPA_FONTDIR", kWinFonts);
    QGuiApplication app(argc, argv);
    // 与主程序一致的样式（默认原生样式不支持控件 background 定制）
    QQuickStyle::setStyle(QStringLiteral("Material"));
    TestEditorIntegration t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_editorintegration.moc"
