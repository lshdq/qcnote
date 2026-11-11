// qcnote 工具栏（M2-B，E3）：标题栏下方一条（toolbarBackground），
// 左侧高频三按钮：新建/打开/保存。图标 GDI 自绘线稿（旧 SVG 资产不可用，
// 语义对齐旧 icons：new=白页+折角、open=掀盖文件夹、save=软盘）。
//
// 交互语义（旧 QML Toolbar 对等）：悬停 hover 半透明高亮、按下 pressed、
// 保存按钮无活动文档时禁用（textDisabled 图标可辨识）；tooltip 延迟 600ms
// （含快捷键提示，禁用态文案「当前没有文档」）；无障碍名与按钮文本一致。
// 键盘进入（Tab/方向键）为旧版能力，M2 暂不移植（焦点在编辑器/欢迎页；
// 命令均可经快捷键触达，无功能缺失）——记录于里程碑报告。
//
// 自绘组件（非子窗口）：绘制/命中共用同一几何纯函数 geometry()，鼠标消息
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
constexpr int Spacing = 2;        // 按钮间距（旧 QML Row spacing 2）
constexpr int IconSize = 16;      // 图标画布 16×16 逻辑像素
constexpr int TooltipDelayMs = 600;

}  // namespace toolbar_metrics

class Toolbar {
public:
    enum class Button { New = 0, Open = 1, Save = 2 };

    void attach(HWND hostWindow);
    void detach();

    // 工具栏区域（客户区坐标）：[CaptionHeight, CaptionHeight + Height)
    RECT rect(const RECT& clientRect) const;
    // 三按钮几何（绘制/命中同一来源；客户区坐标）
    void buttonRects(const RECT& clientRect, RECT out[3]) const;

    // —— 状态 ——
    void setSaveEnabled(bool enabled);  // 无活动文档时保存禁用

    // —— 交互（组装层转发；坐标为客户区）——
    void onMouseMove(POINT clientPt);
    void onMouseLeave();
    void onLButtonDown(POINT clientPt);
    void onTimer(UINT_PTR timerId);  // tooltip 延迟
    void onThemeChanged();           // E5：主题切换（tooltip 换肤；绘制色动态取）

    // —— 绘制（WM_PAINT；主窗口已涂内容底色，本层画工具栏条带与按钮）——
    void paint(HDC dc, const RECT& clientRect);

    // —— 测试钩子 ——
    int hitButton(POINT clientPt, const RECT& clientRect) const;  // -1 = 空白

    // —— 挂接点 ——
    std::function<void(Command)> onCommand;  // 按钮 → 命令（New/Open/Save）

private:
    void drawIcon(HDC dc, Button button, const RECT& iconRect, COLORREF color) const;
    void showTooltipFor(Button button);
    void hideTooltip();
    void invalidateButton(Button button) const;

    HWND hwnd_ = nullptr;
    bool saveEnabled_ = true;
    int hover_ = -1;   // hover 按钮下标（-1 无）
    int pressed_ = -1;
    UINT_PTR tipTimerId_ = 3;  // 主窗口宿主 id 分配：1 会话轮询/2 TabStrip/3 工具栏/4 状态栏/5 转发重试（见 main.cpp）
    bool tipTimerArmed_ = false;
    bool tipVisible_ = false;
    Button tipButton_ = Button::New;
    ManualTooltip tooltip_;
};

}  // namespace ui
