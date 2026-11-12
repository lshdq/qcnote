// qcnote 工具栏（M2-B，E3；F2 扩展）：标题栏下方一条（toolbarBackground），
// 左侧四按钮分两组：新建/打开/保存 + 竖线分隔 + 自动换行。图标 GDI+ 抗锯齿
// 彩色扁平（DEC-001/002/003：结构主描边语义色 + 语义色填充面，每个填充面
// 独立 palette token、两主题各一档；禁用态跳过全部彩色填充仅画主描边灰色
// 线稿剪影；每次绘制直绘不缓存；GDI+ 启动失败永久回退旧 GDI 单色线稿路径
// drawIconGdiFallback，可辨识不崩溃）。自动换行为 toggle（DEC-006）：激活态
// 「淡 accent 底 + accent 描边图标」颜色与饱和度双线索，与菜单勾选语义呼应。
//
// 交互语义（旧 QML Toolbar 对等）：悬停 hover 半透明高亮、按下 pressed、
// 保存按钮无活动文档时禁用（textDisabled 图标可辨识）；tooltip 延迟 600ms
// （含快捷键提示，禁用态文案「当前没有文档」）；无障碍名与按钮文本一致。
// 键盘进入（Tab/方向键）为旧版能力，M2 暂不移植（焦点在编辑器/欢迎页；
// 命令均可经快捷键触达，无功能缺失）——记录于里程碑报告。
//
// 自绘组件（非子窗口）：绘制/命中共用同一几何来源 buttonRects()，鼠标消息
// 由组装层（main.cpp WM_MOUSEMOVE/WM_LBUTTONDOWN）按 y 路由进来。
#pragma once

#include <windows.h>

#include <functional>

#include "commands.h"
#include "theme.h"
#include "tooltip.h"

namespace ui {

namespace toolbar_metrics {

constexpr int Height = 40;        // 工具栏高（旧 QML implicitHeight 44 略收窄，
                                  // 按钮仍 44×32 居中）
constexpr int ButtonWidth = 44;   // 按钮尺寸（Win11 惯例 44 宽）
constexpr int ButtonHeight = 32;
constexpr int LeftMargin = 4;     // 左缘起排（旧 QML Row leftMargin 4）
constexpr int Spacing = 2;        // 组内按钮间距（旧 QML Row spacing 2）
constexpr int SeparatorMargin = 6;  // 组间分隔竖线与两侧按钮的间距（DEC-005）
constexpr int SeparatorWidth = 1;   // 组间分隔竖线宽（pal.border，GDI FillRect）
constexpr int IconSize = 20;      // 图标画布 20×20 逻辑像素（DEC-002/004：
                                  // 形状坐标仍用 16 网格线性映射，仅画布放大）
constexpr int TooltipDelayMs = 600;

}  // namespace toolbar_metrics

class Toolbar {
public:
    enum class Button { New = 0, Open = 1, Save = 2, Wrap = 3 };

    void attach(HWND hostWindow);
    void detach();

    // 工具栏区域（客户区坐标）：[CaptionHeight, CaptionHeight + Height)
    RECT rect(const RECT& clientRect) const;
    // 四按钮几何（绘制/命中同一来源；客户区坐标）
    void buttonRects(const RECT& clientRect, RECT out[4]) const;

    // —— 状态 ——
    void setSaveEnabled(bool enabled);  // 无活动文档时保存禁用
    void setWrapEnabled(bool enabled);  // 自动换行激活态（F2；组装层与 g_wordWrap 同步）

    // —— 交互（组装层转发；坐标为客户区）——
    void onMouseMove(POINT clientPt);
    void onMouseLeave();
    void onLButtonDown(POINT clientPt);
    void onTimer(UINT_PTR timerId);  // tooltip 延迟
    void onThemeChanged();           // E5：主题切换（tooltip 换肤；绘制色动态取）

    // —— 绘制（WM_PAINT；主窗口已涂内容底色，本层画工具栏条带与按钮）——
    void paint(HDC dc, const RECT& clientRect);

    // —— 测试钩子 ——
    int hitButton(POINT clientPt, const RECT& clientRect) const;  // -1 = 空白（含组间间隔区）
    bool tooltipVisibleForTest() const { return tipVisible_; }    // F5 tooltip 链路断言用
    // tooltip 控件窗口句柄（转发 ManualTooltip）：回归钉用——外部发
    // TTM_GETTOOLCOUNT 断言注册成功、IsWindowVisible 断言系统级真实可见
    HWND tooltipHwndForTest() const { return tooltip_.tooltipWindowForTest(); }

    // —— 挂接点 ——
    std::function<void(Command)> onCommand;  // 按钮 → 命令（New/Open/Save/Wrap）

private:
    void drawIcon(HDC dc, Button button, const RECT& iconRect, COLORREF color,
                  bool colorful) const;  // GDI+/回退分发（colorful 仅供 GDI+ 彩色填充）
    void drawIconGdiplus(HDC dc, Button button, const RECT& iconRect, COLORREF stroke,
                         bool colorful) const;
    void drawIconGdiFallback(HDC dc, Button button, const RECT& iconRect, COLORREF color) const;
    void showTooltipFor(Button button);
    void hideTooltip();
    void invalidateButton(Button button) const;

    HWND hwnd_ = nullptr;
    bool saveEnabled_ = true;
    bool wrapActive_ = false;  // 换行 toggle 激活态（DEC-006 激活视觉依据）
    int hover_ = -1;   // hover 按钮下标（-1 无）
    int pressed_ = -1;
    UINT_PTR tipTimerId_ = 3;  // 主窗口宿主 id 分配：1 会话轮询/2 TabStrip/3 工具栏/4 状态栏/5 转发重试（见 main.cpp）
    bool tipTimerArmed_ = false;
    bool tipVisible_ = false;
    Button tipButton_ = Button::New;
    ManualTooltip tooltip_;
};

}  // namespace ui
