// 多光标交互映射层实现。消息常量见 vendor/scintilla/include/Scintilla.h；
// 语义映射表见头文件注释。
#include "editor/multicursorcontroller.h"

#include <windowsx.h>

#include <commctrl.h>

#include <algorithm>

#include "Scintilla.h"
#include "core/encoding.h"

namespace editor {

namespace {

// SetWindowSubclass 子类 id（进程内唯一即可）
constexpr UINT_PTR kSubclassId = 0x51431101;  // 'QC1\1'

// WM_LBUTTONDOWN wParam 的 MK_ALT（0x20；SDK 头按 WIN32_LEAN_AND_MEAN
// 可能未定义，防御性自定义）
#ifndef MK_ALT
constexpr UINT MK_ALT_ = 0x20;
#else
constexpr UINT MK_ALT_ = MK_ALT;
#endif

auto send(HWND sci, UINT msg, WPARAM w = 0, LPARAM l = 0) {
    return ::SendMessageW(sci, msg, w, l);
}

int textLength(HWND sci) {
    return static_cast<int>(send(sci, SCI_GETLENGTH, 0, 0));
}

// UTF-8 字符步进（内容契约：合法 UTF-8；步进结果保持字符边界）
int stepCharLeft(const std::string& text, int pos) {
    if (pos <= 0) return 0;
    int q = pos - 1;
    while (q > 0 && (static_cast<unsigned char>(text[static_cast<size_t>(q)]) & 0xC0) == 0x80) {
        --q;  // 跳过 continuation byte
    }
    return q;
}

int stepCharRight(const std::string& text, int pos) {
    const int n = static_cast<int>(text.size());
    if (pos >= n) return n;
    const unsigned char b = static_cast<unsigned char>(text[static_cast<size_t>(pos)]);
    int len = 1;
    if (b >= 0xF0 && b <= 0xF4) len = 4;
    else if (b >= 0xE0 && b <= 0xEF) len = 3;
    else if (b >= 0xC2 && b <= 0xDF) len = 2;
    return std::min(n, pos + len);
}

std::string editorText(HWND sci) {
    const int len = textLength(sci);
    std::string out(static_cast<size_t>(len) + 1, '\0');
    send(sci, SCI_GETTEXT, out.size(), reinterpret_cast<LPARAM>(out.data()));
    out.resize(static_cast<size_t>(len));
    return out;
}

// (caret, anchor) 升序排序 + 完全相同去重（core::sortAndDedup 同语义）
void sortAndDedup(std::vector<core::SelRange>& list) {
    std::sort(list.begin(), list.end(), [](const core::SelRange& a, const core::SelRange& b) {
        if (a.caret != b.caret) return a.caret < b.caret;
        return a.anchor < b.anchor;
    });
    list.erase(std::unique(list.begin(), list.end(),
                           [](const core::SelRange& a, const core::SelRange& b) {
                               return a.caret == b.caret && a.anchor == b.anchor;
                           }),
               list.end());
}

// 矩形选区 → 普通多选区（定格；清 moveExtends，旧版 finishColumnSelect 语义）
void streamifySelection(HWND sci) {
    if (send(sci, SCI_SELECTIONISRECTANGLE, 0, 0)) {
        // 顺序关键：SCI_CANCEL 在非矩形态会 DropAdditionalRanges 塌缩多选区
        // （Editor.cxx case Cancel），故先在矩形态 CANCEL（只清 moveExtends），
        // 再经 CHANGESELECTIONMODE 转流式（SetSelectionMode(wParam,false) 不
        // 触碰 moveExtends——SETSELECTIONMODE 会把它重置为 true）
        send(sci, SCI_CANCEL, 0, 0);
        send(sci, SCI_CHANGESELECTIONMODE, SC_SEL_STREAM, 0);
    }
}

// 把主选区指到矩形活动角所在行（使 CHOOSECARETX 的 lastXChosen 取活动角 x）
void setMainToRectCaretLine(HWND sci) {
    const int caretPos = static_cast<int>(send(sci, SCI_GETRECTANGULARSELECTIONCARET, 0, 0));
    const int count = static_cast<int>(send(sci, SCI_GETSELECTIONS, 0, 0));
    for (int i = 0; i < count; ++i) {
        if (static_cast<int>(send(sci, SCI_GETSELECTIONNCARET, static_cast<WPARAM>(i), 0)) ==
            caretPos) {
            send(sci, SCI_SETMAINSELECTION, static_cast<WPARAM>(i), 0);
            return;
        }
    }
}

// —— 剪贴板（CF_UNICODETEXT 纯文本；不写 Scintilla 的列剪贴板标记）——

// OpenClipboard 带重试（被其他窗口短暂占用时按惯例重试；持续失败返回 false）
bool OpenClipboardRetry(HWND owner) {
    for (int i = 0; i < 20; ++i) {
        if (::OpenClipboard(owner)) return true;
        ::Sleep(10);
    }
    return false;
}

bool WriteClipboardText(HWND owner, const std::string& utf8) {
    if (!OpenClipboardRetry(owner)) return false;
    ::EmptyClipboard();
    const std::wstring wide = core::utf8ToWide(utf8);
    const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (mem) {
        if (wchar_t* ptr = static_cast<wchar_t*>(::GlobalLock(mem))) {
            memcpy(ptr, wide.c_str(), bytes);
            ::GlobalUnlock(mem);
            // 成功后所有权移交剪贴板（不可再释放）；失败时系统未接管，
            // 由本方释放防泄漏
            ok = ::SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        }
        if (!ok) ::GlobalFree(mem);
    }
    ::CloseClipboard();
    return ok;
}

std::string ReadClipboardText(HWND owner) {
    if (!OpenClipboardRetry(owner)) return {};
    std::string out;
    if (::IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE h = ::GetClipboardData(CF_UNICODETEXT);
        if (h) {
            if (const wchar_t* ptr = static_cast<const wchar_t*>(::GlobalLock(h))) {
                out = core::wideToUtf8(ptr);
                ::GlobalUnlock(h);
            }
        }
    }
    ::CloseClipboard();
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());  // CRLF 兼容
    return out;
}

std::vector<std::string> splitLinesKeepEmpty(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            lines.emplace_back(text, start, i - start);
            start = i + 1;
        }
    }
    if (lines.empty()) lines.emplace_back();  // 空串 → 一行空行（旧版 split KeepEmptyParts）
    return lines;
}

// 区间集合按 start 降序逐个替换（单批撤销由调用方包裹）；返回替换后的光标集
// （每选区落在 start + 文本长）
std::vector<core::SelRange> ReplaceRangesDescending(HWND sci,
                                                    const std::vector<std::pair<core::SelRange, std::string>>& edits) {
    auto ordered = edits;
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& a, const auto& b) {
                  return std::min(a.first.anchor, a.first.caret) >
                         std::min(b.first.anchor, b.first.caret);
              });
    send(sci, SCI_BEGINUNDOACTION, 0, 0);
    std::vector<core::SelRange> carets;
    carets.reserve(ordered.size());
    for (const auto& [range, text] : ordered) {
        const int start = std::min(range.anchor, range.caret);
        const int end = std::max(range.anchor, range.caret);
        send(sci, SCI_SETTARGETRANGE, static_cast<WPARAM>(start), static_cast<LPARAM>(end));
        send(sci, SCI_REPLACETARGET, text.size(),
             reinterpret_cast<LPARAM>(text.c_str()));
        carets.push_back(core::SelRange{start + static_cast<int>(text.size()),
                                        start + static_cast<int>(text.size())});
    }
    send(sci, SCI_ENDUNDOACTION, 0, 0);
    sortAndDedup(carets);
    return carets;
}

}  // namespace

// ---------------------------------------------------------------------------
// 共享工具
// ---------------------------------------------------------------------------

std::vector<core::SelRange> CollectSelections(HWND sci) {
    std::vector<core::SelRange> out;
    if (!sci) return out;
    const int count = static_cast<int>(send(sci, SCI_GETSELECTIONS, 0, 0));
    out.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        core::SelRange r;
        r.anchor = static_cast<int>(send(sci, SCI_GETSELECTIONNANCHOR, static_cast<WPARAM>(i), 0));
        r.caret = static_cast<int>(send(sci, SCI_GETSELECTIONNCARET, static_cast<WPARAM>(i), 0));
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const core::SelRange& a, const core::SelRange& b) {
        if (a.caret != b.caret) return a.caret < b.caret;
        return a.anchor < b.anchor;
    });
    return out;
}

void ApplySelections(HWND sci, const std::vector<core::SelRange>& selections) {
    if (!sci || selections.empty()) return;
    streamifySelection(sci);
    bool first = true;
    for (const core::SelRange& s : selections) {
        if (first) {
            send(sci, SCI_SETSELECTION, static_cast<WPARAM>(s.caret), static_cast<LPARAM>(s.anchor));
            first = false;
        } else {
            send(sci, SCI_ADDSELECTION, static_cast<WPARAM>(s.caret), static_cast<LPARAM>(s.anchor));
        }
    }
}

std::string SelectionTextForSearch(HWND sci) {
    if (!sci) return {};
    if (send(sci, SCI_GETSELECTIONS, 0, 0) != 1) return {};
    // SCI_GETSELTEXT 返回长度不含结尾 NUL（Editor.cxx GetSelText：Length()）
    const LRESULT len = send(sci, SCI_GETSELTEXT, 0, 0);
    if (len <= 0) return {};  // 无选区
    std::string out(static_cast<size_t>(len) + 1, '\0');
    send(sci, SCI_GETSELTEXT, 0, reinterpret_cast<LPARAM>(out.data()));
    out.resize(static_cast<size_t>(len));
    if (out.find('\n') != std::string::npos) return {};  // 多行选区不预填
    return out;
}

// ---------------------------------------------------------------------------
// Host：子类化与分发
// ---------------------------------------------------------------------------

MultiCursorHost::~MultiCursorHost() {
    detachAll();
}

std::string MultiCursorHost::clipboardRead(HWND owner) {
    if (readClipboard_) return readClipboard_(owner);
    return ReadClipboardText(owner);
}

bool MultiCursorHost::clipboardWrite(HWND owner, const std::string& utf8) {
    if (writeClipboard_) return writeClipboard_(owner, utf8);
    return WriteClipboardText(owner, utf8);
}

void MultiCursorHost::setClipboardAccessForTest(
    std::function<std::string(HWND)> read,
    std::function<bool(HWND, const std::string&)> write) {
    readClipboard_ = std::move(read);
    writeClipboard_ = std::move(write);
}

bool MultiCursorHost::owns(HWND sci) const {
    return std::find(windows_.begin(), windows_.end(), sci) != windows_.end();
}

namespace {

MultiCursorHost* g_host = nullptr;  // 单实例（组装层唯一持有）

LRESULT CALLBACK McSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    const bool altKey = (::GetKeyState(VK_MENU) & 0x8000) != 0;
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (g_host && g_host->handleMouseButtonDown(hwnd, static_cast<UINT>(wp),
                                                    POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) {
            return 0;  // Alt+单击加光标：消费（Scintilla 不见此消息）
        }
        break;
    case WM_LBUTTONUP:
        if (g_host) g_host->handleMouseButtonUp(hwnd);
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        if (!g_host) break;
        const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (g_host->handleKeyDown(hwnd, static_cast<UINT>(wp), ctrl, shift, altKey)) {
            return 1;  // 已消费：不透传（亦阻止后续 WM_CHAR）
        }
        break;
    }
    case WM_NCDESTROY:
        if (g_host) g_host->detachWindow(hwnd);  // 防御：窗口销毁时撤销子类化
        break;
    default:
        break;
    }
    return ::DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

void MultiCursorHost::attachWindow(HWND sci) {
    if (!sci || owns(sci)) return;
    g_host = this;  // 单实例假设（组装层唯一）
    if (::SetWindowSubclass(sci, McSubclassProc, kSubclassId, 0)) {
        windows_.push_back(sci);
    }
}

void MultiCursorHost::detachWindow(HWND sci) {
    const auto it = std::find(windows_.begin(), windows_.end(), sci);
    if (it == windows_.end()) return;
    windows_.erase(it);
    if (::IsWindow(sci)) {
        ::RemoveWindowSubclass(sci, McSubclassProc, kSubclassId);
    }
}

void MultiCursorHost::detachAll() {
    while (!windows_.empty()) {
        detachWindow(windows_.front());
    }
    g_host = nullptr;
}

bool MultiCursorHost::active(HWND sci) {
    return sci && send(sci, SCI_GETSELECTIONS, 0, 0) > 1;
}

void MultiCursorHost::collapseIfActive(HWND sci) {
    if (!active(sci)) return;
    const std::vector<core::SelRange> selections = CollectSelections(sci);
    const core::SelRange keep = core::multicursorCollapse(selections);
    send(sci, SCI_SETSELECTION, static_cast<WPARAM>(keep.caret),
         static_cast<LPARAM>(keep.anchor));
    if (onSelectionsRebuilt) onSelectionsRebuilt(sci);
}

// ---------------------------------------------------------------------------
// 鼠标
// ---------------------------------------------------------------------------

bool MultiCursorHost::handleMouseButtonDown(HWND sci, UINT wParamFlags, POINT pt) {
    if (!sci) return false;
    const bool alt = (wParamFlags & MK_ALT_) != 0;
    const bool shift = (wParamFlags & MK_SHIFT) != 0;
    if (!alt || shift) {
        // Alt+Shift（矩形列选拖拽）与普通单击（多光标自动收缩）均放行原生
        return false;
    }
    // Alt+单击：点击处加光标（保持现有选区；位置重复时去重无操作）
    ::SetFocus(sci);
    int pos = static_cast<int>(send(sci, SCI_POSITIONFROMPOINTCLOSE,
                                    static_cast<WPARAM>(pt.x), static_cast<LPARAM>(pt.y)));
    if (pos < 0) {
        // 远离文本（如末行下方空白）：用非严格命中并钳制（旧版 qBound 语义）
        pos = static_cast<int>(send(sci, SCI_POSITIONFROMPOINT,
                                    static_cast<WPARAM>(pt.x), static_cast<LPARAM>(pt.y)));
        pos = std::clamp(pos, 0, textLength(sci));
    }
    std::vector<core::SelRange> selections = CollectSelections(sci);
    selections.push_back(core::SelRange{pos, pos});
    sortAndDedup(selections);
    ApplySelections(sci, selections);
    if (onSelectionsRebuilt) onSelectionsRebuilt(sci);
    return true;
}

void MultiCursorHost::handleMouseButtonUp(HWND sci) {
    if (!sci) return;
    // 列选拖拽结束（或任何以矩形选区收尾的按下）：定格为普通多选区——
    // 否则随后 Enter/普通方向键会走原生矩形分支 DropAdditionalRanges 塌缩
    if (send(sci, SCI_SELECTIONISRECTANGLE, 0, 0)) {
        streamifySelection(sci);
        if (onSelectionsRebuilt) onSelectionsRebuilt(sci);
    }
}

// ---------------------------------------------------------------------------
// 键盘
// ---------------------------------------------------------------------------

namespace {

// Ctrl+Alt+Shift+←/→：字符步进不跨行（B4；原生 *RectExtend 会跨行）
void keyboardColumnStepHorizontal(HWND sci, UINT vk) {
    const std::string text = editorText(sci);
    const bool rect = send(sci, SCI_SELECTIONISRECTANGLE, 0, 0) != 0;
    int pos;
    if (!rect) {
        // 首次进入列选（经 ←/→）：锚 = 游标 = 当前主光标，再行步进
        pos = static_cast<int>(send(sci, SCI_GETCURRENTPOS, 0, 0));
        send(sci, SCI_SETRECTANGULARSELECTIONANCHOR, static_cast<WPARAM>(pos), 0);
        send(sci, SCI_SETRECTANGULARSELECTIONCARET, static_cast<WPARAM>(pos), 0);
    } else {
        pos = static_cast<int>(send(sci, SCI_GETRECTANGULARSELECTIONCARET, 0, 0));
    }
    const int line = static_cast<int>(send(sci, SCI_LINEFROMPOSITION, static_cast<WPARAM>(pos), 0));
    const int lineStart =
        static_cast<int>(send(sci, SCI_POSITIONFROMLINE, static_cast<WPARAM>(line), 0));
    const int lineEnd =
        static_cast<int>(send(sci, SCI_GETLINEENDPOSITION, static_cast<WPARAM>(line), 0));
    int next = pos;
    if (vk == VK_LEFT) {
        if (pos > lineStart) next = stepCharLeft(text, pos);  // 行首 Left：无操作
    } else {
        if (pos < lineEnd) next = stepCharRight(text, pos);   // 行末 Right：无操作
    }
    if (next == pos) return;
    send(sci, SCI_SETRECTANGULARSELECTIONCARET, static_cast<WPARAM>(next), 0);
    // 主选区指到活动角 → CHOOSECARETX 更新的期望 x 取活动角（粘性正确传递
    // 给后续原生 SCI_LINE*RECTEXTEND 的 ↑/↓）
    setMainToRectCaretLine(sci);
    send(sci, SCI_CHOOSECARETX, 0, 0);
    // 两角滚动可见（矩形选区 SET 消息不自滚动）
    const int anchorPos =
        static_cast<int>(send(sci, SCI_GETRECTANGULARSELECTIONANCHOR, 0, 0));
    send(sci, SCI_SCROLLRANGE, static_cast<WPARAM>(anchorPos), static_cast<LPARAM>(next));
}

// 多光标态收到 Enter/方向/Home/End/删除等编辑导航键：若当前是矩形选区先转
// 普通多选区（原生这些命令的矩形分支会 DropAdditionalRanges 塌缩）
bool streamifyBeforeEditKey(HWND sci, UINT vk) {
    switch (vk) {
    case VK_RETURN: case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
    case VK_HOME: case VK_END: case VK_BACK: case VK_DELETE: case VK_TAB:
        streamifySelection(sci);
        return true;
    default:
        return false;
    }
}

}  // namespace

bool MultiCursorHost::handleKeyDown(HWND sci, UINT vk, bool ctrl, bool shift, bool alt) {
    if (!sci) return false;

    const bool multi = send(sci, SCI_GETSELECTIONS, 0, 0) > 1;

    // Ctrl+Alt+Shift+方向：键盘列选
    if (ctrl && alt && shift &&
        (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT)) {
        if (vk == VK_UP) {
            send(sci, SCI_LINEUPRECTEXTEND, 0, 0);  // 原生：lastXChosen 粘性 + 逐行展开
        } else if (vk == VK_DOWN) {
            send(sci, SCI_LINEDOWNRECTEXTEND, 0, 0);
        } else {
            keyboardColumnStepHorizontal(sci, vk);
        }
        if (onSelectionsRebuilt) onSelectionsRebuilt(sci);
        return true;
    }

    // Ctrl+Alt+↑/↓：上/下方加光标
    if (ctrl && alt && !shift && (vk == VK_UP || vk == VK_DOWN)) {
        const std::string text = editorText(sci);
        std::vector<core::SelRange> selections = CollectSelections(sci);
        std::vector<core::SelRange> next =
            core::multicursorAddLine(text, selections, vk == VK_UP);
        sortAndDedup(next);
        ApplySelections(sci, next);
        if (onSelectionsRebuilt) onSelectionsRebuilt(sci);
        return true;
    }

    // Shift+Alt+I：选中行行尾插光标（单行无多行光标不接管——旧版回滚语义）
    if (alt && shift && !ctrl && (vk == 'I' || vk == 'i')) {
        const std::string text = editorText(sci);
        std::vector<core::SelRange> selections = CollectSelections(sci);
        std::vector<core::SelRange> next;
        for (const core::SelRange& s : selections) {
            const std::vector<core::SelRange> ends =
                core::multicursorLineEnds(text, s.anchor, s.caret);
            for (const core::SelRange& e : ends) next.push_back(e);
        }
        sortAndDedup(next);
        if (next.size() > 1) {  // 单行：回滚（保持单光标，不接管）
            ApplySelections(sci, next);
            if (onSelectionsRebuilt) onSelectionsRebuilt(sci);
        }
        return true;
    }

    // Ctrl+D：依次选中相同词
    if (ctrl && !alt && !shift && (vk == 'D' || vk == 'd')) {
        const std::string text = editorText(sci);
        std::vector<core::SelRange> selections = CollectSelections(sci);
        bool anyNonEmpty = false;
        for (const core::SelRange& s : selections) {
            if (s.anchor != s.caret) { anyNonEmpty = true; break; }
        }
        if (!anyNonEmpty) {
            // 无选区：每光标各自选词（无词处保持原光标；全部无词无操作）
            std::vector<core::SelRange> next;
            next.reserve(selections.size());
            bool changed = false;
            for (const core::SelRange& s : selections) {
                if (const auto word = core::wordRangeAt(text, s.caret)) {
                    next.push_back(*word);
                    changed = true;
                } else {
                    next.push_back(s);
                }
            }
            if (changed) {
                sortAndDedup(next);
                ApplySelections(sci, next);
                if (onSelectionsRebuilt) onSelectionsRebuilt(sci);
            }
        } else if (const auto next =
                       core::multicursorNextOccurrence(text, selections)) {
            std::vector<core::SelRange> all = selections;
            all.push_back(*next);
            sortAndDedup(all);
            ApplySelections(sci, all);
            if (onSelectionsRebuilt) onSelectionsRebuilt(sci);
        }
        return true;  // 无可追加：无操作（仍消费按键）
    }

    // Ctrl+Shift+L：选中全部相同词
    if (ctrl && shift && !alt && (vk == 'L' || vk == 'l')) {
        const std::string text = editorText(sci);
        std::vector<core::SelRange> next =
            core::multicursorAllOccurrences(text, CollectSelections(sci));
        ApplySelections(sci, next);
        if (onSelectionsRebuilt) onSelectionsRebuilt(sci);
        return true;
    }

    // Esc：退出多光标（保留按位置最末光标；原生 Esc 无绑定，无需让位）
    if (vk == VK_ESCAPE && multi) {
        const core::SelRange keep = core::multicursorCollapse(CollectSelections(sci));
        send(sci, SCI_SETSELECTION, static_cast<WPARAM>(keep.caret),
             static_cast<LPARAM>(keep.anchor));
        if (onSelectionsRebuilt) onSelectionsRebuilt(sci);
        return true;
    }

    // 多选区剪贴板（Ctrl+C/X/V）：自定义（排序拼接/列标记规避/分发判定）
    if (ctrl && !alt && !shift && multi) {
        if (vk == 'C' || vk == 'c') {
            std::vector<core::SelRange> selections = CollectSelections(sci);
            bool anyNonEmpty = false;
            for (const core::SelRange& s : selections) {
                if (s.anchor != s.caret) { anyNonEmpty = true; break; }
            }
            if (anyNonEmpty) {  // 无任何选区：不写剪贴板（旧版语义）
                const std::string text = editorText(sci);
                std::wstring joined;
                for (size_t i = 0; i < selections.size(); ++i) {
                    const core::SelRange& s = selections[i];
                    if (i) joined += L"\n";
                    joined += core::utf8ToWide(text.substr(
                        static_cast<size_t>(std::min(s.anchor, s.caret)),
                        static_cast<size_t>(std::abs(s.caret - s.anchor))));
                }
                clipboardWrite(sci, core::wideToUtf8(joined));
            }
            return true;
        }
        if (vk == 'X' || vk == 'x') {
            std::vector<core::SelRange> selections = CollectSelections(sci);
            std::vector<std::pair<core::SelRange, std::string>> edits;
            for (const core::SelRange& s : selections) {
                if (s.anchor != s.caret) edits.emplace_back(s, std::string());
            }
            if (!edits.empty()) {
                // 复制内容 = 各选区文本（含空选区的空行）——先读后删
                const std::string text = editorText(sci);
                std::wstring joined;
                for (size_t i = 0; i < selections.size(); ++i) {
                    const core::SelRange& s = selections[i];
                    if (i) joined += L"\n";
                    joined += core::utf8ToWide(text.substr(
                        static_cast<size_t>(std::min(s.anchor, s.caret)),
                        static_cast<size_t>(std::abs(s.caret - s.anchor))));
                }
                clipboardWrite(sci, core::wideToUtf8(joined));
                ApplySelections(sci, ReplaceRangesDescending(sci, edits));
                if (onSelectionsRebuilt) onSelectionsRebuilt(sci);
            }
            return true;
        }
        if (vk == 'V' || vk == 'v') {
            testPaste(sci);
            return true;
        }
    }

    // 多光标（或列选）激活时的编辑/导航键：矩形选区先转普通多选区再放行
    // 原生（Enter/方向键的原生矩形分支会塌缩选区；B4 列选后继续编辑语义）
    if ((multi || send(sci, SCI_SELECTIONISRECTANGLE, 0, 0)) &&
        !alt && streamifyBeforeEditKey(sci, vk)) {
        return false;  // 已按需转流式：交原生逐光标执行（不消费）
    }

    return false;  // 其余：原生处理（Ctrl+A 全选天然塌缩等，见映射表）
}

void MultiCursorHost::testPaste(HWND sci) {
    if (!sci) return;
    std::vector<core::SelRange> selections = CollectSelections(sci);
    if (selections.empty()) return;
    const std::string clip = clipboardRead(sci);
    const std::vector<std::string> lines = splitLinesKeepEmpty(clip);
    std::vector<std::pair<core::SelRange, std::string>> edits;
    if (core::multicursorPasteDistributes(static_cast<int>(lines.size()),
                                          static_cast<int>(selections.size()))) {
        // 行数 == 光标数：第 i 行分发到第 i 光标（集合恒升序）
        for (size_t i = 0; i < selections.size() && i < lines.size(); ++i) {
            edits.emplace_back(selections[i], lines[i]);
        }
    } else {
        for (const core::SelRange& s : selections) {
            edits.emplace_back(s, clip);  // 每光标整份
        }
    }
    ApplySelections(sci, ReplaceRangesDescending(sci, edits));
    if (onSelectionsRebuilt) onSelectionsRebuilt(sci);
}

}  // namespace editor
