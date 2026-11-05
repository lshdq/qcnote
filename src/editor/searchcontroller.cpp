#include "editor/searchcontroller.h"

#include <QMetaObject>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QTextCursor>
#include <QTextDocument>

namespace qcnote {

// ==================== 构造 / 目标与文档 ====================

SearchController::SearchController(QObject *parent)
    : QObject(parent)
{
}

SearchController::~SearchController() = default;

QQuickItem *SearchController::target() const
{
    return m_target;
}

void SearchController::setTarget(QQuickItem *target)
{
    if (m_target == target)
        return;
    m_target = target;
    emit stateChanged();
}

QQuickTextDocument *SearchController::textDocument() const
{
    return m_textDocument;
}

void SearchController::setTextDocument(QQuickTextDocument *textDocument)
{
    if (m_textDocument == textDocument)
        return;
    if (m_doc)
        disconnect(m_doc, &QTextDocument::contentsChange,
                   this, &SearchController::onDocumentContentsChanged);
    m_textDocument = textDocument;
    m_doc = textDocument ? textDocument->textDocument() : nullptr;
    if (m_doc) {
        // 监听文档内容变化（含本控制器替换编辑之外的一切修改：正常键入、
        // 原生撤销/重做、外部整体重置）：重算匹配并钳制当前索引，保证对话
        // 框打开期间匹配列表与文本始终一致（需求边界：外部重置防御性钳制）
        connect(m_doc, &QTextDocument::contentsChange,
                this, &SearchController::onDocumentContentsChanged);
    }
    // 文档对象变化：匹配状态按新文档重算；查找串/选项保留（会话内保留），
    // 同一会话切换文档后无需重新输入
    restartSearch();
}

// ==================== 查找参数属性 ====================

QString SearchController::pattern() const
{
    return m_engine.pattern();
}

void SearchController::setPattern(const QString &pattern)
{
    if (m_engine.pattern() == pattern)
        return;
    m_engine.setPattern(pattern, m_engine.options());
    restartSearch();
}

QString SearchController::replacement() const
{
    return m_replacement;
}

void SearchController::setReplacement(const QString &replacement)
{
    if (m_replacement == replacement)
        return;
    // 替换串不影响匹配集合（只参与未来替换的展开）：无需重算匹配，仅通知
    m_replacement = replacement;
    emit stateChanged();
}

bool SearchController::caseSensitive() const
{
    return m_engine.options().caseSensitive;
}

void SearchController::setCaseSensitive(bool on)
{
    SearchOptions options = m_engine.options();
    if (options.caseSensitive == on)
        return;
    options.caseSensitive = on;
    m_engine.setPattern(m_engine.pattern(), options);
    restartSearch();
}

bool SearchController::wholeWord() const
{
    return m_engine.options().wholeWord;
}

void SearchController::setWholeWord(bool on)
{
    SearchOptions options = m_engine.options();
    if (options.wholeWord == on)
        return;
    options.wholeWord = on;
    m_engine.setPattern(m_engine.pattern(), options);
    restartSearch();
}

bool SearchController::useRegex() const
{
    return m_engine.options().useRegex;
}

void SearchController::setUseRegex(bool on)
{
    SearchOptions options = m_engine.options();
    if (options.useRegex == on)
        return;
    options.useRegex = on;
    m_engine.setPattern(m_engine.pattern(), options);
    restartSearch();
}

// ==================== 匹配状态属性 ====================

int SearchController::matchCount() const
{
    return m_matches.size();
}

int SearchController::currentIndex() const
{
    return m_currentIndex;
}

QVariantList SearchController::matchRanges() const
{
    return m_matchRanges;
}

QVariantList SearchController::currentRange() const
{
    if (m_currentIndex < 0 || m_currentIndex >= m_matches.size())
        return {};
    const SearchMatch &m = m_matches.at(m_currentIndex);
    return QVariantList{m.start, m.end};
}

bool SearchController::overflowed() const
{
    return m_overflowed;
}

QString SearchController::errorMessage() const
{
    return m_engine.errorMessage();
}

// ==================== 状态维护 ====================

void SearchController::recomputeMatches()
{
    const SearchResult result = m_engine.findAll(m_doc ? m_doc->toPlainText() : QString());
    m_matches = result.matches;
    m_overflowed = result.overflowed;
    // matchRanges 属性缓存：高亮层绑定按次整体取值，预构建避免每次取值重建
    m_matchRanges.clear();
    m_matchRanges.reserve(m_matches.size());
    for (const SearchMatch &m : std::as_const(m_matches))
        m_matchRanges.append(QVariantList{m.start, m.end});
}

void SearchController::restartSearch()
{
    recomputeMatches();
    // 查找参数变更 = 全新查找会话：当前匹配定位到首个（无匹配则清除）。
    // 不尝试从光标就近定位——控制器不追踪光标，QML 侧如需就近语义可依
    // currentRange 二次调整
    m_currentIndex = m_matches.isEmpty() ? -1 : 0;
    emit stateChanged();
}

void SearchController::onDocumentContentsChanged()
{
    // 自身编辑（applyEdits）期间跳过：匹配状态已静默清空，编辑完成后由
    // 调用方显式重算并定位（replaceCurrent/replaceAll），此处重算徒劳且
    // 会在 endEditBlock 同步通知里读到中间状态
    if (m_applyingEdits || !m_doc)
        return;
    recomputeMatches();
    // 外部内容变化（正常键入/撤销/整体重置）：保持当前索引钳制到合法范围；
    // 原本无当前匹配（-1）保持不变——外部编辑不应凭空制造当前选区
    m_currentIndex = SearchEngine::clampedIndex(m_currentIndex, m_matches.size());
    emit stateChanged();
}

void SearchController::reset()
{
    m_matches.clear();
    m_matchRanges.clear();
    m_currentIndex = -1;
    m_overflowed = false;
    emit stateChanged();
}

// ==================== 编辑应用 ====================

void SearchController::applyEdits(const QList<SearchEdit> &edits)
{
    if (!m_doc || edits.isEmpty())
        return;
    // 先清空对外匹配状态并立即发 stateChanged：编辑提交（endEditBlock）会
    // 同步发出 contentsChange/textChanged 通知，QML 高亮层的绑定随之立即
    // 重算；此刻若仍暴露旧匹配区间，替换使文档缩短时旧区间越界，经
    // positionToRectangle 触发 QTextCursor::setPosition 越界告警（与
    // MultiCursorController::applyBatch 的“先切新状态再编辑”同因）。若只
    // 清空不发信号，QML 绑定不重估、仍持旧 matchRanges，在文档已变短的
    // 窗口内求段即产生告警——故必须同步补发 stateChanged 让 QML 拉取空
    // 匹配状态（空列表对任何文档长度恒合法），彻底消除观察窗口。编辑完成
    // 后由调用方重算并统一发 stateChanged；此处的信号不会引起重入（下一行
    // 才置 m_applyingEdits，且本控制器不发 contentsChange）。
    m_matches.clear();
    m_matchRanges.clear();
    m_currentIndex = -1;
    emit stateChanged();

    m_applyingEdits = true; // 本控制器编辑触发的 contentsChange 不重算（防重入）
    QTextCursor cursor(m_doc);
    cursor.beginEditBlock(); // 单 editBlock：「替换」一步/「全部替换」整体 = 单次撤销
    for (const SearchEdit &e : std::as_const(edits)) {
        // edits 已按 start 降序：从后往前应用，前面的编辑偏移不受影响
        cursor.setPosition(e.start);
        cursor.setPosition(e.end, QTextCursor::KeepAnchor);
        cursor.insertText(e.text);
    }
    cursor.endEditBlock();
    m_applyingEdits = false;
}

void SearchController::selectInTarget(int start, int end)
{
    if (!m_target || !m_doc)
        return;
    // TextArea 的 select(start,end) 是 QML 可调用方法（TextEdit 暴露），
    // 经元对象调用把区间设为原生选区（start==end 即光标定位）。位置钳制到
    // 文档合法范围；target 未设置或不是 TextArea 时调用落空，静默忽略
    //（QML 侧可改用 currentRange 属性自行联动）
    const int maxPos = int(m_doc->characterCount()) - 1;
    QMetaObject::invokeMethod(m_target, "select",
                              Q_ARG(int, qBound(0, start, maxPos)),
                              Q_ARG(int, qBound(0, end, maxPos)));
}

void SearchController::selectCurrentInTarget()
{
    if (m_currentIndex < 0 || m_currentIndex >= m_matches.size())
        return;
    const SearchMatch &m = m_matches.at(m_currentIndex);
    selectInTarget(m.start, m.end);
}

// ==================== 导航与替换 ====================

void SearchController::findNext()
{
    const int next = SearchEngine::nextIndex(m_matches.size(), m_currentIndex);
    if (next < 0)
        return; // 无匹配：无操作
    m_currentIndex = next;
    selectCurrentInTarget(); // 当前匹配 → 原生选区（DEC-007）
    emit stateChanged();
}

void SearchController::findPrev()
{
    const int prev = SearchEngine::prevIndex(m_matches.size(), m_currentIndex);
    if (prev < 0)
        return; // 无匹配：无操作
    m_currentIndex = prev;
    selectCurrentInTarget();
    emit stateChanged();
}

void SearchController::replaceCurrent()
{
    if (!m_doc || m_matches.isEmpty() || !m_engine.isValid() || m_engine.pattern().isEmpty())
        return;
    if (m_currentIndex < 0 || m_currentIndex >= m_matches.size())
        m_currentIndex = 0; // 无当前匹配时从第一个开始（对话框打开即点替换的场景）

    const SearchMatch match = m_matches.at(m_currentIndex);
    SearchEdit edit;
    edit.start = match.start;
    edit.end = match.end;
    edit.text = SearchEngine::expandReplacement(match.captured, m_replacement);

    // “替换文本之后”的新文本坐标：replaceCurrent 的光标落点与下一个匹配
    // 的查找起点（编辑本身不改变其前方文本，原坐标 start 在新文本中有效）
    const int caret = edit.start + edit.text.length();

    applyEdits(QList<SearchEdit>{edit}); // 单 editBlock：一步一次撤销
    recomputeMatches();
    // 指向下一个匹配：从 caret 起首个匹配，无则环绕到第一个（与环绕导航
    // 语义一致）；全部替换完则无当前匹配
    m_currentIndex = SearchEngine::nextMatchIndexFrom(m_matches, caret);
    selectInTarget(caret, caret); // 光标设置到替换文本之后
    emit stateChanged();
}

int SearchController::replaceAll()
{
    if (!m_doc || m_matches.isEmpty() || !m_engine.isValid() || m_engine.pattern().isEmpty())
        return 0;
    // 按原文本坐标一次性计算（替换产物不参与本轮）；edits 已按 start 降序
    const QList<SearchEdit> edits =
            SearchEngine::computeReplaceEdits(m_doc->toPlainText(), m_matches, m_replacement);
    if (edits.isEmpty())
        return 0;
    applyEdits(edits); // 单 editBlock：全部替换整体 = 单次撤销
    recomputeMatches();
    // 替换产物不参与本轮，但新一轮查找会话从新文本重新计算（如 “aa”→“a”
    // 替换后仍可能有 “aa” 残留匹配，如实呈现）；当前匹配指向首个剩余匹配
    m_currentIndex = m_matches.isEmpty() ? -1 : 0;
    emit stateChanged();
    return edits.size();
}

} // namespace qcnote
