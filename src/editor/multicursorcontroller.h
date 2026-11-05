#pragma once

#include <QtQmlIntegration/qqmlintegration.h>
#include <QObject>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QRectF>
#include <QString>
#include <QVariantList>

#include "editor/multicursorengine.h"

// moc 生成的元对象要求指针属性指向完整类型，QQuickItem/QQuickTextDocument
// 须完整包含；QTextDocument/QEvent 仅在实现中使用，前置声明即可
class QTextDocument;
class QEvent;

namespace qcnote {

/**
 * @brief 多光标 QML 控制器：在 MultiCursorEngine 纯逻辑之上对接 QML TextArea。
 *
 * 职责边界：
 * - 引擎（multicursorengine）只做纯文本坐标计算；本控制器负责把引擎结果
 *   应用到 QTextDocument（QQuickTextDocument），并把光标/选区状态以属性
 *   形式暴露给 QML（覆盖层渲染、状态栏提示）。
 * - active=false 时不拦截任何按键/输入法事件，引擎内保留单光标状态与
 *   原生光标同步（QML 经 syncNativeCursor 维护）。
 *
 * QML 侧典型接入（T3）：
 * - target 绑定对应 TextArea（用于捕捉输入法提交）；
 * - textDocument 绑定 editor.textDocument；
 * - TextArea 的 Keys.onPressed 调 handleKey(...)，返回 true 时置
 *   event.accepted = true；
 * - active 为 false 时在 cursorPosition/selection 变化时调
 *   syncNativeCursor(...)；
 * - Alt+单击 / Alt+Shift+拖拽分别调 altClick / columnDragBeginAt-
 *   UpdateAt-End（拖拽传原始坐标，控制器经布局逐行命中）；
 * - 文档内容被外部重置或关闭文档时调 reset()。
 *
 * 所有属性共用同一个 stateChanged 通知信号：任何命令执行后统一发射，
 * QML 绑定按需重新拉取最新值。
 */
class MultiCursorController : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QQuickItem *target READ target WRITE setTarget NOTIFY stateChanged)
    Q_PROPERTY(QQuickTextDocument *textDocument READ textDocument WRITE setTextDocument NOTIFY stateChanged)
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)
    Q_PROPERTY(bool columnSelecting READ columnSelecting NOTIFY stateChanged)
    Q_PROPERTY(int cursorCount READ cursorCount NOTIFY stateChanged)
    Q_PROPERTY(int primaryPosition READ primaryPosition NOTIFY stateChanged)
    Q_PROPERTY(QVariantList cursorPositions READ cursorPositions NOTIFY stateChanged)
    Q_PROPERTY(QVariantList selectionRanges READ selectionRanges NOTIFY stateChanged)

public:
    explicit MultiCursorController(QObject *parent = nullptr);
    ~MultiCursorController() override;

    QQuickItem *target() const;
    void setTarget(QQuickItem *target);

    QQuickTextDocument *textDocument() const;
    void setTextDocument(QQuickTextDocument *textDocument);

    /// 引擎是否已接管编辑（多光标或存在多光标选区）
    bool active() const;
    /// 是否处于列选拖拽中（透传引擎状态）
    bool columnSelecting() const;
    int cursorCount() const;
    /// 排序最末光标的 position（QML 用于同步 TextArea.cursorPosition）
    int primaryPosition() const;
    /// 全部光标 position 升序
    QVariantList cursorPositions() const;
    /// 每个非空选区一对 [start,end]（内嵌 QVariantList），按 start 升序
    QVariantList selectionRanges() const;

    /// active==false 时由 QML 在原生 cursorPosition/selection 变化时调用
    /// （有选区 anchor=selectionStart，否则 anchor=position），维护引擎基础状态
    Q_INVOKABLE void syncNativeCursor(int position, int anchor);

    /// 集中按键分发；返回 true 表示已消费（QML 将 event.accepted = true）
    Q_INVOKABLE bool handleKey(int key, int modifiers, const QString &text);

    /// Alt+单击加光标
    Q_INVOKABLE void altClick(int position);

    /// Alt+Shift+鼠标拖拽列选（视觉列选：x/y 为 TextArea 子项坐标系原始
    /// 坐标，由控制器经真实布局逐行命中，中文/全角宽度与短行钳制均由
    /// 布局保证；旧位置式接口已移除，位置语义无法表达视觉对齐）
    Q_INVOKABLE void columnDragBeginAt(qreal x, qreal y);
    Q_INVOKABLE void columnDragUpdateAt(qreal x, qreal y);
    Q_INVOKABLE void columnDragEnd();

    /// 多光标剪贴板操作（active 时有效；copy 无任何选区时不写剪贴板）
    Q_INVOKABLE void copy();
    Q_INVOKABLE void cut();
    Q_INVOKABLE void paste();

    /// 退单光标并清引擎状态（文档内容外部重置/关闭文档时 QML 调用）
    Q_INVOKABLE void reset();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    /// 全部属性共用的变更通知
    void stateChanged();

private:
    /// 应用编辑批次：单 editBlock 从后往前应用（一次按键 = 单次撤销），
    /// 然后重设引擎光标并发 stateChanged
    void applyBatch(const McEditBatch &batch);

    /// 外部文本变化（不经 applyBatch，如 text 绑定整体替换、原生撤销）后，
    /// 把引擎光标/选区位置静默钳制到当前文档合法范围（防越界告警，见实现）
    void clampCursorsToDocument();

    /// Ctrl+Alt+Shift+方向键：键盘列选（首次进入列选模式，之后逐格扩展）
    void keyboardColumnSelect(int key, const QString &text);

    /// 布局查询：经 QMetaObject 按名称调用 target（Controls TextArea）的
    /// positionAt/positionToRectangle（均为 Qt 文档化 QML 方法）。不直接
    /// include QQuickTextEdit 头（该公共头未随安装布局转发，编译期不可依赖）
    int layoutPositionAt(qreal x, qreal y) const;   // 失败返回 -1 并 qWarning
    QRectF layoutCursorRect(int position) const;    // 失败返回空 QRectF() 并 qWarning

    /// 视觉列选的逐行命中：对 [yTop, yBottom] 覆盖的每个可视行，以行中心
    /// y 对 x1/x2 各做一次 positionAt 命中，归一为正向区间对 [min,max)。
    /// 短行命中天然钳制行末（不越界到下一行）；查询失败返回空表（DEC-002）
    QList<QPair<int, int>> visualColumnPairs(qreal x1, qreal x2,
                                             qreal yTop, qreal yBottom) const;

    /// active = 多光标或存在选区（任一引擎命令实际改变光标/选区状态即接管）
    void syncActiveFromEngine();

    bool hasSelection() const;

    /// Esc/未支持命令：clearToLast + active=false + stateChanged
    void deactivateToLast();

    /// 结束键盘列选会话（引擎列选状态定格为普通多选区）
    void endKbColumnSession();

    MultiCursorEngine m_engine;
    QQuickItem *m_target = nullptr;
    QQuickTextDocument *m_textDocument = nullptr;
    QTextDocument *m_doc = nullptr;
    bool m_active = false;

    /// applyBatch 编辑文档期间为真：此时引擎已持有批次后新光标，
    /// contentsChange 钳制跳过（避免干扰批次应用）
    bool m_applyingBatch = false;

    // 视觉列选会话状态（鼠标与键盘共用同一套逐行命中，量均为 TextArea
    // 子项坐标系视觉量）：矩形两边由“锚点视觉 x + 活动点期望视觉 x”决定，
    // 期望 x 粘性——垂直移动不改，经过短行再移出后自动恢复
    bool m_kbColumnActive = false;  // 键盘列选会话进行中
    qreal m_colDragAnchorX = 0;     // 鼠标列选锚点视觉 x（按下点）
    qreal m_colDragAnchorY = 0;     // 鼠标列选锚点行顶 y（命中位光标矩形归一）
    qreal m_kbAnchorX = 0;          // 键盘列选锚点视觉 x
    qreal m_kbAnchorY = 0;          // 键盘列选锚点行顶 y
    qreal m_kbCursorX = 0;          // 键盘活动点期望视觉 x（粘性）
    qreal m_kbCursorY = 0;          // 键盘活动点行顶 y
};

} // namespace qcnote
