// qcnote 自绘标题栏（M1）：三段式布局（菜单按钮 → Tab 条区 → 窗口控制按钮）、
// GDI 双缓冲自绘（背景/按钮图标/hover/pressed）、hover 跟踪与点击动作。
// M1-3 起 Tab 条区内容由 ui/tabs 的 TabStrip 绘制（本组件只画 titleBarBackground
// 底色与窗口控制按钮；E2 边界线独立为 paintWindowBorder，供组装层排序）。
// 窗口级无边框机制（NCCALCSIZE/NCHITTEST 的边缘缩放与拖拽锚定/系统菜单）在
// main.cpp 组装，其中命中计算复用本模块的纯函数（ComputeCaptionHitTest）。
// 窗口控制按钮为标准 caption 按钮语义（按下记录 + 捕获、松开才执行），
// 状态机见下方 CaptionPressSession（防点击穿透，方案 A）。
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

// 标题栏窗口控制按钮「按下会话」纯状态机（防点击穿透，方案 A）。
// 语义对齐系统 caption 按钮：按下只记录（组装层另做 SetCapture），松开时光标
// 仍在原按钮上才执行动作。缺陷动机：旧版「down 即执行」在物理按键松开前
// 销毁窗口，鼠标捕获随之失效，用户松开的 WM_LBUTTONUP 落到光标下方此刻的
// 窗口（身后应用），其关闭按钮被连带触发。
// 命中结果由调用方用 CaptionButtonAt / TitleBar::hitButton 预先算好传入，
// 本类不依赖窗口与消息泵（tests/test_ui.cpp 直接单测）。
class CaptionPressSession {
public:
    // 按下：记录目标按钮并进入按下视觉。button == None 时 no-op（不开会话）。
    void begin(CaptionButton button);
    // 捕获期间移动（hit = 光标当前位置命中的按钮）：仍在同一按钮上才保持
    // 按下视觉（拖离取消、拖回恢复，对齐系统标题栏行为）。
    void update(CaptionButton hit);
    // 松开（hit = 松开位置命中的按钮）：与按下时同一按钮才返回该按钮（组装层
    // 据此执行动作），否则返回 None（拖离取消）。无论是否触发都会结束会话。
    CaptionButton complete(CaptionButton hit);
    // 取消：捕获被系统/弹窗夺走等中断路径——清状态、不执行动作。
    void cancel();
    bool active() const;                 // 会话进行中（已按下未松开、未取消）
    CaptionButton button() const;        // 按下时记录的按钮（无会话 = None）
    CaptionButton visualButton() const;  // 当前应显示按下视觉的按钮（拖离时 None）

private:
    CaptionButton pressed_{CaptionButton::None};  // 按下的目标按钮（None = 无会话）
    bool armed_ = true;  // 光标当前是否仍在目标按钮上（按下视觉的先决条件）
};

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
    // 设置/清除按下视觉：由窗口控制按钮按下会话驱动（按下、拖离撤销、拖回
    // 恢复、松开/取消清除），不再是「down 即执行」的瞬时反馈
    void setPressed(CaptionButton button);
    void performAction(CaptionButton button);    // 动作：min/max/restore/close/menu 回调
    // 按下会话结束（松开/捕获被夺）：清按下视觉、复位 NC hover 跟踪
    // （捕获会令系统取消 TrackMouseEvent 跟踪，trackingMouse_ 需复位以便下次
    // onNcMouseMove 重新挂载），并按当前光标位置重算 hover（会话结束未必伴随
    // WM_NCMOUSEMOVE，防高亮残留或丢失）。
    void endCaptureSession();

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
