// Scintilla 编辑器组件实现。消息常量/通知结构见 vendor/scintilla/include/Scintilla.h。
#include "editor/scintillaedit.h"

#include <algorithm>

#include "Scintilla.h"
#include "core/textutil.h"
#include "ui/theme.h"

namespace editor {

EditorHost::~EditorHost() {
    detach();
}

void EditorHost::attach(HWND parent) {
    parent_ = parent;
}

void EditorHost::detach() {
    for (Entry& e : entries_) {
        if (e.hwnd && IsWindow(e.hwnd)) {
            if (onWindowDestroyed) onWindowDestroyed(e.hwnd);
            DestroyWindow(e.hwnd);
        }
    }
    entries_.clear();
    active_ = nullptr;
    applying_ = nullptr;
    parent_ = nullptr;
}

EditorHost::Entry* EditorHost::find(core::Document* doc) {
    for (Entry& e : entries_) {
        if (e.doc == doc) return &e;
    }
    return nullptr;
}

EditorHost::Entry* EditorHost::find(HWND hwnd) {
    for (Entry& e : entries_) {
        if (e.hwnd == hwnd) return &e;
    }
    return nullptr;
}

const EditorHost::Entry* EditorHost::find(const core::Document* doc) const {
    for (const Entry& e : entries_) {
        if (e.doc == doc) return &e;
    }
    return nullptr;
}
HWND EditorHost::createEditor() {
    // 子窗口：无 WS_VISIBLE（显隐由 setActiveDocument 控制）。
    // 垂直滚动条按内容自动出现（B3 竖向「内容超高高时常驻、短内容隐藏」即
    // 平台默认行为）；横向由 SCROLLWIDTHTRACKING + 换行模式联动（wrap 开时
    // Scintilla 自动隐藏横向滚动条）。
    return CreateWindowExW(0, L"Scintilla", nullptr, WS_CHILD, 0, 0, 0, 0, parent_,
                           nullptr, GetModuleHandleW(nullptr), nullptr);
}

void EditorHost::configure(HWND sci, UINT dpi) const {
    auto send = [sci](UINT msg, WPARAM w = 0, LPARAM l = 0) {
        return SendMessageW(sci, msg, w, l);
    };
    // 内容编码：UTF-8 直通（DEC-010）——文档内容与本编辑器字节流一致
    send(SCI_SETCODEPAGE, SC_CP_UTF8);
    // 默认样式：Consolas 11pt（pt→px 由 Scintilla 按 DpiForWindow 换算）；
    // 中文经 GDI font-linking 回退（技术默认 GDI，保持 ClearType 默认质量）
    send(SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<LPARAM>("Consolas"));
    send(SCI_STYLESETSIZE, STYLE_DEFAULT, editor_metrics::FontPt);
    send(SCI_SETFONTQUALITY, SC_EFF_QUALITY_DEFAULT);
    send(SCI_STYLESETFORE, STYLE_DEFAULT,
         static_cast<LPARAM>(ui::ToColorRef(ui::CurrentPalette().textPrimary)));
    send(SCI_STYLESETBACK, STYLE_DEFAULT,
         static_cast<LPARAM>(ui::ToColorRef(ui::CurrentPalette().contentBackground)));
    send(SCI_STYLECLEARALL);
    // 缩进/换行输入语义：内存内容契约 LF（Enter 经 SC_EOL_LF 插入 "\n"）
    send(SCI_SETTABWIDTH, editor_metrics::TabWidth);
    send(SCI_SETEOLMODE, SC_EOL_LF);
    // 纯文本编辑器：无行号边（去掉默认折叠边占位）
    send(SCI_SETMARGINWIDTHN, 0, 0);
    // 长行横向滚动：滚动宽跟踪（NoWrap 长行常驻横向滚动条的前提）
    send(SCI_SETSCROLLWIDTHTRACKING, TRUE);
    // 光标边缘留白（水平方向 SLOP；垂直保持默认策略）
    send(SCI_SETXCARETPOLICY, CARET_SLOP,
         ui::Scale(editor_metrics::CaretSlopX, dpi));
    // 主题色：选区/光标/indicator（retheme 时同样经 applyThemeColors 重设）
    applyThemeColors(sci);
    // 滚动条视觉样式类（E5：深色主题下 Win11 深色滚动条；浅色恢复默认）
    ui::ApplyDarkExplorerThemeClass(sci, ui::IsDarkTheme());
    // 只监听字符级增删（增量回写 Document）；savepoint/样式等其余通知不订阅
    send(SCI_SETMODEVENTMASK, SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT);
    // 右键弹出菜单关闭（对齐旧 QML 版无编辑区右键菜单；编辑命令走快捷键/菜单）
    send(SCI_USEPOPUP, SC_POPUP_NEVER);
    // 换行模式：全局当前值（旧版全局语义）
    send(SCI_SETWRAPMODE, wordWrap_ ? SC_WRAP_WORD : SC_WRAP_NONE);

    // —— 多选/多光标基础（M3-B，B4）——
    send(SCI_SETMULTIPLESELECTION, TRUE);
    // 关键：无此项时键入（FilterSelections→DropAdditionalRanges）会把多选区
    // 塌缩成主选区——B4「可打印字符/Enter/方向键在所有光标执行」依赖它；
    // IME 提交串（GCS_RESULTSTR→AddWString→InsertCharacter）走同一代码路径，
    // 因此提交分发到所有光标且单批撤销（UndoGroup 合并）也是原生获得
    send(SCI_SETADDITIONALSELECTIONTYPING, TRUE);
    send(SCI_SETADDITIONALCARETSBLINK, TRUE);
    send(SCI_SETADDITIONALCARETSVISIBLE, TRUE);
    // 粘贴模式选 EACH：剪贴板行数 != 光标数的「每光标整份」语义（B5/B4）；
    // 行数匹配的逐行分发由多光标控制器拦截 Ctrl+V 自实现（MULTIPASTE 在
    // Scintilla 中无逐行分发概念——EACH=整份×N、ONCE=仅在选区整体起点插
    // 一份并塌缩，两者都不等价分发语义，故拦截自实现 + EACH 兜底
    // Shift+Insert 等未拦截路径）
    send(SCI_SETMULTIPASTE, SC_MULTIPASTE_EACH);
    // 虚拟空间关闭（旧版无虚拟空间；矩形选区短行钳制到行末由此保证）
    send(SCI_SETVIRTUALSPACEOPTIONS, SCVS_NONE);
    // 撤销时还原多选区（B4：多光标编辑单步回滚后光标集合不丢；
    // 默认 Disabled——undo 后塌缩为单光标，偏离旧版控制器保留光标集合语义）
    send(SCI_SETUNDOSELECTIONHISTORY, 1 /* UndoSelectionHistoryOption::Enabled */);
}

// 主题相关配色（浅/深两套取值均来自 CurrentPalette；构造期与 retheme 共用）：
// 默认样式字色/底色、选区底色（token 半透明与内容底色预合成）、光标色、
// 查找高亮 indicator 8/9。STYLECLEARALL 把默认样式传播到全样式槽（本编辑器
// 无词法着色，仅默认样式在用）。
void EditorHost::applyThemeColors(HWND sci) {
    auto send = [sci](UINT msg, WPARAM w = 0, LPARAM l = 0) {
        return SendMessageW(sci, msg, w, l);
    };
    const ui::ThemePalette& pal = ui::CurrentPalette();
    send(SCI_STYLESETFORE, STYLE_DEFAULT,
         static_cast<LPARAM>(ui::ToColorRef(pal.textPrimary)));
    send(SCI_STYLESETBACK, STYLE_DEFAULT,
         static_cast<LPARAM>(ui::ToColorRef(pal.contentBackground)));
    send(SCI_STYLECLEARALL);
    // 选区（token 半透明与内容底色预合成）与光标（accent）
    send(SCI_SETSELBACK, TRUE, static_cast<LPARAM>(ui::ToColorRef(ui::BlendOver(
                                   pal.contentBackground, pal.selection))));
    send(SCI_SETSELALPHA, SC_ALPHA_OPAQUE);
    send(SCI_SETCARETFORE, static_cast<LPARAM>(ui::ToColorRef(pal.accent)));
    // 查找高亮 indicator（B5；序号含义见 scintillaedit.h 常量）：
    // 全部匹配 = 半透明橙黄底（searchMatch：RGB + alpha）；当前匹配 = 描边
    //（BOX + searchMatchBorder 色；形状为非颜色线索）
    send(SCI_INDICSETSTYLE, kIndicatorSearchMatch, INDIC_ROUNDBOX);
    send(SCI_INDICSETFORE, kIndicatorSearchMatch,
         static_cast<LPARAM>(ui::ToColorRef(pal.searchMatch)));
    send(SCI_INDICSETALPHA, kIndicatorSearchMatch,
         static_cast<LPARAM>(pal.searchMatch >> 24));
    send(SCI_INDICSETSTYLE, kIndicatorSearchCurrent, INDIC_BOX);
    send(SCI_INDICSETFORE, kIndicatorSearchCurrent,
         static_cast<LPARAM>(ui::ToColorRef(pal.searchMatchBorder)));
}

void EditorHost::retheme() {
    for (Entry& e : entries_) {
        if (!e.hwnd || !IsWindow(e.hwnd)) continue;
        applyThemeColors(e.hwnd);
        // 滚动条视觉样式类切换 + 通知系统重取主题 + 强制非客户区重绘
        ui::ApplyDarkExplorerThemeClass(e.hwnd, ui::IsDarkTheme());
        SendMessageW(e.hwnd, WM_SETTINGCHANGE, 0,
                     reinterpret_cast<LPARAM>(L"ImmersiveColorSet"));
        SetWindowPos(e.hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                         SWP_FRAMECHANGED);
        InvalidateRect(e.hwnd, nullptr, TRUE);
    }
}


void EditorHost::loadFromDocument(Entry& e) {
    if (!e.hwnd || !e.doc) return;
    e.loading = true;  // 程序化装载：SCN_MODIFIED 不回写（防自反馈）
    const std::string& text = e.doc->content();
    SendMessageW(e.hwnd, SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(e.hwnd, SCI_EMPTYUNDOBUFFER, 0, 0);  // 外部覆盖语义：撤销栈不跨装载保留
    // 光标取文档记录值并钳制（编码重载后 DocumentManager 已钳制；此处双保险）
    const int pos = std::min<int>(e.doc->cursorPosition(),
                                  static_cast<int>(text.size()));
    SendMessageW(e.hwnd, SCI_GOTOPOS, static_cast<WPARAM>(pos), 0);
    SendMessageW(e.hwnd, SCI_SETSAVEPOINT, 0, 0);  // 装载态 == 基线（干净）
    e.loading = false;
}

void EditorHost::addDocument(core::Document* doc) {
    if (!parent_ || !doc || find(doc)) return;
    HWND sci = createEditor();
    if (!sci) return;
    Entry e;
    e.doc = doc;
    e.hwnd = sci;
    configure(sci, ui::WindowDpi(parent_));
    entries_.push_back(e);
    if (onWindowCreated) onWindowCreated(sci);  // 组装层：多光标子类化等挂接
    // 装载当前内容（初始加载发生在 documentsChanged 后：doc 内容已就绪）
    loadFromDocument(entries_.back());
    // 就位到当前内容区（不可见实例同样维持几何，激活即正确显示）
    MoveWindow(sci, contentRect_.left, contentRect_.top,
               contentRect_.right - contentRect_.left,
               contentRect_.bottom - contentRect_.top, FALSE);
}

void EditorHost::removeDocument(core::Document* doc) {
    Entry* e = find(doc);
    if (!e) return;
    if (onWindowDestroyed && e->hwnd) onWindowDestroyed(e->hwnd);  // 先撤销子类化再销毁
    if (e->hwnd && IsWindow(e->hwnd)) {
        DestroyWindow(e->hwnd);
    }
    if (active_ == doc) active_ = nullptr;
    if (applying_ == doc) applying_ = nullptr;
    entries_.erase(entries_.begin() + (e - entries_.data()));
}

void EditorHost::syncToDocuments(const std::vector<core::Document*>& docs) {
    // 移除已不存在的（先收集，removeDocument 会移动 entries_）
    std::vector<core::Document*> gone;
    for (const Entry& e : entries_) {
        if (std::find(docs.begin(), docs.end(), e.doc) == docs.end()) {
            gone.push_back(e.doc);
        }
    }
    for (core::Document* d : gone) {
        removeDocument(d);
    }
    // 新增缺失的
    for (core::Document* d : docs) {
        addDocument(d);
    }
}

void EditorHost::setActiveDocument(core::Document* doc) {
    if (active_ == doc) {
        return;  // 幂等（含双双为空）
    }
    active_ = doc;
    for (Entry& e : entries_) {
        if (e.hwnd && e.doc != doc && IsWindowVisible(e.hwnd)) {
            ShowWindow(e.hwnd, SW_HIDE);
        }
    }
    Entry* e = find(doc);
    if (e && e->hwnd) {
        ShowWindow(e->hwnd, SW_SHOW);
        SetFocus(e->hwnd);  // 旧 QML isActive → forceActiveFocus 同语义
    } else if (parent_) {
        SetFocus(parent_);  // 无文档：焦点回主窗口（欢迎页键盘）
    }
}

void EditorHost::reloadFromDocument(core::Document* doc) {
    Entry* e = find(doc);
    if (!e) return;
    loadFromDocument(*e);
}

void EditorHost::markSaved(core::Document* doc) {
    Entry* e = find(doc);
    if (e && e->hwnd) {
        SendMessageW(e->hwnd, SCI_SETSAVEPOINT, 0, 0);
    }
}

void EditorHost::setWordWrap(bool wrap) {
    if (wordWrap_ == wrap) return;
    wordWrap_ = wrap;
    for (Entry& e : entries_) {
        if (e.hwnd) {
            SendMessageW(e.hwnd, SCI_SETWRAPMODE, wrap ? SC_WRAP_WORD : SC_WRAP_NONE, 0);
        }
    }
}

void EditorHost::layout(const RECT& contentRect) {
    contentRect_ = contentRect;
    const int w = contentRect.right - contentRect.left;
    const int h = contentRect.bottom - contentRect.top;
    for (Entry& e : entries_) {
        if (e.hwnd) {
            MoveWindow(e.hwnd, contentRect.left, contentRect.top, w, h, TRUE);
        }
    }
}

void EditorHost::focusActive() {
    Entry* e = find(active_);
    if (e && e->hwnd) {
        SetFocus(e->hwnd);
    }
}

HWND EditorHost::activeWindow() const {
    const Entry* e = find(active_);
    return e ? e->hwnd : nullptr;
}

HWND EditorHost::windowForTest(const core::Document* doc) const {
    const Entry* e = find(doc);
    return e ? e->hwnd : nullptr;
}

bool EditorHost::handleNotify(const NMHDR* hdr) {
    if (!hdr) return false;
    Entry* e = find(hdr->hwndFrom);
    if (!e) return false;

    if (hdr->code == SCN_MODIFIED) {
        const auto* sn = reinterpret_cast<const SCNotification*>(hdr);
        if (e->loading || !e->doc) return true;  // 程序化装载：忽略
        const int type = sn->modificationType;
        if ((type & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) == 0) return true;
        // 增量回写：insert 用通知 text/length；delete 按 position+length。
        // Document 仅提供整串 setContent——以旧内容为底做局部拼接（O(n) 拷贝，
        // 笔记级文档可接受；>10MB 性能优化明确不做）。
        // 注意：SCN_MODIFIED 在选区推进之前发出（InsertString 内部通知），
        // 此处不得采样光标（读到旧值）——光标/状态栏刷新经 SCN_CHARADDED /
        // SCN_UPDATEUI 路径（见下）
        std::string next = e->doc->content();
        const int pos = static_cast<int>(sn->position);
        if (pos < 0 || pos > static_cast<int>(next.size())) return true;  // 防御
        if (type & SC_MOD_INSERTTEXT) {
            const char* text = sn->text ? sn->text : "";
            const int len = static_cast<int>(sn->length);
            next.insert(static_cast<size_t>(pos), text,
                        static_cast<size_t>(std::min<int>(
                            len, static_cast<int>(strlen(text)))));
        } else {  // SC_MOD_DELETETEXT
            next.erase(static_cast<size_t>(pos),
                       static_cast<size_t>(std::min<long long>(
                           sn->length, static_cast<long long>(next.size()) - pos)));
        }
        applying_ = e->doc;  // 防回灌标志（组装层 documentContentChanged 跳过回灌）
        e->doc->setContent(std::move(next));
        applying_ = nullptr;
        if (onUserEdited) onUserEdited(e->doc);  // 内容事件（M4 草稿防抖挂点）
        return true;
    }
    if (hdr->code == SCN_CHARADDED) {
        // 键入路径的即时状态刷新：InsertCharacter 末尾发出（选区已推进）——
        // SCN_UPDATEUI 为空闲排队（QueueIdleWork），交互下延迟一帧，此处即时
        if (e->doc) {
            const int pos = static_cast<int>(
                SendMessageW(e->hwnd, SCI_GETCURRENTPOS, 0, 0));
            e->doc->setCursorPosition(pos);
        }
        if (onCursorChanged) onCursorChanged(e->doc);
        return true;
    }
    if (hdr->code == SCN_UPDATEUI) {
        // 光标/选区/滚动变化：回写 doc 光标（会话数据源；Document 同值不通知）
        if (e->doc) {
            const int pos = static_cast<int>(
                SendMessageW(e->hwnd, SCI_GETCURRENTPOS, 0, 0));
            e->doc->setCursorPosition(pos);
        }
        if (onCursorChanged) onCursorChanged(e->doc);
        return true;
    }
    return true;  // 本组件实例的其他通知：吞掉（不向组装层扩散）
}

EditorStatus EditorHost::statusOf(const core::Document* doc) const {
    EditorStatus st;
    const Entry* e = find(doc);
    if (!e || !e->hwnd || !e->doc) return st;
    const auto send = [hwnd = e->hwnd](UINT msg, WPARAM w = 0, LPARAM l = 0) {
        return SendMessageW(hwnd, msg, w, l);
    };
    const LRESULT pos = send(SCI_GETCURRENTPOS);
    const LRESULT length = send(SCI_GETLENGTH);
    st.line = static_cast<int>(send(SCI_LINEFROMPOSITION, static_cast<WPARAM>(pos))) + 1;
    // 列 = 行内字符数 + 1（字符列，中文 1 列；与旧 QML cursorColumn 同口径）
    const LRESULT lineStart = send(SCI_POSITIONFROMLINE,
                                   static_cast<WPARAM>(send(SCI_LINEFROMPOSITION,
                                                            static_cast<WPARAM>(pos))));
    const LRESULT from = std::clamp<LRESULT>(lineStart, 0, length);
    const LRESULT to = std::clamp<LRESULT>(pos, from, length);
    const std::string& content = e->doc->content();
    st.column = static_cast<int>(
                    core::utf8CharCount(std::string_view(content).substr(
                        static_cast<size_t>(from), static_cast<size_t>(to - from))) +
                    1);
    st.totalChars = static_cast<long long>(core::utf8CharCount(content));
    // 选区字符数（B4 多选区：各选区合计；矩形选区取逐行展开的各段合计；
    // SCI_COUNTCHARACTERS 按 UTF-8 码点计，中文 1 字符）
    const LRESULT selections = send(SCI_GETSELECTIONS);
    for (int i = 0; i < selections; ++i) {
        const LRESULT sStart = send(SCI_GETSELECTIONNSTART, static_cast<WPARAM>(i));
        const LRESULT sEnd = send(SCI_GETSELECTIONNEND, static_cast<WPARAM>(i));
        if (sEnd > sStart) {
            st.selectedChars += static_cast<long long>(
                send(SCI_COUNTCHARACTERS, static_cast<WPARAM>(sStart),
                     static_cast<LPARAM>(sEnd)));
        }
    }
    st.cursorCount = static_cast<int>(selections);  // B4：状态栏「光标 ×N」
    return st;
}

}  // namespace editor
