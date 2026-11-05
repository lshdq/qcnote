#include "editor/multicursorcontroller.h"

#include <QClipboard>
#include <QEvent>
#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QMetaObject>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QTextCursor>
#include <QTextDocument>

#include <algorithm>
#include <utility>

namespace qcnote {

// ==================== 布局查询（经 QMetaObject 按名称调用，DEC-001） ====================

// target 恒为真实 TextArea（QML 绑定保证），positionAt/positionToRectangle
// 为其 Qt 文档化 Q_INVOKABLE。QQuickTextEdit 公共头未随安装布局转发，
// 按名称调用避免编译期依赖；失败仅属防御分支（DEC-002：保守无操作 + qWarning）
int MultiCursorController::layoutPositionAt(qreal x, qreal y) const
{
    if (!m_target) {
        qWarning("MultiCursorController: layout query without target");
        return -1;
    }
    int pos = -1;
    if (!QMetaObject::invokeMethod(m_target, "positionAt",
                                   Q_RETURN_ARG(int, pos),
                                   Q_ARG(qreal, x), Q_ARG(qreal, y))
            || pos < 0) {
        qWarning("MultiCursorController: positionAt(%.1f, %.1f) failed",
                 double(x), double(y));
        return -1;
    }
    return pos;
}

QRectF MultiCursorController::layoutCursorRect(int position) const
{
    if (!m_target) {
        qWarning("MultiCursorController: layout query without target");
        return QRectF();
    }
    QRectF rect;
    if (!QMetaObject::invokeMethod(m_target, "positionToRectangle",
                                   Q_RETURN_ARG(QRectF, rect),
                                   Q_ARG(int, position))) {
        qWarning("MultiCursorController: positionToRectangle(%d) failed", position);
        return QRectF();
    }
    return rect;
}

// ==================== 视觉列选的逐行命中（鼠标/键盘共用） ====================

QList<QPair<int, int>> MultiCursorController::visualColumnPairs(
        qreal x1, qreal x2, qreal yTop, qreal yBottom) const
{
    // 行高取主光标位光标矩形的高度：等宽字体单一行高（自动换行的同一
    // 文档内所有可视行同高），任意合法位置取值等价；<=0 视为布局不可用
    const qreal lineH = layoutCursorRect(primaryPosition()).height();
    if (lineH <= 0)
        return {};
    QList<QPair<int, int>> pairs;
    // 从 yTop 按行高步进；上限附加 1/4 行高浮点余量：yTop==yBottom 时恰好
    // 命中一行，且行高的小数舍入误差不会提前截断末行
    for (qreal y = yTop; y <= yBottom + lineH * 0.25; y += lineH) {
        // 行中心 y 命中：对 y 向取整/半像素偏差稳健（顶/底缘易命中邻行）
        const qreal yc = y + lineH / 2;
        const int s = layoutPositionAt(x1, yc);
        const int e = layoutPositionAt(x2, yc);
        if (s < 0 || e < 0)
            return {}; // 任一查询失败：保守放弃本次更新，不产生错误选区（DEC-002）
        pairs.append(qMakePair(qMin(s, e), qMax(s, e))); // 归一正向（反向拖拽等价）
    }
    return pairs;
}

// ==================== 构造 / 目标与文档 ====================

MultiCursorController::MultiCursorController(QObject *parent)
    : QObject(parent)
{
}

MultiCursorController::~MultiCursorController() = default;

QQuickItem *MultiCursorController::target() const
{
    return m_target;
}

void MultiCursorController::setTarget(QQuickItem *target)
{
    if (m_target == target)
        return;
    if (m_target)
        m_target->removeEventFilter(this);
    m_target = target;
    if (m_target)
        m_target->installEventFilter(this); // 捕捉输入法提交
    emit stateChanged();
}

QQuickTextDocument *MultiCursorController::textDocument() const
{
    return m_textDocument;
}

void MultiCursorController::setTextDocument(QQuickTextDocument *textDocument)
{
    if (m_textDocument == textDocument)
        return;
    if (m_doc)
        disconnect(m_doc, &QTextDocument::contentsChange,
                   this, &MultiCursorController::clampCursorsToDocument);
    m_textDocument = textDocument;
    m_doc = textDocument ? textDocument->textDocument() : nullptr;
    if (m_doc) {
        // 监听"非本控制器发起"的文档内容变化（如 TextArea 的 text 绑定整体
        // 替换、原生撤销/重做等）：一旦文档缩短，把引擎光标/选区位置静默
        // 钳制到新文档合法范围，避免覆盖层经 positionToRectangle 重算时以
        // 越界位置触发 QTextCursor::setPosition 告警（详见 clampCursorsToDocument）。
        connect(m_doc, &QTextDocument::contentsChange,
                this, &MultiCursorController::clampCursorsToDocument);
    }
    reset(); // 文档对象变化：引擎状态整体重置
    emit stateChanged();
}

// ==================== 属性读取 ====================

bool MultiCursorController::active() const
{
    return m_active;
}

bool MultiCursorController::columnSelecting() const
{
    return m_engine.isColumnSelecting();
}

int MultiCursorController::cursorCount() const
{
    return m_engine.cursorCount();
}

int MultiCursorController::primaryPosition() const
{
    const QList<McCursor> cursors = m_engine.cursors();
    return cursors.isEmpty() ? 0 : cursors.constLast().position;
}

QVariantList MultiCursorController::cursorPositions() const
{
    const QList<McCursor> cursors = m_engine.cursors(); // 已按 position 升序
    QVariantList list;
    list.reserve(cursors.size());
    for (const McCursor &c : cursors)
        list.append(c.position);
    return list;
}

QVariantList MultiCursorController::selectionRanges() const
{
    QList<QPair<int, int>> ranges;
    for (const McCursor &c : m_engine.cursors()) {
        if (c.anchor != c.position)
            ranges.append(qMakePair(qMin(c.anchor, c.position), qMax(c.anchor, c.position)));
    }
    std::sort(ranges.begin(), ranges.end()); // 按 start 升序
    QVariantList list;
    list.reserve(ranges.size());
    for (const QPair<int, int> &r : ranges)
        list.append(QVariantList{r.first, r.second});
    return list;
}

// ==================== 状态同步 / 会话管理 ====================

void MultiCursorController::syncNativeCursor(int position, int anchor)
{
    if (m_active)
        return; // active 时以引擎为准，忽略原生光标变化
    endKbColumnSession();
    if (m_engine.isColumnSelecting())
        m_engine.finishColumnSelect(); // 原生光标移动即终止残留的列选状态
    const int len = m_doc ? m_doc->toPlainText().length() : 0;
    m_engine.setCursors({McCursor{qBound(0, anchor, len), qBound(0, position, len)}});
    emit stateChanged();
}

bool MultiCursorController::hasSelection() const
{
    for (const McCursor &c : m_engine.cursors()) {
        if (c.anchor != c.position)
            return true;
    }
    return false;
}

void MultiCursorController::syncActiveFromEngine()
{
    // 任一命令实际改变了光标/选区状态（多光标或存在选区）即接管
    m_active = m_engine.cursorCount() > 1 || hasSelection();
}

void MultiCursorController::deactivateToLast()
{
    m_kbColumnActive = false;
    if (m_engine.cursorCount() > 0)
        m_engine.clearToLast(); // 内部同时退出列选状态
    else
        m_engine.setCursors({McCursor{0, 0}});
    m_active = false;
    emit stateChanged();
}

void MultiCursorController::endKbColumnSession()
{
    if (!m_kbColumnActive)
        return;
    m_kbColumnActive = false;
    if (m_engine.isColumnSelecting())
        m_engine.finishColumnSelect(); // 定格为普通多选区（之后可正常编辑）
}

void MultiCursorController::reset()
{
    m_kbColumnActive = false;
    if (m_engine.isColumnSelecting())
        m_engine.finishColumnSelect(); // 状态机所有入口自洽；pre-existing 残留，一轮审查发现 4
    // 重置为文档起点单光标（而非保留旧的末光标位置）：reset 用于文档内容
    // 被外部整体替换 / 关闭文档等场景，旧位置对随后的新文本可能越界；
    // 归 0 对任意后续文本恒合法，覆盖层经 positionToRectangle 重算时不会
    // 触发 QTextCursor::setPosition 越界告警。active 变 false 后 QML 会以
    // 原生光标（syncNativeCursor）随即覆盖引擎状态，最终光标位置不受影响。
    m_engine.setCursors({McCursor{0, 0}});
    m_active = false;
    emit stateChanged();
}

// ==================== 编辑应用 ====================

void MultiCursorController::clampCursorsToDocument()
{
    // 外部文本变化（不经 applyBatch）后的防御性钳制：
    // QTextDocument::contentsChange 在 finishEdit 中先于 cursorPositionChanged /
    // textChanged（即先于 QML 覆盖层任何重算）发射，此处把引擎光标静默钳制到
    // 新文档合法范围，可保证后续覆盖层经 positionToRectangle 的每次重算读到的
    // 位置恒合法，不产生 "QTextCursor::setPosition: Position 'N' out of range"
    // 告警。静默更新（不发 stateChanged）：外部替换后 QML 会经 reset() /
    // syncNativeCursor() 重建引擎状态，最终光标/文本结果不受影响；
    // applyBatch 自身的编辑不经过此路径（引擎已提前持有批次后新光标）。
    if (m_applyingBatch || !m_doc)
        return;
    const int maxPos = int(m_doc->characterCount()) - 1;
    QList<McCursor> cursors = m_engine.cursors();
    bool changed = false;
    for (McCursor &c : cursors) {
        const int pos = qBound(0, c.position, maxPos);
        const int anchor = qBound(0, c.anchor, maxPos);
        if (pos != c.position || anchor != c.anchor) {
            c.position = pos;
            c.anchor = anchor;
            changed = true;
        }
    }
    if (changed)
        m_engine.setCursors(cursors); // 纯状态更新，无信号
}

void MultiCursorController::applyBatch(const McEditBatch &batch)
{
    // ---- 修复 "QTextCursor::setPosition: Position 'N' out of range" 告警 ----
    // 根因：文档编辑提交（endEditBlock → finishEdit）会同步发出
    // cursorPositionChanged / textChanged 通知，QML 覆盖层 MultiCursorLayer
    // 的绑定（extraCursorRects / selectionSegments）随之立即重算，经
    // TextArea.positionToRectangle 把控制器暴露的每个光标/选区位置换算为
    // 矩形；该方法内部直接调用 QTextCursor::setPosition 且不做任何钳制，
    // 位置 >= 文档长度即输出上述告警。若引擎此刻仍持有编辑前的旧光标 /
    // 旧选区，文档缩短时旧位置越界，告警由此产生。
    // 因此必须保证"编辑期间的任何中间重算"读到的控制器位置都与变化后的
    // 新文本一致：先把引擎光标静默切换到批次后的新状态（不发 stateChanged；
    // 批次 cursors 由引擎按编辑后文本计算，恒在合法范围），再执行文档编辑；
    // 两者之间为同步 C++ 调用，不存在 QML 观察窗口。stateChanged 仍在最后
    // 统一发射，对外信号时序与语义不变。
    endKbColumnSession();
    if (m_engine.isColumnSelecting())
        m_engine.finishColumnSelect();
    m_engine.setCursors(batch.cursors);
    if (m_doc && !batch.edits.isEmpty()) {
        // edits 已按 start 降序：从后往前应用保证前面的偏移稳定；
        // 单 editBlock 保证一次按键 = 单次撤销
        m_applyingBatch = true; // 本批次编辑触发的 contentsChange 不做钳制
        QTextCursor cursor(m_doc);
        cursor.beginEditBlock();
        for (const McEdit &e : batch.edits) {
            cursor.setPosition(e.start);
            cursor.setPosition(e.end, QTextCursor::KeepAnchor);
            cursor.insertText(e.text);
        }
        // 批量编辑（尤其整行删除）可能使编辑光标最终停留的位置落在
        // 已删除区间内；在 endEditBlock 前把光标钳制到新文档合法范围并
        // 移到批次后第一个新光标位置（始终合法且语义正确），保证编辑
        // 光标的终点状态确定合法（撤销栈记录的块位置随之稳定）。
        // 对外行为（文档内容、引擎光标、信号）均不受影响。
        const int lastPos = batch.cursors.isEmpty()
                ? 0 : batch.cursors.constFirst().position;
        cursor.setPosition(qBound(0, lastPos, int(m_doc->characterCount()) - 1));
        cursor.endEditBlock();
        m_applyingBatch = false;
    }
    // 编辑后光标可能汇聚为单光标且无任何选区（如全部选区被一次输入替换、
    // 同行多光标移动到同一位置）：接管应随汇聚及时回落为 false
    //（与 handleKey 各触发键分支的同步策略一致；上方原生光标的同步逻辑不变）。
    const bool wasActive = m_active;
    syncActiveFromEngine();
    if (wasActive && !m_active && m_doc && m_target) {
        // 回落时把原生光标定位到汇聚点：接管期的“editor.cursorPosition =
        // primaryPosition”反向同步由 QML 在 stateChanged 且仍接管时执行，
        // 回落后不再走该路径；纯移动类汇聚批次无文档编辑，若不在此定位，
        // 可见光标会停在汇聚前的位置、引擎却持有汇聚位置（状态分裂）
        m_target->setProperty(
                    "cursorPosition",
                    qBound(0, primaryPosition(), int(m_doc->characterCount()) - 1));
    }
    emit stateChanged();
}

// ==================== 按键分发 ====================

void MultiCursorController::keyboardColumnSelect(int key, const QString &text)
{
    if (!m_kbColumnActive) {
        // 首次：以当前主光标（active==false 时即原生光标）为锚进入列选模式；
        // 锚点记录为视觉量（光标矩形 x / 行顶 y），后续全部按真实布局逐行命中
        const int start = primaryPosition();
        const QRectF rect = layoutCursorRect(start);
        if (rect.height() <= 0) {
            qWarning("MultiCursorController: keyboard column select "
                     "layout unavailable");
            return; // 布局不可用：不进入会话（DEC-002）
        }
        m_engine.startColumnSelect(text, start, start);
        m_kbAnchorX = m_kbCursorX = rect.x();
        m_kbAnchorY = m_kbCursorY = rect.y();
        m_kbColumnActive = true;
    }
    // 行高取主光标位光标矩形高度（等宽单一行高；<=0 为布局不可用）
    const qreal lineH = layoutCursorRect(primaryPosition()).height();
    if (lineH <= 0)
        return;
    switch (key) {
    case Qt::Key_Up:
    case Qt::Key_Down: {
        const qreal step = key == Qt::Key_Up ? -lineH : lineH;
        const qreal newY = m_kbCursorY + step;
        // 钳制到文档首/末可视行顶：越界取边界值——活动点已在首/末行时
        // newY 钳回同值，随后的逐行重算无变化，等价既有“首行 Up /
        // 末行 Down 无操作”；期望 x 不改（粘性）
        const QRectF firstRect = layoutCursorRect(0);
        const QRectF lastRect = layoutCursorRect(int(text.length()));
        if (firstRect.height() <= 0 || lastRect.height() <= 0)
            return; // 部分失败保守不更新（DEC-002）；同一布局引擎上实际不可达，防御性闭环
        m_kbCursorY = qBound(firstRect.y(), newY, lastRect.y());
        break;
    }
    case Qt::Key_Left:
    case Qt::Key_Right: {
        // 活动行上按字符步进（DEC-005）：先以（粘性的）期望 x 在活动行
        // 中心命中——期望 x 超出短行行末时命中钳制到行末，以钳制位为
        // 基准 ±1 字符（Left 退到行内最后字符左缘；Right 越过行末即跨行）
        const int pos = layoutPositionAt(m_kbCursorX, m_kbCursorY + lineH / 2);
        if (pos < 0)
            return;
        const int delta = key == Qt::Key_Left ? -1 : +1;
        const int np = qBound(0, pos + delta, int(text.length()));
        // 实际步进（np!=pos）且仍在同一可视行（行顶差为 0）时才移动期望 x：
        // 跨行不动（Left/Right 不跨行——行首 Left / 行末 Right 无操作）；
        // np==pos（文档边界）同样无操作，防止把粘性期望 x 塌缩到行末
        if (np != pos) {
            const QRectF pr = layoutCursorRect(pos);
            const QRectF nr = layoutCursorRect(np);
            // 任一矩形无效视为跨行（不动期望 x）：部分失败保守不更新
            //（DEC-002）；同一布局引擎上实际不可达，防御性闭环
            if (pr.height() <= 0 || nr.height() <= 0)
                break;
            if (qAbs(nr.y() - pr.y()) <= lineH / 2)
                m_kbCursorX = nr.x();
        }
        break; // 活动行 y 不变
    }
    default:
        break;
    }
    // 与鼠标列选完全相同的逐行命中重算选区（锚点行 → 活动行）
    const QList<QPair<int, int>> pairs = visualColumnPairs(
                m_kbAnchorX, m_kbCursorX,
                qMin(m_kbAnchorY, m_kbCursorY), qMax(m_kbAnchorY, m_kbCursorY));
    if (pairs.isEmpty())
        return;
    m_engine.updateColumnSelectPairs(pairs);
    syncActiveFromEngine();
    emit stateChanged();
}

bool MultiCursorController::handleKey(int key, int modifiers, const QString &text)
{
    if (!m_doc)
        return false;

    const bool shift = modifiers & Qt::ShiftModifier;
    const bool ctrl = modifiers & (Qt::ControlModifier | Qt::MetaModifier);
    const bool alt = modifiers & Qt::AltModifier;
    const QString docText = m_doc->toPlainText();

    // Esc：退出多光标（列选拖拽/键盘列选一并取消），保留排序最末光标
    if (m_active && key == Qt::Key_Escape) {
        deactivateToLast();
        return true;
    }

    // ---- 多光标触发命令（active 与否均生效：Ctrl+D 等需可重复触发） ----

    // Ctrl+Alt+Shift+方向键：键盘列选，逐格扩展
    if (ctrl && alt && shift
        && (key == Qt::Key_Up || key == Qt::Key_Down
            || key == Qt::Key_Left || key == Qt::Key_Right)) {
        keyboardColumnSelect(key, docText);
        return true;
    }

    // Ctrl+Alt+Up/Down：上/下方加光标（引擎无实际新增则不改变 active）
    if (ctrl && alt && !shift && (key == Qt::Key_Up || key == Qt::Key_Down)) {
        endKbColumnSession();
        if (key == Qt::Key_Up)
            m_engine.addCursorAbove(docText);
        else
            m_engine.addCursorBelow(docText);
        syncActiveFromEngine();
        emit stateChanged();
        return true;
    }

    // Ctrl+D：选中下一个相同文本（无选区时先选中光标处单词）
    if (ctrl && !alt && !shift && key == Qt::Key_D) {
        endKbColumnSession();
        m_engine.selectNextOccurrence(docText); // 产生选区即接管（即使 1 个光标）
        syncActiveFromEngine();
        emit stateChanged();
        return true;
    }

    // Ctrl+Shift+L：选中全部相同文本
    if (ctrl && shift && !alt && key == Qt::Key_L) {
        endKbColumnSession();
        m_engine.selectAllOccurrences(docText);
        syncActiveFromEngine();
        emit stateChanged();
        return true;
    }

    // Alt+Shift+I：选中多行的每一行行尾插入光标（仅多行光标才接管）
    if (alt && shift && !ctrl && key == Qt::Key_I) {
        endKbColumnSession();
        // 列选拖拽中触发：先结束列选会话（与 applyBatch 内同法）。
        // 引擎 endOfLines 不清除列选状态，若残留 columnSelecting，
        // 后续拖拽 move 事件会经 columnDragUpdateAt 用旧锚点重算列选区，
        // 覆盖掉刚生成的行尾光标
        if (m_engine.isColumnSelecting())
            m_engine.finishColumnSelect();
        const QList<McCursor> snapshot = m_engine.cursors();
        m_engine.endOfLines(docText);
        if (m_engine.cursorCount() > 1) {
            syncActiveFromEngine();
        } else {
            // 单行：无多行光标，不接管；回滚引擎状态以保持与原生光标一致
            m_engine.setCursors(snapshot);
        }
        emit stateChanged();
        return true;
    }

    if (!m_active)
        return false; // 非触发键：原生处理

    // ---- active 时的编辑命令 ----

    // Ctrl+C/X/V：多光标剪贴板操作
    if (ctrl && !alt && !shift) {
        switch (key) {
        case Qt::Key_C:
            copy();
            return true;
        case Qt::Key_X:
            cut();
            return true;
        case Qt::Key_V:
            paste();
            return true;
        default:
            break;
        }
    }

    if (!alt) {
        switch (key) {
        case Qt::Key_Backspace:
            if (!ctrl) { // Ctrl+Backspace（删词）不支持：落入未知键
                applyBatch(m_engine.backspace(docText));
                return true;
            }
            break;
        case Qt::Key_Delete:
            if (!ctrl) { // Ctrl+Delete（删词）不支持：落入未知键
                applyBatch(m_engine.deleteChar(docText));
                return true;
            }
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (!ctrl) { // Ctrl+Enter（下方插行）不支持：落入未知键
                applyBatch(m_engine.insertNewline());
                return true;
            }
            break;
        case Qt::Key_Left: // Ctrl 词移动、Shift keepAnchor
            applyBatch(m_engine.moveLeft(docText, shift, ctrl));
            return true;
        case Qt::Key_Right:
            applyBatch(m_engine.moveRight(docText, shift, ctrl));
            return true;
        case Qt::Key_Up: // Ctrl+Up/Down 原生为滚动：不接管，落入未知键
            if (!ctrl) {
                applyBatch(m_engine.moveUp(docText, shift));
                return true;
            }
            break;
        case Qt::Key_Down:
            if (!ctrl) {
                applyBatch(m_engine.moveDown(docText, shift));
                return true;
            }
            break;
        case Qt::Key_Home: // Ctrl+Home/End 原生为文档首/末：不接管
            if (!ctrl) {
                applyBatch(m_engine.moveHome(docText, shift));
                return true;
            }
            break;
        case Qt::Key_End:
            if (!ctrl) {
                applyBatch(m_engine.moveEnd(docText, shift));
                return true;
            }
            break;
        default:
            break;
        }
    }

    // 可打印文本（含 Tab="\t"、空格、中文等）：分发到所有光标（覆盖选区）；
    // Alt 组合不在此分发（与上方方向键块的保护一致，落入未知命令路径：
    // 退单光标 + false 交原生处理）
    if (!text.isEmpty() && !ctrl && !alt) {
        applyBatch(m_engine.insertText(text));
        return true;
    }

    // 其余未知命令（Ctrl+A、Backtab 切焦点等）：退单光标后交还原生按原状态处理
    deactivateToLast();
    return false;
}

// ==================== 鼠标交互 ====================

void MultiCursorController::altClick(int position)
{
    if (!m_doc)
        return;
    endKbColumnSession();
    if (m_engine.isColumnSelecting())
        m_engine.finishColumnSelect();
    const int len = m_doc->toPlainText().length();
    m_engine.addCursorAt(qBound(0, position, len)); // 位置重复时引擎去重，不改变状态
    syncActiveFromEngine();
    emit stateChanged();
}

void MultiCursorController::columnDragBeginAt(qreal x, qreal y)
{
    if (!m_doc)
        return;
    endKbColumnSession(); // 先结束残留键盘列选会话（含引擎列选状态收尾）
    const int pos = layoutPositionAt(x, y);
    if (pos < 0)
        return; // 命中失败：保守不进入列选（DEC-002，layoutPositionAt 已告警）
    const QString text = m_doc->toPlainText();
    m_engine.startColumnSelect(text, pos, pos); // 锚 = 游标 = 按下点，进入列选状态机
    // 锚点记录为视觉 x 与归一行顶 y：行顶取命中位光标矩形（与逐行命中
    // 的“按行顶步进”同基准），此后拖拽期间的行覆盖范围恒由布局决定
    const QRectF rect = layoutCursorRect(pos);
    m_colDragAnchorX = x;
    m_colDragAnchorY = rect.y();
    syncActiveFromEngine();
    emit stateChanged();
}

void MultiCursorController::columnDragUpdateAt(qreal x, qreal y)
{
    if (!m_doc || !m_engine.isColumnSelecting())
        return;
    const int cPos = layoutPositionAt(x, y);
    if (cPos < 0)
        return; // 命中失败：保守无操作，选区停留上一次有效状态（DEC-002）
    const QRectF cRect = layoutCursorRect(cPos); // 当前行顶（归一基准）
    if (cRect.height() <= 0)
        return; // 部分失败保守不更新（DEC-002）；同一布局引擎上实际不可达，防御性闭环
    const qreal cTopY = cRect.y();
    // 锚点行 → 当前行逐可视行命中：矩形两边即锚点 x 与当前 x，反向
    // 拖拽（锚点在下）与正向等价（qMin/qMax 归一行范围，行内区间已归一）
    const QList<QPair<int, int>> pairs = visualColumnPairs(
                m_colDragAnchorX, x,
                qMin(m_colDragAnchorY, cTopY), qMax(m_colDragAnchorY, cTopY));
    if (pairs.isEmpty())
        return;
    m_engine.updateColumnSelectPairs(pairs);
    syncActiveFromEngine();
    emit stateChanged();
}

void MultiCursorController::columnDragEnd()
{
    if (!m_engine.isColumnSelecting())
        return;
    m_engine.finishColumnSelect(); // 光标集合定格为普通多选区
    syncActiveFromEngine();
    emit stateChanged();
}

// ==================== 剪贴板 ====================

void MultiCursorController::copy()
{
    if (!m_active || !m_doc || !hasSelection())
        return; // 无任何选区：不写剪贴板
    const QStringList texts = m_engine.selectedTexts(m_doc->toPlainText());
    QGuiApplication::clipboard()->setText(texts.join(QLatin1Char('\n')));
}

void MultiCursorController::cut()
{
    if (!m_active || !m_doc)
        return;
    copy(); // 无选区时 copy 不写剪贴板，deleteSelections 为无操作批次
    applyBatch(m_engine.deleteSelections());
}

void MultiCursorController::paste()
{
    if (!m_active || !m_doc)
        return;
    QString clip = QGuiApplication::clipboard()->text();
    clip.remove(QLatin1Char('\r')); // 兼容 \r\n（及孤立 \r）
    const QStringList lines = clip.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    applyBatch(m_engine.pasteLines(lines));
}

// ==================== 输入法 ====================

bool MultiCursorController::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_target && event->type() == QEvent::InputMethod) {
        auto *ime = static_cast<QInputMethodEvent *>(event);
        // 仅拦截提交字符串：分发到所有光标并消费（阻止原生再次插入）；
        // 预编辑（preedit）一律放行原生，只在主光标处由 TextArea 显示
        if (m_active && !ime->commitString().isEmpty()) {
            applyBatch(m_engine.insertText(ime->commitString()));
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}

} // namespace qcnote
