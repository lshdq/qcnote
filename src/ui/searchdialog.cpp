// 查找替换对话框实现。窗口骨架（WS_POPUP + CS_SAVEBITS/CS_DROPSHADOW +
// Win11 圆角 + 模态循环 + 把手拖动 + 边界约束）与 ui/dialog.cpp 同模式；
// 输入框为原生 EDIT 子控件（换肤见文件头注释）。
#include "ui/searchdialog.h"

#include <windowsx.h>

#include <dwmapi.h>

#include <algorithm>

#include "ui/theme.h"

namespace ui {

namespace {
constexpr wchar_t kSearchDialogClassName[] = L"QcnoteSearchDialogWindow";
constexpr int kFindEditId = 100;
constexpr int kReplaceEditId = 101;
constexpr UINT_PTR kEditSubclassId = 0x51431102;  // 'QC1\2'

// 读取 EDIT 全文（无固定上限）：WM_GETTEXTLENGTH 先量长再动态取，超长输入
//（粘贴大段文本）不截断
std::wstring EditTextContent(HWND edit) {
    const int len = static_cast<int>(SendMessageW(edit, WM_GETTEXTLENGTH, 0, 0));
    if (len <= 0) return {};
    std::wstring text(static_cast<std::size_t>(len) + 1, L'\0');
    const int copied = static_cast<int>(SendMessageW(edit, WM_GETTEXT,
                                                     static_cast<WPARAM>(len + 1),
                                                     reinterpret_cast<LPARAM>(text.data())));
    text.resize(static_cast<std::size_t>(copied < 0 ? 0 : copied));
    return text;
}
}  // namespace

// 前置声明（定义见窗口过程一节；open() 安装子类化时使用；ui 命名空间——
// 与类内 friend 声明同一实体）
LRESULT CALLBACK SearchEditSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                        UINT_PTR id, DWORD_PTR ref);

namespace {
bool RegisterSearchDialogClass() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_SAVEBITS | CS_DROPSHADOW;
    wc.lpfnWndProc = &SearchDialog::DialogWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kSearchDialogClassName;
    registered = RegisterClassExW(&wc) != 0;
    return registered;
}
}  // namespace

std::wstring BuildSearchStatusText(const SearchDialogState& state, int lastReplacedCount) {
    if (!state.errorMessage.empty()) return state.errorMessage;
    if (lastReplacedCount >= 0) {
        return L"已替换 " + std::to_wstring(lastReplacedCount) + L" 处";
    }
    if (state.matchCount == 0) return L"无匹配";
    if (state.overflowed) return L">10000";
    if (state.currentIndex < 0) {
        return L"共 " + std::to_wstring(state.matchCount) + L" 个";
    }
    return L"第 " + std::to_wstring(state.currentIndex + 1) + L" 个 / 共 " +
           std::to_wstring(state.matchCount) + L" 个";
}

SearchDialog::~SearchDialog() {
    if (editBkBrush_) DeleteObject(editBkBrush_);
    if (titleFont_) DeleteObject(titleFont_);
    if (bodyFont_) DeleteObject(bodyFont_);
}

// ---------------------------------------------------------------------------
// 打开 / 模态循环
// ---------------------------------------------------------------------------

void SearchDialog::open(HWND owner, bool focusReplace) {
    if (active_ || !owner || !IsWindow(owner) || !client_ || !RegisterSearchDialogClass()) {
        return;
    }
    active_ = true;
    done_ = false;
    dpi_ = WindowDpi(owner);
    lastReplacedCount_ = -1;
    hasJumped_ = false;  // 重开 = 全新会话（旧版 onAboutToShow 复位）
    hoverItem_ = pressedItem_ = -1;
    focusVisible_ = false;

    titleFont_ = CreateFontW(-Scale(searchdialog_metrics::TitlePixelSize, dpi_), 0, 0, 0,
                             FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    bodyFont_ = CreateFontW(-Scale(searchdialog_metrics::BodyPixelSize, dpi_), 0, 0, 0,
                            FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    editBkBrush_ = CreateSolidBrush(ToColorRef(CurrentPalette().contentBackground));

    buildLayout();

    // 默认位置：顶部对齐 y≈96（逻辑），水平居中（DEC-002，屏幕坐标）
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    POINT pos{};
    pos.x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width_) / 2;
    pos.y = ownerRect.top + Scale(searchdialog_metrics::TopAlignY, dpi_);
    const RECT initial = clampDialogRect({pos.x, pos.y, pos.x + width_, pos.y + height_});

    EnableWindow(owner, FALSE);  // 模态：宿主禁用（编辑器/Tab/菜单不可达）
    owner_ = owner;
    hwnd_ = CreateWindowExW(0, kSearchDialogClassName, L"qcnote", WS_POPUP,
                            initial.left, initial.top, width_, height_, owner_, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) {
        // 创建失败：回滚宿主使能并释放已建字体/画刷（与 close 清理路径一致）
        EnableWindow(owner, TRUE);
        if (titleFont_) { DeleteObject(titleFont_); titleFont_ = nullptr; }
        if (bodyFont_) { DeleteObject(bodyFont_); bodyFont_ = nullptr; }
        if (editBkBrush_) { DeleteObject(editBkBrush_); editBkBrush_ = nullptr; }
        owner_ = nullptr;
        active_ = false;
        return;
    }
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    DWORD corner = DWMWCP_ROUNDSMALL;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    SetImmersiveDark(hwnd_, IsDarkTheme());  // E5：深色主题下弹出层 DWM 表面随动

    // 输入框（原生 EDIT；1px 内缩让父窗口外圈描边可见）
    const auto createEdit = [&](int id, const RECT& r) {
        HWND edit = CreateWindowExW(0, L"EDIT", nullptr,
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                    r.left + 1, r.top + 1, r.right - r.left - 2,
                                    r.bottom - r.top - 2, hwnd_,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                    GetModuleHandleW(nullptr), nullptr);
        // E5：深色主题给 EDIT 挂 DarkMode_Explorer 视觉样式类——光标/系统绘
        // 制元素随暗色（文字/底色仍由 WM_CTLCOLOREDIT 控制；浅色恢复默认类）
        if (edit) ApplyDarkExplorerThemeClass(edit, IsDarkTheme());
        return edit;
    };
    findEdit_ = createEdit(kFindEditId, findEditRect_);
    replaceEdit_ = createEdit(kReplaceEditId, replaceEditRect_);
    SendMessageW(findEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
    SendMessageW(replaceEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);

    syncInputsFromClient();
    refreshStatusText();

    // EDIT 子类化（dwRefData = this；键拦截见 SearchEditSubclassProc）
    SetWindowSubclass(findEdit_, SearchEditSubclassProc, kEditSubclassId,
                      reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(replaceEdit_, SearchEditSubclassProc, kEditSubclassId,
                      reinterpret_cast<DWORD_PTR>(this));

    ShowWindow(hwnd_, SW_SHOW);
    setFocusItem(focusReplace ? 1 : 0, false);
    if (focusReplace) {
        // Ctrl+H：聚焦替换框（不全选——旧版 focusReplaceField）
    } else {
        SendMessageW(findEdit_, EM_SETSEL, 0, -1);  // Ctrl+F：全选便于直接重输
    }
    if (onOpened) onOpened();

    MSG msg{};
    while (!done_) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) {
            if (got == 0) PostQuitMessage(static_cast<int>(msg.wParam));  // WM_QUIT 透传
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (findEdit_) RemoveWindowSubclass(findEdit_, SearchEditSubclassProc, kEditSubclassId);
    if (replaceEdit_) {
        RemoveWindowSubclass(replaceEdit_, SearchEditSubclassProc, kEditSubclassId);
    }

    if (hwnd_ && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    findEdit_ = replaceEdit_ = nullptr;
    if (titleFont_) { DeleteObject(titleFont_); titleFont_ = nullptr; }
    if (bodyFont_) { DeleteObject(bodyFont_); bodyFont_ = nullptr; }
    if (editBkBrush_) { DeleteObject(editBkBrush_); editBkBrush_ = nullptr; }
    EnableWindow(owner_, TRUE);
    SetActiveWindow(owner_);
    owner_ = nullptr;
    active_ = false;
}

void SearchDialog::close() {
    if (dragging_) endDrag();
    done_ = true;
    if (client_) client_->OnClosed();  // DEC-009：清高亮 + 焦点回编辑器
}

void SearchDialog::endDrag() {
    if (dragging_ && hwnd_) ReleaseCapture();
    dragging_ = false;
}

RECT SearchDialog::clampDialogRect(RECT r) const {
    if (!owner_) return r;
    RECT ownerRect{};
    GetWindowRect(owner_, &ownerRect);
    const int keepW = Scale(searchdialog_metrics::KeepWidth, dpi_);
    const int headerH = header_.bottom - header_.top;
    if (r.left < ownerRect.left) r.left = ownerRect.left;
    if (r.top < ownerRect.top) r.top = ownerRect.top;
    if (r.left > ownerRect.right - keepW) {
        r.left = std::max<LONG>(ownerRect.left, ownerRect.right - keepW);
    }
    if (r.top > ownerRect.bottom - headerH) {
        r.top = std::max<LONG>(ownerRect.top, ownerRect.bottom - headerH);
    }
    r.right = r.left + width_;
    r.bottom = r.top + height_;
    return r;
}

// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------

void SearchDialog::buildLayout() {
    namespace m = searchdialog_metrics;
    const auto S = [dpi = dpi_](int v) { return Scale(v, dpi); };
    width_ = S(m::Width);
    const int padH = S(m::PaddingH);
    const int titleH = S(m::TitlePixelSize + 6);
    const int editH = S(m::EditHeight);

    header_ = {0, 0, width_, 1 + S(m::PaddingTop) + titleH};
    int y = header_.bottom + S(m::SectionSpacing);

    findLabel_ = {padH, y, padH + S(m::LabelWidth), y + editH};
    findEditRect_ = {padH + S(m::LabelWidth), y, width_ - padH, y + editH};
    y += editH + S(m::RowSpacing);
    replaceLabel_ = {padH, y, padH + S(m::LabelWidth), y + editH};
    replaceEditRect_ = {padH + S(m::LabelWidth), y, width_ - padH, y + editH};
    y += editH + S(m::SectionSpacing);

    // 复选行：左起排布
    int x = padH;
    const int box = S(m::CheckBoxBox);
    for (int i = 0; i < 3; ++i) {
        checkBoxes_[i] = {x, y + (editH - box) / 2, x + box, y + (editH - box) / 2 + box};
        x += box + S(6);
        // 文字宽度实量（按复选文本）
        static const wchar_t* kLabels[3] = {L"区分大小写", L"全字匹配", L"正则表达式"};
        if (HDC dc = GetDC(nullptr)) {
            const HGDIOBJ old = SelectObject(dc, bodyFont_ ? bodyFont_ : GetStockObject(SYSTEM_FONT));
            SIZE sz{};
            if (GetTextExtentPoint32W(dc, kLabels[i], lstrlenW(kLabels[i]), &sz)) {
                x += sz.cx;
            }
            SelectObject(dc, old);
            ReleaseDC(nullptr, dc);
        }
        x += S(m::CheckBoxSpacing);
    }
    y += editH + S(m::RowSpacing);
    statusRect_ = {padH, y, width_ - padH, y + S(m::StatusHeight)};
    y += S(m::StatusHeight) + S(m::SectionSpacing);

    // 按钮行：右对齐，从右往左
    static const wchar_t* kButtonLabels[m::ButtonCount] = {
        L"查找下一个", L"上一个", L"替换", L"全部替换", L"关闭"};
    const int btnH = S(m::ButtonHeight);
    int xRight = width_ - padH;
    for (int i = m::ButtonCount - 1; i >= 0; --i) {
        int textW = 0;
        if (HDC dc = GetDC(nullptr)) {
            const HGDIOBJ old = SelectObject(dc, bodyFont_ ? bodyFont_ : GetStockObject(SYSTEM_FONT));
            SIZE sz{};
            if (GetTextExtentPoint32W(dc, kButtonLabels[i], lstrlenW(kButtonLabels[i]), &sz)) {
                textW = sz.cx;
            }
            SelectObject(dc, old);
            ReleaseDC(nullptr, dc);
        }
        const int btnW = std::max(S(m::ButtonMinWidth), textW + S(24));
        buttons_[i] = {xRight - btnW, y, xRight, y + btnH};
        xRight -= btnW + S(m::ButtonSpacing);
    }
    y += btnH + S(m::ButtonsToBottom);
    height_ = y + 1;
}

void SearchDialog::syncInputsFromClient() {
    if (!client_) return;
    SetWindowTextW(findEdit_, client_->GetPattern().c_str());
    SetWindowTextW(replaceEdit_, client_->GetReplacement().c_str());
}

// ---------------------------------------------------------------------------
// 状态与动作
// ---------------------------------------------------------------------------

bool SearchDialog::buttonEnabled(int idx) const {
    namespace m = searchdialog_metrics;
    if (idx == m::ButtonClose) return true;
    return client_ && client_->GetState().matchCount > 0;  // 无匹配（含无效正则）禁用
}

bool SearchDialog::checkBoxEnabled(int idx) const {
    // 正则开启时「全字匹配」禁用置灰（需求选项表）；其余恒可用
    return !(idx == searchdialog_metrics::CheckWholeWord && client_ &&
             client_->GetOption(searchdialog_metrics::CheckRegex));
}

void SearchDialog::refreshStatusText() {
    if (!client_) return;
    const SearchDialogState st = client_->GetState();
    if (st.generation != lastGeneration_) {
        lastGeneration_ = st.generation;
        hasJumped_ = false;  // 全新会话（pattern/选项/文档重绑，旧版各重置源）
    }
    statusText_ = BuildSearchStatusText(st, lastReplacedCount_);
    if (hwnd_ && IsWindow(hwnd_)) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SearchDialog::refocusFind() {
    if (!hwnd_) return;
    setFocusItem(0, false);
    if (findEdit_) SendMessageW(findEdit_, EM_SETSEL, 0, -1);  // 全选便于直接重输
}

void SearchDialog::refocusReplace() {
    if (!hwnd_) return;
    setFocusItem(1, false);
}

void SearchDialog::refreshState() {
    if (!active_) return;
    refreshStatusText();
}

void SearchDialog::onFindEdited() {
    if (!client_) return;
    const std::wstring pattern = EditTextContent(findEdit_);
    lastReplacedCount_ = -1;             // 状态行不再显示「已替换」（旧版语义）
    client_->SetPattern(pattern);        // 触发重算 + 高亮（onChanged → refreshState）
    refreshStatusText();
}

void SearchDialog::onReplaceEdited() {
    if (!client_) return;
    client_->SetReplacement(EditTextContent(replaceEdit_));
}

void SearchDialog::toggleCheckBox(int idx) {
    if (!client_ || !checkBoxEnabled(idx)) return;
    lastReplacedCount_ = -1;
    client_->SetOption(idx, !client_->GetOption(idx));  // 同值早退在控制器
    refreshStatusText();
}

void SearchDialog::activateButton(int idx) {
    namespace m = searchdialog_metrics;
    if (!client_ || idx < 0 || idx >= m::ButtonCount) return;
    switch (idx) {
    case m::ButtonFindNext: doFindNext(); break;
    case m::ButtonFindPrev: doFindPrev(); break;
    case m::ButtonReplace:
        lastReplacedCount_ = -1;
        if (client_->ReplaceCurrent()) refreshStatusText();
        break;
    case m::ButtonReplaceAll:
        lastReplacedCount_ = client_->ReplaceAll();
        hasJumped_ = true;  // 旧版 doReplaceAll 语义
        refreshStatusText();
        break;
    case m::ButtonClose: close(); break;
    default: break;
    }
}

void SearchDialog::doFindNext() {
    if (!client_) return;
    lastReplacedCount_ = -1;
    const SearchDialogState st = client_->GetState();
    if (st.generation != lastGeneration_) {
        lastGeneration_ = st.generation;
        hasJumped_ = false;
    }
    if (st.matchCount == 0) return;
    // 全新会话首跳：先定位当前匹配（不推进）——除非原生选区已在当前匹配上
    if (!hasJumped_ && !client_->SelectionIsCurrentMatch()) {
        client_->SelectCurrent();
    } else {
        client_->FindNext();
    }
    hasJumped_ = true;
    refreshStatusText();
}

void SearchDialog::doFindPrev() {
    if (!client_) return;
    lastReplacedCount_ = -1;
    const SearchDialogState st = client_->GetState();
    if (st.generation != lastGeneration_) {
        lastGeneration_ = st.generation;
        hasJumped_ = false;
    }
    if (st.matchCount == 0) return;
    if (!hasJumped_ && !client_->SelectionIsCurrentMatch()) {
        client_->SelectCurrent();
    } else {
        client_->FindPrev();
    }
    hasJumped_ = true;
    refreshStatusText();
}

// ---------------------------------------------------------------------------
// 焦点流转
// ---------------------------------------------------------------------------

void SearchDialog::setFocusItem(int idx, bool showVisual) {
    focusItem_ = idx;
    focusVisible_ = showVisual;
    namespace m = searchdialog_metrics;
    if (idx == 0 && findEdit_) {
        SetFocus(findEdit_);
    } else if (idx == 1 && replaceEdit_) {
        SetFocus(replaceEdit_);
    } else if (hwnd_) {
        SetFocus(hwnd_);  // 复选/按钮为虚拟焦点（对话框窗口持有键盘焦点）
    }
    if (hwnd_ && IsWindow(hwnd_)) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SearchDialog::moveFocus(int delta) {
    // 0/1 输入框，2..4 复选，5..9 按钮；循环
    int next = focusItem_ + delta;
    if (next < 0) next = searchdialog_metrics::ButtonCount + 4;  // 9
    if (next > 9) next = 0;
    setFocusItem(next, true);
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------

// EDIT 子类化（ui 命名空间：类 friend 声明可见）：Enter/Shift+Enter（下一
// 个/上一个）、Esc（关闭）、F3（DEC-013 转发）、Tab（父级焦点循环）、
// Ctrl+F/Ctrl+H（对话框已打开时重复按下 → 聚焦查找框并全选 / 聚焦替换框，
// 旧版 openSearchDialog 的 visible 分支语义）——其余按键（含 IME/编辑）透传
LRESULT CALLBACK SearchEditSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                        UINT_PTR /*id*/, DWORD_PTR ref) {
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        auto* self = reinterpret_cast<SearchDialog*>(ref);
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (!ctrl) {
            switch (wp) {
            case VK_RETURN:
                if (self) shift ? self->doFindPrev() : self->doFindNext();
                return 1;
            case VK_ESCAPE:
                if (self) self->close();
                return 1;
            case VK_F3:
                if (self) shift ? self->doFindPrev() : self->doFindNext();
                return 1;
            case VK_TAB:
                if (self) self->moveFocus(shift ? -1 : 1);
                return 1;
            default:
                break;
            }
        } else if (wp == 'F' || wp == 'f') {
            if (self) self->refocusFind();  // Ctrl+F：聚焦查找框并全选
            return 1;
        } else if (wp == 'H' || wp == 'h') {
            if (self) self->refocusReplace();  // Ctrl+H：聚焦替换框
            return 1;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK SearchDialog::DialogWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<SearchDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->proc(hwnd, msg, wp, lp);
}

LRESULT SearchDialog::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    namespace m = searchdialog_metrics;
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint();
        return 0;
    case WM_CTLCOLOREDIT: {
        // EDIT 换肤：背景/文字色（主题）；边框/焦点环由父窗口 WM_PAINT 自绘。
        // 兜底 WHITE_BRUSH 仅在 open() 的画刷创建失败（极罕见 OOM）时可达——
        // 正常生命周期内 editBkBrush_ 先于 EDIT 子窗口创建、后于其销毁
        const HDC dc = reinterpret_cast<HDC>(wp);
        SetBkColor(dc, ToColorRef(CurrentPalette().contentBackground));
        SetTextColor(dc, ToColorRef(CurrentPalette().textPrimary));
        if (editBkBrush_) return reinterpret_cast<LRESULT>(editBkBrush_);
        return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
    }
    case WM_COMMAND: {
        const int code = HIWORD(wp);
        const int id = LOWORD(wp);
        if (code == EN_CHANGE) {
            if (id == kFindEditId) onFindEdited();
            else if (id == kReplaceEditId) onReplaceEdited();
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (dragging_) {
            POINT cursor{};
            GetCursorPos(&cursor);
            const RECT next = clampDialogRect(
                {cursor.x - dragOrigin_.x, cursor.y - dragOrigin_.y, 0, 0});
            SetWindowPos(hwnd_, nullptr, static_cast<int>(next.left),
                         static_cast<int>(next.top), 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        int hover = -1;  // 0..2 复选、5..9 按钮（与 focusItem 同轴：+5）
        for (int i = 0; i < 3; ++i) {
            if (PtInRect(&checkBoxes_[i], pt) && checkBoxEnabled(i)) hover = i;
        }
        for (int i = 0; i < m::ButtonCount; ++i) {
            if (PtInRect(&buttons_[i], pt) && buttonEnabled(i)) hover = i + 5;
        }
        if (hover != hoverItem_) {
            hoverItem_ = hover;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        for (int i = 0; i < 3; ++i) {
            if (PtInRect(&checkBoxes_[i], pt)) {
                if (checkBoxEnabled(i)) {
                    toggleCheckBox(i);
                    setFocusItem(i + 2, false);  // 鼠标操作隐藏键盘焦点环
                }
                return 0;
            }
        }
        for (int i = 0; i < m::ButtonCount; ++i) {
            if (PtInRect(&buttons_[i], pt)) {
                if (buttonEnabled(i)) {
                    pressedItem_ = i + 5;
                    SetCapture(hwnd_);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            }
        }
        if (PtInRect(&header_, pt)) {  // E4 拖动把手
            POINT cursor{};
            GetCursorPos(&cursor);
            RECT wr{};
            GetWindowRect(hwnd_, &wr);
            dragOrigin_ = {cursor.x - wr.left, cursor.y - wr.top};
            dragging_ = true;
            SetCapture(hwnd_);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (dragging_) {
            endDrag();
            return 0;
        }
        if (pressedItem_ >= 5) {
            const int idx = pressedItem_ - 5;
            pressedItem_ = -1;
            ReleaseCapture();
            if (PtInRect(&buttons_[idx], pt) && buttonEnabled(idx)) {
                activateButton(idx);
            } else {
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (dragging_) dragging_ = false;
        if (pressedItem_ >= 5) pressedItem_ = -1;
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const UINT vk = static_cast<UINT>(wp);
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (vk == VK_ESCAPE) {
            close();
        } else if (vk == VK_RETURN) {
            if (focusItem_ >= 5) activateButton(focusItem_ - 5);
            else activateButton(m::ButtonFindNext);  // 输入框/复选上 Enter = 默认按钮
        } else if (vk == VK_SPACE) {
            if (focusItem_ >= 2 && focusItem_ <= 4) toggleCheckBox(focusItem_ - 2);
            else if (focusItem_ >= 5) activateButton(focusItem_ - 5);
        } else if (vk == VK_TAB) {
            moveFocus(shift ? -1 : 1);
        } else if (vk == VK_F3) {
            shift ? doFindPrev() : doFindNext();  // DEC-013：打开态 F3 转发
        }
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_CLOSE:
        close();  // Alt+F4/系统关闭：Esc 语义
        return 0;
    case WM_ACTIVATEAPP:
        if (wp == 0 && dragging_) endDrag();
        return 0;
    case WM_DESTROY:
        done_ = true;  // 窗口销毁：无条件结束模态循环（防御）
        return 0;
    default:
        if (msg >= WM_APP && msg <= WM_APP + 9) return 0;  // 测试通道兼容
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ---------------------------------------------------------------------------
// 测试钩子
// ---------------------------------------------------------------------------

RECT SearchDialog::buttonRectForTest(int index) const {
    if (index < 0 || index >= searchdialog_metrics::ButtonCount) return {};
    return buttons_[index];
}

RECT SearchDialog::checkBoxRectForTest(int index) const {
    if (index < 0 || index >= 3) return {};
    return checkBoxes_[index];
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------

void SearchDialog::paint() {
    if (!hwnd_) return;
    namespace m = searchdialog_metrics;
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int w = rc.right, h = rc.bottom;
    const ThemePalette& pal = CurrentPalette();

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    const HGDIOBJ oldBmp = SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(ToColorRef(pal.contentBackground));
    RECT all{0, 0, w, h};
    FillRect(mem, &all, bg);
    DeleteObject(bg);

    SetBkMode(mem, TRANSPARENT);

    // 标题（把手文本）
    const HGDIOBJ oldFont = SelectObject(mem, titleFont_);
    SetTextColor(mem, ToColorRef(pal.textPrimary));
    RECT titleRect = header_;
    titleRect.left = Scale(m::PaddingH, dpi_);
    titleRect.right = w - Scale(m::PaddingH, dpi_);
    titleRect.top += Scale(m::PaddingTop, dpi_);
    DrawTextW(mem, L"查找与替换", -1, &titleRect,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

    SelectObject(mem, bodyFont_);
    const int focusRing = Scale(2, dpi_);

    // 输入框外圈：边框（border）/焦点环（accent）
    const auto drawEditFrame = [&](const RECT& r, bool focused) {
        HPEN pen = CreatePen(PS_SOLID, focused ? focusRing : 1,
                             ToColorRef(focused ? pal.accent : pal.border));
        const HGDIOBJ oldPen = SelectObject(mem, pen);
        const HGDIOBJ oldBrush = SelectObject(mem, GetStockObject(NULL_BRUSH));
        Rectangle(mem, r.left, r.top, r.right, r.bottom);
        SelectObject(mem, oldBrush);
        SelectObject(mem, oldPen);
        DeleteObject(pen);
    };
    drawEditFrame(findEditRect_, focusItem_ == 0);
    drawEditFrame(replaceEditRect_, focusItem_ == 1);

    // 行标签
    SetTextColor(mem, ToColorRef(pal.textSecondary));
    RECT l1 = findLabel_, l2 = replaceLabel_;
    DrawTextW(mem, L"查找：", -1, &l1, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    DrawTextW(mem, L"替换为：", -1, &l2, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);

    // 复选框（勾选图形 = 非颜色线索；禁用置灰）
    static const wchar_t* kCheckLabels[3] = {L"区分大小写", L"全字匹配", L"正则表达式"};
    for (int i = 0; i < 3; ++i) {
        const bool enabled = checkBoxEnabled(i);
        const bool checked = client_ && client_->GetOption(i);
        const RECT& box = checkBoxes_[i];
        HPEN pen = CreatePen(PS_SOLID, 1,
                             ToColorRef(enabled ? pal.border : pal.textDisabled));
        HBRUSH boxBrush = CreateSolidBrush(ToColorRef(pal.contentBackground));
        const HGDIOBJ oldPen = SelectObject(mem, pen);
        const HGDIOBJ oldBrush = SelectObject(mem, boxBrush);
        Rectangle(mem, box.left, box.top, box.right, box.bottom);
        SelectObject(mem, oldBrush);
        SelectObject(mem, oldPen);
        DeleteObject(boxBrush);
        DeleteObject(pen);
        if (checked) {  // 对勾（两段线；形状即线索）
            HPEN cp = CreatePen(PS_SOLID, Scale(2, dpi_),
                                ToColorRef(enabled ? pal.accent : pal.textDisabled));
            const HGDIOBJ op = SelectObject(mem, cp);
            const int t = Scale(3, dpi_);
            const int midY = (box.top + box.bottom) / 2;
            const int x0 = box.left + t;
            const int x1 = box.left + (box.right - box.left) / 2;
            const int x2 = box.right - t;
            MoveToEx(mem, x0, midY, nullptr);
            LineTo(mem, x1, box.bottom - t);
            LineTo(mem, x2, box.top + t);
            SelectObject(mem, op);
            DeleteObject(cp);
        }
        SetTextColor(mem, ToColorRef(enabled ? pal.textPrimary : pal.textDisabled));
        RECT tr = {box.right + Scale(6, dpi_), box.top - Scale(1, dpi_),
                   box.right + Scale(200, dpi_), box.bottom + Scale(1, dpi_)};
        DrawTextW(mem, kCheckLabels[i], -1, &tr,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        // 复选键盘焦点环
        if (focusVisible_ && focusItem_ == i + 2) {
            HPEN pen = CreatePen(PS_SOLID, focusRing, ToColorRef(pal.focus));
            const HGDIOBJ op = SelectObject(mem, pen);
            const HGDIOBJ ob = SelectObject(mem, GetStockObject(NULL_BRUSH));
            Rectangle(mem, tr.left - Scale(4, dpi_), tr.top - Scale(2, dpi_),
                      tr.right - Scale(6, dpi_), tr.bottom + Scale(2, dpi_));
            SelectObject(mem, ob);
            SelectObject(mem, op);
            DeleteObject(pen);
        }
    }

    // 状态行（错误文本 error 色——颜色仅辅助，文本本身即状态）
    SetTextColor(mem, ToColorRef(
                          client_ && !client_->GetState().errorMessage.empty()
                              ? pal.error
                              : pal.textSecondary));
    RECT st = statusRect_;
    DrawTextW(mem, statusText_.c_str(), -1, &st,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

    // 按钮行
    static const wchar_t* kButtonLabels[m::ButtonCount] = {
        L"查找下一个", L"上一个", L"替换", L"全部替换", L"关闭"};
    const RECT& findNext = buttons_[m::ButtonFindNext];
    for (int i = 0; i < m::ButtonCount; ++i) {
        const RECT& b = buttons_[i];
        const bool enabled = buttonEnabled(i);
        const bool hovered = enabled && hoverItem_ == i + 5;
        const bool pressed = enabled && pressedItem_ == i + 5;
        const bool isDefault = (i == m::ButtonFindNext);
        uint32_t fill = pal.contentBackground;
        if (!enabled) {
            fill = pal.contentBackground;
        } else if (isDefault) {
            fill = pal.accent;
            if (pressed) fill = BlendOver(pal.accent, pal.pressed);
            else if (hovered) fill = BlendOver(pal.accent, pal.hover);
        } else {
            if (pressed) fill = BlendOver(pal.contentBackground, pal.pressed);
            else if (hovered) fill = BlendOver(pal.contentBackground, pal.hover);
        }
        HBRUSH fb = CreateSolidBrush(ToColorRef(fill));
        FillRect(mem, &b, fb);
        DeleteObject(fb);
        if (!isDefault) {
            HPEN pen = CreatePen(PS_SOLID, 1,
                                 ToColorRef(enabled ? pal.border : pal.textDisabled));
            HBRUSH border = CreateSolidBrush(ToColorRef(enabled ? pal.border : pal.textDisabled));
            const HGDIOBJ oldPen = SelectObject(mem, pen);
            const HGDIOBJ oldBrush = SelectObject(mem, border);
            Rectangle(mem, b.left, b.top, b.right, b.bottom);
            SelectObject(mem, oldBrush);
            SelectObject(mem, oldPen);
            DeleteObject(pen);
            DeleteObject(border);
        }
        SetTextColor(mem, ToColorRef(!enabled        ? pal.textDisabled
                                     : isDefault     ? pal.onAccentForeground
                                                     : pal.textPrimary));
        RECT tr = b;
        DrawTextW(mem, kButtonLabels[i], -1, &tr,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        // 键盘焦点环
        if (focusVisible_ && focusItem_ == i + 5) {
            HPEN pen = CreatePen(PS_SOLID, focusRing,
                                 ToColorRef(isDefault ? pal.onAccentForeground : pal.focus));
            const HGDIOBJ op = SelectObject(mem, pen);
            const HGDIOBJ ob = SelectObject(mem, GetStockObject(NULL_BRUSH));
            Rectangle(mem, b.left + 2, b.top + 2, b.right - 2, b.bottom - 2);
            SelectObject(mem, ob);
            SelectObject(mem, op);
            DeleteObject(pen);
        }
    }
    SelectObject(mem, oldFont);

    // 1px 边框（投影/圆角由 DWM 提供）
    HBRUSH border = CreateSolidBrush(ToColorRef(pal.border));
    RECT top{0, 0, w, 1}, bottom{0, h - 1, w, h}, left{0, 0, 1, h}, right{w - 1, 0, w, h};
    FillRect(mem, &top, border);
    FillRect(mem, &bottom, border);
    FillRect(mem, &left, border);
    FillRect(mem, &right, border);
    DeleteObject(border);

    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
}

}  // namespace ui
