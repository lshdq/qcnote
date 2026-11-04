// QML 集成测试：
// - 编辑区选中背景色（测试用例 TC-001 至 TC-005）：AppTheme.selection
//   token 取值与 TextArea selectionColor / selectedTextColor 绑定的属性断言
// - 列式编辑（测试用例 TC-101 至 TC-113 的可自动化部分）
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

#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QClipboard>
#include <QColor>
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

    // 从当前 view 的 QML 引擎取 AppTheme QML 单例（pragma Singleton，
    // per-engine 实例）：与 QML 侧 AppTheme.xxx 引用同一对象；
    // 主题切换经 setProperty("dark") 驱动全部 token 重算
    QObject *theme() const
    {
        return view.engine()->singletonInstance<QObject *>(u"qcnote", u"AppTheme");
    }

    // 切换主题并等待 QML 绑定重求值
    void setDark(bool dark)
    {
        theme()->setProperty("dark", dark);
        settle();
    }

    // TextArea 颜色属性（selectionColor / selectedTextColor）
    QColor editorColor(const char *name) const
    {
        return editor->property(name).value<QColor>();
    }

    // AppTheme token 颜色属性（selection / multiSelection / textPrimary）
    QColor themeColor(const char *name) const
    {
        return theme()->property(name).value<QColor>();
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

// ==================== tc008b 独立参考实现（朴素逐位扫描） ====================
//
// 与 MultiCursorLayer.selectionSegments 的二分实现相互独立：按 tc008 参考
// 算法思路（逐 position 扫描 positionToRectangle 的 y 跳变切分可视行）扩展到
// 多逻辑行 + toEdge 兜底语义（规格：折行断点前字符可见 → 段右缘以内容区
// 右缘兜底；空白 → R(断点前位置).x 即可见文本右缘，精确）。被测实现换算法
// 时本参考不变，二者输出必须逐段一致（等价性回归）。

struct RefSegment {
    qreal x = 0;
    qreal y = 0;
    qreal w = 0;
};

// 单逻辑行内选区块 [a, e)（不含 '\n'）→ 可视行段（朴素扫描版）
void refPushLineChunk(QVector<RefSegment> &out, const Bench &b,
                      const QString &text, int a, int e, qreal contentRight)
{
    if (e <= a)
        return;
    int p = a;
    while (true) {
        // 同一可视行内 y 恒等：向后扫到该行最后一个光标位置 last
        int last = p;
        while (last < e && b.rectOf(last + 1).y() == b.rectOf(p).y())
            ++last;
        const QRectF first = b.rectOf(p);
        if (last == e) {
            // 末段终点不在折行断点上：行内精确（宽 = 终点 x − 段起 x）
            const qreal w = b.rectOf(e).x() - first.x();
            if (w > 0)
                out.append({first.x(), first.y(), w});
            return;
        }
        // last < e：折行断点。断点前字符可见（中文逐字折行 / 超长词强制
        // 断行）→ 位置矩形取不到本行精确右缘，以内容区右缘兜底；
        // 空白 → R(last).x 已是可见文本右缘，精确
        qreal w = b.rectOf(last).x() - first.x();
        if (!text.at(last).isSpace() && w < contentRight - first.x())
            w = contentRight - first.x();
        if (w > 0)
            out.append({first.x(), first.y(), w});
        p = last + 1;
    }
}

// 全部选区（controller.selectionPairs 形态）→ 期望段列表：
// 每对先按 '\n' 拆逻辑行块，块内再按可视行拆段（与被测实现同构、算法独立）
QVector<RefSegment> referenceSegments(const Bench &b, qreal contentRight)
{
    const QString text = b.text();
    QVector<RefSegment> out;
    for (const QString &pair : b.selectionPairs()) {
        const QStringList parts = pair.split(QLatin1Char(':'));
        const int start = parts.at(0).toInt();
        const int end = parts.at(1).toInt();
        int segStart = start;
        for (int p = start; p < end; ++p) {
            if (text.at(p) == QLatin1Char('\n')) {
                refPushLineChunk(out, b, text, segStart, p, contentRight);
                segStart = p + 1;
            }
        }
        refPushLineChunk(out, b, text, segStart, end, contentRight);
    }
    return out;
}

} // namespace

// ==================== 测试类 ====================

// QML 集成层测试：
// - 编辑区选中背景色（TC-001 至 TC-005：主题 token 与选区颜色绑定）
// - 列式编辑 T3 验收（TC-101 至 TC-113 可自动化项）
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

    // ---- 编辑区选中背景色（需求：编辑区选中背景色；TC-001 至 TC-005） ----

    // TC-001：浅色主题原生选区背景为半透明强调蓝 #293564d4
    void tc001_lightTheme_selectionColor();

    // TC-002：深色主题原生选区背景为半透明强调蓝 #336f9bff
    void tc002_darkTheme_selectionColor();

    // TC-003：选中文字颜色绑定 textPrimary（两主题，保持原色可读）
    void tc003_selectedTextColor_textPrimary();

    // TC-004：选区存在时切换主题，selectionColor 随绑定即时切换
    void tc004_themeSwitch_selectionFollows();

    // TC-005：multiSelection 取值不变且与 selection 同值（观感统一）
    void tc005_multiSelection_matchesSelection();

    // ---- 自动换行（需求：自动换行） ----

    // TC-008（可自动化部分）：开启换行后跨可视行选区按可视行拆段渲染，
    // 关闭换行回到单段；覆盖 wrapMode 切换路径（onWrapModeChanged）
    void tc008_wordWrap_visualLineSegments();

    // TC-008b（可自动化部分）：中文逐字折行 / 超长 ASCII 词强制断行 / 多逻辑
    // 行跨 '\n' 选区的拆段回归（补齐 tc008 空白断点未覆盖的 toEdge 兜底分支
    // 与按逻辑行外层拆分组合，含段右缘上界断言）
    void tc008b_wordWrap_cjkLongWordMultilineSegments();

private:
    // tc008b 对拍断言：selectionSegments 与朴素参考实现逐段一致
    //（段数/x/y/w、y 严格递增、宽高为正、右缘不超内容区右缘 + 1 字符宽）
    void checkSegmentsAgainstReference(const Bench &b, const QString &phase);
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

// ==================== 编辑区选中背景色（TC-001 至 TC-005） ====================
//
// offscreen 无法做像素级视觉验证，以属性断言为准：经真实 EditorArea
// 组件（与主程序同接线，非最小复制品）读取 TextArea 的 selectionColor /
// selectedTextColor 与 AppTheme token 比较，并以 nativeDragSelect 验证
// 原生选择交互不变（选区确实产生）。颜色字面量为 #AARRGGBB 半透明格式。

// TC-001：浅色主题（默认）下原生拖选产生选区，selectionColor 绑定
// AppTheme.selection 的浅色值 #293564d4（半透明强调蓝，非系统红色）
void TestEditorIntegration::tc001_lightTheme_selectionColor()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("hello selection color")), "testbench open failed");

    QVERIFY(!b.theme()->property("dark").toBool()); // 默认浅色主题

    // 原生拖选交互不变，产生 [0,5) 选区
    b.nativeDragSelect(0, 5);
    QCOMPARE(b.selectionStart(), 0);
    QCOMPARE(b.selectionEnd(), 5);

    const QColor expected(QStringLiteral("#293564d4"));
    QCOMPARE(b.editorColor("selectionColor"), expected);
    QCOMPARE(b.themeColor("selection"), expected);
}

// TC-002：深色主题下 selectionColor 为 #336f9bff
void TestEditorIntegration::tc002_darkTheme_selectionColor()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("hello selection color")), "testbench open failed");

    b.setDark(true);
    QVERIFY(b.theme()->property("dark").toBool());

    b.nativeDragSelect(0, 5);
    QCOMPARE(b.selectionStart(), 0);
    QCOMPARE(b.selectionEnd(), 5);

    const QColor expected(QStringLiteral("#336f9bff"));
    QCOMPARE(b.editorColor("selectionColor"), expected);
    QCOMPARE(b.themeColor("selection"), expected);
}

// TC-003：selectedTextColor 绑定 AppTheme.textPrimary，两主题下选中文字
// 均保持原色（不回落到默认白色——半透明底上对比不足）
void TestEditorIntegration::tc003_selectedTextColor_textPrimary()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("hello selection color")), "testbench open failed");

    b.nativeDragSelect(0, 5);
    QVERIFY(b.selectionEnd() > b.selectionStart());

    // 浅色：#212734；正文与选中文字同色（绑定同一 token）
    QCOMPARE(b.editorColor("color"), b.themeColor("textPrimary"));
    QCOMPARE(b.editorColor("selectedTextColor"), b.themeColor("textPrimary"));
    QCOMPARE(b.editorColor("selectedTextColor"), QColor(QStringLiteral("#212734")));

    // 深色：#e8eaf0
    b.setDark(true);
    QCOMPARE(b.editorColor("color"), b.themeColor("textPrimary"));
    QCOMPARE(b.editorColor("selectedTextColor"), b.themeColor("textPrimary"));
    QCOMPARE(b.editorColor("selectedTextColor"), QColor(QStringLiteral("#e8eaf0")));
}

// TC-004：选区保持存在时切换主题（浅→深→浅），selectionColor 随
// AppTheme.selection 绑定即时切换，选区不被打断
void TestEditorIntegration::tc004_themeSwitch_selectionFollows()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("hello selection color")), "testbench open failed");

    b.nativeDragSelect(0, 5);
    QVERIFY(b.selectionEnd() > b.selectionStart());

    const QColor light(QStringLiteral("#293564d4"));
    const QColor dark(QStringLiteral("#336f9bff"));
    QCOMPARE(b.editorColor("selectionColor"), light);

    b.setDark(true); // 浅 → 深
    QCOMPARE(b.editorColor("selectionColor"), dark);
    QVERIFY(b.selectionEnd() > b.selectionStart());

    b.setDark(false); // 深 → 浅
    QCOMPARE(b.editorColor("selectionColor"), light);
    QVERIFY(b.selectionEnd() > b.selectionStart());
}

// TC-005：多光标选区 token multiSelection 取值不变（既有半透明蓝），
// 且两主题下与新增 selection 同值，原生/多光标选区观感统一
void TestEditorIntegration::tc005_multiSelection_matchesSelection()
{
    Bench b;
    QVERIFY2(b.open(QString()), "testbench open failed");

    const QColor light(QStringLiteral("#293564d4"));
    const QColor dark(QStringLiteral("#336f9bff"));

    QCOMPARE(b.themeColor("multiSelection"), light);
    QCOMPARE(b.themeColor("selection"), light);
    QCOMPARE(b.themeColor("multiSelection"), b.themeColor("selection"));

    b.setDark(true);
    QCOMPARE(b.themeColor("multiSelection"), dark);
    QCOMPARE(b.themeColor("selection"), dark);
    QCOMPARE(b.themeColor("multiSelection"), b.themeColor("selection"));
}

// ---- 自动换行（需求：自动换行） ----

// TC-008（可自动化部分）：开启换行后，跨可视行选区高亮按可视行拆段：
// 段数与独立参考实现（逐 position 扫描 positionToRectangle 的 y，按 y 变化
// 切分可视行——即修复前的朴素算法思路，与被测二分实现相互独立）一致、
// 段 y 与可视行对齐且严格递增、宽度全为正（无负宽/漏渲染）；关闭换行后
// 同一选区回到单段（NoWrap 旧行为零回归）。同时覆盖 wrapMode 切换路径
// （onWrapModeChanged → Qt.callLater(scrollCursorIntoView)）：切换回调若
// 抛 QML 运行时错误，QtTest 会因 qWarning 直接判 FAIL。
void TestEditorIntegration::tc008_wordWrap_visualLineSegments()
{
    Bench b;
    // 200 个 "word "（1000 字符）在 520px 等宽视口下折成多个可视行；
    // 末尾补第二行短文本，保证长行选区终点不与文档末尾重合
    QString words;
    for (int i = 0; i < 200; ++i)
        words += QStringLiteral("word ");
    const int len = words.size();
    QVERIFY2(b.open(words + QStringLiteral("\ntail")), "testbench open failed");

    // 显式归零基线：测试进程的 SessionManager 单例读的是用户目录真实 ini
    // （用户可能已开启 wordWrap），直接驱动 TextArea.wrapMode 与用户配置
    // 解耦；改 SessionManager.wordWrap 会经防抖写盘污染用户配置，禁走。
    // 覆盖绑定后 onWrapModeChanged 与 MultiCursorLayer 布局依赖路径不变。
    b.editor->setProperty("wrapMode", 0); // 0 = TextEdit.NoWrap
    b.settle();
    QCOMPARE(b.editor->property("wrapMode").toInt(), 0);

    // ---- 开启换行（4 = TextEdit.WrapAtWordBoundaryOrAnywhere，Qt6 枚举） ----
    b.editor->setProperty("wrapMode", 4);
    b.settle();
    QCOMPARE(b.editor->property("wrapMode").toInt(), 4);

    // 独立参考：选区 [0, len] 逐 position 扫描 y（同一逻辑行内 y 随
    // position 单调不减），按 y 变化切分可视行——与被测二分实现等价的
    // 朴素算法，二者输出必须一致（等价性回归）
    struct VisualLine { qreal y; int firstPos; };
    QVector<VisualLine> lines;
    for (int p = 0; p <= len; ++p) {
        const qreal y = b.rectOf(p).y();
        if (lines.isEmpty() || lines.last().y != y)
            lines.append({ y, p });
    }
    // 诊断提示：本断言失败（got 1）说明切换 wrap 后长行未折行——
    // ScrollView 中 TextArea 宽度只增不减，运行中 NoWrap→wrap 切换后
    // 宽度不回收（详见测试报告缺陷记录）
    QVERIFY2(lines.size() >= 3,
             qPrintable(QStringLiteral("expected 3+ visual lines, got %1").arg(lines.size())));

    // 构造跨可视行选区 [0, len)：引擎列选为纯逻辑行列计算（不经鼠标、
    // 不受可视布局影响），单逻辑行内 anchor→cursor 即一个行内选区
    b.mc->columnDragBegin(0, len);
    b.mc->columnDragEnd();
    b.settle();
    QVERIFY(b.mc->active());
    QCOMPARE(b.selectionPairs(), QStringList() << QStringLiteral("0:%1").arg(len));

    // 被测输出：覆盖层分段（MultiCursorLayer.selectionSegments 二分拆段）
    const QVariantList segs = b.layer->property("selectionSegments").toList();
    QCOMPARE(segs.size(), lines.size()); // 每可视行恰一段：无漏行、无多余段

    qreal prevY = -1;
    for (int i = 0; i < segs.size(); ++i) {
        const QVariantMap seg = segs.at(i).toMap();
        const qreal x = seg.value("x").toDouble();
        const qreal y = seg.value("y").toDouble();
        const qreal w = seg.value("w").toDouble();
        const qreal h = seg.value("h").toDouble();
        QVERIFY2(w > 0, qPrintable(QStringLiteral("segment %1 width %2 must be positive")
                                       .arg(i).arg(w)));
        QVERIFY(h > 0);
        // 段落在对应可视行的 y 上（与 positionToRectangle 同源，容差收紧）
        QVERIFY2(qAbs(y - lines.at(i).y) < 0.01,
                 qPrintable(QStringLiteral("segment %1 y %2 != visual line y %3")
                                .arg(i).arg(y).arg(lines.at(i).y)));
        QVERIFY2(y > prevY, "segment y must strictly increase");
        prevY = y;
        if (i == 0)
            QVERIFY(qAbs(x - b.rectOf(0).x()) < 0.01); // 首段从选区起点开始
    }

    // 末段终点 len 是行末（非折行断点），宽度精确 = 终点光标 x − 段起 x
    const QVariantMap lastSeg = segs.last().toMap();
    const qreal expectedW = b.rectOf(len).x() - b.rectOf(lines.last().firstPos).x();
    QVERIFY2(qAbs(lastSeg.value("w").toDouble() - expectedW) < 0.01,
             qPrintable(QStringLiteral("last segment width %1, expected %2")
                            .arg(lastSeg.value("w").toDouble()).arg(expectedW)));

    // ---- 关闭换行：同一选区回到单段（NoWrap 单逻辑行 = 单段，零回归） ----
    b.editor->setProperty("wrapMode", 0);
    b.settle();
    QCOMPARE(b.editor->property("wrapMode").toInt(), 0);
    QCOMPARE(b.layer->property("selectionSegments").toList().size(), 1);
}

// tc008b 对拍断言：被测 selectionSegments 与朴素参考实现逐段一致。
// phase 仅用于失败信息前缀定位（①跨 '\n' 单对 / ②超长词 / ③多对列选）
void TestEditorIntegration::checkSegmentsAgainstReference(const Bench &b,
                                                          const QString &phase)
{
    const qreal contentRight = b.editor->property("width").toDouble()
                             - b.editor->property("rightPadding").toDouble();
    // 容差 = 1 个中文字符宽（等宽字体 CJK 全角 advance，R(1).x − R(0).x 实测）。
    // 原因：toEdge 兜底段宽精确等于 contentRight − 段起 x（理论零超界），非兜底
    // 段右缘来自 positionToRectangle 的光标位置（恒在内容区内）；容差仅防御
    // 浮点舍入与字体度量在强制断行处至多一个可见字符的亚像素溢出——
    // 右缘超出 1 字符宽即为真实超界缺陷
    const qreal charWidth = b.rectOf(1).x() - b.rectOf(0).x();
    QVERIFY2(charWidth > 0, "CJK char width must be positive (layout not ready?)");

    const QVector<RefSegment> ref = referenceSegments(b, contentRight);
    const QVariantList segs = b.layer->property("selectionSegments").toList();
    QVERIFY2(segs.size() == ref.size(),
             qPrintable(QStringLiteral("[%1] segment count %2 != reference %3 "
                                       "(selections: %4)")
                            .arg(phase).arg(segs.size()).arg(ref.size())
                            .arg(b.selectionPairs().join(QLatin1Char(',')))));

    qreal prevY = -1;
    for (int i = 0; i < segs.size(); ++i) {
        const QVariantMap seg = segs.at(i).toMap();
        const qreal x = seg.value("x").toDouble();
        const qreal y = seg.value("y").toDouble();
        const qreal w = seg.value("w").toDouble();
        const qreal h = seg.value("h").toDouble();
        const RefSegment &r = ref.at(i);
        QVERIFY2(w > 0, qPrintable(QStringLiteral("[%1] segment %2 width %3 must be positive")
                                       .arg(phase).arg(i).arg(w)));
        QVERIFY2(h > 0, qPrintable(QStringLiteral("[%1] segment %2 height %3 must be positive")
                                       .arg(phase).arg(i).arg(h)));
        QVERIFY2(qAbs(y - r.y) < 0.01,
                 qPrintable(QStringLiteral("[%1] segment %2 y %3 != reference %4")
                                .arg(phase).arg(i).arg(y).arg(r.y)));
        QVERIFY2(y > prevY,
                 qPrintable(QStringLiteral("[%1] segment %2 y %3 must strictly increase (prev %4)")
                                .arg(phase).arg(i).arg(y).arg(prevY)));
        QVERIFY2(qAbs(x - r.x) < 0.01,
                 qPrintable(QStringLiteral("[%1] segment %2 x %3 != reference %4")
                                .arg(phase).arg(i).arg(x).arg(r.x)));
        QVERIFY2(qAbs(w - r.w) < 0.01,
                 qPrintable(QStringLiteral("[%1] segment %2 width %3 != reference %4")
                                .arg(phase).arg(i).arg(w).arg(r.w)));
        QVERIFY2(x + w <= contentRight + charWidth,
                 qPrintable(QStringLiteral("[%1] segment %2 right edge %3 exceeds content "
                                           "right %4 (tolerance 1 char width %5)")
                                .arg(phase).arg(i).arg(x + w).arg(contentRight)
                                .arg(charWidth)));
        prevY = y;
    }
}

// TC-008b（可自动化部分）：拆段算法的中文/超长词/多逻辑行回归。
// tc008 的文本（200×"word "）折行断点全落在空白处，导致 MultiCursorLayer
// 拆段的 toEdge 兜底分支（可见字符处强制断行以内容区右缘兜底）从未执行、
// 多逻辑行组合（选区跨 '\n' 的外层逻辑行拆分 + 行内可视行拆段）未覆盖、
// 段宽亦无右缘上界断言。本用例补齐（第 2 轮代码审查发现）：
// - 文本：3 行 60 字中文长行（无空格 → 逐字折行，断点全在可见字符处）+
//   1 行超长 ASCII 单词（280 字符，强制断行）+ 中文尾行，4 行均折 ≥2 可视行；
// - 选区三形态：① Ctrl+D 选词 + Shift+Down×2 → 跨 2 个 '\n' 的单对选区
//   （外层按逻辑行拆分 + 行内按可视行拆段的组合路径）；② 超长词整词选区
//   （全部断点为词内强制断行 → 每个中间段走 toEdge 兜底）；③ 跨 3 行列选
//   （多选区对形态，覆盖选区数组外层循环）；
// - 断言：段数/段坐标与独立朴素参考实现（逐 position 扫描 y 跳变，与被测
//   二分实现相互独立）完全一致；段 y 严格递增；宽高全为正；每段右缘
//   x+w ≤ 内容区右缘 + 1 个中文字符宽（容差原因见对拍函数注释）；
// - 关闭换行：同一列选区恢复为每逻辑行单段、行内精确宽度（NoWrap 零回归）。
void TestEditorIntegration::tc008b_wordWrap_cjkLongWordMultilineSegments()
{
    Bench b;
    // 行长按 520px 等宽视口（Consolas 11pt：ASCII ≈8px、CJK 全角 ≈15px）
    // 预留充分余量保证折 ≥2 可视行（下方前置校验兜底诊断）
    const QString cjkLine1 = QStringLiteral("春眠不觉晓，处处闻啼鸟。").repeated(5); // 60 字
    const QString cjkLine2 = QStringLiteral("海内存知己，天涯若比邻。").repeated(5); // 60 字
    const QString longWord = QStringLiteral("supercalifragilistic").repeated(14);     // 280 字符
    const QString cjkTail = QStringLiteral("行到水穷处，坐看云起时。").repeated(5);  // 60 字（尾行）
    const int nl0 = cjkLine1.size();            // 行 1 的 '\n' 位置
    const int l1Start = nl0 + 1;                // 行 2 起始
    const int nl1 = l1Start + cjkLine2.size();  // 行 2 的 '\n' 位置
    const int l2Start = nl1 + 1;                // 行 3（超长词）起始
    const int nl2 = l2Start + longWord.size();  // 行 3 的 '\n' 位置
    const int l3Start = nl2 + 1;                // 尾行起始
    QVERIFY2(b.open(cjkLine1 + QLatin1Char('\n') + cjkLine2 + QLatin1Char('\n')
                        + longWord + QLatin1Char('\n') + cjkTail),
             "testbench open failed");

    // 与 tc008 相同：显式驱动 wrapMode，与用户 SessionManager 配置解耦
    b.editor->setProperty("wrapMode", 0);
    b.settle();
    b.editor->setProperty("wrapMode", 4); // WrapAtWordBoundaryOrAnywhere
    b.settle();
    QCOMPARE(b.editor->property("wrapMode").toInt(), 4);

    // 前置校验：4 条逻辑行均折为 ≥2 可视行（失败即 wrap 切换未收窄编辑器
    // 宽度——同 tc008 记录的 ScrollView 宽度只增不减缺陷）
    const int lineStarts[4] = { 0, l1Start, l2Start, l3Start };
    const int lineEnds[4] = { nl0, nl1, nl2, l3Start + int(cjkTail.size()) };
    for (int li = 0; li < 4; ++li) {
        int visualLines = 1;
        for (int p = lineStarts[li] + 1; p <= lineEnds[li]; ++p) {
            if (b.rectOf(p).y() != b.rectOf(p - 1).y())
                ++visualLines;
        }
        QVERIFY2(visualLines >= 2,
                 qPrintable(QStringLiteral("logical line %1 wraps into %2 visual "
                                           "lines, need >= 2")
                                .arg(li).arg(visualLines)));
    }

    // ---- 形态 ①：跨 2 个 '\n' 的单对选区（外层逻辑行拆分 + 行内可视行拆段） ----
    b.setCursorPosition(0);
    b.settle();
    b.key(Qt::Key_D, Qt::ControlModifier); // 选词「春眠不觉晓」（标点前截断）→ [0,5)
    QCOMPARE(b.selectionPairs(), QStringList() << QStringLiteral("0:5"));
    b.key(Qt::Key_Down, Qt::ShiftModifier); // 游标 → 行 2 列 5（keepAnchor 扩展）
    b.key(Qt::Key_Down, Qt::ShiftModifier); // 游标 → 行 3 列 5：选区跨 nl0/nl1 两个 '\n'
    const int phase1End = l2Start + 5;
    QCOMPARE(b.selectionPairs(),
             QStringList() << QStringLiteral("0:%1").arg(phase1End));
    QVERIFY(b.mc->active());
    // 行 1/行 2 各为完整逻辑行块（各 ≥2 可视行）+ 行 3 词首 5 字符（单可视行）
    QVERIFY2(b.segmentCount() >= 5,
             qPrintable(QStringLiteral("phase1 expected >= 5 segments, got %1")
                            .arg(b.segmentCount())));
    checkSegmentsAgainstReference(b, QStringLiteral("phase1-crossNewline"));

    // ---- 形态 ②：超长 ASCII 单词整词选区（断点全为词内强制断行 → toEdge 兜底） ----
    b.key(Qt::Key_Escape);
    b.setCursorPosition(l2Start + 10); // 词内部
    b.settle();
    b.key(Qt::Key_D, Qt::ControlModifier); // 整词 [l2Start, nl2)
    QCOMPARE(b.selectionPairs(),
             QStringList() << QStringLiteral("%1:%2").arg(l2Start).arg(nl2));
    QVERIFY2(b.segmentCount() >= 3,
             qPrintable(QStringLiteral("phase2 long word should force-break into "
                                       ">= 3 visual lines, got %1")
                            .arg(b.segmentCount())));
    checkSegmentsAgainstReference(b, QStringLiteral("phase2-longWord"));

    // ---- 形态 ③：跨 3 行列选（多选区对形态：ranges 外层循环 + 每行内拆段） ----
    b.key(Qt::Key_Escape);
    b.mc->columnDragBegin(0, l2Start + 55); // 列区间 [0,55)，覆盖行 1~3
    b.mc->columnDragEnd();
    b.settle();
    QVERIFY(b.mc->active());
    QCOMPARE(b.selectionPairs(),
             QStringList() << QStringLiteral("0:55")
                           << QStringLiteral("%1:%2").arg(l1Start).arg(l1Start + 55)
                           << QStringLiteral("%1:%2").arg(l2Start).arg(l2Start + 55));
    QVERIFY2(b.segmentCount() >= 5,
             qPrintable(QStringLiteral("phase3 expected >= 5 segments, got %1")
                            .arg(b.segmentCount())));
    checkSegmentsAgainstReference(b, QStringLiteral("phase3-columnPairs"));

    // ---- 关闭换行：同一列选区恢复按逻辑行单段（NoWrap 零回归） ----
    b.editor->setProperty("wrapMode", 0);
    b.settle();
    QCOMPARE(b.editor->property("wrapMode").toInt(), 0);
    // 选区不变（文档坐标）；每对单逻辑行 = 单可视行 = 单段，宽度为行内精确
    // 值（无 toEdge 兜底——兜底若在 NoWrap 下误生效会在此暴露）
    const QVariantList flat = b.layer->property("selectionSegments").toList();
    QCOMPARE(flat.size(), 3);
    const int selStarts[3] = { 0, l1Start, l2Start };
    qreal prevY = -1;
    for (int i = 0; i < 3; ++i) {
        const QVariantMap seg = flat.at(i).toMap();
        const qreal x = seg.value("x").toDouble();
        const qreal y = seg.value("y").toDouble();
        const qreal w = seg.value("w").toDouble();
        const int selStart = selStarts[i];
        const qreal expectedW = b.rectOf(selStart + 55).x() - b.rectOf(selStart).x();
        QVERIFY2(w > 0,
                 qPrintable(QStringLiteral("no-wrap segment %1 width %2 must be positive")
                                .arg(i).arg(w)));
        QVERIFY2(qAbs(y - b.rectOf(selStart).y()) < 0.01,
                 qPrintable(QStringLiteral("no-wrap segment %1 y %2 != logical line y %3")
                                .arg(i).arg(y).arg(b.rectOf(selStart).y())));
        QVERIFY2(y > prevY,
                 qPrintable(QStringLiteral("no-wrap segment %1 y %2 must strictly "
                                           "increase across logical lines (prev %3)")
                                .arg(i).arg(y).arg(prevY)));
        QVERIFY2(qAbs(x - b.rectOf(selStart).x()) < 0.01,
                 qPrintable(QStringLiteral("no-wrap segment %1 x %2 != line start x %3")
                                .arg(i).arg(x).arg(b.rectOf(selStart).x())));
        QVERIFY2(qAbs(w - expectedW) < 0.01,
                 qPrintable(QStringLiteral("no-wrap segment %1 width %2 != inline "
                                           "exact %3 (toEdge must not apply)")
                                .arg(i).arg(w).arg(expectedW)));
        prevY = y;
    }
}

int main(int argc, char *argv[])
{
    // GUI 红线：自动化验证一律 offscreen，禁止可见窗口
    qputenv("QT_QPA_PLATFORM", "offscreen");
    // 副作用隔离：EditorArea.qml 引用 SessionManager（QML 单例），加载该组件
    // 会实例化 SessionManager，newDocument 等操作经防抖落盘到用户目录的
    // 真实 qcnote.ini，导致测试运行清空开发机 [documents] 节。在任何
    // QML 引擎/组件加载之前，把 UserScope ini 重定向到测试临时目录
    // （QTemporaryDir 为 main 局部变量，生命周期覆盖整个测试运行）。
    // 便携模式检测依赖 exe 旁 qcnote.portable 标记，测试环境无此标记，不受影响。
    QTemporaryDir settingsDir;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDir.path());
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
