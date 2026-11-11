// 工具栏实现（F2/F5：4 按钮 + GDI+ 双色扁平描边图标 + wrap toggle）：
// - 绘制分层：GDI 先画条带底色与 hover/pressed 叠层，GDI+ 只画图标路径
//   （同一 WM_PAINT 内混用，DEC-001）。
// - GDI+ 进程级惰性初始化（DEC-005）：std::call_once + GdiplusStartup，
//   首次需要绘制时启动；失败置标志永久回退旧 GDI 线稿路径。不显式
//   GdiplusShutdown——进程退出由 OS 回收，避免与窗口销毁顺序耦合。
// - 每次绘制直绘、无位图缓存（DEC-002）：4 个小图标路径绘制微秒级，
//   状态×主题×DPI 的缓存键管理复杂度不划算。
// - 旧 GDI 线稿保留为回退（drawIconGdiFallback）：16×16 逻辑画布按 DPI
//   换算物理坐标绘制。
#include "toolbar.h"

#include <windowsx.h>

#include <algorithm>
#include <mutex>

#include <gdiplus.h>

namespace ui {

namespace {
// 按钮对应命令与 tooltip 文案（旧 QML ToolTip.text 同文案）。wrap 文案按
// 简洁取舍不带「（开/关）」状态后缀：激活态有独立的底色+描边双线索
// （DEC-006），tooltip 只承担功能说明
constexpr Command kCommands[4] = {Command::NewDocument, Command::OpenDialog,
                                  Command::Save, Command::ToggleWordWrap};
const wchar_t* const kNames[4] = {L"新建文档", L"打开文件", L"保存文档", L"自动换行"};
const wchar_t* const kTips[4] = {L"新建文档（Ctrl+N）", L"打开文件（Ctrl+O）",
                                 L"保存文档（Ctrl+S）", L"自动换行"};
constexpr int kTipCursorOffsetX = 16;  // tooltip 锚点：光标右下（屏幕坐标）
constexpr int kTipCursorOffsetY = 20;

// —— GDI+ 进程级惰性启动（DEC-005）——
// attach/detach 与多实例（单测）不感知：首次 drawIcon 时启动一次；
// 失败则 g_gdiplusOk 恒为 false，永久走 GDI 回退路径
std::once_flag g_gdiplusOnce;
bool g_gdiplusOk = false;
ULONG_PTR g_gdiplusToken = 0;

void ensureGdiplusStarted() {
    std::call_once(g_gdiplusOnce, [] {
        Gdiplus::GdiplusStartupInput input;  // 默认参数（无后台线程回调需求）
        g_gdiplusOk =
            Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr) == Gdiplus::Ok;
    });
}

// 0xAARRGGBB token → Gdiplus::Color（GDI+ 可直接携带 alpha：半透明点缀
// 色走原生 alpha 合成；不透明面仍经 BlendOver 预合成后传入）
Gdiplus::Color argbToGdiplus(uint32_t argb) {
    return {static_cast<BYTE>((argb >> 24) & 0xFF), static_cast<BYTE>((argb >> 16) & 0xFF),
            static_cast<BYTE>((argb >> 8) & 0xFF), static_cast<BYTE>(argb & 0xFF)};
}

// COLORREF（0x00BBGGRR）→ 不透明 Gdiplus::Color（主描边语义色）
Gdiplus::Color colorRefToGdiplus(COLORREF c) {
    return {static_cast<BYTE>(255), GetRValue(c), GetGValue(c), GetBValue(c)};
}
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

void Toolbar::buttonRects(const RECT& clientRect, RECT out[4]) const {
    const UINT dpi = hwnd_ ? WindowDpi(hwnd_) : 96;
    const RECT bar = rect(clientRect);
    const int bw = Scale(toolbar_metrics::ButtonWidth, dpi);
    const int bh = Scale(toolbar_metrics::ButtonHeight, dpi);
    const int spacing = Scale(toolbar_metrics::Spacing, dpi);
    const int y = bar.top + (Scale(toolbar_metrics::Height, dpi) - bh) / 2;
    int x = bar.left + Scale(toolbar_metrics::LeftMargin, dpi);
    for (int i = 0; i < 4; ++i) {
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
        RECT btns[4];
        buttonRects(client, btns);
        InvalidateRect(hwnd_, &btns[2], FALSE);
    }
}

void Toolbar::setWrapEnabled(bool enabled) {
    if (wrapActive_ == enabled) return;
    wrapActive_ = enabled;
    // 局部失效模式同 setSaveEnabled：只重绘换行按钮（第 4 按钮）
    if (hwnd_) {
        RECT client{};
        GetClientRect(hwnd_, &client);
        RECT btns[4];
        buttonRects(client, btns);
        InvalidateRect(hwnd_, &btns[3], FALSE);
    }
}

int Toolbar::hitButton(POINT clientPt, const RECT& clientRect) const {
    RECT btns[4];
    buttonRects(clientRect, btns);
    for (int i = 0; i < 4; ++i) {
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
            RECT btns[4];
            buttonRects(client, btns);
            InvalidateRect(hwnd_, &btns[prev], FALSE);
        }
        if (hover_ >= 0) {
            RECT btns[4];
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
        RECT btns[4];
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
    RECT btns[4];
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

    RECT btns[4];
    buttonRects(clientRect, btns);
    const bool disabled[4] = {false, false, !saveEnabled_, false};
    for (int i = 0; i < 4; ++i) {
        const RECT& b = btns[i];
        if (disabled[i]) continue;
        // 按钮底：wrap 激活为淡 accent 底（DEC-006），其余与条带同色。
        // 统一落一笔（普通按钮等价于底色重涂，视觉与旧实现一致），保证
        // 激活底在 hover/pressed 叠层之前存在
        uint32_t base = pal.toolbarBackground;
        if (i == 3 && wrapActive_) {
            base = BlendOver(base, (pal.accent & 0x00FFFFFFu) | 0x1E000000u);
        }
        // hover/pressed 语义与既有按钮一致：在当前底上继续叠半透明 token
        const uint32_t overlay =
            (pressed_ == i) ? pal.pressed : ((hover_ == i) ? pal.hover : 0u);
        if (overlay) base = BlendOver(base, overlay);
        HBRUSH hb = CreateSolidBrush(ToColorRef(base));
        FillRect(dc, &b, hb);
        DeleteObject(hb);
    }

    // 图标（居中于按钮；禁用 textDisabled；wrap 激活 accent 描边——DEC-006）
    const int icon = Scale(toolbar_metrics::IconSize, dpi);
    for (int i = 0; i < 4; ++i) {
        const RECT& b = btns[i];
        const RECT ir{b.left + (b.right - b.left - icon) / 2,
                      b.top + (b.bottom - b.top - icon) / 2,
                      b.left + (b.right - b.left - icon) / 2 + icon,
                      b.top + (b.bottom - b.top - icon) / 2 + icon};
        const COLORREF stroke = ToColorRef(disabled[i] ? pal.textDisabled
                                           : ((i == 3 && wrapActive_) ? pal.accent
                                                                      : pal.textPrimary));
        drawIcon(dc, static_cast<Button>(i), ir, stroke);
    }
}

void Toolbar::drawIcon(HDC dc, Button button, const RECT& iconRect, COLORREF color) const {
    // GDI+ 可用性惰性判定（DEC-005）：启动失败时永久走 GDI 线稿回退，
    // 对调用方（paint）透明
    ensureGdiplusStarted();
    if (g_gdiplusOk) {
        drawIconGdiplus(dc, button, iconRect, color);
    } else {
        drawIconGdiFallback(dc, button, iconRect, color);
    }
}

// GDI+ 抗锯齿双色扁平描边图标（DEC-001/002）：16×16 逻辑画布按 iconRect
// 物理尺寸映射（REAL 浮点坐标，不做整数取整，抗锯齿边缘更平滑）。
// 配色：主描边 = 调用方语义色（textPrimary / 禁用 textDisabled / wrap 激活
// accent）；点缀 = pal.accent（wrap 激活时主次反转换 textPrimary——DEC-006）；
// 淡填充面 = toolbarBackground 与亮色（closeGlyphForeground，两主题均为白）
// 低 alpha 预合成的不透明色——「颜色一律经 CurrentPalette token」约束下的
// 主题自适应亮面。所有 GDI+ 对象栈上构造，析构即释放（RAII，异常安全）；
// Graphics 不超出本次调用存活期。
void Toolbar::drawIconGdiplus(HDC dc, Button button, const RECT& iconRect,
                              COLORREF stroke) const {
    const ThemePalette& pal = CurrentPalette();
    const UINT dpi = hwnd_ ? WindowDpi(hwnd_) : 96;

    // 16×16 逻辑网格 → 物理坐标（x/y ∈ [0,16] 线性映射）
    const auto gx = [&](Gdiplus::REAL v) {
        return static_cast<Gdiplus::REAL>(iconRect.left) +
               v * static_cast<Gdiplus::REAL>(iconRect.right - iconRect.left) / 16.0f;
    };
    const auto gy = [&](Gdiplus::REAL v) {
        return static_cast<Gdiplus::REAL>(iconRect.top) +
               v * static_cast<Gdiplus::REAL>(iconRect.bottom - iconRect.top) / 16.0f;
    };
    const auto pf = [&](Gdiplus::REAL x, Gdiplus::REAL y) { return Gdiplus::PointF{gx(x), gy(y)}; };

    const bool wrapActive = (button == Button::Wrap && wrapActive_);
    const uint32_t spotToken = wrapActive ? pal.textPrimary : pal.accent;  // 点缀色
    const auto spotColor = argbToGdiplus(spotToken);

    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    // 主描边笔：约 1.5 逻辑像素（max(1, 1.5·dpi/96) 物理像素），圆帽圆角连接
    const Gdiplus::REAL penW = std::max(1.0f, 1.5f * static_cast<Gdiplus::REAL>(dpi) / 96.0f);
    Gdiplus::Pen mainPen(colorRefToGdiplus(stroke), penW);
    mainPen.SetStartCap(Gdiplus::LineCapRound);
    mainPen.SetEndCap(Gdiplus::LineCapRound);
    mainPen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::Pen spotPen(spotColor, penW);  // 点缀笔（accent 短线）：同宽圆帽
    spotPen.SetStartCap(Gdiplus::LineCapRound);
    spotPen.SetEndCap(Gdiplus::LineCapRound);

    switch (button) {
    case Button::New: {
        // 形状意图：文档页轮廓（右上狗耳折角；pen 圆角连接近似「略圆角」）
        // + 折角三角 accent 半透明填充（alpha 0x66，GDI+ 原生 alpha）
        // + 页面内右下 accent「+」角标（F2「新增」语义，避开折角区）
        Gdiplus::GraphicsPath page;
        page.AddLine(pf(3.5f, 1.5f), pf(9.5f, 1.5f));      // 顶边（至折角起点）
        page.AddLine(pf(9.5f, 1.5f), pf(12.5f, 4.5f));     // 折角斜边
        page.AddLine(pf(12.5f, 4.5f), pf(12.5f, 14.5f));   // 右边
        page.AddLine(pf(12.5f, 14.5f), pf(3.5f, 14.5f));   // 底边
        page.CloseFigure();                                // 左边闭合
        Gdiplus::GraphicsPath fold;                        // 狗耳三角填充面
        const Gdiplus::PointF foldPts[3] = {pf(9.5f, 1.5f), pf(12.5f, 4.5f), pf(9.5f, 4.5f)};
        fold.AddPolygon(foldPts, 3);
        Gdiplus::SolidBrush foldBrush(
            argbToGdiplus((pal.accent & 0x00FFFFFFu) | 0x66000000u));
        graphics.FillPath(&foldBrush, &fold);   // 先填面后描边（描边盖住公共边）
        graphics.DrawPath(&mainPen, &page);
        graphics.DrawLine(&spotPen, gx(8.5f), gy(11.0f), gx(11.5f), gy(11.0f));  // 「+」横
        graphics.DrawLine(&spotPen, gx(10.0f), gy(9.5f), gx(10.0f), gy(12.5f));  // 「+」竖
        break;
    }
    case Button::Open: {
        // 形状意图：双色文件夹。后板（含左侧折耳斜肩）仅描边，底边开口由
        // 前盖覆盖；前盖平行四边形淡填充 + 主描边（掀开的挡板）；前后盖
        // 之间缝隙的中部画一条 accent 开口短线（「打开」状态点缀）
        Gdiplus::GraphicsPath back;
        back.AddLine(pf(2.5f, 13.5f), pf(2.5f, 4.5f));    // 左边
        back.AddLine(pf(2.5f, 4.5f), pf(6.0f, 4.5f));     // 顶边左段（折耳）
        back.AddLine(pf(6.0f, 4.5f), pf(7.5f, 6.5f));     // 折耳斜肩
        back.AddLine(pf(7.5f, 6.5f), pf(13.5f, 6.5f));    // 顶边右段（低一档）
        back.AddLine(pf(13.5f, 6.5f), pf(13.5f, 13.5f));  // 右边
        const Gdiplus::PointF flapPts[4] = {pf(2.0f, 13.5f), pf(4.5f, 8.5f),
                                            pf(14.0f, 8.5f), pf(11.5f, 13.5f)};
        Gdiplus::GraphicsPath flap;
        flap.AddPolygon(flapPts, 4);
        // 淡填充面：toolbarBackground 叠白预合成（两主题自适应的亮面）
        const uint32_t face = BlendOver(pal.toolbarBackground,
                                        (pal.closeGlyphForeground & 0x00FFFFFFu) | 0x3D000000u);
        Gdiplus::SolidBrush faceBrush(argbToGdiplus(face));
        graphics.FillPath(&faceBrush, &flap);
        graphics.DrawPath(&mainPen, &flap);
        graphics.DrawPath(&mainPen, &back);
        graphics.DrawLine(&spotPen, gx(6.0f), gy(7.5f), gx(12.5f), gy(7.5f));  // 开口线
        break;
    }
    case Button::Save: {
        // 形状意图：软盘。外框右上直角、右下切角（老式软盘轮廓）；顶部
        // 遮板矩形描边（金属滑板）；底部标签框 accent 淡填充 + 描边
        Gdiplus::GraphicsPath body;
        body.AddLine(pf(3.5f, 2.5f), pf(12.5f, 2.5f));     // 顶边
        body.AddLine(pf(12.5f, 2.5f), pf(12.5f, 11.0f));   // 右边（右上直角）
        body.AddLine(pf(12.5f, 11.0f), pf(10.5f, 13.5f));  // 右下切角
        body.AddLine(pf(10.5f, 13.5f), pf(3.5f, 13.5f));   // 底边
        body.CloseFigure();                                // 左边闭合
        Gdiplus::GraphicsPath shutter;                     // 顶部遮板
        shutter.AddRectangle(
            Gdiplus::RectF{gx(5.5f), gy(2.5f), gx(10.5f) - gx(5.5f), gy(6.0f) - gy(2.5f)});
        Gdiplus::GraphicsPath label;                       // 底部标签框
        label.AddRectangle(
            Gdiplus::RectF{gx(5.0f), gy(8.5f), gx(11.0f) - gx(5.0f), gy(13.5f) - gy(8.5f)});
        // 标签 accent 淡填充：预合成不透明（叠在公共边上不透底，描边随后覆盖）
        const uint32_t labelFace = BlendOver(pal.toolbarBackground,
                                             (pal.accent & 0x00FFFFFFu) | 0x2E000000u);
        Gdiplus::SolidBrush labelBrush(argbToGdiplus(labelFace));
        graphics.FillPath(&labelBrush, &label);
        graphics.DrawPath(&mainPen, &body);
        graphics.DrawPath(&mainPen, &shutter);
        graphics.DrawPath(&mainPen, &label);
        break;
    }
    case Button::Wrap: {
        // 形状意图：折行符号「↵」——上横线右行，右上圆角下弯，右侧短竖线
        // （下降一段），右下圆角折返左行，末端 accent 实心小三角箭头（指向
        // 左，即「折回到下一行」）。激活/非激活仅主描边与点缀色互换
        // （DEC-006 主次反转），形状不变
        const Gdiplus::REAL r = 2.0f;  // 圆角半径（16 格逻辑坐标）
        Gdiplus::GraphicsPath hook;
        hook.AddLine(pf(2.5f, 5.0f), pf(10.5f, 5.0f));  // 上横线
        // 右上圆角下弯：圆心 (10.5, 5+r)，自顶点顺时针扫 90° 到右顶点
        // （进入切线向右、离开切线向下）；GDI+ 角度顺时针、y 轴向下
        hook.AddArc(Gdiplus::RectF{gx(10.5f - r), gy(5.0f), gx(r) - gx(0.0f),
                                   gy(r) - gy(0.0f)},
                    270.0f, 90.0f);
        hook.AddLine(pf(10.5f + r, 5.0f + r), pf(10.5f + r, 8.0f));  // 右侧竖直短线
        // 右下圆角折返：圆心 (10.5, 8)，自右顶点顺时针扫 90° 到底点（离开
        // 切线向左，接折返线）
        hook.AddArc(Gdiplus::RectF{gx(10.5f - r), gy(8.0f - r), gx(r) - gx(0.0f),
                                   gy(r) - gy(0.0f)},
                    0.0f, 90.0f);
        hook.AddLine(pf(10.5f, 10.0f), pf(4.5f, 10.0f));  // 折返线（回到左侧）
        graphics.DrawPath(&mainPen, &hook);
        // accent 实心小三角箭头（指向左）
        const Gdiplus::PointF arrowPts[3] = {pf(2.5f, 10.0f), pf(5.2f, 8.4f), pf(5.2f, 11.6f)};
        Gdiplus::SolidBrush arrowBrush(spotColor);
        graphics.FillPolygon(&arrowBrush, arrowPts, 3);
        break;
    }
    }
}

void Toolbar::drawIconGdiFallback(HDC dc, Button button, const RECT& iconRect,
                                  COLORREF color) const {
    // 旧 GDI 线稿回退（GDI+ 启动失败时唯一路径，DEC-005）：16×16 逻辑网格
    // → 物理坐标（x∈[0,16]）。线稿：1 逻辑像素宽的笔画，按斜率走线
    // （折角/掀盖的倾斜元素用线段近似）。
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
    case Button::Wrap:  // 折行「↵」：横线 + 圆角折返（线段近似圆弧）+ 左箭头
        line(2, 5, 11, 5);     // 上横线
        line(11, 5, 12, 6);    // 右上斜切（近似下弯）
        line(12, 6, 12, 7);    // 右侧短竖线
        line(12, 7, 11, 8);    // 右下斜切（近似折返）
        line(11, 8, 4, 8);     // 折返线
        line(4, 6, 2, 8);      // 箭头折线（指左）
        line(4, 10, 2, 8);
        break;
    }

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

}  // namespace ui
