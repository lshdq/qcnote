// Scintilla 编辑器组件实现。消息常量/通知结构见 vendor/scintilla/include/Scintilla.h。
#include "editor/scintillaedit.h"

#include <commctrl.h>   // SetWindowSubclass/DefSubclassProc/RemoveWindowSubclass
#include <windowsx.h>   // GET_X_LPARAM/GET_Y_LPARAM（WM_NCHITTEST 屏幕坐标分解）

#include <algorithm>

#include "Scintilla.h"
#include "editor/multicursorcontroller.h"  // kVirtualSpaceOptions（C-1 共享常量）
#include "core/textutil.h"
#include "ui/theme.h"
#include "ui/titlebar.h"

namespace editor {

namespace {

// 编辑器子窗口边缘命中透传子类过程（F6）。子窗口横跨客户区全宽，窗口左/
// 右/底边缘的 WM_NCHITTEST 先命中子窗口返回 HTCLIENT，主窗口的命中计算
//（ui::ComputeCaptionHitTest）收不到，导致无 resize 光标、无法拖宽窗口。
// 处理：命中换算到父窗口客户区后调 ComputeCaptionHitTest，若落在 8 向缩放
// 命中区则返回 HTTRANSPARENT（Windows 会把该命中消息落到下层/父窗口，由父
// 窗口处理 resize 光标与拖拽）；其余一律走默认处理，不影响文本交互。
LRESULT CALLBACK EdgeHitSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                     UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    switch (msg) {
    case WM_NCDESTROY:
        // 窗口销毁时撤销本子类化（防悬挂）；继续默认流程走原销毁路径
        ::RemoveWindowSubclass(hwnd, EdgeHitSubclassProc, kEdgeHitSubclassId);
        break;
    case WM_NCHITTEST: {
        const HWND parent = ::GetParent(hwnd);
        if (!parent) break;  // 无父窗口（异常态）：走默认处理
        // lParam 为屏幕坐标（多显示器可能为负，GET_X/Y_LPARAM 做符号扩展）
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        POINT client = pt;
        if (!::ScreenToClient(parent, &client)) break;
        RECT rc{};
        if (!::GetClientRect(parent, &rc)) break;
        const LRESULT hit = ui::ComputeCaptionHitTest(
            client, SIZE{rc.right - rc.left, rc.bottom - rc.top},
            ui::WindowDpi(parent), ::IsZoomed(parent) != FALSE);
        switch (hit) {
        case HTLEFT:
        case HTRIGHT:
        case HTTOP:
        case HTBOTTOM:
        case HTTOPLEFT:
        case HTTOPRIGHT:
        case HTBOTTOMLEFT:
        case HTBOTTOMRIGHT:
            return HTTRANSPARENT;  // 缩放命中区：透传给父窗口处理
        default:
            break;  // HTCLIENT 等：Scintilla 正常自处理
        }
        break;
    }
    default:
        break;
    }
    return ::DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace

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
    HWND sci = CreateWindowExW(0, L"Scintilla", nullptr, WS_CHILD, 0, 0, 0, 0,
                               parent_, nullptr, GetModuleHandleW(nullptr),
                               nullptr);
    if (sci) {
        // F6：子类化拦截 WM_NCHITTEST 做边缘缩放透传（过程见文件头匿名
        // namespace）；与组装层 onWindowCreated 挂的多光标子类化并存
        //（后挂者先收消息，两者对彼此关心的消息均不拦截冲突）。
        ::SetWindowSubclass(sci, EdgeHitSubclassProc, kEdgeHitSubclassId, 0);
    }
    return sci;
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
    // 虚拟空间仅矩形选区启用（SCVS_RECTANGULARSELECTION）：SCVS_NONE 时
    // SetRectangularRange 对每行 ClearVirtualSpace（Editor.cxx:598-599），
    // 空行/短行的列选终点被钳回行首；启用后矩形拖拽终点跟随鼠标 x（虚拟列），
    // 键盘列选（Ctrl+Alt+Shift+↑/↓ 的 lastXChosen 粘性）同样受益；松手
    // streamify 定格后虚拟列保留、键入自动补空格（VS Code 同款，方案 A
    // 用户已确认，见决策记录 DEC-005）。应用层集合重建路径（Esc 收缩、
    // Ctrl+Alt+↑/↓、Ctrl+D、Shift+Alt+I、粘贴分发）读基础位置、丢虚拟列
    //（与旧版钳制语义一致，接受，不改）。stream 选区仍无虚拟空间（与
    // 旧版普通选区语义一致）。
    // C-1：引用 editor 共享常量（与 handleKeyDown 瞬态恢复点同一唯一口径，
    // 防恢复值漂移；语义见上方注释与 DEC-003）
    send(SCI_SETVIRTUALSPACEOPTIONS, kVirtualSpaceOptions);
    // 撤销时还原多选区（B4：多光标编辑单步回滚后光标集合不丢；
    // 默认 Disabled——undo 后塌缩为单光标，偏离旧版控制器保留光标集合语义）
    send(SCI_SETUNDOSELECTIONHISTORY, 1 /* UndoSelectionHistoryOption::Enabled */);
}

// 主题相关配色（浅/深两套取值均来自 CurrentPalette；构造期与 retheme 共用）：
// 默认样式字色/底色、选区实底、光标色、查找高亮 indicator 8/9。STYLECLEARALL
// 把默认样式传播到全样式槽（本编辑器无词法着色，仅默认样式在用）。
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
    // 选区（F3）：不透明实底（VS Code 同款 ADD6FF/264F78）。有意不设置
    // SCI_SETSELFORE——选区内文字保持原样式字色（「原字色」语义；未设置
    // SelectionText 元素时 Scintilla 的 SelectionForeground 返回空、绘制用
    // 原样式字色，EditView.cxx DrawForeground 的 selectionFore 空值分支）。
    send(SCI_SETSELBACK, TRUE,
         static_cast<LPARAM>(ui::ToColorRef(pal.selectionBackground)));
    // 多光标附加选区（B4）：与主选区同色（SCI_SETADDITIONALSELBACK 色值在
    // wParam，Editor.cxx Message::SetAdditionalSelBack）
    send(SCI_SETADDITIONALSELBACK,
         static_cast<WPARAM>(ui::ToColorRef(pal.selectionBackground)));
    // 选区绘制层锁定文字层之下（SC_LAYER_BASE，选区作为文字背景）。为什么
    // 不再用 SCI_SETSELALPHA(SC_ALPHA_OPAQUE)（F3 修复决策，详见决策记录
    // DEC）：源码链 Editor.cxx Message::SetSelAlpha——alpha≠SC_ALPHA_NOALPHA
    // 时 selection.layer 被联动为 SC_LAYER_OVER_TEXT（文字层之上），选区在
    // 文字之后绘制（EditView.cxx DrawPhase::selectionTranslucent），alpha=255
    // 时 SurfaceGDI 纯色填充（IsOpaque→ExtTextOut ETO_OPAQUE），实底选区整块
    // 盖住文字——正是「选中后看不见文字」的根因。Base 层下选区背景在文字
    // 之前绘制，文字保持可见。
    send(SCI_SETSELECTIONLAYER, SC_LAYER_BASE);
    send(SCI_SETCARETFORE, static_cast<LPARAM>(ui::ToColorRef(pal.accent)));
    // 失焦非活动选区（F3）：比活动选区淡一档——在内容底色上按 ~1/3 强度
    //（overlay alpha 0x55）预合成。为什么必须预合成：Base 层绘制路径把选区
    // 背景色强制 Opaque（EditView.cxx TextBackground），元素 alpha 不参与
    // 混合，「淡」只能进色值本身。SCI_SETELEMENTCOLOUR 的 lParam 是 Scintilla
    // 内部 ColourRGBA 位布局 0xAABBGGRR（Geometry.h：R 最低字节、A 最高字
    // 节），低 24 位与 GDI COLORREF 同序，故传「0xFF<<24 | COLORREF」；alpha
    // 传非 0xFF 也不报错但该值不生效，且 alpha=0 不表示重置（重置走
    // SCI_RESETELEMENTCOLOUR）而等于全透明，传 0xFF 保证确定生效。
    const uint32_t inactiveBack = ui::BlendOver(
        pal.contentBackground,
        0x55000000u | (pal.selectionBackground & 0xFFFFFFu));
    send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_INACTIVE_BACK,
         static_cast<LPARAM>((static_cast<uint32_t>(0xFFu) << 24) |
                             ui::ToColorRef(inactiveBack)));
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
    // 选区端点缓存重新基线（F3b）：装载后端点已重置，避免下次 UPDATEUI 把
    // 陈旧端点差值误判为「选区变化」触发多余刷新
    e.lastSelStart = SendMessageW(e.hwnd, SCI_GETSELECTIONSTART, 0, 0);
    e.lastSelEnd = SendMessageW(e.hwnd, SCI_GETSELECTIONEND, 0, 0);
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
    // 无条件隐藏非活动编辑器，不能用 IsWindowVisible 做守卫：它返回含祖先
    // 链的有效可见性，启动流程主窗口隐藏期间（先恢复会话再显示）恒为
    // FALSE，SW_HIDE 被跳过后多个编辑器残留 WS_VISIBLE 样式位，主窗口
    // 显示时全部重叠、非活动者盖住活动者（切换一次标签才恢复）。
    // SW_HIDE 对已隐藏窗口是 no-op，无条件调用无副作用
    for (Entry& e : entries_) {
        if (e.hwnd && e.doc != doc) {
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
        // SCN_UPDATEUI 为空闲排队（QueueIdleWork），交互下延迟一帧，此处即时。
        // 刷新统一经 setCursorPosition 触发的 metadata 链（OnDocumentMetadata-
        // Changed → RefreshStatusBar/RefreshWindowTitle）：键入必推进光标 →
        // 恰好一次状态栏采样；不再直连 onCursorChanged（每次通知双采样的根因，
        // F3b）。选区拖拽每帧 SCN_UPDATEUI 经同一条链，未变即零采样
        if (e->doc) {
            const int pos = static_cast<int>(
                SendMessageW(e->hwnd, SCI_GETCURRENTPOS, 0, 0));
            e->doc->setCursorPosition(pos);
        }
        return true;
    }
    if (hdr->code == SCN_UPDATEUI) {
        // 光标/选区/滚动变化：回写 doc 光标（会话数据源；Document 同值不通知）。
        // 刷新只走 metadata 链——位置未变（如纯滚动）时零采样；位置变化恰好
        // 一次采样（此前本分支无条件 onCursorChanged + metadata 链 → 每帧双
        // 采样，F3b 根因）。
        // 保底：位置不变而主选区端点变化时 metadata 链不触发（selectedChars
        // 会滞留），仅此场景直连一次 onCursorChanged 补采样，不与链路叠加
        if (e->doc) {
            const int pos = static_cast<int>(
                SendMessageW(e->hwnd, SCI_GETCURRENTPOS, 0, 0));
            const bool changed = e->doc->setCursorPosition(pos);
            const LRESULT selStart =
                SendMessageW(e->hwnd, SCI_GETSELECTIONSTART, 0, 0);
            const LRESULT selEnd = SendMessageW(e->hwnd, SCI_GETSELECTIONEND, 0, 0);
            const bool selChanged =
                selStart != e->lastSelStart || selEnd != e->lastSelEnd;
            e->lastSelStart = selStart;
            e->lastSelEnd = selEnd;
            if (!changed && selChanged && onCursorChanged) {
                onCursorChanged(e->doc);
            }
        }
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
    // 总字符数走 Document 惰性缓存（F3a）：全文档 O(N) 扫描只在内容变更后的
    // 首次采样发生，逐帧状态刷新零扫描
    st.totalChars = static_cast<long long>(e->doc->charCount());
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
