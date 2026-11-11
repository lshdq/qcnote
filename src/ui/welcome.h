// qcnote 欢迎页（M1-3，A5）：无文档时客户区中央显示应用名 + 一行说明 +
// 「打开文件…」文字按钮。有文档时不绘制（组装层按 TabStrip::count()==0 显隐）。
//
// 按钮为文字按钮：accent 色文字、悬停半透明高亮、键盘焦点环（Tab 聚焦、
// Enter/Space 触发）——状态表达非纯颜色（高亮块 + 焦点环形状线索）。
// 点击经 onOpenClicked 回调 → RouteCommand(Command::OpenDialog)（M2 接文件对话框）。
#pragma once

#include <windows.h>

#include <functional>

#include "theme.h"

namespace ui {

// —— 尺寸 token（逻辑像素，96 DPI 基准；使用处经 Scale(dpi) 换算）——
namespace welcome_metrics {

constexpr int TitlePixelSize = 44;   // 应用名字号（旧 QML WelcomePage 44）
constexpr int DescPixelSize = 14;    // 说明文字字号
constexpr int ButtonPixelSize = 14;  // 按钮文字字号
constexpr int TitleToDescGap = 20;   // 标题 → 说明 间距（旧 QML Column spacing 20）
constexpr int DescToButtonGap = 20;  // 说明 → 按钮 间距
constexpr int ButtonPaddingH = 16;   // 按钮文字左右内边距
constexpr int ButtonPaddingV = 8;    // 按钮文字上下内边距
constexpr int CornerRadius = 4;      // 悬停高亮/焦点环圆角
constexpr int FocusInset = 2;        // 焦点环内缩
constexpr int FocusPenWidth = 2;     // 焦点环线宽

}  // namespace welcome_metrics

class WelcomePage {
public:
    void attach(HWND hwnd);
    void detach();

    // M2-B：工具栏/状态栏占位（逻辑高度；内容区 = 客户区 − 标题栏 − 上下 chrome）。
    // 默认 0（无 chrome，等价 M1 行为）
    void setChromeInsets(int toolbarHeightLogical, int statusBarHeightLogical);

    void reset();  // 离开欢迎态（出现文档）时清 hover/焦点

    // —— 几何（绘制/命中同一来源）——
    RECT buttonRect() const;  // 客户区坐标；窗口不存在时空矩形
    bool hitButton(POINT clientPt) const;

    // —— 交互（main.cpp 转发；仅无文档时转发到本组件）——
    void onMouseMove(POINT clientPt);
    void onMouseLeave();
    void onLButtonDown(POINT clientPt);
    bool onKeyDown(UINT vk, bool shiftDown);  // Tab 聚焦 / Enter、Space 触发；返回是否消费

    // —— 绘制（WM_PAINT；整片内容区重涂 contentBackground）——
    void paint(HDC dc, RECT const& clientRect);

    // —— 测试钩子 ——
    bool buttonFocusedForTest() const { return focused_; }

    // —— 挂接点 ——
    std::function<void()> onOpenClicked;  // 「打开文件…」→ RouteCommand(OpenDialog)

private:
    struct Layout {
        RECT content{};  // 内容区（客户区去标题栏）
        RECT title{};
        RECT desc{};
        RECT button{};
        bool valid = false;  // 内容区过矮时不绘制
    };

    Layout computeLayout(RECT const& clientRect) const;
    void ensureFontsAndMetrics() const;
    void triggerOpen();
    void invalidateButton() const;

    HWND hwnd_{};
    bool hover_ = false;
    bool focused_ = false;
    int toolbarInset_ = 0;  // 逻辑高度（工具栏）
    int statusInset_ = 0;   // 逻辑高度（状态栏）
    mutable HFONT titleFont_ = nullptr;
    mutable HFONT bodyFont_ = nullptr;
    mutable UINT fontDpi_ = 0;
    mutable int titleH_ = 0;   // 文字高度缓存（随 DPI 重测）
    mutable int descH_ = 0;
    mutable int btnTextW_ = 0;  // 「打开文件…」文字宽
    mutable int btnTextH_ = 0;
};

}  // namespace ui
