// qcnote 自绘标题栏（M1）：三段式布局（菜单按钮 → Tab 条区 → 窗口控制按钮）、
// GDI 双缓冲自绘（背景/按钮图标/hover/pressed）、hover 跟踪与点击动作。
// M1-3 起 Tab 条区内容由 ui/tabs 的 TabStrip 绘制（本组件只画 titleBarBackground
// 底色与窗口控制按钮；E2 边界线独立为 paintWindowBorder，供组装层排序）。
// 窗口级无边框机制（NCCALCSIZE/NCHITTEST 的边缘缩放与拖拽锚定/系统菜单）在
// main.cpp 组装，其中命中计算复用本模块的纯函数（ComputeCaptionHitTest）。
#pragma once

#include <windows.h>

#include <functional>

#include "theme.h"

namespace ui {

// 标题栏布局（客户区像素坐标）。由纯函数生成，绘制/命中/单测三方共用，
// 保证「画出来的」与「命中的」永远同一份几何。
struct TitleBarLayout {
    RECT caption{};         // 标题栏整条
    RECT menuButton{};      // 左：菜单按钮（≡）
    RECT tabArea{};         // 中：Tab 条占位（M1-下一阶段接 Win11 连通式 Tab 条）
    RECT minimizeButton{};  // 右三联窗口控制按钮（固定贴右缘，永不被 Tab 挤压）
    RECT maxRestoreButton{};
    RECT closeButton{};
    int captionHeight{};
};

// 三段式布局纯函数：全部尺寸经 Scale(dpi) 换算。
TitleBarLayout ComputeTitleBarLayout(int clientWidth, UINT dpi);

// 标题栏按钮枚举（含菜单按钮）
enum class CaptionButton { None, Menu, Minimize, MaximizeRestore, Close };

// 按钮命中（纯函数）：命中任一按钮矩形则返回之，否则 None。
CaptionButton CaptionButtonAt(TitleBarLayout const& layout, POINT clientPt);

// WM_NCHITTEST 核心纯函数（main.cpp 做屏幕→客户坐标转换后调用；单测直接覆盖）。
// 优先级：四角 → 四边缩放（zoomed 时禁用，顶部让位给 HTCAPTION 以支持
// 最大化态拖拽还原）→ 窗口控制按钮（HTMAXBUTTON 交系统接管 Snap Layouts 悬停）
// → 菜单按钮（HTBORDER，点击自行处理）→ HTCAPTION → HTCLIENT。
LRESULT ComputeCaptionHitTest(POINT clientPt, SIZE clientSize, UINT dpi, bool zoomed);

class TitleBar {
public:
    void attach(HWND hwnd);
    void detach();  // WM_DESTROY 时释放 GDI 资源

    // —— 消息（main.cpp 转发；坐标为屏幕坐标，内部换算客户区）——
    void onNcMouseMove(POINT screenPt);  // hover 更新 + TrackMouseEvent 补挂
    void onNcMouseLeave();               // 清 hover/pressed
    void onLButtonUp();                  // 清 pressed
    void onDpiChanged();                 // 整窗重绘（字体按新 DPI 惰性重建）

    // —— 交互 ——
    void setPressed(CaptionButton button);       // 按下视觉状态（down 即执行的瞬时反馈）
    void performAction(CaptionButton button);    // 动作：min/max/restore/close/menu 回调

    // —— 绘制（WM_PAINT 调用）——
    // 内容区底色 + 标题栏条带（双缓冲，含 Tab 区背景；Tab 内容由 TabStrip 另画）。
    // E2 窗口边界线拆到 paintWindowBorder：需在全部内容（含欢迎页）之后绘制，
    // 避免被内容区重涂覆盖。
    void paint(HDC dc, RECT const& clientRect, bool active, bool zoomed);
    // E2 窗口边界线：普通窗口四边 1 物理像素，活动/非活动换色，最大化隐藏；
    // 仅绘制层，不参与任何命中。
    void paintWindowBorder(HDC dc, RECT const& clientRect, bool active, bool zoomed);

    // —— 查询 ——
    TitleBarLayout layout() const;
    UINT dpi() const { return WindowDpi(hwnd_); }
    CaptionButton hitButton(POINT clientPt) const;

    // —— 挂接点（M1-3：Tab 条）——
    // 菜单按钮点击 → 主菜单系统（M1-2 已接线，main.cpp 组装层弹出）。
    std::function<void()> onMenuButtonClicked;
    // Tab 条模块挂接：layout() 的 tabArea 即 Tab 条可用区域（贴标题栏底，
    // 连通式约束在 M2 工具栏落地后生效）；空白区命中已由 HTCAPTION 覆盖拖动。

private:
    struct BackBuffer {
        HDC dc{};
        HBITMAP bitmap{};
        int width{};
        int height{};
    };

    void ensureBackBuffer(HDC paintDc, int width, int height);
    void releaseBackBuffer();
    void invalidateCaption() const;
    void drawButton(HDC dc, TitleBarLayout const& layout, CaptionButton button,
                    ThemePalette const& pal, bool zoomed);
    void drawGlyph(HDC dc, RECT const& buttonRect, CaptionButton button, bool zoomed,
                   COLORREF color);

    HWND hwnd_{};
    CaptionButton hover_{CaptionButton::None};
    CaptionButton pressed_{CaptionButton::None};
    bool trackingMouse_{};
    BackBuffer back_;
};

}  // namespace ui
