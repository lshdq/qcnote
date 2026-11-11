// 工具栏实现：GDI 线稿图标（16×16 逻辑画布，按 DPI 换算成物理坐标绘制；
// 线宽 1 物理像素在高 DPI 下视觉过细——用 max(1, Scale(1)) 并对主要轮廓
// 保持细线风格）。悬停/按下以 hover/pressed token 与工具栏底色预合成。
#include "toolbar.h"

#include <windowsx.h>

#include <algorithm>

namespace ui {

namespace {
// 按钮对应命令与 tooltip 文案（旧 QML ToolTip.text 同文案）
constexpr Command kCommands[3] = {Command::NewDocument, Command::OpenDialog, Command::Save};
const wchar_t* const kNames[3] = {L"新建文档", L"打开文件", L"保存文档"};
const wchar_t* const kTips[3] = {L"新建文档（Ctrl+N）", L"打开文件（Ctrl+O）",
                                 L"保存文档（Ctrl+S）"};
constexpr int kTipCursorOffsetX = 16;  // tooltip 锚点：光标右下（屏幕坐标）
constexpr int kTipCursorOffsetY = 20;
}  // namespace

void Toolbar::attach(HWND hostWindow) {
    hwnd_ = hostWindow;
    tooltip_.create(hostWindow);
}

void Toolbar::detach() {
    hideTooltip();
    tooltip_.destroy();
    if (hwnd_ && tipTimerArmed_) {
        KillTimer(hwnd_, tipTimerId_);
    }
    tipTimerArmed_ = false;
    tipVisible_ = false;
    hwnd_ = nullptr;
    hover_ = pressed_ = -1;
}

RECT Toolbar::rect(const RECT& clientRect) const {
    const UINT dpi = hwnd_ ? WindowDpi(hwnd_) : 96;
    const int top = clientRect.top + Scale(metrics::CaptionHeight, dpi);
    return {clientRect.left, top, clientRect.right, top + Scale(toolbar_metrics::Height, dpi)};
}

void Toolbar::buttonRects(const RECT& clientRect, RECT out[3]) const {
    const UINT dpi = hwnd_ ? WindowDpi(hwnd_) : 96;
    const RECT bar = rect(clientRect);
    const int bw = Scale(toolbar_metrics::ButtonWidth, dpi);
    const int bh = Scale(toolbar_metrics::ButtonHeight, dpi);
    const int spacing = Scale(toolbar_metrics::Spacing, dpi);
    const int y = bar.top + (Scale(toolbar_metrics::Height, dpi) - bh) / 2;
    int x = bar.left + Scale(toolbar_metrics::LeftMargin, dpi);
    for (int i = 0; i < 3; ++i) {
        out[i] = {x, y, x + bw, y + bh};
        x += bw + spacing;
    }
}

void Toolbar::setSaveEnabled(bool enabled) {
    if (saveEnabled_ == enabled) return;
    saveEnabled_ = enabled;
    if (hwnd_) {
        RECT client{};
        GetClientRect(hwnd_, &client);
        RECT btns[3];
        buttonRects(client, btns);
        InvalidateRect(hwnd_, &btns[2], FALSE);
    }
}

int Toolbar::hitButton(POINT clientPt, const RECT& clientRect) const {
    RECT btns[3];
    buttonRects(clientRect, btns);
    for (int i = 0; i < 3; ++i) {
        if (PtInRect(&btns[i], clientPt)) return i;
    }
    return -1;
}

void Toolbar::onMouseMove(POINT clientPt) {
    if (!hwnd_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int hit = hitButton(clientPt, client);
    if (hit != hover_) {
        const int prev = hover_;
        hover_ = hit;
        if (prev >= 0) {
            RECT btns[3];
            buttonRects(client, btns);
            InvalidateRect(hwnd_, &btns[prev], FALSE);
        }
        if (hover_ >= 0) {
            RECT btns[3];
            buttonRects(client, btns);
            InvalidateRect(hwnd_, &btns[hover_], FALSE);
            // 进入新按钮：重新计时 tooltip（600ms 延迟）
            hideTooltip();
            if (!tipTimerArmed_) {
                SetTimer(hwnd_, tipTimerId_, toolbar_metrics::TooltipDelayMs, nullptr);
                tipTimerArmed_ = true;
            }
        } else {
            hideTooltip();
        }
    }
}

void Toolbar::onMouseLeave() {
    hideTooltip();
    if (hover_ >= 0 && hwnd_) {
        const int prev = hover_;
        hover_ = -1;
        RECT client{};
        GetClientRect(hwnd_, &client);
        RECT btns[3];
        buttonRects(client, btns);
        InvalidateRect(hwnd_, &btns[prev], FALSE);
    }
}

void Toolbar::onLButtonDown(POINT clientPt) {
    if (!hwnd_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const int hit = hitButton(clientPt, client);
    if (hit < 0) return;
    if (hit == 2 && !saveEnabled_) return;  // 禁用按钮：吞掉点击（无视觉反馈）
    pressed_ = hit;
    invalidateButton(static_cast<Button>(hit));
    const Command cmd = kCommands[hit];
    pressed_ = -1;  // down 即执行（与标题栏按钮同款瞬时反馈语义）
    invalidateButton(static_cast<Button>(hit));
    if (onCommand) onCommand(cmd);
}

void Toolbar::onTimer(UINT_PTR timerId) {
    if (timerId != tipTimerId_ || !hwnd_) return;
    tipTimerArmed_ = false;
    KillTimer(hwnd_, tipTimerId_);
    if (hover_ >= 0) {
        showTooltipFor(static_cast<Button>(hover_));
    }
}

void Toolbar::onThemeChanged() {
    tooltip_.applyThemeColors();  // E5：tooltip 换肤（绘制色经 CurrentPalette 动态取）
}

void Toolbar::showTooltipFor(Button button) {
    if (!hwnd_) return;
    POINT cursor{};
    GetCursorPos(&cursor);
    const int idx = static_cast<int>(button);
    // 保存禁用态提示「当前没有文档」（旧 QML enabled 三元文案）
    tooltip_.show(idx == 2 && !saveEnabled_ ? L"保存文档（当前没有文档）" : kTips[idx],
                  {cursor.x + kTipCursorOffsetX, cursor.y + kTipCursorOffsetY});
    tipVisible_ = true;
    tipButton_ = button;
}

void Toolbar::hideTooltip() {
    if (tipTimerArmed_ && hwnd_) {
        KillTimer(hwnd_, tipTimerId_);
        tipTimerArmed_ = false;
    }
    if (tipVisible_) {
        tooltip_.hide();
        tipVisible_ = false;
    }
}

void Toolbar::invalidateButton(Button button) const {
    if (!hwnd_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    RECT btns[3];
    buttonRects(client, btns);
    InvalidateRect(hwnd_, &btns[static_cast<int>(button)], FALSE);
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------

void Toolbar::paint(HDC dc, const RECT& clientRect) {
    if (!dc) return;
    const ThemePalette& pal = CurrentPalette();
    const UINT dpi = hwnd_ ? WindowDpi(hwnd_) : 96;
    const RECT bar = rect(clientRect);

    // 工具栏条带（toolbarBackground）
    HBRUSH bg = CreateSolidBrush(ToColorRef(pal.toolbarBackground));
    FillRect(dc, &bar, bg);
    DeleteObject(bg);

    RECT btns[3];
    buttonRects(clientRect, btns);
    const bool disabled[3] = {false, false, !saveEnabled_};
    for (int i = 0; i < 3; ++i) {
        const RECT& b = btns[i];
        if (disabled[i] || hover_ != i) continue;
        const uint32_t overlay = (pressed_ == i) ? pal.pressed : pal.hover;
        HBRUSH hb = CreateSolidBrush(ToColorRef(BlendOver(pal.toolbarBackground, overlay)));
        FillRect(dc, &b, hb);
        DeleteObject(hb);
    }

    // 图标（居中于按钮；禁用 textDisabled）
    const int icon = Scale(toolbar_metrics::IconSize, dpi);
    for (int i = 0; i < 3; ++i) {
        const RECT& b = btns[i];
        const RECT ir{b.left + (b.right - b.left - icon) / 2,
                      b.top + (b.bottom - b.top - icon) / 2,
                      b.left + (b.right - b.left - icon) / 2 + icon,
                      b.top + (b.bottom - b.top - icon) / 2 + icon};
        drawIcon(dc, static_cast<Button>(i), ir,
                 ToColorRef(disabled[i] ? pal.textDisabled : pal.textPrimary));
    }
}

void Toolbar::drawIcon(HDC dc, Button button, const RECT& iconRect, COLORREF color) const {
    // 16×16 逻辑网格 → 物理坐标（x∈[0,16]）。线稿：1 逻辑像素宽的笔画，
    // 按斜率走线（折角/掀盖的倾斜元素用线段近似）。
    const UINT dpi = hwnd_ ? WindowDpi(hwnd_) : 96;
    const auto gx = [&](int v) { return iconRect.left + MulDiv(v, iconRect.right - iconRect.left, 16); };
    const auto gy = [&](int v) { return iconRect.top + MulDiv(v, iconRect.bottom - iconRect.top, 16); };
    const int penW = std::max(1, Scale(1, dpi));

    HPEN pen = CreatePen(PS_SOLID, penW, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));

    const auto line = [&](int x1, int y1, int x2, int y2) {
        MoveToEx(dc, gx(x1), gy(y1), nullptr);
        LineTo(dc, gx(x2), gy(y2));
    };

    switch (button) {
    case Button::New:  // 白页 + 折角（右上角狗耳）
        line(4, 2, 10, 2);
        line(10, 2, 12, 4);
        line(12, 4, 12, 14);
        line(12, 14, 4, 14);
        line(4, 14, 4, 2);
        line(10, 2, 10, 4);   // 折角竖边
        line(10, 4, 12, 4);   // 折角横边
        break;
    case Button::Open: {  // 掀盖文件夹：平行四边形盖 + 矩形盒
        // 盖（掀起）：(2,7)-(4,4)-(14,4)-(12,7) 闭合
        line(2, 7, 4, 4);
        line(4, 4, 14, 4);
        line(14, 4, 12, 7);
        line(12, 7, 2, 7);
        // 盒：(3,7)-(13,13)
        line(3, 7, 3, 13);
        line(3, 13, 13, 13);
        line(13, 13, 13, 7);
        break;
    }
    case Button::Save:  // 软盘：外框切角 + 顶部遮板 + 底部标签
        line(3, 2, 11, 2);
        line(11, 2, 13, 4);
        line(13, 4, 13, 12);
        line(13, 12, 11, 14);  // 右下切角
        line(11, 14, 3, 14);
        line(3, 14, 3, 2);
        line(6, 2, 6, 5);      // 遮板
        line(6, 5, 10, 5);
        line(10, 5, 10, 2);
        line(5, 9, 11, 9);     // 标签框
        line(11, 9, 11, 13);
        line(11, 13, 5, 13);
        line(5, 13, 5, 9);
        break;
    }

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

}  // namespace ui
