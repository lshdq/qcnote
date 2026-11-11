// 查找替换控制器实现。indicator 配置见 scintillaedit.cpp（每实例统一）：
//   indicator 8 = 全部匹配（INDIC_ROUNDBOX + alpha，searchMatch 色）
//   indicator 9 = 当前匹配（INDIC_BOX 描边，searchMatchBorder 色）
#include "editor/searchcontroller.h"

#include <algorithm>

#include "Scintilla.h"
#include "editor/scintillaedit.h"

namespace editor {

namespace {

auto send(HWND sci, UINT msg, WPARAM w = 0, LPARAM l = 0) {
    return ::SendMessageW(sci, msg, w, l);
}

}  // namespace

void SearchController::bind(core::Document* doc, HWND sci) {
    doc_ = doc;
    sci_ = sci;
    restartSearch();  // 文档对象变化：按新文档重算（索引归 0）；参数保留
}

void SearchController::setPattern(std::string_view utf8) {
    if (params_.pattern == utf8) return;
    params_.pattern = utf8;
    restartSearch();  // 参数变更 = 全新查找会话
}

void SearchController::setReplacement(std::string_view utf8) {
    params_.replacement = utf8;  // 不影响匹配集合
    if (onChanged) onChanged();
}

void SearchController::setOptions(core::SearchOptions options) {
    if (params_.options.matchCase == options.matchCase &&
        params_.options.wholeWord == options.wholeWord &&
        params_.options.regex == options.regex) {
        return;
    }
    params_.options = options;
    restartSearch();
}

void SearchController::recompute() {
    matches_.clear();
    overflowed_ = false;
    errorMessage_.clear();
    const std::string& content = doc_ ? doc_->content() : std::string();
    if (params_.pattern.empty()) return;  // 空查找串：恒无匹配（不报错）
    const core::SearchResult r = core::searchFindAll(content, params_.pattern, params_.options);
    matches_ = std::move(r.matches);
    overflowed_ = r.overflowed;
    errorMessage_ = r.errorMessage;
}

void SearchController::clampIndex() {
    const int n = static_cast<int>(matches_.size());
    if (n == 0) {
        currentIndex_ = -1;
    } else if (currentIndex_ >= n) {
        currentIndex_ = n - 1;  // 外部编辑收缩匹配表：钳制（保持当前定位）
    }
}

void SearchController::restartSearch() {
    recompute();
    currentIndex_ = matches_.empty() ? -1 : 0;
    ++generation_;
    refreshHighlights();
    if (onChanged) onChanged();
}

void SearchController::onContentChanged() {
    if (applying_ || !doc_) return;
    recompute();
    clampIndex();
    refreshHighlights();
    if (onChanged) onChanged();
}

int SearchController::firstMatchAtOrAfter(int pos) const {
    const int n = static_cast<int>(matches_.size());
    if (n == 0) return -1;
    for (int i = 0; i < n; ++i) {
        if (matches_[static_cast<size_t>(i)].start >= pos) return i;
    }
    return 0;  // 环绕到首个
}

void SearchController::applyCurrentSelection() {
    if (!sci_ || currentIndex_ < 0 ||
        currentIndex_ >= static_cast<int>(matches_.size())) {
        return;
    }
    const core::SearchMatch& m = matches_[static_cast<size_t>(currentIndex_)];
    send(sci_, SCI_SETSEL, static_cast<WPARAM>(m.start), static_cast<LPARAM>(m.start + m.length));
    send(sci_, SCI_SCROLLCARET, 0, 0);
    refreshHighlights();
}

void SearchController::findNext() {
    // 计数重载：免构造 SearchResult 临时（其按值拷贝整个 matches_ 向量）
    const int next = core::searchNextIndex(static_cast<int>(matches_.size()), currentIndex_,
                                           true);
    if (next < 0) return;
    currentIndex_ = next;
    applyCurrentSelection();
    if (onChanged) onChanged();
}

void SearchController::findPrev() {
    const int next = core::searchNextIndex(static_cast<int>(matches_.size()), currentIndex_,
                                           false);
    if (next < 0) return;
    currentIndex_ = next;
    applyCurrentSelection();
    if (onChanged) onChanged();
}

void SearchController::selectCurrent() {
    if (currentIndex_ < 0 || !sci_) return;
    applyCurrentSelection();
    if (onChanged) onChanged();
}

bool SearchController::replaceCurrent() {
    if (!sci_ || !doc_ || matches_.empty() || params_.pattern.empty() ||
        !errorMessage_.empty()) {
        return false;
    }
    if (currentIndex_ < 0 || currentIndex_ >= static_cast<int>(matches_.size())) {
        currentIndex_ = 0;  // 打开即点替换：从第一个开始
    }
    const core::SearchMatch match = matches_[static_cast<size_t>(currentIndex_)];
    const std::string& content = doc_->content();

    // 捕获组展开（正则模式重建匹配数据；纯文本直插）
    const void* matchData =
        core::searchRebuildMatchData(content, params_.pattern, params_.options, match);
    const std::string replacement = core::searchExpandReplacement(
        params_.replacement, content, match, matchData);
    core::searchFreeMatchData(matchData);

    applying_ = true;
    // 一次 SCI_REPLACETARGET 自成一步撤销（B5「替换」= 一步一次撤销）
    send(sci_, SCI_SETTARGETRANGE, static_cast<WPARAM>(match.start),
         static_cast<LPARAM>(match.start + match.length));
    send(sci_, SCI_REPLACETARGET, replacement.size(),
         reinterpret_cast<LPARAM>(replacement.c_str()));
    const int caret = match.start + static_cast<int>(replacement.size());
    recompute();
    // 指向替换文本之后的下一个匹配（环绕）；全部替换完则无当前匹配
    currentIndex_ = firstMatchAtOrAfter(caret);
    if (currentIndex_ >= 0) {
        // 光标落在替换文本之后（不选中新匹配——旧版 selectInTarget(caret,caret)）
        send(sci_, SCI_SETSEL, static_cast<WPARAM>(caret), static_cast<LPARAM>(caret));
    }
    applying_ = false;
    refreshHighlights();
    if (onChanged) onChanged();
    return true;
}

int SearchController::replaceAll() {
    if (!sci_ || !doc_ || matches_.empty() || params_.pattern.empty() ||
        !errorMessage_.empty()) {
        return 0;
    }
    // 按原文本坐标一次性计算每处替换文本（替换产物不参与本轮——升序缝合
    // 与 core::searchReplaceAll 契约产物一致），从后往前应用，单批撤销
    const std::string& content = doc_->content();
    std::vector<std::pair<core::SearchMatch, std::string>> edits;
    edits.reserve(matches_.size());
    for (const core::SearchMatch& m : matches_) {
        const void* matchData =
            core::searchRebuildMatchData(content, params_.pattern, params_.options, m);
        edits.emplace_back(
            m, core::searchExpandReplacement(params_.replacement, content, m, matchData));
        core::searchFreeMatchData(matchData);
    }

    applying_ = true;
    send(sci_, SCI_BEGINUNDOACTION, 0, 0);
    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
        send(sci_, SCI_SETTARGETRANGE, static_cast<WPARAM>(it->first.start),
             static_cast<LPARAM>(it->first.start + it->first.length));
        send(sci_, SCI_REPLACETARGET, it->second.size(),
             reinterpret_cast<LPARAM>(it->second.c_str()));
    }
    send(sci_, SCI_ENDUNDOACTION, 0, 0);
    // 光标落在首处替换文本之后（旧版 editBlock 从后往前应用后的落点）
    const int caret = edits.front().first.start +
                      static_cast<int>(edits.front().second.size());
    recompute();
    currentIndex_ = matches_.empty() ? -1 : 0;
    send(sci_, SCI_SETSEL, static_cast<WPARAM>(caret), static_cast<LPARAM>(caret));
    applying_ = false;
    refreshHighlights();
    if (onChanged) onChanged();
    return static_cast<int>(edits.size());
}

void SearchController::clearHighlights() {
    // DEC-009：关闭对话框——清全部高亮，匹配状态（matches/currentIndex/
    // 参数）保留供 F3 继续跳转
    if (!sci_) return;
    const int len = static_cast<int>(send(sci_, SCI_GETLENGTH, 0, 0));
    send(sci_, SCI_SETINDICATORCURRENT, editor::kIndicatorSearchMatch, 0);
    send(sci_, SCI_INDICATORCLEARRANGE, 0, static_cast<LPARAM>(len));
    send(sci_, SCI_SETINDICATORCURRENT, editor::kIndicatorSearchCurrent, 0);
    send(sci_, SCI_INDICATORCLEARRANGE, 0, static_cast<LPARAM>(len));
}

void SearchController::setHighlightsEnabled(bool on) {
    highlightsOn_ = on;
    if (on) {
        refreshHighlights();
    } else {
        clearHighlights();
    }
}

bool SearchController::currentMatchRange(int* start, int* length) const {
    if (!start || !length || currentIndex_ < 0 ||
        currentIndex_ >= static_cast<int>(matches_.size())) {
        return false;
    }
    const core::SearchMatch& m = matches_[static_cast<size_t>(currentIndex_)];
    *start = m.start;
    *length = m.length;
    return true;
}

void SearchController::refreshHighlights() {
    if (!sci_) return;
    clearHighlights();
    if (!highlightsOn_) return;  // 对话框关闭态：不绘制（DEC-009）
    // >10000（溢出）：停止绘制全部匹配底色（导航/替换仍可用，DEC-004）
    if (!overflowed_) {
        send(sci_, SCI_SETINDICATORCURRENT, editor::kIndicatorSearchMatch, 0);
        for (const core::SearchMatch& m : matches_) {
            if (m.length <= 0) continue;  // 零长度匹配不高亮（DEC-005）
            send(sci_, SCI_INDICATORFILLRANGE, static_cast<WPARAM>(m.start),
                 static_cast<LPARAM>(m.length));
        }
    }
    // 当前匹配描边（单段开销可忽略，溢出时保留——旧版语义）
    if (currentIndex_ >= 0 && currentIndex_ < static_cast<int>(matches_.size())) {
        const core::SearchMatch& m = matches_[static_cast<size_t>(currentIndex_)];
        if (m.length > 0) {
            send(sci_, SCI_SETINDICATORCURRENT, editor::kIndicatorSearchCurrent, 0);
            send(sci_, SCI_INDICATORFILLRANGE, static_cast<WPARAM>(m.start),
                 static_cast<LPARAM>(m.length));
        }
    }
}

}  // namespace editor
