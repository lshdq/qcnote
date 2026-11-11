// 欢迎页实现。字体/文字尺寸按窗口 DPI 惰性重建并缓存（布局稳定，不随鼠标变化）；
// 绘制全 GDI 直写（WM_ERASEBKGND 已禁）。布局经 computeLayout 单一来源，
// 绘制（buttonRect）与命中（hitButton）共用同一几何。
#include "welcome.h"

#include <algorithm>

namespace ui {

namespace {
constexpr wchar_t kAppName[] = L"qcnote";
constexpr wchar_t kDescription[] = L"轻量纯文本编辑器";
constexpr wchar_t kOpenButtonText[] = L"打开文件…";
}  // namespace

void WelcomePage::attach(HWND hwnd) {
    hwnd_ = hwnd;
}

void WelcomePage::detach() {
    if (titleFont_) {
        DeleteObject(titleFont_);
        titleFont_ = nullptr;
    }
    if (bodyFont_) {
        DeleteObject(bodyFont_);
        bodyFont_ = nullptr;
    }
    fontDpi_ = 0;
    hwnd_ = nullptr;
    hover_ = false;
    focused_ = false;
}

void WelcomePage::reset() {
    hover_ = false;
    focused_ = false;
}

void WelcomePage::setChromeInsets(int toolbarHeightLogical, int statusBarHeightLogical) {
    toolbarInset_ = toolbarHeightLogical;
    statusInset_ = statusBarHeightLogical;
}

// ---------------------------------------------------------------------------
// 字体与测量
// ---------------------------------------------------------------------------

void WelcomePage::ensureFontsAndMetrics() const {
    const UINT dpi = WindowDpi(hwnd_);
    if (titleFont_ && bodyFont_ && fontDpi_ == dpi && btnTextW_ > 0) {
        return;
    }
    if (titleFont_) {
        DeleteObject(titleFont_);
    }
    if (bodyFont_) {
        DeleteObject(bodyFont_);
    }
    titleFont_ = CreateFontW(-Scale(welcome_metrics::TitlePixelSize, dpi), 0, 0, 0, FW_BOLD,
                             FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    bodyFont_ = CreateFontW(-Scale(welcome_metrics::DescPixelSize, dpi), 0, 0, 0, FW_NORMAL,
                            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    fontDpi_ = dpi;

    // 文字高度/宽度实测缓存（布局用；测量失败时退化为字号近似值）
    titleH_ = Scale(welcome_metrics::TitlePixelSize, dpi);
    descH_ = Scale(welcome_metrics::DescPixelSize, dpi);
    btnTextW_ = Scale(64, dpi);
    btnTextH_ = Scale(welcome_metrics::ButtonPixelSize, dpi);
    HDC dc = GetDC(nullptr);
    if (dc) {
        // 测量成功时覆写 w/h（SIZE 双出参），失败保留上面的退化值
        const auto measure = [&dc](HFONT font, const wchar_t* text, int& w, int& h) {
            const HGDIOBJ old = SelectObject(dc, font);
            SIZE sz{};
            if (GetTextExtentPoint32W(dc, text, lstrlenW(text), &sz)) {
                w = sz.cx;
                h = sz.cy;
            }
            SelectObject(dc, old);
        };
        int w = titleH_, h = titleH_;
        measure(titleFont_, kAppName, w, h);
        titleH_ = h;
        w = h = descH_;
        measure(bodyFont_, kDescription, w, h);
        descH_ = h;
        measure(bodyFont_, kOpenButtonText, btnTextW_, btnTextH_);
        ReleaseDC(nullptr, dc);
    }
}

// ---------------------------------------------------------------------------
// 布局（绘制与命中共用）
// ---------------------------------------------------------------------------

WelcomePage::Layout WelcomePage::computeLayout(RECT const& clientRect) const {
    Layout out;
    ensureFontsAndMetrics();
    const UINT dpi = fontDpi_;
    const int capH = Scale(metrics::CaptionHeight, dpi);
    // 内容区 = 客户区 − 标题栏 − 工具栏/状态栏占位（M2-B chrome insets）
    out.content = {clientRect.left,
                   clientRect.top + capH + Scale(toolbarInset_, dpi),
                   clientRect.right,
                   clientRect.bottom - Scale(statusInset_, dpi)};
    const int contentH = out.content.bottom - out.content.top;
    const int contentW = out.content.right - out.content.left;
    const int gap1 = Scale(welcome_metrics::TitleToDescGap, dpi);
    const int gap2 = Scale(welcome_metrics::DescToButtonGap, dpi);
    const int btnH = btnTextH_ + 2 * Scale(welcome_metrics::ButtonPaddingV, dpi);
    const int total = titleH_ + gap1 + descH_ + gap2 + btnH;
    if (contentH < total + Scale(16, dpi) || contentW <= 0) {
        return out;  // 过矮/过窄：不绘制（防溢出），按钮矩形空
    }
    const int cx = (out.content.left + out.content.right) / 2;
    int y = out.content.top + (contentH - total) / 2;
    out.title = {out.content.left, y, out.content.right, y + titleH_};
    y += titleH_ + gap1;
    out.desc = {out.content.left, y, out.content.right, y + descH_};
    y += descH_ + gap2;
    const int btnW = btnTextW_ + 2 * Scale(welcome_metrics::ButtonPaddingH, dpi);
    out.button = {cx - btnW / 2, y, cx - btnW / 2 + btnW, y + btnH};
    out.valid = true;
    return out;
}

RECT WelcomePage::buttonRect() const {
    if (!hwnd_) {
        return {};
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    return computeLayout(client).button;
}

bool WelcomePage::hitButton(POINT clientPt) const {
    if (!hwnd_) {
        return false;
    }
    RECT b = buttonRect();
    return PtInRect(&b, clientPt) != FALSE;
}

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------

void WelcomePage::onMouseMove(POINT clientPt) {
    const bool hit = hitButton(clientPt);
    if (hit != hover_) {
        hover_ = hit;
        invalidateButton();
    }
}

void WelcomePage::onMouseLeave() {
    if (hover_) {
        hover_ = false;
        invalidateButton();
    }
}

void WelcomePage::onLButtonDown(POINT clientPt) {
    if (hitButton(clientPt)) {
        triggerOpen();
    }
}

bool WelcomePage::onKeyDown(UINT vk, bool shiftDown) {
    switch (vk) {
    case VK_TAB:  // 单一可聚焦元素：Tab/Shift+Tab 均聚焦（焦点环可见）
        (void)shiftDown;
        if (!focused_) {
            focused_ = true;
            invalidateButton();
        }
        return true;
    case VK_RETURN:
    case VK_SPACE:
        if (focused_) {
            triggerOpen();
            return true;
        }
        return false;
    default:
        return false;
    }
}

void WelcomePage::triggerOpen() {
    if (onOpenClicked) {
        onOpenClicked();
    }
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------

void WelcomePage::paint(HDC dc, RECT const& clientRect) {
    if (!hwnd_ || !dc) {
        return;
    }
    const Layout layout = computeLayout(clientRect);
    const ThemePalette& pal = CurrentPalette();
    const UINT dpi = WindowDpi(hwnd_);

    // 内容区底色（A5：contentBackground）
    HBRUSH bg = CreateSolidBrush(ToColorRef(pal.contentBackground));
    FillRect(dc, &layout.content, bg);
    DeleteObject(bg);
    if (!layout.valid) {
        return;
    }

    SetBkMode(dc, TRANSPARENT);
    // 应用名（textPrimary，粗体大字）
    const HGDIOBJ oldFont = SelectObject(dc, titleFont_);
    SetTextColor(dc, ToColorRef(pal.textPrimary));
    RECT tr = layout.title;
    DrawTextW(dc, kAppName, -1, &tr, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    // 一行说明（textSecondary）
    SelectObject(dc, bodyFont_);
    SetTextColor(dc, ToColorRef(pal.textSecondary));
    RECT dr = layout.desc;
    DrawTextW(dc, kDescription, -1, &dr, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);

    // 「打开文件…」文字按钮：accent 色文字；悬停半透明高亮（与内容底色预合成）
    if (hover_) {
        const COLORREF hoverBg = ToColorRef(BlendOver(pal.contentBackground, pal.hover));
        HBRUSH hb = CreateSolidBrush(hoverBg);
        HGDIOBJ oldBrush = SelectObject(dc, hb);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        const int r = Scale(welcome_metrics::CornerRadius, dpi) * 2;
        RoundRect(dc, layout.button.left, layout.button.top, layout.button.right - 1,
                  layout.button.bottom - 1, r, r);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(hb);
    }
    SetTextColor(dc, ToColorRef(pal.accent));
    RECT br = layout.button;
    DrawTextW(dc, kOpenButtonText, -1, &br,
              DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);

    // 键盘焦点环（accent 线框，形状线索非颜色）
    if (focused_) {
        const int inset = Scale(welcome_metrics::FocusInset, dpi);
        const int pw = Scale(welcome_metrics::FocusPenWidth, dpi);
        const int fr = Scale(welcome_metrics::CornerRadius, dpi) * 2;
        HPEN pen = CreatePen(PS_SOLID, pw, ToColorRef(pal.focus));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, layout.button.left + inset, layout.button.top + inset,
                  layout.button.right - inset - 1, layout.button.bottom - inset - 1, fr, fr);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }
    SelectObject(dc, oldFont);
}

void WelcomePage::invalidateButton() const {
    if (!hwnd_) {
        return;
    }
    RECT b = buttonRect();
    if (b.right > b.left) {
        InvalidateRect(hwnd_, &b, FALSE);
    }
}

}  // namespace ui
