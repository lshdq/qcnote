#pragma once

#include <QtQmlIntegration/qqmlintegration.h>
#include <QObject>
#include <QQuickItem>
#include <QQuickTextDocument>
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
 * - Alt+单击 / Alt+Shift+拖拽分别调 altClick / columnDragBegin-Update-End；
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

    /// Alt+Shift+鼠标拖拽列选（position 均为文档坐标）
    Q_INVOKABLE void columnDragBegin(int anchorPos, int cursorPos);
    Q_INVOKABLE void columnDragUpdate(int cursorPos);
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

    // 键盘列选会话：控制器侧记录固定锚点与当前游标角点
    // （引擎只保存锚点，扩展方向由这两点推算）
    bool m_kbColumnActive = false;
    int m_kbColumnAnchor = 0;
    int m_kbColumnCursor = 0;
};

} // namespace qcnote
