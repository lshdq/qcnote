// 自绘标题栏实现。绘制全部走 GDI（纯色块 + 线段），标题栏条带经内存 DC
// 双缓冲后一次 BitBlt，避免 hover/窗口状态切换时闪烁。
#include "titlebar.h"

namespace ui {

// —— 用户可见按钮名称（M1 以常量承载语义；UIA provider / tooltip 为后续
//    里程碑接入点，届时直接复用本表）——
// 最大化按钮名称随窗口状态切换：最大化态显示「还原」（与图形切换同步）。
constexpr wchar_t kNameMenu[] = L"菜单";
constexpr wchar_t kNameMinimize[] = L"最小化";
constexpr wchar_t kNameMaximize[] = L"最大化";
constexpr wchar_t kNameRestore[] = L"还原";
constexpr wchar_t kNameClose[] = L"关闭";

TitleBarLayout ComputeTitleBarLayout(int clientWidth, UINT dpi) {
    const int capH = Scale(metrics::CaptionHeight, dpi);
    const int btnW = Scale(metrics::CaptionButtonWidth, dpi);
    const int leftGap = Scale(metrics::TabAreaLeftGap, dpi);
    const int rightGap = Scale(metrics::TabAreaRightGap, dpi);

    TitleBarLayout L;
    L.captionHeight = capH;
    L.caption = {0, 0, clientWidth, capH};

    // 左：菜单按钮
    L.menuButton = {0, 0, btnW, capH};

    // 右：三联窗口控制按钮，固定贴右缘（需求 A2：窗口控制按钮永不被挤压）
    const int right = clientWidth;
    L.closeButton = {right - btnW, 0, right, capH};
    L.maxRestoreButton = {right - 2 * btnW, 0, right - btnW, capH};
    L.minimizeButton = {right - 3 * btnW, 0, right - 2 * btnW, capH};

    // 中：Tab 占位区（随窗口伸缩；退化窗口宽时收缩为空矩形，不产生负宽）
    const int tabLeft = btnW + leftGap;
    const int tabRight = right - 3 * btnW - rightGap;
    L.tabArea = {tabLeft < tabRight ? tabLeft : tabRight, 0,
                 tabLeft < tabRight ? tabRight : tabLeft, capH};
    return L;
}

CaptionButton CaptionButtonAt(TitleBarLayout const& layout, POINT pt) {
    if (PtInRect(&layout.closeButton, pt)) return CaptionButton::Close;
    if (PtInRect(&layout.maxRestoreButton, pt)) return CaptionButton::MaximizeRestore;
    if (PtInRect(&layout.minimizeButton, pt)) return CaptionButton::Minimize;
    if (PtInRect(&layout.menuButton, pt)) return CaptionButton::Menu;
    return CaptionButton::None;
}

LRESULT ComputeCaptionHitTest(POINT pt, SIZE size, UINT dpi, bool zoomed) {
    const TitleBarLayout L = ComputeTitleBarLayout(size.cx, dpi);
    if (!zoomed) {
        // 8 向边缘缩放：命中宽随 DPI 换算；四角区（corner×corner）优先于
        // 相邻两边命中（对齐系统惯例：角落命中斜向缩放）
        const int edge = Scale(metrics::ResizeEdge, dpi);
        const int corner = Scale(metrics::ResizeCorner, dpi);
        const bool onLeft = pt.x < edge;
        const bool onRight = pt.x >= size.cx - edge;
        const bool onTop = pt.y < edge;
        const bool onBottom = pt.y >= size.cy - edge;
        if (pt.x < corner && pt.y < corner) return HTTOPLEFT;
        if (pt.x >= size.cx - corner && pt.y < corner) return HTTOPRIGHT;
        if (pt.x < corner && pt.y >= size.cy - corner) return HTBOTTOMLEFT;
        if (pt.x >= size.cx - corner && pt.y >= size.cy - corner) return HTBOTTOMRIGHT;
        if (onLeft) return HTLEFT;
        if (onRight) return HTRIGHT;
        if (onTop) return HTTOP;
        if (onBottom) return HTBOTTOM;
    }
    // 最大化时不给 resize 命中：顶部边缘归 HTCAPTION（支持最大化态拖拽还原）；
    // 按钮命中优先级不变（最大化态菜单/控制按钮仍可点击）
    switch (CaptionButtonAt(L, pt)) {
    case CaptionButton::Close: return HTCLOSE;
    case CaptionButton::MaximizeRestore: return HTMAXBUTTON;  // Win11 Snap Layouts 悬停由系统接管
    case CaptionButton::Minimize: return HTMINBUTTON;
    case CaptionButton::Menu: return HTBORDER;  // 菜单按钮命中值（点击在 WndProc 自行处理）
    default: break;
    }
    if (pt.y >= 0 && pt.y < L.captionHeight) return HTCAPTION;
    return HTCLIENT;
}

// ---------------------------------------------------------------------------
// TitleBar
// ---------------------------------------------------------------------------

void TitleBar::attach(HWND hwnd) {
    hwnd_ = hwnd;
}

void TitleBar::detach() {
    releaseBackBuffer();
}

TitleBarLayout TitleBar::layout() const {
    RECT client{};
    if (hwnd_) {
        GetClientRect(hwnd_, &client);
    }
    return ComputeTitleBarLayout(client.right, WindowDpi(hwnd_));
}

CaptionButton TitleBar::hitButton(POINT clientPt) const {
    return CaptionButtonAt(layout(), clientPt);
}

void TitleBar::invalidateCaption() const {
    if (!hwnd_) return;
    const TitleBarLayout L = layout();
    InvalidateRect(hwnd_, &L.caption, FALSE);
}

void TitleBar::onNcMouseMove(POINT screenPt) {
    if (!hwnd_) return;
    POINT c = screenPt;
    ScreenToClient(hwnd_, &c);
    const CaptionButton hit = hitButton(c);
    if (hit != hover_) {
        hover_ = hit;
        invalidateCaption();
    }
    // 首次进入非客户区时补挂离开跟踪；从按钮到 caption 不触发 leave，
    // 只有 hit test 落到客户区（标题栏以下）才发 WM_NCMOUSELEAVE 清 hover
    if (!trackingMouse_) {
        TRACKMOUSEEVENT t{};
        t.cbSize = sizeof(t);
        t.dwFlags = TME_LEAVE | TME_NONCLIENT;
        t.hwndTrack = hwnd_;
        trackingMouse_ = TrackMouseEvent(&t) != FALSE;
    }
}

void TitleBar::onNcMouseLeave() {
    trackingMouse_ = false;
    if (hover_ != CaptionButton::None || pressed_ != CaptionButton::None) {
        hover_ = CaptionButton::None;
        pressed_ = CaptionButton::None;
        invalidateCaption();
    }
}

void TitleBar::onLButtonUp() {
    if (pressed_ != CaptionButton::None) {
        pressed_ = CaptionButton::None;
        invalidateCaption();
    }
}

void TitleBar::onDpiChanged() {
    // 字体在下次绘制时按新 DPI 惰性重建（ensureFont 内有 DPI 比对）；
    // 双缓冲尺寸随 paint 矩形自动重建。这里只负责整窗重绘。
    if (hwnd_) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void TitleBar::setPressed(CaptionButton button) {
    pressed_ = button;
    invalidateCaption();
}

void TitleBar::performAction(CaptionButton button) {
    if (!hwnd_) return;
    switch (button) {
    case CaptionButton::Minimize:
        ShowWindow(hwnd_, SW_MINIMIZE);
        break;
    case CaptionButton::MaximizeRestore:
        ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
        break;
    case CaptionButton::Close:
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        break;
    case CaptionButton::Menu:
        // 主菜单弹出经 onMenuButtonClicked 挂接（main.cpp 组装：M1-2 菜单系统，
        // 锚点=按钮左下角 layout().menuButton）
        if (onMenuButtonClicked) onMenuButtonClicked();
        break;
    default:
        break;
    }
}

void TitleBar::paint(HDC dc, RECT const& client, bool active, bool zoomed) {
    (void)active;  // E2 边界线活动换色移至 paintWindowBorder（标题栏配色不随焦点变化，E1）
    const ThemePalette& pal = CurrentPalette();

    // 内容区底色（M2 起由编辑区接管，欢迎页重涂 contentBackground）
    HBRUSH content = CreateSolidBrush(ToColorRef(pal.contentBackground));
    FillRect(dc, &client, content);
    DeleteObject(content);

    const TitleBarLayout L = ComputeTitleBarLayout(client.right, WindowDpi(hwnd_));
    const int capH = L.captionHeight;
    const int w = client.right;

    // —— 标题栏条带双缓冲（titleBarBackground；Tab 区背景即此底色，
    //    Tab 内容由 TabStrip::paint 画在其上——活动卡片/悬停/分隔线）——
    ensureBackBuffer(dc, w, capH);
    HDC mem = back_.dc;
    HBRUSH capBg = CreateSolidBrush(ToColorRef(pal.titleBarBackground));
    RECT rcAll{0, 0, w, capH};
    FillRect(mem, &rcAll, capBg);
    DeleteObject(capBg);

    drawButton(mem, L, CaptionButton::Menu, pal, false);
    drawButton(mem, L, CaptionButton::Minimize, pal, false);
    drawButton(mem, L, CaptionButton::MaximizeRestore, pal, zoomed);
    drawButton(mem, L, CaptionButton::Close, pal, false);

    BitBlt(dc, 0, 0, w, capH, mem, 0, 0, SRCCOPY);
}

void TitleBar::paintWindowBorder(HDC dc, RECT const& client, bool active, bool zoomed) {
    // E2 窗口边界线：普通窗口四边 1 物理像素（对齐 DWM 系统边框不随 DPI
    // 放大的惯例；100% 下 1px、200% 下仍 1px，观感与系统窗口一致），
    // 活动/非活动换色，最大化隐藏；仅绘制层，不参与任何命中。
    // 在全部内容绘制之后调用（否则被内容区/欢迎页重涂覆盖边缘 1px）。
    if (zoomed) {
        return;
    }
    const ThemePalette& pal = CurrentPalette();
    const COLORREF line =
        ToColorRef(active ? pal.windowBorder : pal.windowBorderInactive);
    HBRUSH b = CreateSolidBrush(line);
    RECT topRc{client.left, client.top, client.right, client.top + 1};
    RECT bottomRc{client.left, client.bottom - 1, client.right, client.bottom};
    RECT leftRc{client.left, client.top + 1, client.left + 1, client.bottom - 1};
    RECT rightRc{client.right - 1, client.top + 1, client.right, client.bottom - 1};
    FillRect(dc, &topRc, b);
    FillRect(dc, &bottomRc, b);
    FillRect(dc, &leftRc, b);
    FillRect(dc, &rightRc, b);
    DeleteObject(b);
}

void TitleBar::drawButton(HDC dc, TitleBarLayout const& layout, CaptionButton button,
                          ThemePalette const& pal, bool zoomed) {
    const RECT* rc = nullptr;
    switch (button) {
    case CaptionButton::Menu: rc = &layout.menuButton; break;
    case CaptionButton::Minimize: rc = &layout.minimizeButton; break;
    case CaptionButton::MaximizeRestore: rc = &layout.maxRestoreButton; break;
    case CaptionButton::Close: rc = &layout.closeButton; break;
    default: return;
    }
    const bool isHover = hover_ == button;
    const bool isPressed = pressed_ == button;

    // 背景：关闭按钮悬停/按下用红底（closeHover/closePressed 不透明 token）；
    // 其余按钮用 hover/pressed 半透明叠加与标题栏底色预合成（纯 GDI 无 alpha 通道）
    if (button == CaptionButton::Close && (isHover || isPressed)) {
        HBRUSH b = CreateSolidBrush(ToColorRef(isPressed ? pal.closePressed : pal.closeHover));
        FillRect(dc, rc, b);
        DeleteObject(b);
    } else if (isHover || isPressed) {
        const uint32_t overlay = isPressed ? pal.pressed : pal.hover;
        HBRUSH b = CreateSolidBrush(ToColorRef(BlendOver(pal.titleBarBackground, overlay)));
        FillRect(dc, rc, b);
        DeleteObject(b);
    }  // 常态透明（露出标题栏背景）

    // 图标：关闭按钮在红底上换白字形，其余用主文本色
    const COLORREF glyph = (button == CaptionButton::Close && (isHover || isPressed))
                               ? ToColorRef(pal.closeGlyphForeground)
                               : ToColorRef(pal.textPrimary);
    drawGlyph(dc, *rc, button, zoomed, glyph);
}

void TitleBar::drawGlyph(HDC dc, RECT const& btn, CaptionButton button, bool zoomed,
                         COLORREF color) {
    // 10×10 逻辑网格（0..9）按 DPI 缩放到像素；线宽 = 1 逻辑像素（200% 下自动 2px）
    const UINT dpi = WindowDpi(hwnd_);
    const int s = Scale(1, dpi);
    const int g = Scale(metrics::CaptionGlyphSize, dpi) - s;  // 图标跨度（含线宽共 10 逻辑像素）
    const int cx = (btn.left + btn.right) / 2;
    const int cy = (btn.top + btn.bottom) / 2;
    const int ox = cx - g / 2;
    const int oy = cy - g / 2;
    const auto pt = [&](int lx, int ly) {
        return POINT{ox + ::MulDiv(lx, g, 9), oy + ::MulDiv(ly, g, 9)};
    };
    const auto seg = [&](int x1, int y1, int x2, int y2) {
        const POINT a = pt(x1, y1);
        const POINT b = pt(x2, y2);
        MoveToEx(dc, a.x, a.y, nullptr);
        LineTo(dc, b.x, b.y);
    };

    HPEN pen = CreatePen(PS_SOLID, s, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    switch (button) {
    case CaptionButton::Menu:
        // ≡ 三横线（上/中/下，垂直分布于 10 格）
        seg(0, 1, 9, 1);
        seg(0, 5, 9, 5);
        seg(0, 9, 9, 9);
        break;
    case CaptionButton::Minimize:
        // 中心横线（y=5，对齐旧 CaptionButton 修正后行为：最小化字形在按钮垂直中心）
        seg(0, 5, 9, 5);
        break;
    case CaptionButton::MaximizeRestore:
        if (zoomed) {
            // 还原：双矩形层叠（左下后层 + 右上前层 L 形边）
            seg(0, 2, 7, 2);
            seg(7, 2, 7, 9);
            seg(7, 9, 0, 9);
            seg(0, 9, 0, 2);
            seg(2, 2, 2, 0);
            seg(2, 0, 9, 0);
            seg(9, 0, 9, 7);
            seg(9, 7, 7, 7);
        } else {
            // 最大化：10×10 外框
            seg(0, 0, 9, 0);
            seg(9, 0, 9, 9);
            seg(9, 9, 0, 9);
            seg(0, 9, 0, 0);
        }
        break;
    case CaptionButton::Close:
        // × 对角线
        seg(0, 0, 9, 9);
        seg(9, 0, 0, 9);
        break;
    default:
        break;
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void TitleBar::ensureBackBuffer(HDC paintDc, int width, int height) {
    if (back_.dc && back_.width == width && back_.height == height) {
        return;
    }
    releaseBackBuffer();
    back_.dc = CreateCompatibleDC(paintDc);
    back_.bitmap = CreateCompatibleBitmap(paintDc, width, height);
    SelectObject(back_.dc, back_.bitmap);
    back_.width = width;
    back_.height = height;
}

void TitleBar::releaseBackBuffer() {
    // 先 DeleteDC（选入关系随 DC 销毁解除），bitmap 才能安全删除
    if (back_.dc) {
        DeleteDC(back_.dc);
        back_.dc = nullptr;
    }
    if (back_.bitmap) {
        DeleteObject(back_.bitmap);
        back_.bitmap = nullptr;
    }
    back_.width = back_.height = 0;
}

}  // namespace ui
