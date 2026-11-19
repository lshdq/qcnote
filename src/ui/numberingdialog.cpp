// 插入编号对话框实现。窗口骨架（WS_POPUP + CS_SAVEBITS/CS_DROPSHADOW +
// Win11 圆角 + 模态循环 + 把手拖动 + 边界约束 + DPI/双主题）与
// ui/searchdialog.cpp 同模式（DEC-003）；输入框为原生 EDIT 子控件（换肤
// 见头文件注释）。
#include "ui/numberingdialog.h"

#include <windowsx.h>

#include <dwmapi.h>

#include <algorithm>

#include "ui/theme.h"

namespace ui {

namespace {
constexpr wchar_t kNumberingDialogClassName[] = L"QcnoteNumberingDialogWindow";
constexpr int kStartEditId = 100;
constexpr int kStepEditId = 101;
constexpr UINT_PTR kEditSubclassId = 0x51431103;  // 'QC1\3'

// 进程内参数记忆（DEC-005）：确定成功后更新，取消不动；不落盘，重启回默认
NumberingParams s_remembered{1, 1, false};

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

// 合法十进制整数 = 可选前导 '-' + 至少一位数字（全数字；空串/仅负号/小数点/
// 字母/空白/前导 '+' 均非法）。溢出防护：幅值累积超过 10^18 即停止累积并置
// outOfRange（语法仍合法，由范围校验给出用户可读错误；10^18 < LLONG_MAX，
// 无符号溢出）。解析失败返回 false。
bool ParseIntegerText(const std::wstring& text, long long& out, bool& outOfRange) {
    outOfRange = false;
    size_t i = 0;
    bool negative = false;
    if (!text.empty() && text[0] == L'-') {
        negative = true;
        i = 1;
    }
    if (i >= text.size()) return false;  // 空串 / 仅 '-'
    constexpr unsigned long long kMagCap = 1000000000000000000ULL;  // 10^18
    unsigned long long mag = 0;
    for (; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c < L'0' || c > L'9') return false;
        if (!outOfRange) {
            mag = mag * 10 + static_cast<unsigned long long>(c - L'0');
            if (mag > kMagCap) {
                outOfRange = true;
                mag = kMagCap;  // 钳到界内值（负号还原不溢出；错误由范围校验给）
            }
        }
    }
    out = negative ? -static_cast<long long>(mag) : static_cast<long long>(mag);
    return true;
}
}  // namespace

// 前置声明（定义见窗口过程一节；open() 安装子类化时使用；ui 命名空间——
// 与类内 friend 声明同一实体）
LRESULT CALLBACK NumberingEditSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                           UINT_PTR id, DWORD_PTR ref);

namespace {
bool RegisterNumberingDialogClass() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_SAVEBITS | CS_DROPSHADOW;
    wc.lpfnWndProc = &NumberingDialog::DialogWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kNumberingDialogClassName;
    registered = RegisterClassExW(&wc) != 0;
    return registered;
}
}  // namespace

bool ParseNumberingParams(const std::wstring& startText, const std::wstring& stepText,
                          NumberingParams& out, std::wstring& errorText) {
    errorText.clear();
    long long start = 0, step = 0;
    bool startOverflow = false, stepOverflow = false;
    // 错误优先级：起始值（语法→范围）→ 增量（语法→非 0→范围）——与对话框
    //「焦点回首个非法输入框」的口径一致（起始值相关错误先于增量相关）
    if (!ParseIntegerText(startText, start, startOverflow)) {
        errorText = L"起始值必须是整数（仅数字与可选负号）";
        return false;
    }
    if (startOverflow || start > kStartAbsMax || start < -kStartAbsMax) {
        errorText = L"起始值超出范围（绝对值不能大于 10^15）";
        return false;
    }
    if (!ParseIntegerText(stepText, step, stepOverflow)) {
        errorText = L"增量必须是整数（仅数字与可选负号）";
        return false;
    }
    if (step == 0 && !stepOverflow) {
        errorText = L"增量不能为 0";
        return false;
    }
    if (stepOverflow || step > kStepAbsMax || step < -kStepAbsMax) {
        errorText = L"增量超出范围（绝对值不能大于 10^6）";
        return false;
    }
    out.start = start;
    out.step = step;
    return true;  // padZero 不在解析范围（保持调用方原值）
}

NumberingDialog::~NumberingDialog() {
    if (editBkBrush_) DeleteObject(editBkBrush_);
    if (titleFont_) DeleteObject(titleFont_);
    if (bodyFont_) DeleteObject(bodyFont_);
}

// ---------------------------------------------------------------------------
// 打开 / 模态循环
// ---------------------------------------------------------------------------

bool NumberingDialog::showModal(HWND owner, NumberingParams& params) {
    if (active_ || !owner || !IsWindow(owner) || !RegisterNumberingDialogClass()) {
        return false;
    }
    active_ = true;
    done_ = false;
    result_ = false;
    dpi_ = WindowDpi(owner);
    hoverItem_ = pressedItem_ = -1;
    focusVisible_ = false;
    statusText_.clear();
    statusIsError_ = false;
    padZero_ = s_remembered.padZero;  // DEC-005：预填上次确定值（初始 1/1/关）

    titleFont_ = CreateFontW(-Scale(numberingdialog_metrics::TitlePixelSize, dpi_), 0,
                             0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    bodyFont_ = CreateFontW(-Scale(numberingdialog_metrics::BodyPixelSize, dpi_), 0, 0,
                            0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    editBkBrush_ = CreateSolidBrush(ToColorRef(CurrentPalette().contentBackground));

    buildLayout();

    // 默认位置：宿主客户区居中（对话框惯例；DEC 决策无强制顶部对齐）
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    POINT pos{};
    pos.x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width_) / 2;
    pos.y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height_) / 2;
    const RECT initial = clampDialogRect({pos.x, pos.y, pos.x + width_, pos.y + height_});

    EnableWindow(owner, FALSE);  // 模态：宿主禁用（编辑器/Tab/菜单不可达）
    owner_ = owner;
    hwnd_ = CreateWindowExW(0, kNumberingDialogClassName, L"qcnote", WS_POPUP,
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
        return false;
    }
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    DWORD corner = DWMWCP_ROUNDSMALL;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    SetImmersiveDark(hwnd_, IsDarkTheme());  // 深色主题下弹出层 DWM 表面随动

    // 输入框（原生 EDIT；1px 内缩让父窗口外圈描边可见）
    const auto createEdit = [&](int id, const RECT& r) {
        HWND edit = CreateWindowExW(0, L"EDIT", nullptr,
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                    r.left + 1, r.top + 1, r.right - r.left - 2,
                                    r.bottom - r.top - 2, hwnd_,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                    GetModuleHandleW(nullptr), nullptr);
        // 深色主题给 EDIT 挂 DarkMode_Explorer 视觉样式类——光标/系统绘制
        // 元素随暗色（文字/底色仍由 WM_CTLCOLOREDIT 控制；浅色恢复默认类）
        if (edit) ApplyDarkExplorerThemeClass(edit, IsDarkTheme());
        return edit;
    };
    startEdit_ = createEdit(kStartEditId, startEditRect_);
    stepEdit_ = createEdit(kStepEditId, stepEditRect_);
    SendMessageW(startEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
    SendMessageW(stepEdit_, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
    SetWindowTextW(startEdit_, std::to_wstring(s_remembered.start).c_str());
    SetWindowTextW(stepEdit_, std::to_wstring(s_remembered.step).c_str());

    // EDIT 子类化（dwRefData = this；键拦截见 NumberingEditSubclassProc）
    SetWindowSubclass(startEdit_, NumberingEditSubclassProc, kEditSubclassId,
                      reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(stepEdit_, NumberingEditSubclassProc, kEditSubclassId,
                      reinterpret_cast<DWORD_PTR>(this));

    ShowWindow(hwnd_, SW_SHOW);
    setFocusItem(0, false);
    SendMessageW(startEdit_, EM_SETSEL, 0, -1);  // 全选便于直接重输
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
    if (startEdit_) RemoveWindowSubclass(startEdit_, NumberingEditSubclassProc, kEditSubclassId);
    if (stepEdit_) {
        RemoveWindowSubclass(stepEdit_, NumberingEditSubclassProc, kEditSubclassId);
    }

    if (hwnd_ && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    startEdit_ = stepEdit_ = nullptr;
    if (titleFont_) { DeleteObject(titleFont_); titleFont_ = nullptr; }
    if (bodyFont_) { DeleteObject(bodyFont_); bodyFont_ = nullptr; }
    if (editBkBrush_) { DeleteObject(editBkBrush_); editBkBrush_ = nullptr; }
    EnableWindow(owner_, TRUE);
    SetActiveWindow(owner_);
    owner_ = nullptr;
    active_ = false;
    if (result_) params = accepted_;  // 确定：写回用户值（取消不动调用方 params）
    return result_;
}

void NumberingDialog::close() {
    if (dragging_) endDrag();
    done_ = true;
}

void NumberingDialog::endDrag() {
    if (dragging_ && hwnd_) ReleaseCapture();
    dragging_ = false;
}

RECT NumberingDialog::clampDialogRect(RECT r) const {
    if (!owner_) return r;
    RECT ownerRect{};
    GetWindowRect(owner_, &ownerRect);
    const int keepW = Scale(numberingdialog_metrics::KeepWidth, dpi_);
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

void NumberingDialog::buildLayout() {
    namespace m = numberingdialog_metrics;
    const auto S = [dpi = dpi_](int v) { return Scale(v, dpi); };
    width_ = S(m::Width);
    const int padH = S(m::PaddingH);
    const int titleH = S(m::TitlePixelSize + 6);
    const int editH = S(m::EditHeight);

    header_ = {0, 0, width_, 1 + S(m::PaddingTop) + titleH};
    int y = header_.bottom + S(m::SectionSpacing);

    startLabel_ = {padH, y, padH + S(m::LabelWidth), y + editH};
    startEditRect_ = {padH + S(m::LabelWidth), y, width_ - padH, y + editH};
    y += editH + S(m::RowSpacing);
    stepLabel_ = {padH, y, padH + S(m::LabelWidth), y + editH};
    stepEditRect_ = {padH + S(m::LabelWidth), y, width_ - padH, y + editH};
    y += editH + S(m::SectionSpacing);

    // 复选行（单复选框，左起）
    const int box = S(m::CheckBoxBox);
    checkBox_ = {padH, y + (editH - box) / 2, padH + box, y + (editH - box) / 2 + box};
    y += editH + S(m::RowSpacing);
    statusRect_ = {padH, y, width_ - padH, y + S(m::StatusHeight)};
    y += S(m::StatusHeight) + S(m::SectionSpacing);

    // 按钮行：右对齐，从右往左（取消在最右、确定在其左）
    static const wchar_t* kButtonLabels[m::ButtonCount] = {L"确定", L"取消"};
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

// ---------------------------------------------------------------------------
// 动作
// ---------------------------------------------------------------------------

void NumberingDialog::accept() {
    NumberingParams candidate{};
    candidate.padZero = padZero_;
    std::wstring error;
    if (!ParseNumberingParams(EditTextContent(startEdit_), EditTextContent(stepEdit_),
                              candidate, error)) {
        statusText_ = error;
        statusIsError_ = true;
        // 焦点回首个非法输入框：起始值非法（语法/范围）回起始框，否则增量框
        //（与 ParseNumberingParams 错误优先级一致）
        long long v = 0;
        bool overflow = false;
        const std::wstring startText = EditTextContent(startEdit_);
        const bool startBad = !ParseIntegerText(startText, v, overflow) || overflow ||
                              v > kStartAbsMax || v < -kStartAbsMax;
        setFocusItem(startBad ? 0 : 1, false);
        if (hwnd_ && IsWindow(hwnd_)) InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    s_remembered = candidate;  // DEC-005：确定成功才记忆（取消不记忆中间态）
    accepted_ = candidate;     // showModal 返回前写回调用方 params
    result_ = true;
    close();
}

void NumberingDialog::toggleCheckBox() {
    padZero_ = !padZero_;
    if (hwnd_ && IsWindow(hwnd_)) InvalidateRect(hwnd_, nullptr, FALSE);
}

void NumberingDialog::activateButton(int idx) {
    namespace m = numberingdialog_metrics;
    if (idx < 0 || idx >= m::ButtonCount) return;
    if (idx == m::ButtonOk) {
        accept();
    } else {
        close();  // 取消：result_ 保持 false（params 不写回）
    }
}

// ---------------------------------------------------------------------------
// 焦点流转
// ---------------------------------------------------------------------------

void NumberingDialog::setFocusItem(int idx, bool showVisual) {
    focusItem_ = idx;
    focusVisible_ = showVisual;
    if (idx == 0 && startEdit_) {
        SetFocus(startEdit_);
    } else if (idx == 1 && stepEdit_) {
        SetFocus(stepEdit_);
    } else if (hwnd_) {
        SetFocus(hwnd_);  // 复选/按钮为虚拟焦点（对话框窗口持有键盘焦点）
    }
    if (hwnd_ && IsWindow(hwnd_)) InvalidateRect(hwnd_, nullptr, FALSE);
}

void NumberingDialog::moveFocus(int delta) {
    // 0/1 输入框，2 复选，3/4 按钮；循环
    int next = focusItem_ + delta;
    if (next < 0) next = 4;
    if (next > 4) next = 0;
    setFocusItem(next, true);
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------

// EDIT 子类化（ui 命名空间：类 friend 声明可见）：Enter（确定）、Esc（取消）、
// Tab（父级焦点循环）——其余按键（含 IME/编辑）透传原生
LRESULT CALLBACK NumberingEditSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                           UINT_PTR /*id*/, DWORD_PTR ref) {
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        auto* self = reinterpret_cast<NumberingDialog*>(ref);
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        switch (wp) {
        case VK_RETURN:
            if (self) self->accept();  // Enter = 默认按钮「确定」
            return 1;
        case VK_ESCAPE:
            if (self) self->close();
            return 1;
        case VK_TAB:
            if (self) self->moveFocus(shift ? -1 : 1);
            return 1;
        default:
            break;
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK NumberingDialog::DialogWndProc(HWND hwnd, UINT msg, WPARAM wp,
                                                LPARAM lp) {
    auto* self = reinterpret_cast<NumberingDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->proc(hwnd, msg, wp, lp);
}

LRESULT NumberingDialog::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    namespace m = numberingdialog_metrics;
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint();
        return 0;
    case WM_CTLCOLOREDIT: {
        // EDIT 换肤：背景/文字色（主题）；边框/焦点环由父窗口 WM_PAINT 自绘。
        // 兜底 WHITE_BRUSH 仅在 showModal 的画刷创建失败（极罕见 OOM）时可达
        const HDC dc = reinterpret_cast<HDC>(wp);
        SetBkColor(dc, ToColorRef(CurrentPalette().contentBackground));
        SetTextColor(dc, ToColorRef(CurrentPalette().textPrimary));
        if (editBkBrush_) return reinterpret_cast<LRESULT>(editBkBrush_);
        return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
    }
    case WM_COMMAND:
        return 0;  // EN_CHANGE 无实时校验（确定时才校验）
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
        int hover = -1;  // 2 复选、3/4 按钮（与 focusItem 同轴）
        if (PtInRect(&checkBox_, pt)) hover = 2;
        for (int i = 0; i < m::ButtonCount; ++i) {
            if (PtInRect(&buttons_[i], pt)) hover = i + 3;
        }
        if (hover != hoverItem_) {
            hoverItem_ = hover;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (PtInRect(&checkBox_, pt)) {
            toggleCheckBox();
            setFocusItem(2, false);  // 鼠标操作隐藏键盘焦点环
            return 0;
        }
        for (int i = 0; i < m::ButtonCount; ++i) {
            if (PtInRect(&buttons_[i], pt)) {
                pressedItem_ = i + 3;
                SetCapture(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }
        if (PtInRect(&header_, pt)) {  // 标题条拖动把手
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
        if (pressedItem_ >= 3) {
            const int idx = pressedItem_ - 3;
            pressedItem_ = -1;
            ReleaseCapture();
            if (PtInRect(&buttons_[idx], pt)) {
                activateButton(idx);
            } else {
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (dragging_) dragging_ = false;
        if (pressedItem_ >= 3) pressedItem_ = -1;
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const UINT vk = static_cast<UINT>(wp);
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (vk == VK_ESCAPE) {
            close();
        } else if (vk == VK_RETURN) {
            if (focusItem_ >= 3) activateButton(focusItem_ - 3);
            else accept();  // 输入框/复选上 Enter = 默认按钮「确定」
        } else if (vk == VK_SPACE) {
            if (focusItem_ == 2) toggleCheckBox();
            else if (focusItem_ >= 3) activateButton(focusItem_ - 3);
        } else if (vk == VK_TAB) {
            moveFocus(shift ? -1 : 1);
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

RECT NumberingDialog::buttonRectForTest(int index) const {
    if (index < 0 || index >= numberingdialog_metrics::ButtonCount) return {};
    return buttons_[index];
}

RECT NumberingDialog::checkBoxRectForTest() const {
    return checkBox_;
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------

void NumberingDialog::paint() {
    if (!hwnd_) return;
    namespace m = numberingdialog_metrics;
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
    DrawTextW(mem, L"插入编号", -1, &titleRect,
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
    drawEditFrame(startEditRect_, focusItem_ == 0);
    drawEditFrame(stepEditRect_, focusItem_ == 1);

    // 行标签
    SetTextColor(mem, ToColorRef(pal.textSecondary));
    RECT l1 = startLabel_, l2 = stepLabel_;
    DrawTextW(mem, L"起始值：", -1, &l1, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    DrawTextW(mem, L"增量：", -1, &l2, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);

    // 复选框（勾选图形 = 非颜色线索）
    {
        const RECT& box = checkBox_;
        HPEN pen = CreatePen(PS_SOLID, 1, ToColorRef(pal.border));
        HBRUSH boxBrush = CreateSolidBrush(ToColorRef(pal.contentBackground));
        const HGDIOBJ oldPen = SelectObject(mem, pen);
        const HGDIOBJ oldBrush = SelectObject(mem, boxBrush);
        Rectangle(mem, box.left, box.top, box.right, box.bottom);
        SelectObject(mem, oldBrush);
        SelectObject(mem, oldPen);
        DeleteObject(boxBrush);
        DeleteObject(pen);
        if (padZero_) {  // 对勾（两段线；形状即线索）
            HPEN cp = CreatePen(PS_SOLID, Scale(2, dpi_), ToColorRef(pal.accent));
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
        SetTextColor(mem, ToColorRef(pal.textPrimary));
        RECT tr = {box.right + Scale(6, dpi_), box.top - Scale(1, dpi_),
                   box.right + Scale(200, dpi_), box.bottom + Scale(1, dpi_)};
        DrawTextW(mem, L"补零（自动位数）", -1, &tr,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        // 复选键盘焦点环
        if (focusVisible_ && focusItem_ == 2) {
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

    // 状态行（错误文本 error 色——文本本身即错误线索，颜色仅辅助）
    SetTextColor(mem, ToColorRef(statusIsError_ ? pal.error : pal.textSecondary));
    RECT st = statusRect_;
    DrawTextW(mem, statusText_.c_str(), -1, &st,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

    // 按钮行
    static const wchar_t* kButtonLabels[m::ButtonCount] = {L"确定", L"取消"};
    for (int i = 0; i < m::ButtonCount; ++i) {
        const RECT& b = buttons_[i];
        const bool hovered = hoverItem_ == i + 3;
        const bool pressed = pressedItem_ == i + 3;
        const bool isDefault = (i == m::ButtonOk);
        uint32_t fill = pal.contentBackground;
        if (isDefault) {
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
            HPEN pen = CreatePen(PS_SOLID, 1, ToColorRef(pal.border));
            HBRUSH border = CreateSolidBrush(ToColorRef(pal.border));
            const HGDIOBJ oldPen = SelectObject(mem, pen);
            const HGDIOBJ oldBrush = SelectObject(mem, border);
            Rectangle(mem, b.left, b.top, b.right, b.bottom);
            SelectObject(mem, oldBrush);
            SelectObject(mem, oldPen);
            DeleteObject(pen);
            DeleteObject(border);
        }
        SetTextColor(mem, ToColorRef(isDefault ? pal.onAccentForeground : pal.textPrimary));
        RECT tr = b;
        DrawTextW(mem, kButtonLabels[i], -1, &tr,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        // 键盘焦点环
        if (focusVisible_ && focusItem_ == i + 3) {
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
