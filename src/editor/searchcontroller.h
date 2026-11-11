// qcnote 查找替换控制器（M3-B，B5）：core/searchengine 纯逻辑与 Scintilla
// 编辑器之间的接线层（对齐旧版 SearchController 的状态机语义）。
//
// 职责：
// - 持有查找参数（pattern/replacement/三开关）与匹配状态（matches/
//   currentIndex/overflowed/errorMessage）；参数会话内保留（内存，不落盘）；
// - pattern/选项变更 → 全新查找会话（重算 + 索引归 0）；
// - 活动文档重绑 → 重算（索引归 0，B5「活动文档切换时匹配状态重算」）；
// - 内容变化（非本控制器编辑）→ 重算并钳制索引（对话框关闭态同样重算，
//   供 F3 继续跳转——照旧版 contentsChange 恒重算）；
// - 全部匹配高亮（indicator 8）+ 当前匹配描边（indicator 9），随对话框
//   生命周期应用/清除（DEC-009：关闭对话框清高亮、不清匹配状态）；
// - 跳转 = 原生选区（SCI_SETSEL + SCI_SCROLLCARET）+ 描边；
// - 替换当前：单步撤销（一次 SCI_REPLACETARGET），完成后索引指向替换文本
//   之后的下一个匹配（环绕），光标落在替换文本之后；
// - 全部替换：按原文本坐标逐匹配从后往前应用，单批撤销（一步回滚全文）。
//
// 与 Document 内容同步：替换编辑经 SCI_REPLACETARGET 触发 SCN_MODIFIED →
// EditorHost 增量回写 Document → 组装层 documentContentChanged → 本控制器
// onContentChanged。自编辑期间 applying_ 防重入（对齐旧版 m_applyingEdits）。
#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "core/document.h"
#include "core/searchengine.h"

namespace editor {

// 会话保留查找参数（应用级内存，B5）
struct SearchParams {
    std::string pattern;      // UTF-8
    std::string replacement;  // UTF-8
    core::SearchOptions options;
};

class SearchController {
public:
    // 活动文档重绑（null 清空状态；模态保证对话框打开期间不发生切换）
    void bind(core::Document* doc, HWND sci);
    core::Document* document() const { return doc_; }
    HWND window() const { return sci_; }

    const SearchParams& params() const { return params_; }
    void setPattern(std::string_view utf8);
    void setReplacement(std::string_view utf8);
    void setOptions(core::SearchOptions options);

    // 匹配状态（对话框显示；DEC-009：关闭对话框不清）
    int matchCount() const { return static_cast<int>(matches_.size()); }
    int currentIndex() const { return currentIndex_; }
    bool overflowed() const { return overflowed_; }
    const std::wstring& errorMessage() const { return errorMessage_; }
    bool hasPattern() const { return !params_.pattern.empty(); }

    // 全新会话代数（pattern/选项/文档重绑时 +1；对话框 hasJumped 判定源）
    int generation() const { return generation_; }

    // 跳转：环绕推进 + 原生选区 + 滚动 + 当前匹配描边；无匹配无操作
    void findNext();
    void findPrev();
    // 定位当前匹配（不推进）：全新会话首跳
    void selectCurrent();

    // 替换当前匹配（返回是否发生替换；无可替换 false）
    bool replaceCurrent();
    // 全部替换（返回替换处数；单批撤销）
    int replaceAll();

    // 清除全部高亮（indicator 8/9 全范围清；匹配状态保留，DEC-009）
    void clearHighlights();
    // 按当前状态重画高亮（对话框打开期间的内容变化重算后调用）
    void refreshHighlights();

    // 高亮开关（对话框打开 true / 关闭 false——关闭态内容重算不恢复全部
    // 高亮，DEC-009；false 时即刻清除）
    void setHighlightsEnabled(bool on);

    // 当前匹配区间（无效/无当前匹配返回 false）
    bool currentMatchRange(int* start, int* length) const;

    // 内容变化钩子（组装层 documentContentChanged 转发；自编辑防重入）
    void onContentChanged();

    // 状态变化通知（重算/跳转/替换后；组装层刷新对话框计数与状态栏）
    std::function<void()> onChanged;

private:
    void restartSearch();   // 重算 + 索引归 0 + 代数 +1 + 高亮刷新
    void recompute();       // 只重算 matches/overflowed/error
    void clampIndex();
    int firstMatchAtOrAfter(int pos) const;  // 环绕：pos 起首个匹配，无则回首个
    void applyCurrentSelection();

    core::Document* doc_ = nullptr;
    HWND sci_ = nullptr;
    SearchParams params_;
    std::vector<core::SearchMatch> matches_;
    int currentIndex_ = -1;
    bool overflowed_ = false;
    std::wstring errorMessage_;
    int generation_ = 0;
    bool applying_ = false;       // 本控制器替换编辑进行中（onContentChanged 防重入）
    bool highlightsOn_ = false;   // 对话框打开期才绘制 indicator（DEC-009）
};

}  // namespace editor
