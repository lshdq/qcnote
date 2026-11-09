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
#include <QPointingDevice>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickView>
#include <QStringList>
#include <QTextLayout>
#include <QVariant>
#include <QWheelEvent>

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

// ScrollView.contentItem 取内部 Flickable（EditorArea 的 scrollCursorIntoView
// 依赖同一路径）：非空且类名含 Flickable 时返回，否则 nullptr（调用方
// QVERIFY2 诊断实际类名）
QQuickItem *contentFlickable(QQuickItem *scroll)
{
    if (!scroll)
        return nullptr;
    QQuickItem *flick = scroll->property("contentItem").value<QQuickItem *>();
    if (!flick)
        return nullptr;
    if (!classNameMatches(flick, "Flickable"))
        return nullptr;
    return flick;
}

// item 的祖先 ScrollView：TextArea 位于其 contentItem（内部 Flickable）
// 内，向上遍历可视树父链即得（不依赖"全树唯一"假设，多文档 delegate
// 时同样命中各自编辑器的滚动视图）
QQuickItem *ancestorScrollView(QQuickItem *item)
{
    QQuickItem *p = item ? item->parentItem() : nullptr;
    while (p) {
        if (classNameMatches(p, "ScrollView"))
            return p;
        p = p->parentItem();
    }
    return nullptr;
}

// 显式 Mouse 类型的合成指针设备：offscreen 默认合成设备
//（primaryPointingDevice）类型非 Mouse，而 QQuickTextControlPrivate::
// mouseDoubleClickEvent 的双击选词分支要求 isEventFromMouseOrTouchpad
//（设备 Mouse/TouchPad）；且手工构造的 DblClick 不是 begin 事件、须借
// 第二击 press 建立的独占抓取通道送达（真实平台分别由窗口系统设备与
// 平台双击合成保证，见 tcD009）。函数内静态（首次调用时
// QGuiApplication 已就绪）
const QPointingDevice *synthMouse()
{
    static const QPointingDevice dev(QStringLiteral("tst-synth-mouse"), 1,
                                     QInputDevice::DeviceType::Mouse,
                                     QPointingDevice::PointerType::Generic,
                                     QInputDevice::Capability::Position, 1, 3);
    return &dev;
}

// editor 所在 delegate（editorHolder）：向上找持 nativeDragGuard 属性的
// 项（拖拽守卫状态的直接读取入口；找不到说明 EditorArea 结构变化）
QQuickItem *dragGuardHolder(QQuickItem *editor)
{
    QQuickItem *p = editor ? editor->parentItem() : nullptr;
    while (p) {
        if (p->property("nativeDragGuard").isValid())
            return p;
        p = p->parentItem();
    }
    return nullptr;
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

    // TextEdit.positionAt(x, y)（Q_INVOKABLE，TextArea 子项坐标系）：与
    // 控制器视觉列选的逐行命中同源，测试用它计算各行的期望区间
    int positionAt(qreal x, qreal y) const
    {
        int pos = -1;
        QMetaObject::invokeMethod(const_cast<QQuickItem *>(editor),
                                  "positionAt",
                                  Q_RETURN_ARG(int, pos),
                                  Q_ARG(qreal, x), Q_ARG(qreal, y));
        return pos;
    }

    // 行中心 y：该行任一位置光标矩形的垂直中心（行顶 y + 行高/2），
    // 与控制器逐行命中的行中心同基准
    qreal rowCenterY(int pos) const
    {
        const QRectF r = rectOf(pos);
        return r.y() + r.height() / 2;
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

    // 拖拽步进用短等待：全部 move 须在 QQuickPressHandler 的 800ms
    // 延迟按下窗口内完成，收紧裕度防 CI 重载偶发假失败
    void settleDrag() { QTest::qWait(10); }

    // 活动编辑器的内部 Flickable（ScrollView.contentItem）：拖拽守卫的
    // 直接作用对象（interactive 绑定）与滚动状态观测点（contentY）
    QQuickItem *flick() const
    {
        QQuickItem *scroll = ancestorScrollView(editor);
        return scroll ? contentFlickable(scroll) : nullptr;
    }

    // 文档选区文本（TextEdit.getText(start, end) Q_INVOKABLE）
    QString selectedText(int start, int end) const
    {
        QString ret;
        QMetaObject::invokeMethod(const_cast<QQuickItem *>(editor), "getText",
                                  Q_RETURN_ARG(QString, ret),
                                  Q_ARG(int, start), Q_ARG(int, end));
        return ret;
    }

    // 文档位置 → 窗口坐标（字符左缘中点；positionAt 与之互逆）
    QPoint pointOf(int docPos) const
    {
        const QRectF r = rectOf(docPos);
        const QPointF scene = editor->mapToScene(QPointF(r.x() + 0.5, r.y() + r.height() / 2));
        return scene.toPoint();
    }

    // 合成鼠标事件：手动构造 QMouseEvent 发送（QTest::mouseMove 生成的
    // move 事件 buttons 状态不符合按下拖拽语义，导致原生/MouseArea 拖拽
    // 选择失效；此处完全控制 button/buttons/modifiers/device）。
    // device：默认 primaryPointingDevice（offscreen 下类型非 Mouse）——
    // 双击用例（tcD009）必须传 synthMouse()：QQuickTextControl 双击选词
    // 分支的 isEventFromMouseOrTouchpad 门槛接受 Mouse 或 TouchPad，
    // offscreen 默认指点设备两者皆非；press/move/release 路径无设备门槛。
    // 注意：手工构造的 MouseButtonDblClick 事件 isBeginEvent()==false，
    // delivery 层不做 press 分发、update 通道对合成事件不可达（诊断实证），
    // 须直接 sendEvent 给 editor 驱动 item 内部链路，见 tcD009
    void sendMouse(QEvent::Type type, const QPointF &local, Qt::MouseButton button,
                   Qt::MouseButtons buttons, Qt::KeyboardModifiers mods,
                   const QPointingDevice *device = nullptr)
    {
        const QPointF global = QPointF(view.mapToGlobal(local.toPoint()));
        QMouseEvent ev(type, local, global, button, buttons, mods,
                       device ? device
                              : QPointingDevice::primaryPointingDevice());
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

    // Alt+Shift 拖拽列选（坐标驱动：文档位置经 positionToRectangle 换算为
    // TextArea 本地坐标，直接驱动控制器的视觉列选接口——与 QML MouseArea
    // 传原始坐标同源；不再合成鼠标事件，等长 ASCII 文本上与旧位置驱动
    // 结果一致）
    void columnDrag(int from, int to)
    {
        const QRectF r1 = rectOf(from);
        const QRectF r2 = rectOf(to);
        mc->columnDragBeginAt(r1.x(), r1.y() + r1.height() / 2);
        mc->columnDragUpdateAt(r2.x(), r2.y() + r2.height() / 2);
        mc->columnDragEnd();
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

    // 引擎全部光标 position 升序：空选区（start==end）不进 selectionRanges，
    // 竖直窄矩形等“全空选区”形态经此断言（与 selectionPairs 互补）
    QList<int> cursorPositionList() const
    {
        QList<int> out;
        const QVariantList raw = mc->cursorPositions();
        out.reserve(raw.size());
        for (const QVariant &v : raw)
            out.append(v.toInt());
        return out;
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

    // ---- 滚动边界反弹（需求：编辑器滚动边界反弹） ----

    // TC-004（可自动化部分）：编辑器 ScrollView（id: editorScroll）内部
    // Flickable 的 boundsBehavior == Flickable.StopAtBounds（0）；
    // Binding 按 delegate 实例化（多文档各自生效），且 wrapMode 切换
    // （onWrapModeChanged 读写 contentItem 的既有链路）后仍保持
    void tcSB004_scrollBounds_stopAtBounds();

    // ---- 滚动条常驻显示（需求：编辑器滚动条常驻显示） ----

    // TC-001/TC-002（可自动化部分）：超高内容竖向滚动条 policy ==
    // ScrollBar.AlwaysOn 且可见、interactive 可拖拽；短内容竖向滚动条
    // policy == ScrollBar.AlwaysOff 且不可见（动态 policy 切换）
    void tcSB001_002_verticalPolicy_dynamic();

    // TC-003/TC-004（可自动化部分）：NoWrap 长行横向滚动条 AlwaysOn；
    // 开启换行后内容收窄至视口宽 → size >= 1.0 → policy 自动切 AlwaysOff
    void tcSB003_004_horizontalPolicy_wrapToggle();

    // TC-009（P0，防回归）：滚动条几何可见性——常驻（AlwaysOn）时
    // effectiveScrollBarWidth/Height > 0 且滚动条实例 width/height > 0，
    // 隐藏（AlwaysOff）时 effective 恒为 0。第 1 轮缺陷根因是整体替换
    // 丢失风格层定位绑定致滚动条 0 尺寸不可见，policy/visible 断言
    // 无法拦截，本用例以几何断言兜底
    void tcSB009_geometry_nonZero();

    // ---- 列选区视觉对齐（需求：列选区视觉对齐；TC-V003 至 TC-V013） ----

    // TC-V003（P0）：鼠标列选——中文混排视觉对齐（各行起点视觉 x 一致，
    // 区间按各行实际布局命中，非字符列复制）
    void tc114_columnDrag_cjkVisualAlignment();

    // TC-V004（P0）：鼠标列选——短行不塌缩（竖直窄矩形：全空选区，
    // 旧行为会得宽区间）
    void tc115_columnDrag_shortLine_verticalNarrow();

    // TC-V005（P0）：鼠标列选——短行不塌缩（水平端点：锚行宽度不因
    // 短行端点变窄）
    void tc116_columnDrag_shortLine_horizontalEnd();

    // TC-V006（P0）：键盘列选——短行不塌缩 + 期望 x 粘性
    void tc117_keyboardColumn_shortLine_stickyX();

    // TC-V007（P1）：键盘列选——Left/Right 全角整字步进与不跨行
    void tc118_keyboardColumn_leftRight_fullWidth();

    // TC-V008（P1）：键盘列选——中文混排视觉对齐
    void tc119_keyboardColumn_cjkVisualAlignment();

    // TC-V009（P1）：列选——自动换行可视行分段（同一逻辑行多段选区）
    void tc120_columnSelect_wordWrap_visualRowSegments();

    // TC-V010（P1）：鼠标列选——反向拖拽等价
    void tc121_columnDrag_reverseDrag();

    // TC-V013（P1）：边界与状态机（单行/空文档/越界 y/Esc 退出）
    void tc122_columnSelect_boundaries_stateMachine();

    // ---- 编辑区竖直拖拽选区修复（需求：编辑区竖直拖拽选区修复） ----
    // 对应《测试用例-编辑区竖直拖拽选区修复》TC-001 至 TC-004 的可自动化
    // 部分：经合成鼠标事件驱动完整事件链路（窗口 → TextArea 的
    // QQuickPressHandler 延迟按下 → 补放 press → QTextDocument 选区），
    // 以内部 Flickable 的 contentY 恒定断言"视图未被抢为滚动"。

    // TC-001（P0）：竖直先行拖拽选区（原始缺陷场景）——先竖直下移 42px
    // 再横移，选区自按下点连续产生；守卫期间 Flickable 非交互、
    // contentY 纹丝不动；释放后守卫复位
    void tcD001_verticalFirst_dragSelect();

    // TC-002（P0）：近乎纯竖直拖拽选区——竖直为主、带自然横向漂移
    //（累计 > startDragDistance，触发延迟按下补放）跨多行
    void tcD002_mostlyVertical_dragSelect();

    // TC-003（P0）：横向先行拖拽选区（回归）——先横移超阈值（补放 press、
    // keepMouseGrab 生效）再竖直，行为与修复前一致
    void tcD003_horizontalFirst_regression();

    // TC-004（P0，可自动化部分）：守卫复位后滚轮滚动恢复——拖拽选区
    // 释放（守卫复位、interactive 恢复 true）后合成滚轮事件，
    // Flickable 正常滚动（守卫无残留禁用）
    void tcD004_wheelScroll_afterGuardRelease();

    // TC-005（P0，防回归）：非左键单击不得置守卫——QQuickPressHandler 对
    // 任意按键都发 pressed/released，若 onPressed 不过滤按键而 onReleased
    // 过滤，右/中键单击会置守卫 true 且其 release 被过滤挡住不复位，
    // 守卫卡死 → Flickable interactive 持续 false → 滚轮失效直到下次
    // 左键点击或焦点转移。断言右/中键 press-release 后守卫复位、
    // interactive 为 true；再做左键对照保持主路径覆盖
    void tcD005_nonLeftClick_guardNotStuck();

    // TC-006（P0，第 4 轮根因守护）：纯竖直拖拽（全程 |dx| ≤ 10px）立即
    // 产生选区且随步进增长——守护 QQuickPressHandler delayed-press 销毁
    // 缺陷（800ms 定时器仅横向取消，到期销毁保存的 press 后原生 move
    // 永不扩展选区），对 PointHandler 被动驱动敏感：移除 PointHandler 后
    // 本用例应失败
    void tcD006_pureVertical_immediateSelection();

    // TC-007（P1，第 4 轮新机制回归）：单击不拖拽——press→release 无位移
    //（及 ≤ 字符内微移）后无选区残留、光标定位正确（onPressed 立即定位；
    // onReleased 重放条件 last !== anchor 不触发）
    void tcD007_plainClick_noSelectionResidue();

    // TC-008（P0，第 4 轮新机制回归）：纯竖直拖拽释放后选区不被原生补放
    // 清空——onReleased 以 Qt.callLater 延迟一拍重放选区；释放后等待 2 帧
    // 再查、并跨 800ms 延迟按下定时器窗口（delayed press 到期销毁路径）
    // 复查，选区仍保留且锚/终点精确
    void tcD008_releaseReplay_survivesNativeReplay();

    // TC-009（P1，第 4 轮新机制回归，可自动化部分）：双击路径——手工构造
    // DblClick（synthMouse 设备 + 直接 sendEvent 给 editor）验证原生选词
    // 仍工作、双击会话结束后守卫/锚点复位无残留。offscreen 合成的 delivery
    // 通道与平台序列差异（第二击含 Press）如实记录；双击后拖拽按词扩展
    // 见人工待验
    void tcD009_doubleClick_nativeWordSelect();

    // TC-010（P0，第 4 轮新机制回归）：释放后立即二次拖拽——连续两次
    // press-drag-release 循环，第二次锚点正确更新为新按下点（nativeDragAnchor
    // 即时换新）、nativeDragLastPos 无上次残留（release 已复位为 -1），
    // 第二次选区锚 = 第二次按下点而非旧锚/旧终点
    void tcD010_secondDrag_anchorRenewal();

    // TC-011（P0，第 5 轮回归）：Shift+单击扩展选区——守护第 5 轮应改项：
    // 第 4 轮 onPressed 无条件 cursorPosition = pos（MoveAnchor 语义）把锚
    // 改写为按下点，Shift 扩展长度恒 0 且旧选区被清除。修复后 Shift 分支
    // 保持 KeepAnchor 语义（锚 = 当前光标 / 既有选区中光标的对端）。
    // 原生基线（qquicktextcontrol.cpp mousePressEvent KeepAnchor 分支）：
    // 锚不跟随更新——连续 Shift+click 始终自首次光标锚扩展
    void tcD011_shiftClick_extendSelection();

    // TC-012（P0，第 5 轮回归）：Shift+拖拽扩展选区——单击定位 A 后
    // Shift+press 于 B 拖到 C：选区起点必须仍是 A（不得被改写为 B），
    // 终点随拖拽更新到 C；守卫期间选区持续由 PointHandler 驱动扩展
    void tcD012_shiftDrag_extendSelection();

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

    // 构造跨可视行选区 [0, len)：视觉列选在 NoWrap 下先行构造——
    // 单逻辑行 = 单可视行，锚点（rect(0) 中心）与终点（rect(len) 中心）
    // 同行逐对命中，恰为整行区间 [0, len)（等长 ASCII 上与旧位置驱动
    // columnDragBegin(0, len) 结果一致）；随后开启 wrap，选区为文档坐标、
    // 保持不变，覆盖层照常按可视行拆段。原位置式接口已移除（DEC-003），
    // wrap 下视觉列选按可视行逐行命中、无法表达单逻辑行整行区间
    const QRectF r0 = b.rectOf(0);
    const QRectF r1 = b.rectOf(len);
    b.mc->columnDragBeginAt(r0.x(), r0.y() + r0.height() / 2);
    b.mc->columnDragUpdateAt(r1.x(), r1.y() + r1.height() / 2);
    b.mc->columnDragEnd();
    b.settle();
    QVERIFY(b.mc->active());
    QCOMPARE(b.selectionPairs(), QStringList() << QStringLiteral("0:%1").arg(len));

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
    // 原位置式接口 columnDragBegin(0, l2Start + 55) 已移除（DEC-003）：
    // 本用例行 1/2 为中文（全角 ≈15px/字）、行 3 为 ASCII（≈8px/字），
    // 视觉列选在同一 x 下的各行命中字符数必然不同，无法再现字符列
    // [0,55) 的三对选区。改用“Alt+单击 3 行行首光标 + Shift+Right 逐格
    // 扩展 55 次”构造逐位相同的三对选区（keepAnchor 锚点恒为行首）
    b.setCursorPosition(0);
    b.settle();
    b.altClick(l1Start);
    b.altClick(l2Start);
    for (int i = 0; i < 55; ++i)
        b.key(Qt::Key_Right, Qt::ShiftModifier);
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

// ==================== 滚动边界反弹（需求：编辑器滚动边界反弹） ====================
//
// TC-004 可自动化部分：offscreen 下断言活动编辑器 ScrollView（id:
// editorScroll）的 contentItem（即内部 Flickable，类名 QQuickFlickable）
// boundsBehavior == Flickable.StopAtBounds（枚举数值 0；默认
// DragAndOvershootBounds 为 3，是滚轮到边界过冲回弹的根源）。
// 修复方式为 EditorArea.qml 中对 contentItem 挂 Binding——若 Binding 的
// 目标/属性写错，QML 会输出告警且属性保持默认 3，本用例两侧任一都会
// FAIL（QtTest 将未忽略的 qWarning 直接判 FAIL，同时覆盖用例"无 QML
// 告警"的预期）。TC-001/002/003 的边界回弹动画为视觉/手感行为，
// offscreen 无法断言，人工待验（见测试报告）。

// TC-004：内部 Flickable boundsBehavior == StopAtBounds（0）；Binding 按
// delegate 实例化（多文档各自生效）；wrapMode 切换（onWrapModeChanged 读写
// contentItem 的 contentWidth 与 Qt.callLater(scrollCursorIntoView) 写
// contentY/contentX 的既有链路）后仍保持，即修复不干扰既有滚动行为
// （兼作 TC-005"换行切换后光标可见"链路的不回归验证）
void TestEditorIntegration::tcSB004_scrollBounds_stopAtBounds()
{
    // 长文本：内容远高于视口，保证 ScrollView 内部 Flickable 处于可滚动的
    // 真实状态（而非内容不足时的高度钳制路径）
    QString lines;
    for (int i = 0; i < 200; ++i)
        lines += QStringLiteral("line %1\n").arg(i);
    Bench b;
    QVERIFY2(b.open(lines), "testbench open failed");

    // 收集可视树中全部编辑器 ScrollView（Bench 只实例化 EditorArea，
    // 不含 DocumentTabs 等其他组件，单文档时应恰一个）
    const auto collectScrollViews = [&b](QList<QQuickItem *> &out) {
        out.clear();
        visitItems(b.root, [&](QQuickItem *it) {
            if (classNameMatches(it, "ScrollView"))
                out.append(it);
        });
    };
    QList<QQuickItem *> scrolls;
    collectScrollViews(scrolls);
    QVERIFY2(scrolls.size() == 1,
             qPrintable(QStringLiteral("expected 1 editor ScrollView, got %1")
                            .arg(scrolls.size())));

    QQuickItem *flick = contentFlickable(scrolls.at(0));
    QVERIFY2(flick != nullptr,
             "ScrollView.contentItem is not a Flickable (or is null)");
    const QVariant behavior = flick->property("boundsBehavior");
    QVERIFY(behavior.isValid());
    QCOMPARE(behavior.toInt(), 0); // Flickable::StopAtBounds

    // wrapMode 切换链路（与 tc008 相同显式驱动，与用户配置解耦）：
    // NoWrap → Wrap → NoWrap 后 Binding 仍生效
    b.editor->setProperty("wrapMode", 0);
    b.settle();
    b.editor->setProperty("wrapMode", 4);
    b.settle();
    b.editor->setProperty("wrapMode", 0);
    b.settle();
    QCOMPARE(flick->property("boundsBehavior").toInt(), 0);

    // 多文档：Repeater 为第二文档新建 delegate，Binding 逐实例生效
    //（若 Binding 只在首个 delegate 或 contentItem 上一次性挂载，此处会暴露）
    b.manager()->newDocument();
    b.settle();
    collectScrollViews(scrolls);
    QVERIFY2(scrolls.size() == 2,
             qPrintable(QStringLiteral("expected 2 editor ScrollViews, got %1")
                            .arg(scrolls.size())));
    for (int i = 0; i < scrolls.size(); ++i) {
        QQuickItem *f = contentFlickable(scrolls.at(i));
        QVERIFY2(f != nullptr,
                 qPrintable(QStringLiteral("ScrollView %1 contentItem is not "
                                           "a Flickable (or is null)")
                                .arg(i)));
        QCOMPARE(f->property("boundsBehavior").toInt(), 0);
    }
}

// ==================== 滚动条常驻显示（需求：编辑器滚动条常驻显示） ====================
//
// EditorArea.qml 为 editorScroll 声明显式 ScrollBar.vertical /
// ScrollBar.horizontal（interactive: true + 动态 policy
// `size < 1.0 ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff`）。
// offscreen 下断言真实组件树上 ScrollBar 的 policy/visible/interactive/size：
// QQuickScrollBar::Policy 枚举 AsNeeded=0 / AlwaysOff=1 / AlwaysOn=2；
// orientation：Qt.Vertical==2 / Qt.Horizontal==1。
// 滚动条滑块的悬停高亮、5 秒不淡出等纯视觉结论 offscreen 无法断言，
// 人工待验（TC-001/003/005 视觉部分，见测试报告）。

namespace {

// ScrollView 的某个方向"活动"ScrollBar：ScrollView 子树中可能残留
// 实例化早期的 size==0 占位 ScrollBar（policy 保持默认 AsNeeded），真正
// 随内容布局的活动实例 size 恒 > 0（可滚动 < 1.0，不可滚动 == 1.0），
// 按此筛选并断言唯一，避免命中占位实例
QQuickItem *scrollBarOf(QObject *obj, Qt::Orientation ori)
{
    QQuickItem *found = nullptr;
    const auto scan = [&](QObject *o, const auto &self) -> void {
        for (QObject *kid : o->children()) {
            if (classNameMatches(kid, "ScrollBar")
                && kid->property("orientation").toInt() == int(ori)
                && kid->property("size").toDouble() > 0.0) {
                found = qobject_cast<QQuickItem *>(kid);
            }
            self(kid, self);
        }
    };
    scan(obj, scan);
    return found;
}

// 活动编辑器 ScrollView（tcSB004 同款定位：可视树中收集）
QQuickItem *activeScrollView(QQuickItem *root)
{
    QQuickItem *scroll = nullptr;
    visitItems(root, [&](QQuickItem *it) {
        if (!scroll && classNameMatches(it, "ScrollView"))
            scroll = it;
    });
    return scroll;
}

} // namespace

// TC-001/TC-002：竖向滚动条动态 policy——超高内容 AlwaysOn/可见/可拖拽，
// 短内容 AlwaysOff/不可见
void TestEditorIntegration::tcSB001_002_verticalPolicy_dynamic()
{
    // 超高内容：200 行远超 400px 视口
    QString lines;
    for (int i = 0; i < 200; ++i)
        lines += QStringLiteral("line %1\n").arg(i);
    Bench b;
    QVERIFY2(b.open(lines), "testbench open failed");
    QTest::qWait(100); // 等待滚动条 size 布局求值

    QQuickItem *scroll = activeScrollView(b.root);
    QVERIFY2(scroll != nullptr, "editor ScrollView not found");
    QQuickItem *vbar = scrollBarOf(scroll, Qt::Vertical);
    QVERIFY2(vbar != nullptr,
             "vertical ScrollBar not found (ScrollBar.vertical not declared?)");

    // TC-001：可滚动 → size < 1.0 → AlwaysOn（常驻）
    QVERIFY2(vbar->property("size").toDouble() < 1.0,
             qPrintable(QStringLiteral("tall content: vertical size %1, "
                                       "expected < 1.0")
                            .arg(vbar->property("size").toDouble())));
    QCOMPARE(vbar->property("policy").toInt(), 2); // ScrollBar.AlwaysOn
    QVERIFY(vbar->property("visible").toBool());
    QVERIFY(vbar->property("interactive").toBool()); // TC-005 可拖拽前置
    // 滑块可见且长度有效（非零宽度/高度，常驻渲染的代理断言）
    QVERIFY(vbar->height() > 0);

    // 滚到中部后静置：policy 常驻不依赖交互状态（active 由 policy 保证）
    QQuickItem *flick = contentFlickable(scroll);
    QVERIFY(flick != nullptr);
    flick->setProperty("contentY", 500);
    QTest::qWait(100);
    QCOMPARE(vbar->property("policy").toInt(), 2);
    QVERIFY(vbar->property("visible").toBool());
    QVERIFY(flick->property("contentY").toDouble() > 0); // 确已滚动

    // TC-002：短内容一屏放下 → size >= 1.0 → AlwaysOff、不可见
    Bench b2;
    QVERIFY2(b2.open(QStringLiteral("ab\ncd")), "testbench open failed");
    QTest::qWait(100);
    QQuickItem *scroll2 = activeScrollView(b2.root);
    QVERIFY2(scroll2 != nullptr, "editor ScrollView not found (short doc)");
    QQuickItem *vbar2 = scrollBarOf(scroll2, Qt::Vertical);
    QVERIFY2(vbar2 != nullptr,
             "vertical ScrollBar not found on short doc");
    QVERIFY2(vbar2->property("size").toDouble() >= 1.0,
             qPrintable(QStringLiteral("short content: vertical size %1, "
                                       "expected >= 1.0")
                            .arg(vbar2->property("size").toDouble())));
    QCOMPARE(vbar2->property("policy").toInt(), 1); // ScrollBar.AlwaysOff
    QVERIFY(!vbar2->property("visible").toBool());
}

// TC-003/TC-004：横向滚动条动态 policy——NoWrap 超长行 AlwaysOn，
// 开启换行后内容收窄至视口宽 → size >= 1.0 → AlwaysOff
void TestEditorIntegration::tcSB003_004_horizontalPolicy_wrapToggle()
{
    // 超长单行：1000 字符在 520px 视口 NoWrap 下远超视口宽
    QString words;
    for (int i = 0; i < 200; ++i)
        words += QStringLiteral("word ");
    Bench b;
    QVERIFY2(b.open(words + QStringLiteral("\ntail")), "testbench open failed");
    // 与 tc008 相同：显式驱动 wrapMode，与用户 SessionManager 配置解耦
    b.editor->setProperty("wrapMode", 0); // NoWrap
    QTest::qWait(100);

    QQuickItem *scroll = activeScrollView(b.root);
    QVERIFY2(scroll != nullptr, "editor ScrollView not found");
    QQuickItem *hbar = scrollBarOf(scroll, Qt::Horizontal);
    QVERIFY2(hbar != nullptr,
             "horizontal ScrollBar not found (ScrollBar.horizontal not declared?)");

    // TC-003：NoWrap 长行 → 横向可滚动 → AlwaysOn（常驻）
    QVERIFY2(hbar->property("size").toDouble() < 1.0,
             qPrintable(QStringLiteral("NoWrap long line: horizontal size %1, "
                                       "expected < 1.0")
                            .arg(hbar->property("size").toDouble())));
    QCOMPARE(hbar->property("policy").toInt(), 2); // ScrollBar.AlwaysOn
    QVERIFY(hbar->property("visible").toBool());
    QVERIFY(hbar->property("interactive").toBool());

    // TC-004：开启换行 → 内容宽收窄至视口宽 → size >= 1.0 → AlwaysOff
    b.editor->setProperty("wrapMode", 4); // WrapAtWordBoundaryOrAnywhere
    QTest::qWait(100);
    QVERIFY2(hbar->property("size").toDouble() >= 1.0,
             qPrintable(QStringLiteral("wrap on: horizontal size %1, "
                                       "expected >= 1.0")
                            .arg(hbar->property("size").toDouble())));
    QCOMPARE(hbar->property("policy").toInt(), 1); // ScrollBar.AlwaysOff
    QVERIFY(!hbar->property("visible").toBool());

    // 切回 NoWrap 恢复 AlwaysOn（动态 policy 双向切换回归）
    b.editor->setProperty("wrapMode", 0);
    QTest::qWait(100);
    QCOMPARE(hbar->property("policy").toInt(), 2);
    QVERIFY(hbar->property("visible").toBool());
}

// TC-009：滚动条几何可见性（防回归）。
// ScrollView.effectiveScrollBarWidth / effectiveScrollBarHeight（Qt 6.6+）
// 在滚动条不可见或 policy==AlwaysOff 时恒为 0，常驻可见时 > 0——这是
// 对"定位绑定丢失 → 0 尺寸滚动条"缺陷最直接的拦截断言（该缺陷下
// policy/visible 均正常但几何塌缩为 0）。同时断言滚动条实例自身
// width/height > 0（风格层 parent/x/y/height 定位链存活的代理信号）。
// 覆盖三个场景：超高内容竖向几何 > 0、短内容竖向归零、NoWrap 长行
// 横向几何 > 0（且短内容横向归零）。
void TestEditorIntegration::tcSB009_geometry_nonZero()
{
    // ---- 场景 1：超高内容 → 竖向常驻，几何非零 ----
    QString lines;
    for (int i = 0; i < 200; ++i)
        lines += QStringLiteral("line %1\n").arg(i);
    Bench b;
    QVERIFY2(b.open(lines), "testbench open failed");
    QTest::qWait(100); // 等待滚动条 size/几何布局求值

    QQuickItem *scroll = activeScrollView(b.root);
    QVERIFY2(scroll != nullptr, "editor ScrollView not found");
    const qreal tallEffV = scroll->property("effectiveScrollBarWidth").toDouble();
    QVERIFY2(tallEffV > 0.0,
             qPrintable(QStringLiteral("tall content: effectiveScrollBarWidth "
                                       "= %1, expected > 0 (0-size bar defect?)")
                            .arg(tallEffV)));
    QQuickItem *vbar = scrollBarOf(scroll, Qt::Vertical);
    QVERIFY2(vbar != nullptr, "vertical ScrollBar not found");
    QVERIFY2(vbar->width() > 0.0 && vbar->height() > 0.0,
             qPrintable(QStringLiteral("tall content: vertical bar %1x%2, "
                                       "expected non-zero (style positioning "
                                       "bindings lost?)")
                            .arg(vbar->width()).arg(vbar->height())));
    // 竖向滚动条常驻时，横向滚动条不应占位（短行横向不可滚动）
    const qreal tallEffH = scroll->property("effectiveScrollBarHeight").toDouble();
    QCOMPARE(tallEffH, 0.0);

    // ---- 场景 2：短内容 → 竖向 AlwaysOff，几何归零 ----
    Bench b2;
    QVERIFY2(b2.open(QStringLiteral("ab\ncd")), "testbench open failed");
    QTest::qWait(100);
    QQuickItem *scroll2 = activeScrollView(b2.root);
    QVERIFY2(scroll2 != nullptr, "editor ScrollView not found (short doc)");
    QCOMPARE(scroll2->property("effectiveScrollBarWidth").toDouble(), 0.0);
    QCOMPARE(scroll2->property("effectiveScrollBarHeight").toDouble(), 0.0);

    // ---- 场景 3：NoWrap 长行 → 横向常驻，几何非零 ----
    QString words;
    for (int i = 0; i < 200; ++i)
        words += QStringLiteral("word ");
    Bench b3;
    QVERIFY2(b3.open(words + QStringLiteral("\ntail")), "testbench open failed");
    b3.editor->setProperty("wrapMode", 0); // NoWrap
    QTest::qWait(100);

    QQuickItem *scroll3 = activeScrollView(b3.root);
    QVERIFY2(scroll3 != nullptr, "editor ScrollView not found (NoWrap doc)");
    const qreal wideEffH = scroll3->property("effectiveScrollBarHeight").toDouble();
    QVERIFY2(wideEffH > 0.0,
             qPrintable(QStringLiteral("NoWrap long line: "
                                       "effectiveScrollBarHeight = %1, "
                                       "expected > 0 (0-size bar defect?)")
                            .arg(wideEffH)));
    QQuickItem *hbar3 = scrollBarOf(scroll3, Qt::Horizontal);
    QVERIFY2(hbar3 != nullptr, "horizontal ScrollBar not found");
    QVERIFY2(hbar3->width() > 0.0 && hbar3->height() > 0.0,
             qPrintable(QStringLiteral("NoWrap long line: horizontal bar "
                                       "%1x%2, expected non-zero (style "
                                       "positioning bindings lost?)")
                            .arg(hbar3->width()).arg(hbar3->height())));
}

// ==================== 列选区视觉对齐（TC-V003 至 TC-V013） ====================
//
// 需求：列选区视觉对齐。鼠标 Alt+Shift 拖拽与 Ctrl+Alt+Shift+方向键共用
// 控制器的“视觉列选会话”：矩形两边 = 锚点视觉 x 与活动点期望视觉 x，
// 对覆盖的每个可视行用 positionAt(边 x, 行中心 y) 逐行命中（区间归一
// 正向、短行各自钳制行末；键盘期望 x 粘性；反向拖拽等价）。
// 断言约定：坐标一律经 editor 的 positionToRectangle / positionAt 换算，
// 不写死像素常量（字体度量不可移植）；视觉对齐容差 ±2.0（验收标准 3）。
// 注意：空选区不进 selectionRanges（控制器只报非空区间），竖直窄矩形
// 等“全空选区”形态以 cursorPositionList 断言。

// TC-V003（P0）：鼠标列选——中文混排视觉对齐。
// 文本 3 行（中文/ASCII/中文混排）；两边取行 1 位置 2（`你好` 之后）与
// 位置 5（`wor` 之后）的光标矩形 x，跨 3 行拖拽。断言：
// - 恰 3 个选区（每行一段）；
// - 各行选区起点视觉 x 与行 1 起点一致（±2.0，验收标准 3）；
// - 各行字符区间 = positionAt(同 x, 行中心) 命中（按各行实际布局），
//   非把行 1 的字符列复制到所有行：行 1 起点为 2，行 2 全 ASCII 同一
//   视觉 x 命中约 4 个字符之后，既 ≠ 2，也 ≠ 旧行为复制的 8+2=10。
void TestEditorIntegration::tc114_columnDrag_cjkVisualAlignment()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("你好world\nabcdefgh\n中文xyzabc")),
             "testbench open failed");

    const qreal x1 = b.rectOf(2).x();   // `你好` 之后（CJK 边界）
    const qreal x2 = b.rectOf(5).x();   // `wor` 之后（混排边界）
    QVERIFY2(x2 - x1 > 10.0,
             qPrintable(QStringLiteral("layout sanity: x2-x1=%1").arg(x2 - x1)));

    // 锚点行 1 → 活动点行 3；行中心 y 取各逻辑行首位置的光标矩形
    b.mc->columnDragBeginAt(x1, b.rowCenterY(0));
    b.mc->columnDragUpdateAt(x2, b.rowCenterY(17));
    b.mc->columnDragEnd();
    b.settle();

    QVERIFY(b.mc->active());
    QVERIFY(!b.mc->columnSelecting());
    const QStringList pairs = b.selectionPairs();
    QCOMPARE(pairs.size(), 3);

    const int lineStarts[3] = { 0, 8, 17 };
    int rowStarts[3] = { -1, -1, -1 };
    for (int i = 0; i < 3; ++i) {
        const QStringList se = pairs.at(i).split(QLatin1Char(':'));
        const int s = se.at(0).toInt();
        const int e = se.at(1).toInt();
        rowStarts[i] = s;
        // 区间端点 = 同 x 在该行中心的 positionAt 命中（逐行命中语义）
        const qreal yc = b.rowCenterY(lineStarts[i]);
        const int expS = b.positionAt(x1, yc);
        const int expE = b.positionAt(x2, yc);
        QVERIFY2(s == expS && e == expE,
                 qPrintable(QStringLiteral("row %1 pair [%2,%3) != layout hit "
                                           "[%4,%5)")
                                .arg(i).arg(s).arg(e).arg(expS).arg(expE)));
        // 起点视觉对齐行 1 起点（±2.0）
        QVERIFY2(qAbs(b.rectOf(s).x() - x1) <= 2.0,
                 qPrintable(QStringLiteral("row %1 start x %2 vs anchor x %3 "
                                           "(diff %4)")
                                .arg(i).arg(b.rectOf(s).x()).arg(x1)
                                .arg(b.rectOf(s).x() - x1)));
    }
    // 非“字符列复制”：行 2（全 ASCII）同一视觉 x 命中的起点既不同于行 1
    // 起点字符位置 2，也不同于旧行为直接套用的字符列 8+2=10
    QVERIFY2(rowStarts[1] != 2 && rowStarts[1] != 8 + 2,
             qPrintable(QStringLiteral("row 2 start %1 must be layout-hit "
                                       "(not 2, not char-column copy 10)")
                            .arg(rowStarts[1])));
}

// TC-V004（P0）：鼠标列选——短行不塌缩（竖直窄矩形）。
// `aaaaaaaaaa\nbb\ncccccccccc`：行 1 位置 8 的光标 x 竖直拖到行 3 同 x。
// 期望 3 个空选区光标（文档坐标按实际行偏移计算）：行 1 命中列 8（8:8）、
// 行 2 钳制行末（`bb` 之后 = 13:13）、行 3 命中列 22（22:22）。旧行为把
// 锚行字符列矩形套到所有行，短行钳制列参与 min/max 后会产生宽区间
// （如 [2,8)），绝不能出现——空选区不进 selectionRanges，故
// selectionPairs 必为空、光标集合恰为上述三点。
void TestEditorIntegration::tc115_columnDrag_shortLine_verticalNarrow()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("aaaaaaaaaa\nbb\ncccccccccc")),
             "testbench open failed");

    const qreal x = b.rectOf(8).x();   // 行 1 位置 8 的光标 x
    b.mc->columnDragBeginAt(x, b.rowCenterY(0));
    b.mc->columnDragUpdateAt(x, b.rowCenterY(14));   // 行 3 首字符位置
    b.mc->columnDragEnd();
    b.settle();

    QVERIFY(b.mc->active());
    QCOMPARE(b.mc->cursorCount(), 3);
    QVERIFY(!b.mc->columnSelecting());
    // 绝不能出现宽区间（旧行为 [2,8) 类塌缩矩形）
    QCOMPARE(b.selectionPairs().size(), 0);
    QCOMPARE(b.root->property("selectionLength").toInt(), 0);
    // 各行独立钳制：行 1 命中列 8、行 2 行末 13、行 3 命中列 22
    QCOMPARE(b.cursorPositionList(), QList<int>() << 8 << 13 << 22);
}

// TC-V005（P0）：鼠标列选——短行不塌缩（水平端点）。
// 同 TC-V004 文本：锚点行 1 位置 2，活动点拖到行 2、x 超出行 2 行末
// （取行 1 位置 8 的 x）。期望：行 1 保持完整 [2,8)（宽度不因短行端点
// 变窄）；行 2 钳制行末 13（空选区）。
void TestEditorIntegration::tc116_columnDrag_shortLine_horizontalEnd()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("aaaaaaaaaa\nbb\ncccccccccc")),
             "testbench open failed");

    const qreal xA = b.rectOf(2).x();  // 锚点：行 1 位置 2
    const qreal x8 = b.rectOf(8).x();  // 活动点 x：行 1 位置 8（远超行 2 行末）
    b.mc->columnDragBeginAt(xA, b.rowCenterY(0));
    b.mc->columnDragUpdateAt(x8, b.rowCenterY(11));  // 行 2 首字符位置
    b.mc->columnDragEnd();
    b.settle();

    QVERIFY(b.mc->active());
    QCOMPARE(b.mc->cursorCount(), 2);
    // 行 1 完整 [2,8)；行 2 钳制行末 13（空选区不进 selectionRanges）
    QCOMPARE(b.selectionPairs(), QStringList() << QStringLiteral("2:8"));
    QCOMPARE(b.root->property("selectionLength").toInt(), 6);
    QCOMPARE(b.cursorPositionList(), QList<int>() << 8 << 13);
}

// TC-V006（P0）：键盘列选——短行不塌缩 + 期望 x 粘性。
// 同 TC-V004 文本；光标行 1 列 8（位置 8）。Ctrl+Alt+Shift+Down：
// 第一次 → 行 1-2，行 1 恒空选区 8（不塌缩）、行 2 钳制行末 13；
// 第二次 → 行 1-3，行 3 恢复粘性列 22（行 1 视觉 x 在行 3 的命中），
// 而非短行钳制列 16（= 行 3 起始 14 + 行 2 行末列 2）。
void TestEditorIntegration::tc117_keyboardColumn_shortLine_stickyX()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("aaaaaaaaaa\nbb\ncccccccccc")),
             "testbench open failed");

    b.setCursorPosition(8);
    b.settle();
    const auto sel = Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier;

    // 第一次 Down：行 1-2，全部空选区（无宽区间、行 1 不塌缩）
    b.key(Qt::Key_Down, sel);
    QVERIFY(b.mc->active());
    QCOMPARE(b.selectionPairs().size(), 0);
    QCOMPARE(b.cursorPositionList(), QList<int>() << 8 << 13);

    // 第二次 Down：行 3 恢复期望 x 粘性列 22（非短行钳制列 16）
    b.key(Qt::Key_Down, sel);
    QCOMPARE(b.selectionPairs().size(), 0);
    QCOMPARE(b.mc->cursorCount(), 3);
    QCOMPARE(b.cursorPositionList(), QList<int>() << 8 << 13 << 22);
}

// TC-V007（P1）：键盘列选——Left/Right 全角整字步进与不跨行。
// `你好world\nabcdefgh`：
// - 行首光标 Ctrl+Alt+Shift+Right 一次 → 选区 [0,1)（整字跨过 `你`，
//   期望 x 更新为其右缘）；再一次 → [0,2)；
// - Down 扩展到行 2：行 2 选区起点 = 行首（视觉对齐 x(0)），终点 =
//   positionAt(x(2), 行 2 中心) 命中（非字符列复制的 8+2=10）；
// - 行末（文档末）继续 Right：实际步进防护（DEC-006）→ 严格无操作，
//   选区区间不变；
// - 行首 Left：跨行被拒（Left/Right 不跨行，DEC-005）→ 无操作。
void TestEditorIntegration::tc118_keyboardColumn_leftRight_fullWidth()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("你好world\nabcdefgh")),
             "testbench open failed");
    const auto sel = Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier;

    // ---- 全角整字步进 ----
    b.setCursorPosition(0);
    b.settle();
    b.key(Qt::Key_Right, sel);
    QCOMPARE(b.selectionPairs(), QStringList() << QStringLiteral("0:1"));
    b.key(Qt::Key_Right, sel);
    QCOMPARE(b.selectionPairs(), QStringList() << QStringLiteral("0:2"));

    // ---- Down 扩展到行 2（视觉对齐起点：x(0) 即行首左缘，字符位置不同） ----
    b.key(Qt::Key_Down, sel);
    const QStringList pairs = b.selectionPairs();
    QCOMPARE(pairs.size(), 2);
    QCOMPARE(pairs.at(0), QStringLiteral("0:2"));   // 行 1 选区不变
    const QStringList p2 = pairs.at(1).split(QLatin1Char(':'));
    QCOMPARE(p2.at(0).toInt(), 8);                  // 行 2 起点 = 行首
    const int row2End = p2.at(1).toInt();
    QCOMPARE(row2End, b.positionAt(b.rectOf(2).x(), b.rowCenterY(8)));
    QVERIFY2(row2End != 8 + 2,
             qPrintable(QStringLiteral("row 2 end %1 must be layout-hit, not "
                                       "char-column copy (8+2=10)")
                            .arg(row2End)));

    // ---- 行末（文档末）Right 无操作：步进到底后继续按，选区不变 ----
    for (int i = 0; i < 30; ++i)   // 远超行 2 剩余字符数，到底后余下均无操作
        b.key(Qt::Key_Right, sel);
    const QStringList atEnd = b.selectionPairs();
    QCOMPARE(atEnd.size(), 2);
    QCOMPARE(atEnd.at(1).split(QLatin1Char(':')).at(1).toInt(), 16);  // 到文档末
    for (int i = 0; i < 3; ++i)
        b.key(Qt::Key_Right, sel);
    QCOMPARE(b.selectionPairs(), atEnd);   // 越界 Right：严格无操作

    // ---- 行首 Left 无操作：新会话锚在行 2 行首（位置 8），Left 跨行被拒 ----
    b.key(Qt::Key_Escape);
    b.setCursorPosition(8);
    b.settle();
    b.key(Qt::Key_Left, sel);
    b.key(Qt::Key_Left, sel);
    b.key(Qt::Key_Left, sel);
    QCOMPARE(b.selectionPairs().size(), 0);      // 无选区变化（无操作）
    QCOMPARE(b.cursorPositionList(), QList<int>() << 8);
    QCOMPARE(b.mc->cursorCount(), 1);
}

// TC-V008（P1）：键盘列选——中文混排视觉对齐。
// `你好world\nabcdefgh`：光标行 1 位置 4（`你好` 之后的混排边界）；
// Ctrl+Alt+Shift+Right×2 后 Down 扩展到行 2。断言：行 2 选区起点视觉 x
// 与行 1 起点一致（±2.0）；字符位置按行 2 布局命中（全 ASCII 行同一
// 视觉 x 命中更多字符），既 ≠ 行 1 起点字符位置 4，也 ≠ 字符列复制的
// 8+4=12——即不是同字符列复制。
void TestEditorIntegration::tc119_keyboardColumn_cjkVisualAlignment()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("你好world\nabcdefgh")),
             "testbench open failed");
    const auto sel = Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier;

    b.setCursorPosition(4);
    b.settle();
    b.key(Qt::Key_Right, sel);
    b.key(Qt::Key_Right, sel);
    QCOMPARE(b.selectionPairs(), QStringList() << QStringLiteral("4:6"));
    b.key(Qt::Key_Down, sel);
    b.settle();

    const QStringList pairs = b.selectionPairs();
    QCOMPARE(pairs.size(), 2);
    QCOMPARE(pairs.at(0), QStringLiteral("4:6"));   // 行 1 选区不变
    const QStringList p2 = pairs.at(1).split(QLatin1Char(':'));
    const int row2Start = p2.at(0).toInt();
    // 行内区间 = 布局命中
    QCOMPARE(row2Start, b.positionAt(b.rectOf(4).x(), b.rowCenterY(8)));
    QCOMPARE(p2.at(1).toInt(), b.positionAt(b.rectOf(6).x(), b.rowCenterY(8)));
    // 非字符列复制：≠ 行 1 起点字符位置 4，≠ 旧行为套用的 8+4=12
    QVERIFY2(row2Start != 4 && row2Start != 8 + 4,
             qPrintable(QStringLiteral("row 2 start %1 must be layout-hit "
                                       "(not 4, not char-column copy 12)")
                            .arg(row2Start)));
    // 视觉对齐：行 2 起点视觉 x 与行 1 起点（锚 x）一致（±2.0）
    const qreal anchorX = b.rectOf(4).x();
    QVERIFY2(qAbs(b.rectOf(row2Start).x() - anchorX) <= 2.0,
             qPrintable(QStringLiteral("row 2 start x %1 vs anchor x %2 "
                                       "(diff %3)")
                            .arg(b.rectOf(row2Start).x()).arg(anchorX)
                            .arg(b.rectOf(row2Start).x() - anchorX)));
}

// TC-V009（P1）：列选——自动换行可视行分段。
// 单逻辑行长文本（120×"word "，600 字符）在 wrap 下折为 ≥3 可视行；
// 键盘列选（行首锚点 Right×5 后逐可视行 Down）：同一逻辑行按可视行
// 产生多段选区（每段在各自段内命中），覆盖层段数同步增加（选区与
// 拆段渲染链路一致，参考 tc008 的 segmentCount 断言）。
void TestEditorIntegration::tc120_columnSelect_wordWrap_visualRowSegments()
{
    QString words;
    for (int i = 0; i < 120; ++i)
        words += QStringLiteral("word ");
    Bench b;
    QVERIFY2(b.open(words), "testbench open failed");

    // 显式驱动 wrapMode（与用户配置解耦，同 tc008 模式）：NoWrap → Wrap
    b.editor->setProperty("wrapMode", 0);
    b.settle();
    b.editor->setProperty("wrapMode", 4);   // WrapAtWordBoundaryOrAnywhere
    b.settle();
    QCOMPARE(b.editor->property("wrapMode").toInt(), 4);

    // 前置校验 + 期望数据：逐 position 扫描 y 跳变切分可视行（tc008 的
    // 朴素参考法），取各可视行首个 position（文本全 ASCII，各行同网格）
    QVector<int> rowFirst;
    rowFirst.append(0);
    for (int p = 1; p <= words.size(); ++p) {
        if (b.rectOf(p).y() != b.rectOf(p - 1).y())
            rowFirst.append(p);
    }
    QVERIFY2(rowFirst.size() >= 3,
             qPrintable(QStringLiteral("expected >= 3 visual lines, got %1")
                            .arg(rowFirst.size())));

    const auto sel = Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier;
    b.setCursorPosition(0);
    b.settle();
    for (int i = 0; i < 5; ++i)
        b.key(Qt::Key_Right, sel);
    QCOMPARE(b.selectionPairs(), QStringList() << QStringLiteral("0:5"));

    // Down 一次：同一逻辑行的前两个可视行各产生一段选区
    b.key(Qt::Key_Down, sel);
    const int r2 = rowFirst.at(1);
    QStringList pairs = b.selectionPairs();
    QCOMPARE(pairs,
             QStringList() << QStringLiteral("0:5")
                           << QStringLiteral("%1:%2").arg(r2).arg(r2 + 5));
    QCOMPARE(b.segmentCount(), 2);

    // 再 Down 一次：第三个可视行再增一段（选区与段数同步增长）
    b.key(Qt::Key_Down, sel);
    const int r3 = rowFirst.at(2);
    pairs = b.selectionPairs();
    QCOMPARE(pairs,
             QStringList() << QStringLiteral("0:5")
                           << QStringLiteral("%1:%2").arg(r2).arg(r2 + 5)
                           << QStringLiteral("%1:%2").arg(r3).arg(r3 + 5));
    QCOMPARE(b.segmentCount(), 3);

    // 每段选区各落在单一可视行内（起/终点同 y），且逐段对应第 i 个可视行
    for (int i = 0; i < pairs.size(); ++i) {
        const QStringList se = pairs.at(i).split(QLatin1Char(':'));
        const qreal sy = b.rectOf(se.at(0).toInt()).y();
        QVERIFY2(qAbs(sy - b.rectOf(se.at(1).toInt()).y()) < 0.01,
                 qPrintable(QStringLiteral("pair %1 [%2] spans multiple visual "
                                           "lines")
                                .arg(i).arg(pairs.at(i))));
        QVERIFY2(qAbs(sy - b.rectOf(rowFirst.at(i)).y()) < 0.01,
                 qPrintable(QStringLiteral("pair %1 [%2] not on visual line %3")
                                .arg(i).arg(pairs.at(i)).arg(i)));
    }
}

// TC-V010（P1）：鼠标列选——反向拖拽等价。
// 与 tc114（TC-V003）同文本同矩形，自下而上拖拽（BeginAt 在下行 3、
// UpdateAt 在上行 1）：行范围与行内区间均 qMin/qMax 归一，选区集合与
// 正向拖拽完全相同。
void TestEditorIntegration::tc121_columnDrag_reverseDrag()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("你好world\nabcdefgh\n中文xyzabc")),
             "testbench open failed");

    const qreal x1 = b.rectOf(2).x();
    const qreal x2 = b.rectOf(5).x();

    // 正向：行 1 (x1) → 行 3 (x2)
    b.mc->columnDragBeginAt(x1, b.rowCenterY(0));
    b.mc->columnDragUpdateAt(x2, b.rowCenterY(17));
    b.mc->columnDragEnd();
    b.settle();
    const QStringList forward = b.selectionPairs();
    QCOMPARE(forward.size(), 3);

    // 反向：同一矩形，锚点在行 3 (x2)，活动点拖到行 1 (x1)
    b.mc->columnDragBeginAt(x2, b.rowCenterY(17));
    b.mc->columnDragUpdateAt(x1, b.rowCenterY(0));
    b.mc->columnDragEnd();
    b.settle();

    QCOMPARE(b.mc->cursorCount(), 3);
    QCOMPARE(b.selectionPairs(), forward);   // 与正向拖拽完全相同
}

// TC-V013（P1）：边界与状态机。
// - 单行文档：键盘列选 Up/Down 收敛回锚行（首/末行为同一行：无操作、
//   不塌陷、不报错）；
// - 空文档：拖拽与键盘列选不崩溃、不产生越界位置；
// - 拖拽 y 越出文本上/下边界：positionAt 钳制到文档边界，区间对收敛、
//   端点不越界（需求“边界与约束”）；
// - Esc 退出：恢复单光标（保留末光标），列选状态结束。
void TestEditorIntegration::tc122_columnSelect_boundaries_stateMachine()
{
    const auto sel = Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier;

    // ---- 单行文档：Up/Down 均收敛（无操作，不塌陷不报错） ----
    {
        Bench b;
        QVERIFY2(b.open(QStringLiteral("hello")), "testbench open failed");
        b.setCursorPosition(2);
        b.settle();
        b.key(Qt::Key_Up, sel);    // 首行 Up：钳制回锚行（首行即末行）
        QCOMPARE(b.mc->cursorCount(), 1);
        QVERIFY(!b.mc->active());
        QCOMPARE(b.selectionPairs().size(), 0);
        QCOMPARE(b.cursorPositionList(), QList<int>() << 2);
        b.key(Qt::Key_Down, sel);  // 末行 Down：同样钳制回锚行
        QCOMPARE(b.mc->cursorCount(), 1);
        QCOMPARE(b.selectionPairs().size(), 0);
        QCOMPARE(b.cursorPositionList(), QList<int>() << 2);
    }

    // ---- 空文档：拖拽/键盘列选不崩溃、无越界位置 ----
    {
        Bench b;
        QVERIFY2(b.open(QString()), "testbench open failed");
        b.mc->columnDragBeginAt(10.0, 4.0);
        b.mc->columnDragUpdateAt(60.0, 4.0);
        b.mc->columnDragEnd();
        b.settle();
        QCOMPARE(b.mc->cursorCount(), 1);
        QVERIFY(!b.mc->active());
        QVERIFY(!b.mc->columnSelecting());
        QCOMPARE(b.cursorPositionList(), QList<int>() << 0);

        b.key(Qt::Key_Down, sel);
        b.key(Qt::Key_Up, sel);
        QCOMPARE(b.mc->cursorCount(), 1);
        QCOMPARE(b.cursorPositionList(), QList<int>() << 0);
    }

    // ---- 拖拽 y 越界：钳制收敛、区间不越界；Esc 退出恢复 ----
    {
        Bench b;
        QVERIFY2(b.open(QStringLiteral("aaaa\nbbbb\ncccc")), "testbench open failed");
        const int len = 14;
        const qreal x1 = b.rectOf(1).x();
        const qreal x2 = b.rectOf(3).x();
        b.mc->columnDragBeginAt(x1, b.rowCenterY(0));

        // y 越出文本上方（y=-50）：positionAt 钳制到首行 → 收敛为单行区间
        b.mc->columnDragUpdateAt(x2, -50.0);
        b.settle();
        QCOMPARE(b.selectionPairs(), QStringList() << QStringLiteral("1:3"));

        // y 越出文末下方（远超文末）：钳制到末行 → 三行全区间
        b.mc->columnDragUpdateAt(x2, 10000.0);
        b.settle();
        const QStringList pairs = b.selectionPairs();
        QCOMPARE(pairs, QStringList() << QStringLiteral("1:3")
                                      << QStringLiteral("6:8")
                                      << QStringLiteral("11:13"));
        // 收敛不越界：全部区间端点在 [0, len]
        for (const QString &p : pairs) {
            const QStringList se = p.split(QLatin1Char(':'));
            QVERIFY2(se.at(0).toInt() >= 0 && se.at(1).toInt() <= len,
                     qPrintable(QStringLiteral("pair %1 out of range [0,%2]")
                                    .arg(p).arg(len)));
        }
        b.mc->columnDragEnd();
        b.settle();
        QVERIFY(b.mc->active());

        // Esc：退出多光标（保留末光标），列选状态结束
        b.key(Qt::Key_Escape);
        QCOMPARE(b.mc->cursorCount(), 1);
        QVERIFY(!b.mc->active());
        QVERIFY(!b.mc->columnSelecting());
    }
}

// ==================== 编辑区竖直拖拽选区修复（TC-001 至 TC-004 可自动化部分） ====================
//
// 需求：编辑区竖直拖拽选区修复。根因在 Qt 框架内部机制（QQuickPressHandler
// 延迟按下窗口内 keepMouseGrab 未设置，ScrollView 内部 Flickable 经
// childMouseEventFilter 抢夺竖直拖拽转为视图滚动），项目侧以 EditorArea.qml
// 的 nativeDragGuard 守卫规避：按下期间将 contentItem（内部 Flickable）的
// interactive 绑定为 false，阻断抢夺与拖动；释放（或失焦兜底）即恢复。
//
// 断言设计：
// - 拖拽全程 contentY 恒等于初始值：视图未被抢为滚动（缺陷的直接可观测面）
//   ——抢夺一旦发生，竖直位移会转换为 contentY 增长；
// - 按下期间 nativeDragGuard == true 且 Flickable.interactive == false
//   （守卫与 Binding 生效的直接断言）；
// - 选区产生与更新：selectionEnd > selectionStart、锚 == 按下点（补放的
//   press 保留原始坐标）、终点 == positionAt(终点坐标)（与逐行命中同源）；
// - 释放后守卫复位（nativeDragGuard == false、interactive == true），
//   合成滚轮事件滚动恢复（无残留禁用）。
// 合成时序：tcD001/tcD002 每步 settleDrag() 10ms（全部 move 须在 800ms
// 延迟窗口内完成）；tcD003 及之后每步 settle() 30ms。横向
// 位移超 startDragDistance（约 10px）触发延迟 press 补放；窗口外拖拽、
// 真实滚轮/滚动条实体手感等见人工待验清单。
//
// 公共文本：200 行（内容高度远超 400px 视口）保证垂直可滚动——守卫失效
// 时 Flickable 才具备抢夺竖直拖拽的前提（该轴可滚动）。

// TC-001（P0）：竖直先行拖拽选区（原始缺陷场景）。
// 按下后先纯竖直下移 6 步（累计 42px，超 Flickable 抢夺阈值约 10px，
// 横向零位移），再横移 24px（超 startDragDistance 触发 press 补放）：
// 选区自按下点连续产生，contentY 全程恒定，释放后守卫复位。
// 注意：全部 move 须在 800ms 延迟窗口（QQuickPressHandler 定时器）内
// 完成，故步进用 10ms 短等待（含 press 后 settle，同样构成窗口压力）；
// 若 CI 重载导致偶发假失败，直接重跑本用例甄别。
void TestEditorIntegration::tcD001_verticalFirst_dragSelect()
{
    QString lines;
    for (int i = 0; i < 200; ++i)
        lines += QStringLiteral("line %1\n").arg(i);
    Bench b;
    QVERIFY2(b.open(lines), "testbench open failed");

    QQuickItem *flick = b.flick();
    QVERIFY2(flick != nullptr,
             "editor ScrollView contentItem is not a Flickable (or is null)");
    QVERIFY2(flick->property("contentHeight").toDouble()
                     > flick->property("height").toDouble(),
             "content must overflow viewport (vertical scroll steal premise)");
    QQuickItem *holder = dragGuardHolder(b.editor);
    QVERIFY2(holder != nullptr, "nativeDragGuard holder (delegate) not found");

    // 初始状态：守卫复位、Flickable 交互开启、位于顶部
    QCOMPARE(holder->property("nativeDragGuard").toBool(), false);
    QCOMPARE(flick->property("interactive").toBool(), true);
    const qreal contentY0 = flick->property("contentY").toDouble();
    QCOMPARE(contentY0, 0.0);

    const int pressPos = 3; // 第 0 行 "lin|e 0"
    const QPoint p0 = b.pointOf(pressPos);

    // 按下：守卫立即生效（interactive = false，两条抢夺路径同时被阻断）
    b.sendMouse(QEvent::MouseButtonPress, p0, Qt::LeftButton, Qt::LeftButton,
                Qt::NoModifier);
    b.settleDrag();
    QCOMPARE(holder->property("nativeDragGuard").toBool(), true);
    QCOMPARE(flick->property("interactive").toBool(), false);

    // 先竖直向下 6 步（每步 7px、累计 42px，横向零位移）：无守卫时
    // Flickable 将在第 2 步起抢夺独占抓取转为视图滚动（contentY 增长）
    for (int i = 1; i <= 6; ++i) {
        b.sendMouse(QEvent::MouseMove, p0 + QPoint(0, i * 7), Qt::NoButton,
                    Qt::LeftButton, Qt::NoModifier);
        b.settleDrag();
        QVERIFY2(flick->property("contentY").toDouble() == contentY0,
                 qPrintable(QStringLiteral("step %1: contentY %2 changed "
                                           "(drag stolen as scroll)")
                                .arg(i)
                                .arg(flick->property("contentY").toDouble())));
    }

    // 再横移 24px（超 startDragDistance）：延迟按下补放，选区自按下点展开
    const QPoint p1 = p0 + QPoint(24, 42);
    b.sendMouse(QEvent::MouseMove, p1, Qt::NoButton, Qt::LeftButton,
                Qt::NoModifier);
    b.settleDrag();
    QVERIFY2(b.selectionEnd() > b.selectionStart(),
             qPrintable(QStringLiteral("no selection after horizontal move "
                                       "(start=%1 end=%2)")
                            .arg(b.selectionStart()).arg(b.selectionEnd())));
    QCOMPARE(b.selectionStart(), pressPos); // 锚 = 按下点（正向拖拽）
    const QPointF local1 = b.editor->mapFromScene(QPointF(p1));
    QCOMPARE(b.selectionEnd(), b.positionAt(local1.x(), local1.y()));
    QCOMPARE(flick->property("contentY").toDouble(), contentY0);
    const int end1 = b.selectionEnd();

    // 选区持续更新：继续横移并下移一行（目标行是短行，纯横移会双双
    // 钳制到行末同一位置，无法体现更新；跨行下移驱动选区真实增长）
    const QPoint p2 = p0 + QPoint(48, 56);
    b.sendMouse(QEvent::MouseMove, p2, Qt::NoButton, Qt::LeftButton,
                Qt::NoModifier);
    b.settleDrag();
    QVERIFY2(b.selectionEnd() > end1,
             qPrintable(QStringLiteral("selection must keep updating "
                                       "(end %1 not > %2)")
                            .arg(b.selectionEnd()).arg(end1)));
    const QPointF local2 = b.editor->mapFromScene(QPointF(p2));
    QCOMPARE(b.selectionEnd(), b.positionAt(local2.x(), local2.y()));

    // 释放：守卫复位、滚动恢复交互（滚轮不再被禁用）、选区定格
    b.sendMouse(QEvent::MouseButtonRelease, p2, Qt::LeftButton, Qt::NoButton,
                Qt::NoModifier);
    b.settle();
    QCOMPARE(holder->property("nativeDragGuard").toBool(), false);
    QCOMPARE(flick->property("interactive").toBool(), true);
    QCOMPARE(flick->property("contentY").toDouble(), contentY0);
    QVERIFY(b.selectionEnd() > b.selectionStart());
}

// TC-002（P0）：近乎纯竖直拖拽选区（自然操作）。
// 竖直为主、每步带 2px 横向漂移（7 步累计 +14px > startDragDistance，
// 代表真实人手拖拽；全程纯竖直 ≤10px 是 Qt 上游缺陷已知限制，见
// docs/已知问题，不在本用例覆盖）：跨多行选区产生，contentY 恒定。
// 注意：全部 move 须在 800ms 延迟窗口（QQuickPressHandler 定时器）内
// 完成，故步进用 10ms 短等待（含 press 后 settle，同样构成窗口压力）；
// 若 CI 重载导致偶发假失败，直接重跑本用例甄别。
void TestEditorIntegration::tcD002_mostlyVertical_dragSelect()
{
    QString lines;
    for (int i = 0; i < 200; ++i)
        lines += QStringLiteral("line %1\n").arg(i);
    Bench b;
    QVERIFY2(b.open(lines), "testbench open failed");

    QQuickItem *flick = b.flick();
    QVERIFY2(flick != nullptr,
             "editor ScrollView contentItem is not a Flickable (or is null)");
    QQuickItem *holder = dragGuardHolder(b.editor);
    QVERIFY2(holder != nullptr, "nativeDragGuard holder (delegate) not found");
    const qreal contentY0 = flick->property("contentY").toDouble();

    const int pressPos = 3;
    const QPoint p0 = b.pointOf(pressPos);

    b.sendMouse(QEvent::MouseButtonPress, p0, Qt::LeftButton, Qt::LeftButton,
                Qt::NoModifier);
    b.settleDrag();
    QCOMPARE(flick->property("interactive").toBool(), false);

    // 7 步 (+2px, +8px)：累计 (+14, +56)，竖直为主跨多行
    const QPoint last = p0 + QPoint(14, 56);
    for (int i = 1; i <= 7; ++i) {
        b.sendMouse(QEvent::MouseMove, p0 + QPoint(i * 2, i * 8), Qt::NoButton,
                    Qt::LeftButton, Qt::NoModifier);
        b.settleDrag();
        QVERIFY2(flick->property("contentY").toDouble() == contentY0,
                 qPrintable(QStringLiteral("step %1: contentY %2 changed "
                                           "(drag stolen as scroll)")
                                .arg(i)
                                .arg(flick->property("contentY").toDouble())));
    }

    b.sendMouse(QEvent::MouseButtonRelease, last, Qt::LeftButton, Qt::NoButton,
                Qt::NoModifier);
    b.settle();

    // 跨多行选区：锚 = 按下点、终点 = 终点坐标布局命中、含换行
    QVERIFY2(b.selectionEnd() > b.selectionStart(),
             qPrintable(QStringLiteral("no selection (start=%1 end=%2)")
                            .arg(b.selectionStart()).arg(b.selectionEnd())));
    QCOMPARE(b.selectionStart(), pressPos);
    const QPointF local = b.editor->mapFromScene(QPointF(last));
    QCOMPARE(b.selectionEnd(), b.positionAt(local.x(), local.y()));
    const QString sel = b.selectedText(pressPos, b.selectionEnd());
    QVERIFY2(sel.contains(QLatin1Char('\n')),
             qPrintable(QStringLiteral("selection must span multiple lines, "
                                       "got '%1'")
                            .arg(sel)));

    // 守卫复位、视图未滚动
    QCOMPARE(holder->property("nativeDragGuard").toBool(), false);
    QCOMPARE(flick->property("interactive").toBool(), true);
    QCOMPARE(flick->property("contentY").toDouble(), contentY0);
}

// TC-003（P0）：横向先行拖拽选区（回归）。
// 先横移 24px（超 startDragDistance → press 补放、keepMouseGrab 生效，
// 修复前即正常）再竖直 5 步共 40px：行为与修复前一致——同行选区先产生，
// 竖直扩展跨行，contentY 恒定（守卫不得反向破坏既有正常路径）。
void TestEditorIntegration::tcD003_horizontalFirst_regression()
{
    QString lines;
    for (int i = 0; i < 200; ++i)
        lines += QStringLiteral("line %1\n").arg(i);
    Bench b;
    QVERIFY2(b.open(lines), "testbench open failed");

    QQuickItem *flick = b.flick();
    QVERIFY2(flick != nullptr,
             "editor ScrollView contentItem is not a Flickable (or is null)");
    QQuickItem *holder = dragGuardHolder(b.editor);
    QVERIFY2(holder != nullptr, "nativeDragGuard holder (delegate) not found");
    const qreal contentY0 = flick->property("contentY").toDouble();

    const int pressPos = 3;
    const QPoint p0 = b.pointOf(pressPos);

    b.sendMouse(QEvent::MouseButtonPress, p0, Qt::LeftButton, Qt::LeftButton,
                Qt::NoModifier);
    b.settle();
    QCOMPARE(holder->property("nativeDragGuard").toBool(), true);
    QCOMPARE(flick->property("interactive").toBool(), false);

    // 先横向 24px（同行）：选区立即产生、不含换行
    const QPoint p1 = p0 + QPoint(24, 0);
    b.sendMouse(QEvent::MouseMove, p1, Qt::NoButton, Qt::LeftButton,
                Qt::NoModifier);
    b.settle();
    QVERIFY2(b.selectionEnd() > b.selectionStart(),
             qPrintable(QStringLiteral("no selection after horizontal-first "
                                       "move (start=%1 end=%2)")
                            .arg(b.selectionStart()).arg(b.selectionEnd())));
    QCOMPARE(b.selectionStart(), pressPos);
    const int endH = b.selectionEnd();
    QVERIFY2(!b.selectedText(pressPos, endH).contains(QLatin1Char('\n')),
             "horizontal-first selection must stay on one line");
    QCOMPARE(flick->property("contentY").toDouble(), contentY0);

    // 再竖直 5 步共 40px：选区跨行扩展
    const QPoint p2 = p0 + QPoint(24, 40);
    for (int i = 1; i <= 5; ++i) {
        b.sendMouse(QEvent::MouseMove, p0 + QPoint(24, i * 8), Qt::NoButton,
                    Qt::LeftButton, Qt::NoModifier);
        b.settle();
        QVERIFY2(flick->property("contentY").toDouble() == contentY0,
                 qPrintable(QStringLiteral("vertical step %1: contentY %2 "
                                           "changed (drag stolen as scroll)")
                                .arg(i)
                                .arg(flick->property("contentY").toDouble())));
    }
    QVERIFY2(b.selectionEnd() > endH, "selection must extend vertically");
    QVERIFY2(b.selectedText(pressPos, b.selectionEnd())
                     .contains(QLatin1Char('\n')),
             "selection must span multiple lines after vertical extension");

    b.sendMouse(QEvent::MouseButtonRelease, p2, Qt::LeftButton, Qt::NoButton,
                Qt::NoModifier);
    b.settle();
    QCOMPARE(holder->property("nativeDragGuard").toBool(), false);
    QCOMPARE(flick->property("interactive").toBool(), true);
    QCOMPARE(flick->property("contentY").toDouble(), contentY0);
    QVERIFY(b.selectionEnd() > b.selectionStart());
}

// TC-004（P0，可自动化部分）：守卫复位后滚轮滚动恢复（无残留禁用）。
// 完整链路：拖拽选区（守卫激活→复位）→ 合成滚轮事件 → Flickable
// contentY 增长（wheelEvent 仅在 interactive 时处理，若守卫未复位则
// 滚轮失效——这正是"不得永久禁用 interactive"约束的回归面）。
// NoWrap 横向滚轮、真实滚轮实体手感见人工待验清单。
void TestEditorIntegration::tcD004_wheelScroll_afterGuardRelease()
{
    QString lines;
    for (int i = 0; i < 200; ++i)
        lines += QStringLiteral("line %1\n").arg(i);
    Bench b;
    QVERIFY2(b.open(lines), "testbench open failed");

    QQuickItem *flick = b.flick();
    QVERIFY2(flick != nullptr,
             "editor ScrollView contentItem is not a Flickable (or is null)");
    const qreal contentY0 = flick->property("contentY").toDouble();

    // 先走一遍拖拽选区（守卫激活→释放复位），与真实使用时序一致
    const QPoint p0 = b.pointOf(3);
    b.sendMouse(QEvent::MouseButtonPress, p0, Qt::LeftButton, Qt::LeftButton,
                Qt::NoModifier);
    b.settle();
    QCOMPARE(flick->property("interactive").toBool(), false);
    b.sendMouse(QEvent::MouseMove, p0 + QPoint(24, 32), Qt::NoButton,
                Qt::LeftButton, Qt::NoModifier);
    b.settle();
    b.sendMouse(QEvent::MouseButtonRelease, p0 + QPoint(24, 32), Qt::LeftButton,
                Qt::NoButton, Qt::NoModifier);
    b.settle();
    QCOMPARE(flick->property("interactive").toBool(), true);
    QVERIFY(b.selectionEnd() > b.selectionStart());

    // 合成滚轮（向后滚一格 120）：contentY 增长（QTRY 等待平滑滚动动画）
    const QPointF pos = QPointF(b.pointOf(3));
    QWheelEvent wheel(pos, QPointF(b.view.mapToGlobal(pos.toPoint())),
                      QPoint(0, 0), QPoint(0, -120), Qt::NoButton,
                      Qt::NoModifier, Qt::ScrollUpdate, false);
    QCoreApplication::sendEvent(&b.view, &wheel);
    QTRY_VERIFY2(flick->property("contentY").toDouble() > contentY0,
                 qPrintable(QStringLiteral("wheel scroll dead after guard "
                                           "release (contentY=%1)")
                                .arg(flick->property("contentY").toDouble())));
}

// TC-005（P0，防回归）：非左键单击不得置守卫。
// 守护缺陷（第 2 轮回归，实验实证）：onReleased 加了左键过滤而
// onPressed 未加，右/中键单击编辑器会置守卫 true，其 release 又被
// 左键过滤挡住不复位 → Flickable interactive 持续 false → 滚轮失效
// 直到下次左键点击或焦点转移，违反"非按下状态下滚轮滚动正常"。
// 步骤：右键 press → release、中键 press → release，各自断言守卫
// 未被置位（false）且 interactive 保持 true；再左键 press（守卫置位
// true、interactive false）→ release（复位），保持守卫主路径覆盖。
void TestEditorIntegration::tcD005_nonLeftClick_guardNotStuck()
{
    QString lines;
    for (int i = 0; i < 200; ++i)
        lines += QStringLiteral("line %1\n").arg(i);
    Bench b;
    QVERIFY2(b.open(lines), "testbench open failed");

    QQuickItem *flick = b.flick();
    QVERIFY2(flick != nullptr,
             "editor ScrollView contentItem is not a Flickable (or is null)");
    QQuickItem *holder = dragGuardHolder(b.editor);
    QVERIFY2(holder != nullptr, "nativeDragGuard holder (delegate) not found");

    const QPoint p = b.pointOf(3);

    // 右键单击：press/release 全程守卫不得置位
    b.sendMouse(QEvent::MouseButtonPress, p, Qt::RightButton, Qt::RightButton,
                Qt::NoModifier);
    b.settle();
    QCOMPARE(holder->property("nativeDragGuard").toBool(), false);
    b.sendMouse(QEvent::MouseButtonRelease, p, Qt::RightButton, Qt::NoButton,
                Qt::NoModifier);
    b.settle();
    QCOMPARE(holder->property("nativeDragGuard").toBool(), false);
    QCOMPARE(flick->property("interactive").toBool(), true);

    // 中键单击：同上
    b.sendMouse(QEvent::MouseButtonPress, p, Qt::MiddleButton,
                Qt::MiddleButton, Qt::NoModifier);
    b.settle();
    QCOMPARE(holder->property("nativeDragGuard").toBool(), false);
    b.sendMouse(QEvent::MouseButtonRelease, p, Qt::MiddleButton, Qt::NoButton,
                Qt::NoModifier);
    b.settle();
    QCOMPARE(holder->property("nativeDragGuard").toBool(), false);
    QCOMPARE(flick->property("interactive").toBool(), true);

    // 左键对照：按下守卫置位、释放复位（主路径不因过滤受损）
    b.sendMouse(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton,
                Qt::NoModifier);
    b.settle();
    QCOMPARE(holder->property("nativeDragGuard").toBool(), true);
    QCOMPARE(flick->property("interactive").toBool(), false);
    b.sendMouse(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton,
                Qt::NoModifier);
    b.settle();
    QCOMPARE(holder->property("nativeDragGuard").toBool(), false);
    QCOMPARE(flick->property("interactive").toBool(), true);
}

// TC-006（P0，第 4 轮根因守护）：纯竖直拖拽（全程 |dx| ≤ 10px）**立即**
// 产生选区且随步进增长。
// 根因守护：QQuickPressHandler 的 800ms 延迟按下定时器只被横向位移取消，
// 纯竖直拖拽超时后保存的 press 被销毁、control 侧 mousePressed 恒 false，
// 原生 move 永不扩展选区——本用例在 press 后**无需等待**即逐步竖直 move，
// 每步断言选区非空且较上步增长；对 EditorArea 的 PointHandler 被动驱动
// （onPressed 自持锚点 + select(锚点, positionAt(当前点))）敏感，
// 移除 PointHandler 后本用例应失败（第 4 步起每步选区断言失败）。
// 全程横向漂移每步 ≤ +1px（累计 4px ≤ startDragDistance≈10px），
// 不会触发延迟 press 补放，选区增长只能来自 PointHandler 驱动。
void TestEditorIntegration::tcD006_pureVertical_immediateSelection()
{
    QString lines;
    for (int i = 0; i < 200; ++i)
        lines += QStringLiteral("line %1\n").arg(i);
    Bench b;
    QVERIFY2(b.open(lines), "testbench open failed");

    QQuickItem *flick = b.flick();
    QVERIFY2(flick != nullptr,
             "editor ScrollView contentItem is not a Flickable (or is null)");
    QQuickItem *holder = dragGuardHolder(b.editor);
    QVERIFY2(holder != nullptr, "nativeDragGuard holder (delegate) not found");
    const qreal contentY0 = flick->property("contentY").toDouble();

    const int pressPos = 3;
    const QPoint p0 = b.pointOf(pressPos);

    // 按下后无需等待（脱离 800ms 定时器窗口约束），立即竖直 move
    b.sendMouse(QEvent::MouseButtonPress, p0, Qt::LeftButton, Qt::LeftButton,
                Qt::NoModifier);
    b.settleDrag();
    QCOMPARE(holder->property("nativeDragGuard").toBool(), true);
    QCOMPARE(flick->property("interactive").toBool(), false);
    QCOMPARE(holder->property("nativeDragAnchor").toInt(), pressPos);

    // 4 步逐行下移（文本 "line %1\n"：行 i 起始位置 = i*7）：每步 y 取目标行
    // 行中心（保证每步恰好跨一行，选区长度必然增长）、x 每步 +1（累计 4px
    // ≤ startDragDistance≈10px，不触发延迟 press 补放），选区增长只能来自
    // PointHandler 驱动。每步：选区非空且较上步增长、含换行、视图未被滚动、守卫真
    int prevLen = 0;
    QPoint cur = p0;
    for (int i = 1; i <= 4; ++i) {
        const int lineStart = i * 7;               // 行 i 首字符位置
        const qreal targetY = b.rowCenterY(lineStart);
        const qreal targetX = p0.x() + i * 1;
        cur = QPoint(qRound(targetX), qRound(targetY));
        b.sendMouse(QEvent::MouseMove, cur, Qt::NoButton, Qt::LeftButton,
                    Qt::NoModifier);
        b.settleDrag();
        const int len = b.selectionEnd() - b.selectionStart();
        QVERIFY2(len > prevLen,
                 qPrintable(QStringLiteral("step %1: selection length %2 not "
                                           "growing (prev %3, start=%4 end=%5)")
                                .arg(i).arg(len).arg(prevLen)
                                .arg(b.selectionStart()).arg(b.selectionEnd())));
        QVERIFY2(b.selectedText(b.selectionStart(), b.selectionEnd())
                         .contains(QLatin1Char('\n')),
                 qPrintable(QStringLiteral("step %1: selection must span "
                                           "multiple lines")
                                .arg(i)));
        QVERIFY2(flick->property("contentY").toDouble() == contentY0,
                 qPrintable(QStringLiteral("step %1: contentY %2 changed "
                                           "(drag stolen as scroll)")
                                .arg(i)
                                .arg(flick->property("contentY").toDouble())));
        QCOMPARE(holder->property("nativeDragGuard").toBool(), true);
        prevLen = len;
    }

    // 释放：守卫复位、锚点复位、interactive 恢复、选区保留
    b.sendMouse(QEvent::MouseButtonRelease, cur, Qt::LeftButton, Qt::NoButton,
                Qt::NoModifier);
    b.settle();
    QCOMPARE(holder->property("nativeDragGuard").toBool(), false);
    QCOMPARE(holder->property("nativeDragAnchor").toInt(), -1);
    QCOMPARE(holder->property("nativeDragLastPos").toInt(), -1);
    QCOMPARE(flick->property("interactive").toBool(), true);
    QCOMPARE(flick->property("contentY").toDouble(), contentY0);
    QVERIFY2(b.selectionEnd() > b.selectionStart(), "selection lost on release");
}

// TC-007（P1，第 4 轮新机制回归）：单击不拖拽——press→release 无位移后
// 无选区残留、光标定位正确。
// 第 4 轮 onPressed 立即定位光标并自持锚点（nativeDragAnchor），onReleased
// 以 last !== anchor 条件决定是否重放选区。本用例验证单击路径：
// - 前置一次纯竖直拖拽产生选区（PointHandler 驱动），单击后选区不得残留
//   （onPressed 的 cursorPosition 赋值为 MoveAnchor 语义，清除旧选区）；
// - 按下即定位：press 后光标已在按下点（不必等 release，桌面编辑器惯例）；
// - 无位移释放：PointHandler 未更新 last（-1）→ 重放条件不触发，
//   选区保持空、光标停在单击处；
// - 字符内微移（+2px，命中同一字符位置）释放：last == anchor → 重放条件
//   仍不触发，无选区（真实单击的自然抖动不得被误判为拖拽）。
void TestEditorIntegration::tcD007_plainClick_noSelectionResidue()
{
    QString lines;
    for (int i = 0; i < 200; ++i)
        lines += QStringLiteral("line %1\n").arg(i);
    Bench b;
    QVERIFY2(b.open(lines), "testbench open failed");

    QQuickItem *flick = b.flick();
    QVERIFY2(flick != nullptr,
              "editor ScrollView contentItem is not a Flickable (or is null)");
    QQuickItem *holder = dragGuardHolder(b.editor);
    QVERIFY2(holder != nullptr, "nativeDragGuard holder (delegate) not found");

    // ---- 前置：纯竖直拖拽 2 行产生选区（PointHandler 驱动，横向零位移） ----
    const int dragPos = 3;
    const QPoint p0 = b.pointOf(dragPos);
    b.sendMouse(QEvent::MouseButtonPress, p0, Qt::LeftButton, Qt::LeftButton,
                Qt::NoModifier);
    b.settleDrag();
    const QPoint p1(p0.x(), qRound(b.rowCenterY(2 * 7)));
    b.sendMouse(QEvent::MouseMove, p1, Qt::NoButton, Qt::LeftButton,
                Qt::NoModifier);
    b.settleDrag();
    b.sendMouse(QEvent::MouseButtonRelease, p1, Qt::LeftButton, Qt::NoButton,
                Qt::NoModifier);
    b.settle();
    QVERIFY2(b.selectionEnd() > b.selectionStart(),
             "precondition: drag selection must exist before plain click");

    // ---- 单击（press→release 无位移）第 5 行中部 ----
    const int clickPos = 5 * 7 + 2; // "li|ne 5"
    const QPoint pc = b.pointOf(clickPos);
    const QPointF localC = b.editor->mapFromScene(QPointF(pc));
    const int clickHit = b.positionAt(localC.x(), localC.y());
    QVERIFY2(clickHit >= 5 * 7 && clickHit < 5 * 7 + 6,
             qPrintable(QStringLiteral("click point must hit line 5, got %1")
                            .arg(clickHit)));

    b.sendMouse(QEvent::MouseButtonPress, pc, Qt::LeftButton, Qt::LeftButton,
                Qt::NoModifier);
    b.settleDrag();
    // 按下即定位（第 4 轮新行为）：光标已到按下点、锚点自持、守卫置位、
    // 上次拖拽的选区已被清除（cursorPosition 赋值 MoveAnchor 语义）
    QCOMPARE(holder->property("nativeDragGuard").toBool(), true);
    QCOMPARE(holder->property("nativeDragAnchor").toInt(), clickHit);
    QCOMPARE(holder->property("nativeDragLastPos").toInt(), -1);
    QCOMPARE(b.cursorPosition(), clickHit);
    QCOMPARE(b.selectionStart(), b.selectionEnd());

    b.sendMouse(QEvent::MouseButtonRelease, pc, Qt::LeftButton, Qt::NoButton,
                Qt::NoModifier);
    b.settle();
    // 无位移释放：重放条件 last !== anchor 不触发（last 恒 -1），
    // 无选区残留、光标停在单击处、守卫复位
    QCOMPARE(b.selectionStart(), b.selectionEnd());
    QCOMPARE(b.cursorPosition(), clickHit);
    QCOMPARE(holder->property("nativeDragGuard").toBool(), false);
    QCOMPARE(holder->property("nativeDragAnchor").toInt(), -1);
    QCOMPARE(holder->property("nativeDragLastPos").toInt(), -1);
    QCOMPARE(flick->property("interactive").toBool(), true);

    // ---- 字符内微移释放：last 更新但命中同位置（== anchor），不重放 ----
    const int clickPos2 = 8 * 7 + 4; // "line| 8"
    const QPoint pd = b.pointOf(clickPos2);
    const QPointF localD = b.editor->mapFromScene(QPointF(pd));
    const int clickHit2 = b.positionAt(localD.x(), localD.y());

    b.sendMouse(QEvent::MouseButtonPress, pd, Qt::LeftButton, Qt::LeftButton,
                Qt::NoModifier);
    b.settleDrag();
    QCOMPARE(holder->property("nativeDragAnchor").toInt(), clickHit2);
    // 微移 +2px（等宽 ASCII ≈8px 宽，前半字符内，命中同一位置）
    b.sendMouse(QEvent::MouseMove, pd + QPoint(2, 0), Qt::NoButton,
                Qt::LeftButton, Qt::NoModifier);
    b.settleDrag();
    QCOMPARE(holder->property("nativeDragLastPos").toInt(), clickHit2);
    QCOMPARE(b.selectionStart(), b.selectionEnd()); // select(a, a) 空选区
    b.sendMouse(QEvent::MouseButtonRelease, pd + QPoint(2, 0), Qt::LeftButton,
                Qt::NoButton, Qt::NoModifier);
    b.settle();
    // last === anchor：重放条件不触发，单击微抖动不得产生选区
    QCOMPARE(b.selectionStart(), b.selectionEnd());
    QCOMPARE(b.cursorPosition(), clickHit2);
    QCOMPARE(holder->property("nativeDragGuard").toBool(), false);
}

// TC-008（P0，第 4 轮新机制回归）：纯竖直拖拽释放后选区不被原生补放清空。
// 机制：延迟按下窗口内 release 时，QQuickPressHandler 先补放保存的 press
// 再放 release，原生处理将清空 PointHandler 驱动的选区；onReleased 以
// Qt.callLater 延迟一拍用自持锚点/终点重放。本用例在释放后分两段时间复查：
// - 释放后等待约 2 帧（qWait 40ms，覆盖 Qt.callLater 一拍 + 渲染节拍）；
// - 再等 850ms 跨过 800ms 延迟按下定时器窗口（定时器到期销毁 delayed press
//   的路径不得影响已重放的选区）。
// 选区锚/终点必须与释放前 PointHandler 最后驱动的状态精确一致。
void TestEditorIntegration::tcD008_releaseReplay_survivesNativeReplay()
{
    QString lines;
    for (int i = 0; i < 200; ++i)
        lines += QStringLiteral("line %1\n").arg(i);
    Bench b;
    QVERIFY2(b.open(lines), "testbench open failed");

    QQuickItem *flick = b.flick();
    QVERIFY2(flick != nullptr,
              "editor ScrollView contentItem is not a Flickable (or is null)");
    QQuickItem *holder = dragGuardHolder(b.editor);
    QVERIFY2(holder != nullptr, "nativeDragGuard holder (delegate) not found");
    const qreal contentY0 = flick->property("contentY").toDouble();

    // 纯竖直拖拽 2 行（横向零位移，全程不触发延迟 press 补放，
    // 选区完全由 PointHandler 驱动）
    const int pressPos = 3;
    const QPoint p0 = b.pointOf(pressPos);
    b.sendMouse(QEvent::MouseButtonPress, p0, Qt::LeftButton, Qt::LeftButton,
                Qt::NoModifier);
    b.settleDrag();
    QCOMPARE(holder->property("nativeDragAnchor").toInt(), pressPos);
    const QPoint p1(p0.x(), qRound(b.rowCenterY(2 * 7)));
    b.sendMouse(QEvent::MouseMove, p1, Qt::NoButton, Qt::LeftButton,
                Qt::NoModifier);
    b.settleDrag();
    QVERIFY2(b.selectionEnd() > b.selectionStart(),
             "PointHandler must drive selection during pure-vertical drag");
    const int selStart = b.selectionStart();
    const int selEnd = b.selectionEnd();
    QCOMPARE(selStart, pressPos);

    // 释放（仍在 800ms 窗口内：原生补放 press + release 将清选区，
    // onReleased Qt.callLater 延迟一拍重放）
    b.sendMouse(QEvent::MouseButtonRelease, p1, Qt::LeftButton, Qt::NoButton,
                Qt::NoModifier);
    QTest::qWait(40); // 等 2 帧以上：callLater 重放完成
    QVERIFY2(b.selectionEnd() > b.selectionStart(),
             qPrintable(QStringLiteral("selection lost after release (native "
                                       "replay not compensated): start=%1 end=%2")
                            .arg(b.selectionStart()).arg(b.selectionEnd())));
    QCOMPARE(b.selectionStart(), selStart);
    QCOMPARE(b.selectionEnd(), selEnd);

    // 跨 800ms 延迟按下定时器窗口复查：定时器到期销毁 delayed press 的
    // 路径不得回溯影响已重放的选区
    QTest::qWait(850);
    QVERIFY2(b.selectionEnd() > b.selectionStart(),
             qPrintable(QStringLiteral("selection lost after 800ms timer "
                                       "expiry: start=%1 end=%2")
                            .arg(b.selectionStart()).arg(b.selectionEnd())));
    QCOMPARE(b.selectionStart(), selStart);
    QCOMPARE(b.selectionEnd(), selEnd);

    // 守卫复位、interactive 恢复、视图未被滚动
    QCOMPARE(holder->property("nativeDragGuard").toBool(), false);
    QCOMPARE(holder->property("nativeDragAnchor").toInt(), -1);
    QCOMPARE(holder->property("nativeDragLastPos").toInt(), -1);
    QCOMPARE(flick->property("interactive").toBool(), true);
    QCOMPARE(flick->property("contentY").toDouble(), contentY0);
}

// TC-009（P1，第 4 轮新机制回归，可自动化部分）：双击路径——原生选词仍
// 工作、双击会话结束后守卫/锚点无残留。
// Windows 真实序列：Press → Release →（双击间隔内同位置）DblClick →
// Release（第二击由系统直接以 DblClick 送达，不经 QQuickPressHandler 的
// mousePressEvent，onPressed 不触发）。offscreen 合成的环境门槛（真实平台
// 由窗口系统保证）须在测试内补偿：
// 1. 手工 DblClick 事件 isBeginEvent()==false，delivery 层不做 press 分发
//    （诊断实证：经 delivery 送达 item 亦不触发选词），故直接 sendEvent
//    给 editor，驱动 QQuickTextArea::mouseDoubleClickEvent 完整内部链路
//   （补放 delayed press → QQuickTextControl 原生选词）；
// 2. QQuickTextControl 选词分支要求事件设备类型 Mouse，全序列统一
//    synthMouse()。
// 第二击含物理 Press（Windows 真实序列无）为平台序列差异而非产品缺陷，
// 守卫短暂置位属预期。断言：
// - DblClick 原生选词工作：选区 == 双击处整词区间（第 4 轮 onPressed
//   立即定位 + PointHandler 驱动不破坏双击选词，定位被选词覆盖）；
// - 第二击 release 后：守卫复位、锚点/终点复位（无残留，滚轮不受影响）；
// - 选区在 release 后保留。
// 双击后拖拽按词扩展依赖平台双击间隔节奏，人工待验。
void TestEditorIntegration::tcD009_doubleClick_nativeWordSelect()
{
    Bench b;
    QVERIFY2(b.open(QStringLiteral("alpha beta gamma")), "testbench open failed");

    QQuickItem *holder = dragGuardHolder(b.editor);
    QVERIFY2(holder != nullptr, "nativeDragGuard holder (delegate) not found");

    // 双击 'beta' 内部（位置 7 = 'e'）：词区间 [6,10)。
    // offscreen 双击合成的两道环境门槛（真实平台分别由窗口系统双击合成与
    // 物理鼠标设备保证）：
    // 1. 手工构造的 DblClick 事件 isBeginEvent()==false，delivery 层不做
    //    press 分发，只能作为 update 事件送给独占抓取者——故第二击先发
    //    press（TextArea 接受并成为 grabber）再发 DblClick（经 grabber
    //    通道送达）；
    // 2. QQuickTextControl 双击选词分支的 isEventFromMouseOrTouchpad 门槛
    //    接受 Mouse 或 TouchPad，offscreen 默认指点设备两者皆非——全序列
    //    统一用 synthMouse()（真实平台一次双击的所有事件来自同一鼠标）。
    // 序列：Press→Release（第一击）→Press（第二击，经 onPressed 置守卫/
    // 定位锚点）→DblClick（QQuickTextArea::mouseDoubleClickEvent 先补放
    // delayed press 再 QQuickTextControl 原生选词）→Release（守卫复位）。
    // 第二击含物理 Press 为 offscreen 合成序列与 Windows 真实序列（系统
    // 直接发 DblClick、无第二击 Press）的平台差异，如实记录。
    const QPointingDevice *mouse = synthMouse();
    const QPoint p = b.pointOf(7);

    // 第一击
    b.sendMouse(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton,
                Qt::NoModifier, mouse);
    b.settleDrag();
    b.sendMouse(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton,
                Qt::NoModifier, mouse);
    b.settle();
    QCOMPARE(holder->property("nativeDragGuard").toBool(), false);
    QCOMPARE(holder->property("nativeDragAnchor").toInt(), -1);

    // 第二击 press：守卫置位（物理第二击按下，onPressed 语义）；锚点自持
    b.sendMouse(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton,
                Qt::NoModifier, mouse);
    b.settleDrag();
    QCOMPARE(holder->property("nativeDragGuard").toBool(), true);
    QCOMPARE(holder->property("nativeDragAnchor").toInt(), 7);

    // DblClick：直接 sendEvent 给 editor（TextArea 完整内部链路：
    // QQuickTextArea::mouseDoubleClickEvent → 补放 delayed press（control
    // mousePressed 置位）→ QQuickTextControl::mouseDoubleClickEvent 原生
    // 选词）。不走 window delivery：offscreen 合成事件的设备 point 注册
    // 状态与平台注入不同，delivery 层的 update 通道对本事件不可达（诊断
    // 实证：同坐标同设备事件经 delivery 送达 item 亦不选词、直接送 item
    // 则选词成功；真实 Windows DblClick 由系统注入带正确状态，不受此
    // 环境限制影响——见测试报告）。断言：onPressed 的光标定位被选词
    // 覆盖（选区 == 整词 [6,10)），第 4 轮机制不破坏双击选词
    {
        const QPointF local = b.editor->mapFromScene(QPointF(p));
        QMouseEvent dbl(QEvent::MouseButtonDblClick, local,
                        QPointF(b.view.mapToGlobal(p)), Qt::LeftButton,
                        Qt::LeftButton, Qt::NoModifier, mouse);
        QCoreApplication::sendEvent(b.editor, &dbl);
        b.settle();
    }
    QVERIFY2(b.selectionStart() == 6 && b.selectionEnd() == 10,
             qPrintable(QStringLiteral("native word select not working on "
                                       "dblclick (start=%1 end=%2, expected 6..10)")
                            .arg(b.selectionStart()).arg(b.selectionEnd())));

    // 第二击 release：选区保留（release 不清选区）、守卫复位（无残留，
    // 滚轮不受双击影响）、锚点/终点复位
    b.sendMouse(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton,
                Qt::NoModifier, mouse);
    b.settle();
    QCOMPARE(b.selectionStart(), 6);
    QCOMPARE(b.selectionEnd(), 10);
    QCOMPARE(holder->property("nativeDragGuard").toBool(), false);
    QCOMPARE(holder->property("nativeDragAnchor").toInt(), -1);
    QCOMPARE(holder->property("nativeDragLastPos").toInt(), -1);
}

// TC-010（P0，第 4 轮新机制回归）：释放后立即二次拖拽——锚点正确更新、
// 无上次 lastPos 残留。
// 第 4 轮自持锚点（nativeDragAnchor）/终点（nativeDragLastPos）均为
// delegate 级状态，若释放复位不彻底（或第二次 press 未即时换新），
// 第二次拖拽的选区会以旧锚点/旧终点为基准。本用例背靠背执行两次
// press-drag-release（中间仅 settle），断言：
// - 第二次 press 后锚点即时 == 新按下点命中、lastPos 已复位 -1；
// - 第二次拖拽中/释放后选区锚 == 第二次按下点（selectionStart 为之），
//   与第一次选区终点无关；
// - 两次均纯竖直（PointHandler 驱动路径，对状态残留最敏感）。
void TestEditorIntegration::tcD010_secondDrag_anchorRenewal()
{
    QString lines;
    for (int i = 0; i < 200; ++i)
        lines += QStringLiteral("line %1\n").arg(i);
    Bench b;
    QVERIFY2(b.open(lines), "testbench open failed");

    QQuickItem *holder = dragGuardHolder(b.editor);
    QVERIFY2(holder != nullptr, "nativeDragGuard holder (delegate) not found");

    // ---- 第一次：第 0 行位置 3 纯竖直下移 2 行，释放 ----
    const QPoint a0 = b.pointOf(3);
    b.sendMouse(QEvent::MouseButtonPress, a0, Qt::LeftButton, Qt::LeftButton,
                Qt::NoModifier);
    b.settleDrag();
    const QPoint a1(a0.x(), qRound(b.rowCenterY(2 * 7)));
    b.sendMouse(QEvent::MouseMove, a1, Qt::NoButton, Qt::LeftButton,
                Qt::NoModifier);
    b.settleDrag();
    b.sendMouse(QEvent::MouseButtonRelease, a1, Qt::LeftButton, Qt::NoButton,
                Qt::NoModifier);
    b.settle();
    QVERIFY2(b.selectionEnd() > b.selectionStart(),
             "first drag: selection must exist");
    const int firstEnd = b.selectionEnd();
    QVERIFY2(firstEnd > 2 * 7, "first drag must span to line 2+");

    // ---- 第二次：立即从第 6 行位置 2 纯竖直下移 2 行 ----
    const int secondPressPos = 6 * 7 + 2;
    const QPoint c0 = b.pointOf(secondPressPos);
    b.sendMouse(QEvent::MouseButtonPress, c0, Qt::LeftButton, Qt::LeftButton,
                Qt::NoModifier);
    b.settleDrag();
    // 锚点即时更新为新按下点、上次 lastPos 无残留（release 已复位）
    QCOMPARE(holder->property("nativeDragAnchor").toInt(), secondPressPos);
    QCOMPARE(holder->property("nativeDragLastPos").toInt(), -1);
    QCOMPARE(holder->property("nativeDragGuard").toBool(), true);

    const QPoint c1(c0.x(), qRound(b.rowCenterY(8 * 7)));
    b.sendMouse(QEvent::MouseMove, c1, Qt::NoButton, Qt::LeftButton,
                Qt::NoModifier);
    b.settleDrag();
    // 选区锚 = 第二次按下点（正向拖拽 selectionStart == 锚），
    // 终点跨 2 行（远超第一次终点 firstEnd 的行范围基准）
    QVERIFY2(b.selectionEnd() > b.selectionStart(),
             "second drag: selection must exist");
    QCOMPARE(b.selectionStart(), secondPressPos);
    QVERIFY2(b.selectionEnd() > 8 * 7,
             qPrintable(QStringLiteral("second drag must reach line 8, end=%1")
                            .arg(b.selectionEnd())));

    b.sendMouse(QEvent::MouseButtonRelease, c1, Qt::LeftButton, Qt::NoButton,
                Qt::NoModifier);
    b.settle();
    // 释放重放后锚仍是第二次按下点（无旧锚/旧 lastPos 混入）
    QCOMPARE(b.selectionStart(), secondPressPos);
    QVERIFY2(b.selectionEnd() > 8 * 7,
             "second drag: selection must survive release replay");
    QCOMPARE(holder->property("nativeDragGuard").toBool(), false);
    QCOMPARE(holder->property("nativeDragAnchor").toInt(), -1);
    QCOMPARE(holder->property("nativeDragLastPos").toInt(), -1);
}

// TC-011（P0，第 5 轮回归）：Shift+单击扩展选区。
// 守护回归：第 5 轮应改项——第 4 轮 onPressed 无条件
// editor.cursorPosition = pos（MoveAnchor 语义）把锚改写为按下点，
// Shift 扩展长度恒 0 且旧选区被清除。修复后 Shift 分支保持 KeepAnchor：
// - 先普通单击定位光标到 A（onPressed 定位路径）；
// - Shift+click 到 B → 选区 [A,B]（锚 = A 即首次光标）；
// - 再 Shift+click 到更远 C → 选区 [A,C]——原生基线（QQuickTextControl
//   mousePressEvent 的 KeepAnchor 分支：锚保持既有 text cursor 的锚、
//   仅位置移到点击点）为锚不跟随更新，连续 Shift+click 始终自首次
//   光标锚扩展；QML 侧经 cursorPosition 对端推导复现该语义。
//   （锚跟随更新则为 [B,C]，与原生不符，判失败）
void TestEditorIntegration::tcD011_shiftClick_extendSelection()
{
    QString lines;
    for (int i = 0; i < 200; ++i)
        lines += QStringLiteral("line %1\n").arg(i);
    Bench b;
    QVERIFY2(b.open(lines), "testbench open failed");

    QQuickItem *holder = dragGuardHolder(b.editor);
    QVERIFY2(holder != nullptr, "nativeDragGuard holder (delegate) not found");

    // 普通单击定位 A（第 0 行位置 3）：无修饰，走 onPressed 定位分支
    b.plainClick(3);
    QCOMPARE(b.cursorPosition(), 3);

    // Shift+click 到 B（第 2 行位置 16）：press 即 KeepAnchor 扩展
    const int aPos = 3;
    const int bPos = 2 * 7 + 2;
    const QPoint pb = b.pointOf(bPos);
    b.sendMouse(QEvent::MouseButtonPress, pb, Qt::LeftButton, Qt::LeftButton,
                Qt::ShiftModifier);
    b.settleDrag();
    // 锚 = 首次光标 A，不得被改写为按下点 B
    QCOMPARE(holder->property("nativeDragAnchor").toInt(), aPos);
    QVERIFY2(b.selectionStart() == aPos && b.selectionEnd() == bPos,
             qPrintable(QStringLiteral("shift+click: expected [%1,%2], got "
                                       "[%3,%4]")
                            .arg(aPos).arg(bPos)
                            .arg(b.selectionStart()).arg(b.selectionEnd())));
    b.sendMouse(QEvent::MouseButtonRelease, pb, Qt::LeftButton, Qt::NoButton,
                Qt::ShiftModifier);
    b.settle();
    // 释放后选区保留（原生补放 press 同为 Shift KeepAnchor，不回退）
    QVERIFY2(b.selectionStart() == aPos && b.selectionEnd() == bPos,
             qPrintable(QStringLiteral("shift+click: selection lost on "
                                       "release: [%1,%2]")
                            .arg(b.selectionStart()).arg(b.selectionEnd())));
    QCOMPARE(holder->property("nativeDragGuard").toBool(), false);
    QCOMPARE(holder->property("nativeDragAnchor").toInt(), -1);

    // 再 Shift+click 到更远 C（第 4 行位置 30）：锚仍为 A（原生基线锚不
    // 跟随更新），选区 [A,C]
    const int cPos = 4 * 7 + 2;
    const QPoint pc = b.pointOf(cPos);
    b.sendMouse(QEvent::MouseButtonPress, pc, Qt::LeftButton, Qt::LeftButton,
                Qt::ShiftModifier);
    b.settleDrag();
    QCOMPARE(holder->property("nativeDragAnchor").toInt(), aPos);
    QVERIFY2(b.selectionStart() == aPos && b.selectionEnd() == cPos,
             qPrintable(QStringLiteral("second shift+click: expected [%1,%2] "
                                       "(anchor must NOT follow), got [%3,%4]")
                            .arg(aPos).arg(cPos)
                            .arg(b.selectionStart()).arg(b.selectionEnd())));
    b.sendMouse(QEvent::MouseButtonRelease, pc, Qt::LeftButton, Qt::NoButton,
                Qt::ShiftModifier);
    b.settle();
    QVERIFY2(b.selectionStart() == aPos && b.selectionEnd() == cPos,
             qPrintable(QStringLiteral("second shift+click: selection lost on "
                                       "release: [%1,%2]")
                            .arg(b.selectionStart()).arg(b.selectionEnd())));
    QCOMPARE(holder->property("nativeDragGuard").toBool(), false);
}

// TC-012（P0，第 5 轮回归）：Shift+拖拽扩展选区。
// 守护回归：第 5 轮应改项——onPressed 无条件定位破坏 Shift 扩展（锚被改写
// 为 Shift 按下点）。修复后：单击定位 A，Shift+press 于 B 并拖到 C：
// - Shift+press 时锚 = A（当前光标），选区立即 [A,B]；
// - 拖拽（纯竖直，PointHandler 驱动路径）期间选区持续更新，
//   起点恒为 A，终点随拖拽到达 C 所在行；
// - 释放后选区保留，锚/守卫复位。
void TestEditorIntegration::tcD012_shiftDrag_extendSelection()
{
    QString lines;
    for (int i = 0; i < 200; ++i)
        lines += QStringLiteral("line %1\n").arg(i);
    Bench b;
    QVERIFY2(b.open(lines), "testbench open failed");

    QQuickItem *holder = dragGuardHolder(b.editor);
    QVERIFY2(holder != nullptr, "nativeDragGuard holder (delegate) not found");

    // 单击定位 A（第 0 行位置 3）
    const int aPos = 3;
    b.plainClick(aPos);
    QCOMPARE(b.cursorPosition(), aPos);

    // Shift+press 于 B（第 2 行位置 16）：锚 = A，选区立即 [A,B]
    const int bPos = 2 * 7 + 2;
    const QPoint pb = b.pointOf(bPos);
    b.sendMouse(QEvent::MouseButtonPress, pb, Qt::LeftButton, Qt::LeftButton,
                Qt::ShiftModifier);
    b.settleDrag();
    QCOMPARE(holder->property("nativeDragGuard").toBool(), true);
    QCOMPARE(holder->property("nativeDragAnchor").toInt(), aPos);
    QVERIFY2(b.selectionStart() == aPos && b.selectionEnd() == bPos,
             qPrintable(QStringLiteral("shift+press: expected [%1,%2], got "
                                       "[%3,%4]")
                            .arg(aPos).arg(bPos)
                            .arg(b.selectionStart()).arg(b.selectionEnd())));

    // 纯竖直拖到 C（第 4 行同列）：守卫期间选区持续由 PointHandler 驱动，
    // 起点恒为 A（不得被改写为 B）
    const int cPos = 4 * 7 + 2;
    const QPoint pc = b.pointOf(cPos);
    b.sendMouse(QEvent::MouseMove, pc, Qt::NoButton, Qt::LeftButton,
                Qt::ShiftModifier);
    b.settleDrag();
    QCOMPARE(b.selectionStart(), aPos);
    QVERIFY2(b.selectionEnd() >= 4 * 7,
             qPrintable(QStringLiteral("shift+drag: selection end %1 must "
                                       "reach line 4 (>= 28)")
                            .arg(b.selectionEnd())));
    QCOMPARE(holder->property("nativeDragGuard").toBool(), true);

    // 释放：选区保留（重放锚 = A）、守卫复位
    b.sendMouse(QEvent::MouseButtonRelease, pc, Qt::LeftButton, Qt::NoButton,
                Qt::ShiftModifier);
    b.settle();
    QCOMPARE(b.selectionStart(), aPos);
    QVERIFY2(b.selectionEnd() >= 4 * 7,
             qPrintable(QStringLiteral("shift+drag: selection lost on "
                                       "release, end=%1")
                            .arg(b.selectionEnd())));
    QCOMPARE(holder->property("nativeDragGuard").toBool(), false);
    QCOMPARE(holder->property("nativeDragAnchor").toInt(), -1);
    QCOMPARE(holder->property("nativeDragLastPos").toInt(), -1);
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
