// 基础对话框实现。窗口：WS_POPUP（owner 弹层）+ CS_SAVEBITS + CS_DROPSHADOW，
// Win11 圆角 DWMWCP_ROUNDSMALL（Win10 直角兜底，同 MenuHost）。按钮/把手为
// 自绘（非子窗口控件）：命中与绘制共用 buildLayout 几何。
#include "dialog.h"

#include <windowsx.h>

#include <dwmapi.h>

#include <algorithm>

#include "theme.h"

namespace ui {

namespace {
constexpr wchar_t kDialogClassName[] = L"QcnoteDialogWindow";
constexpr int kMinMessageLines = 1;
}  // namespace

// 高度 = 顶留白 + 标题 + 间距 + 正文行 + 间距 + 按钮高 + 底留白（含 2px 边框）
int ComputeDialogHeight(int lineCount, UINT dpi) {
    const int lines = std::max(kMinMessageLines, lineCount);
    const int titleH = Scale(dialog_metrics::TitlePixelSize + 6, dpi);   // 行高 ≈ 字号+6
    const int msgH = Scale(dialog_metrics::MessagePixelSize + 8, dpi) * lines;
    return Scale(dialog_metrics::PaddingTop, dpi) + titleH +
           Scale(dialog_metrics::TitleToMessageGap, dpi) + msgH +
           Scale(dialog_metrics::MessageToButtonsGap, dpi) +
           Scale(dialog_metrics::ButtonHeight, dpi) +
           Scale(dialog_metrics::ButtonsToBottom, dpi) + 2;
}

DialogHost::~DialogHost() = default;

namespace {
bool RegisterDialogWindowClass() {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    // CS_SAVEBITS：移走后系统按保存位图恢复底面；CS_DROPSHADOW：投影
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_SAVEBITS | CS_DROPSHADOW;
    wc.lpfnWndProc = &DialogHost::DialogWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // WM_PAINT 全覆盖
    wc.lpszClassName = kDialogClassName;
    registered = RegisterClassExW(&wc) != 0;
    return registered;
}
}  // namespace

int DialogHost::showModal(HWND owner, const DialogSpec& spec) {
    if (active_ || !owner || !IsWindow(owner) || spec.buttons.empty()) {
        return spec.escId;
    }
    if (!RegisterDialogWindowClass()) return spec.escId;

    active_ = true;
    done_ = false;
    result_ = spec.escId;
    spec_ = spec;
    owner_ = owner;
    dpi_ = WindowDpi(owner);
    hoverButton_ = pressedButton_ = -1;
    focusButton_ = 0;
    focusVisible_ = false;
    for (size_t i = 0; i < spec_.buttons.size(); ++i) {
        if (spec_.buttons[i].isDefault) {
            focusButton_ = static_cast<int>(i);  // 初始键盘焦点在默认按钮
            break;
        }
    }

    buildLayout();
    const int width = layout_.width;
    const int height = layout_.height;

    // 默认位置：宿主客户区居中（覆写 defaultPos；屏幕坐标）
    RECT ownerRect{};
    GetWindowRect(owner_, &ownerRect);
    POINT pos{};
    if (spec_.defaultPos) {
        pos = *spec_.defaultPos;
    } else {
        pos.x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
        pos.y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
    }
    const RECT initial = clampDialogRect({pos.x, pos.y, pos.x + width, pos.y + height});

    // 模态：禁用宿主（其子窗口输入一并失效）→ 创建并激活弹层
    EnableWindow(owner_, FALSE);
    hwnd_ = CreateWindowExW(0, kDialogClassName, L"qcnote",
                            WS_POPUP, initial.left, initial.top,
                            initial.right - initial.left, initial.bottom - initial.top,
                            owner_, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd_) {
        // 创建失败：回滚宿主使能并释放 buildLayout 已建的字体（与正常关闭路径一致）
        EnableWindow(owner_, TRUE);
        if (titleFont_) { DeleteObject(titleFont_); titleFont_ = nullptr; }
        if (messageFont_) { DeleteObject(messageFont_); messageFont_ = nullptr; }
        owner_ = nullptr;
        active_ = false;
        return spec_.escId;
    }
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    DWORD corner = DWMWCP_ROUNDSMALL;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    SetImmersiveDark(hwnd_, IsDarkTheme());  // E5：深色主题下弹出层 DWM 表面随动
    ShowWindow(hwnd_, SW_SHOW);
    SetFocus(hwnd_);
    if (onOpened) onOpened();

    MSG msg{};
    while (!done_) {
        const BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r <= 0) {
            if (r == 0) PostQuitMessage(static_cast<int>(msg.wParam));  // WM_QUIT 透传外层
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hwnd_ && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
    if (titleFont_) { DeleteObject(titleFont_); titleFont_ = nullptr; }
    if (messageFont_) { DeleteObject(messageFont_); messageFont_ = nullptr; }
    EnableWindow(owner_, TRUE);
    SetActiveWindow(owner_);  // 激活归还（输入焦点由组装层按需定向）
    owner_ = nullptr;
    active_ = false;
    return result_;
}

void DialogHost::buildLayout() {
    const int width = Scale(spec_.widthLogical, dpi_);
    const int padH = Scale(dialog_metrics::PaddingH, dpi_);

    // 正文折行测量（宽 = 弹窗宽 - 2*padH；显式 \n 计入行数）
    titleFont_ = CreateFontW(-Scale(dialog_metrics::TitlePixelSize, dpi_), 0, 0, 0, FW_SEMIBOLD,
                             FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    messageFont_ = CreateFontW(-Scale(dialog_metrics::MessagePixelSize, dpi_), 0, 0, 0, FW_NORMAL,
                               FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    int lineCount = 1;
    HDC dc = GetDC(nullptr);
    if (dc) {
        // 逐显式行（\n 分段）按可用宽折行测量：DT_CALCRECT 得整块高，
        // 除以单行高（GetTextExtentPoint32 实测）折算行数
        const std::wstring& text = spec_.message;
        const int textW = width - 2 * padH;
        SIZE single{};
        const HGDIOBJ oldFont = SelectObject(dc, messageFont_);
        GetTextExtentPoint32W(dc, L"测试Ag", 4, &single);
        const int lineHeight = std::max(1, static_cast<int>(single.cy));
        int totalLines = 0;
        size_t lineStartIdx = 0;
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == L'\n') {
                const std::wstring seg = text.substr(lineStartIdx, i - lineStartIdx);
                lineStartIdx = i + 1;
                RECT rc{0, 0, textW, 0};
                const int blockH = DrawTextW(dc, seg.c_str(), -1, &rc,
                                             DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
                totalLines += std::max(1, (std::max(1, blockH) + lineHeight - 1) / lineHeight);
            }
        }
        SelectObject(dc, oldFont);
        ReleaseDC(nullptr, dc);
        lineCount = std::max(1, totalLines);
    }

    layout_ = Layout{};
    layout_.width = width;
    layout_.height = ComputeDialogHeight(lineCount, dpi_);

    const int titleH = Scale(dialog_metrics::TitlePixelSize + 6, dpi_);
    const int headerBottom = 1 + Scale(dialog_metrics::PaddingTop, dpi_) + titleH;
    // header 把手：标题所在整条（含左右 padding 全宽；把手高度 ≥ 标题行高）
    layout_.header = {0, 0, width, headerBottom};
    layout_.message = {padH, layout_.header.bottom + Scale(dialog_metrics::TitleToMessageGap, dpi_),
                       width - padH, 0};
    const int msgH = Scale(dialog_metrics::MessagePixelSize + 8, dpi_) * std::max(1, lineCount);
    layout_.message.bottom = layout_.message.top + msgH;

    // 按钮行：右对齐，从右往左排
    const int btnH = Scale(dialog_metrics::ButtonHeight, dpi_);
    const int btnSpacing = Scale(dialog_metrics::ButtonSpacing, dpi_);
    const int btnMinW = Scale(dialog_metrics::ButtonMinWidth, dpi_);
    int btnTop = layout_.message.bottom + Scale(dialog_metrics::MessageToButtonsGap, dpi_);
    int xRight = width - padH;
    layout_.buttons.resize(spec_.buttons.size());
    for (int i = static_cast<int>(spec_.buttons.size()) - 1; i >= 0; --i) {
        const std::wstring& text = spec_.buttons[static_cast<size_t>(i)].text;
        int textW = 0;
        HDC bdc = GetDC(nullptr);
        if (bdc) {
            const HGDIOBJ oldFont = SelectObject(bdc, messageFont_);
            SIZE sz{};
            if (GetTextExtentPoint32W(bdc, text.c_str(), static_cast<int>(text.size()), &sz)) {
                textW = sz.cx;
            }
            SelectObject(bdc, oldFont);
            ReleaseDC(nullptr, bdc);
        }
        const int btnW = std::max(btnMinW, textW + Scale(28, dpi_));
        layout_.buttons[static_cast<size_t>(i)] = {xRight - btnW, btnTop, xRight, btnTop + btnH};
        xRight -= btnW + btnSpacing;
        if (spec_.buttons[static_cast<size_t>(i)].isDefault) {
            layout_.defaultButton = i;
        }
    }
}

void DialogHost::close(int result) {
    result_ = result;
    if (dragging_) endDrag();
    done_ = true;
}

void DialogHost::endDrag() {
    if (dragging_ && hwnd_) {
        ReleaseCapture();
    }
    dragging_ = false;
}

// E4 边界约束：左上角 clamp 在宿主窗口内（把手整体保留可抓回）
RECT DialogHost::clampDialogRect(RECT r) const {
    if (!owner_) return r;
    RECT ownerRect{};
    GetWindowRect(owner_, &ownerRect);
    const int keepW = Scale(dialog_metrics::KeepWidth, dpi_);
    const int headerH = (layout_.header.bottom - layout_.header.top);
    if (r.left < ownerRect.left) r.left = ownerRect.left;
    if (r.top < ownerRect.top) r.top = ownerRect.top;
    if (r.left > ownerRect.right - keepW) r.left = std::max(ownerRect.left, ownerRect.right - keepW);
    if (r.top > ownerRect.bottom - headerH) r.top = std::max(ownerRect.top, ownerRect.bottom - headerH);
    r.right = r.left + layout_.width;
    r.bottom = r.top + layout_.height;
    return r;
}

LRESULT CALLBACK DialogHost::DialogWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<DialogHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->proc(hwnd, msg, wp, lp);
}

LRESULT DialogHost::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint();
        return 0;
    case WM_SETCURSOR:
        // header 把手区域用系统标题栏惯例光标（箭头），其余默认
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_MOUSEMOVE: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (dragging_) {
            POINT cursor{};
            GetCursorPos(&cursor);
            const RECT next = clampDialogRect(
                {cursor.x - dragOrigin_.x, cursor.y - dragOrigin_.y, 0, 0});
            SetWindowPos(hwnd_, nullptr, next.left, next.top, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        int hover = -1;
        for (size_t i = 0; i < layout_.buttons.size(); ++i) {
            if (PtInRect(&layout_.buttons[i], pt)) hover = static_cast<int>(i);
        }
        if (hover != hoverButton_) {
            hoverButton_ = hover;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        for (size_t i = 0; i < layout_.buttons.size(); ++i) {
            if (PtInRect(&layout_.buttons[i], pt)) {
                pressedButton_ = static_cast<int>(i);
                focusButton_ = static_cast<int>(i);
                focusVisible_ = false;  // 鼠标操作隐藏键盘焦点环
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }
        if (PtInRect(&layout_.header, pt)) {
            // E4 拖动把手：捕获后按光标移动窗口（clampDialogRect 约束）
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
        if (pressedButton_ >= 0) {
            const int pressed = pressedButton_;
            pressedButton_ = -1;
            if (pressed < static_cast<int>(layout_.buttons.size()) &&
                PtInRect(&layout_.buttons[static_cast<size_t>(pressed)], pt)) {
                close(spec_.buttons[static_cast<size_t>(pressed)].id);
            } else {
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (dragging_) dragging_ = false;
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const UINT vk = static_cast<UINT>(wp);
        if (vk == VK_ESCAPE) {
            close(spec_.escId);
        } else if (vk == VK_RETURN) {
            const int idx = layout_.defaultButton >= 0 ? layout_.defaultButton : focusButton_;
            if (idx >= 0 && idx < static_cast<int>(spec_.buttons.size())) {
                close(spec_.buttons[static_cast<size_t>(idx)].id);
            }
        } else if (vk == VK_TAB) {
            if (!layout_.buttons.empty()) {
                const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                int next = focusButton_ + (shift ? -1 : 1);
                if (next < 0) next = static_cast<int>(layout_.buttons.size()) - 1;
                if (next >= static_cast<int>(layout_.buttons.size())) next = 0;
                focusButton_ = next;
                focusVisible_ = true;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_CLOSE:
        close(spec_.escId);  // Alt+F4/系统关闭：按 Esc 语义退出（防模态循环悬挂）
        return 0;
    case WM_ACTIVATEAPP:
        // 应用失活：仅结束拖动（对话框保持——错误框可被切走查看）
        if (wp == 0 && dragging_) endDrag();
        return 0;
    case WM_DESTROY:
        done_ = true;  // 窗口不复存在：无条件结束模态循环（防御）
        return 0;
    default:
        if (msg >= WM_APP && msg <= WM_APP + 9) return 0;  // 与 MenuHost 测试通道兼容
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void DialogHost::paint() {
    if (!hwnd_) return;
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

    // 标题（header 把手文本；textPrimary 粗体）
    SetBkMode(mem, TRANSPARENT);
    const HGDIOBJ oldFont = SelectObject(mem, titleFont_);
    SetTextColor(mem, ToColorRef(pal.textPrimary));
    RECT titleRect = layout_.header;
    titleRect.left = Scale(dialog_metrics::PaddingH, dpi_);
    titleRect.right = w - Scale(dialog_metrics::PaddingH, dpi_);
    titleRect.top += Scale(dialog_metrics::PaddingTop, dpi_);
    DrawTextW(mem, spec_.title.c_str(), -1, &titleRect,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(mem, messageFont_);
    SetTextColor(mem, ToColorRef(pal.textSecondary));
    RECT msgRect = layout_.message;
    DrawTextW(mem, spec_.message.c_str(), -1, &msgRect,
              DT_WORDBREAK | DT_LEFT | DT_NOPREFIX | DT_TOP | DT_EXPANDTABS);

    // 按钮
    for (size_t i = 0; i < layout_.buttons.size(); ++i) {
        const RECT& b = layout_.buttons[i];
        const bool isDefault = spec_.buttons[i].isDefault;
        const bool hovered = hoverButton_ == static_cast<int>(i);
        const bool pressed = pressedButton_ == static_cast<int>(i);
        if (isDefault) {
            uint32_t fill = pal.accent;
            if (pressed) fill = BlendOver(pal.accent, pal.pressed);
            else if (hovered) fill = BlendOver(pal.accent, pal.hover);
            HBRUSH fb = CreateSolidBrush(ToColorRef(fill));
            FillRect(mem, &b, fb);
            DeleteObject(fb);
        } else {
            uint32_t fill = pal.contentBackground;
            if (pressed) fill = BlendOver(pal.contentBackground, pal.pressed);
            else if (hovered) fill = BlendOver(pal.contentBackground, pal.hover);
            HBRUSH fb = CreateSolidBrush(ToColorRef(fill));
            FillRect(mem, &b, fb);
            DeleteObject(fb);
            HBRUSH border = CreateSolidBrush(ToColorRef(pal.border));
            HPEN pen = CreatePen(PS_SOLID, 1, ToColorRef(pal.border));
            const HGDIOBJ oldPen = SelectObject(mem, pen);
            const HGDIOBJ oldBrush = SelectObject(mem, border);
            Rectangle(mem, b.left, b.top, b.right, b.bottom);
            SelectObject(mem, oldPen);
            SelectObject(mem, oldBrush);
            DeleteObject(pen);
            DeleteObject(border);
        }
        SetTextColor(mem, ToColorRef(isDefault ? pal.onAccentForeground : pal.textPrimary));
        RECT tr = b;
        DrawTextW(mem, spec_.buttons[i].text.c_str(), -1, &tr,
                  DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);

        // 键盘焦点环（Tab 移动后显示；形状线索非颜色）
        if (focusVisible_ && focusButton_ == static_cast<int>(i)) {
            HPEN pen = CreatePen(PS_SOLID, Scale(2, dpi_), ToColorRef(pal.focus));
            const HGDIOBJ oldPen = SelectObject(mem, pen);
            const HGDIOBJ oldBrush = SelectObject(mem, GetStockObject(NULL_BRUSH));
            Rectangle(mem, b.left + 2, b.top + 2, b.right - 2, b.bottom - 2);
            SelectObject(mem, oldBrush);
            SelectObject(mem, oldPen);
            DeleteObject(pen);
        }
    }
    SelectObject(mem, oldFont);

    // 1px 边框线（投影/圆角由 DWM 提供）
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
