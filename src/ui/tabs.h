// qcnote Win11 连通式 Tab 条（M1-3，A2/A3）。
//
// 三层结构（前两层纯函数可单测，第三层为有状态组件）：
//   DistributeTabWidths        —— 宽度自适应算法（照旧 QML DocumentTabs 的 DEC 规格移植：
//                                  下限 100、单 Tab 上限 max(100, W/4)、按可收缩量等比收缩、
//                                  结果向下取整、饱和回退最小宽 + 横向滚动）
//   ComputeTabStripGeometry    —— 几何纯函数：各 Tab 本体/文字/● 标记矩形、分隔线可见性、
//                                  内容宽/溢出判定/最大滚动量（布局/命中/绘制三方共用）
//   TabStrip                   —— 数据（TabItem mock，M2 换文档系统）+ 绘制 + 交互
//                                  （激活点击/右键菜单/键盘导航/滚轮滚动/tooltip）
//
// 命中链整合：Tab 本体与「溢出滚动区」归 HTCLIENT（main.cpp 在 ComputeCaptionHitTest
// 返回 HTCAPTION 后追加 hitInteractive 判定）；未溢出时空白区维持 HTCAPTION（可拖动
// 窗口/双击最大化）——对齐旧 QML tabsOverflow 语义。
//
// 右键菜单复用 MenuHost/MenuModel（ui/menu）：命令经 track 返回值 → onTabContextCommand
// 上抛（组装层接 A3 关闭流程），期间 uiLocked 语义走 MenuHost onOpened/onClosed。
#pragma once

#include <windows.h>
#include <commctrl.h>  // TOOLINFO / TOOLTIPS_CLASSW（Tab tooltip）

#include <functional>
#include <string>
#include <vector>

#include "commands.h"
#include "menu.h"  // MenuModel（右键菜单构建）
#include "theme.h"

namespace ui {

// —— Tab 数据项（M1-3 mock；M2 接文档系统时扩展为文档 id + 查询回调）——
struct TabItem {
    std::wstring displayName;
    bool modified = false;
};

// —— 尺寸 token（逻辑像素，96 DPI 基准；使用处经 Scale(dpi) 换算）——
// 取值照旧 QML DocumentTabs.qml 单点定义的装饰尺寸常量，禁止在别处出现魔法数字。
namespace tab_metrics {

constexpr int Height = 34;             // Tab 高（旧 QML DocumentTabs height，贴标题栏底）
constexpr int Spacing = 2;             // Tab 间距（gaps = Spacing×(N−1)）
constexpr int MinWidth = 100;          // Tab 最小宽（固定下限）
constexpr int LeftMarginWithMark = 8;  // 有未保存 ● 标记时的左边距
constexpr int LeftMarginPlain = 10;    // 无标记时的左边距
constexpr int MarkToLabelSpacing = 5;  // ● 标记与标签文字的间距
constexpr int RightPadding = 10;       // 标签文字右内边距
constexpr int MarkPixelSize = 9;       // ● 标记字号
constexpr int LabelPixelSize = 13;     // Tab 标签字号（测量与渲染共用同一字体）
constexpr int Radius = 5;              // 活动 Tab 顶部圆角半径
constexpr int HoverRadius = 4;         // 非活动 Tab 悬停高亮圆角
constexpr int FocusRadius = 4;         // 键盘焦点环圆角
constexpr int FocusInset = 1;          // 焦点环内缩
constexpr int FocusPenWidth = 2;       // 焦点环线宽
constexpr int SeparatorHeight = 20;    // 标签间竖分隔线高（≈标签高 34 的 60%，垂直居中）
constexpr int SeparatorWidth = 1;      // 竖分隔线宽
constexpr int TooltipDelayMs = 600;    // tooltip 悬停延迟（旧 QML ToolTip.delay）
constexpr int WheelStep = 60;          // 滚轮每格滚动步长（逻辑像素）

}  // namespace tab_metrics

// ---------------------------------------------------------------------------
// 宽度自适应（纯函数；输入自然宽、输出分配宽，均物理像素）
// ---------------------------------------------------------------------------

// 算法（照旧 QML 规格，A2）：
//   1) 单 Tab 动态上限 cap = max(Scale(MinWidth), floor(W/4))，极窄窗口退化为下限；
//   2) n_i 夹取到 [min, cap]（自然宽度不足下限时抬到下限，恒有 min ≤ w_i）；
//   3) 空间充足（Σn_i + gaps ≤ W）：各取自然宽；
//   4) 空间不足：缺口 D 按各 Tab 可收缩量 (n_i − min) 等比分配，
//      w_i = floor(n_i − D/C × (n_i − min))——结果向下取整，保证 Σw_i + gaps ≤ W
//      恒成立（contentWidth 不因舍入虚增被误判溢出/滚动条闪烁）；
//   5) 收缩饱和（D ≥ C）：全部取最小宽，由横向滚动接管。
std::vector<int> DistributeTabWidths(const std::vector<int>& naturalWidths, int areaWidth,
                                     UINT dpi);

// ---------------------------------------------------------------------------
// 几何（纯函数；宽度由 DistributeTabWidths 产出，markWidthPx 为 ● 标记实测宽）
// ---------------------------------------------------------------------------

struct TabGeometry {
    RECT bounds{};              // Tab 本体（窗口客户区坐标，已含滚动偏移）
    RECT label{};               // 标签文字区（超宽 DT_END_ELLIPSIS 截断）
    RECT mark{};                // ● 标记区（无标记时空矩形）
    bool hasMark = false;
    bool separatorLeft = false;  // 左缘竖分隔线可见（index>0 且本标签非活动且左邻非活动）
};

struct TabStripGeometry {
    std::vector<TabGeometry> tabs;
    RECT area{};                // Tab 条可用区（= TitleBarLayout.tabArea，y 贴标题栏底）
    int contentWidth = 0;       // Σ宽 + gaps
    int viewWidth = 0;          // 可视宽（area 宽）
    int maxScroll = 0;          // 最大滚动偏移（contentWidth − viewWidth，不小于 0）
    bool overflow = false;      // contentWidth > viewWidth
};

// items 与 widths 等长传入（内部取小者防御）；scrollOffset 由调用方钳制后传入。
TabStripGeometry ComputeTabStripGeometry(const std::vector<TabItem>& items, int activeIndex,
                                         const std::vector<int>& widths, int markWidthPx,
                                         RECT tabArea, UINT dpi, int scrollOffset);

// 命中（窗口客户区坐标）：Tab 本体 → 下标；空白区 → -1。
int TabStripHitTest(const TabStripGeometry& geo, POINT clientPt);

// ---------------------------------------------------------------------------
// 右键菜单与无障碍（纯函数）
// ---------------------------------------------------------------------------

// A3：关闭当前文件 / 关闭右边所有 (N) / 关闭左边所有 (N) / 关闭其他文件 (N)。
// 数量为动态文本；无可关闭对象时置灰不隐藏（对齐旧 QML enabled 规则）。
MenuModel BuildTabContextMenu(int index, int totalCount);

// 无障碍名称：未保存状态入名（A2：「readme.md，未保存」）。
std::wstring AccessibleTabName(const TabItem& item);

// ---------------------------------------------------------------------------
// Tab 条组件（mock 数据驱动；M2 接真实文档系统）
// ---------------------------------------------------------------------------

class TabStrip {
public:
    void attach(HWND hwnd, MenuHost* menuHost);
    void detach();

    // —— 数据接口（M2 换真实文档系统；当前 mock）——
    void setItems(std::vector<TabItem> items);      // 整表替换（active 尽量保留）
    int addTab(const TabItem& item);                // 追加（空表激活首项，否则激活不变）
    void removeTab(int index);                      // 移除并钳制 active/focus/scroll
    void removeRange(int from, int to);             // 移除 [from, to)
    void setModified(int index, bool modified);
    void setDisplayName(int index, const std::wstring& name);
    int count() const { return static_cast<int>(items_.size()); }
    int activeIndex() const { return activeIndex_; }
    bool setActiveIndex(int index);                 // 程序化激活（不触发回调；越界 false）
    const TabItem& item(int index) const;           // 越界返回空项（防御弹窗期索引失效）

    // —— 几何/查询 ——
    RECT areaRect() const;                          // = ComputeTitleBarLayout(client).tabArea
    TabStripGeometry geometry() const;              // 惰性测量 + 宽度分配 + 几何
    bool overflow() const { return geometry().overflow; }
    int scrollOffset() const { geometry(); return scrollOffset_; }
    int focusIndex() const { return focusIndex_; }  // 键盘焦点（仅指示，非活动文档）

    // —— 命中（窗口客户区坐标；供 main.cpp 的 NCHITTEST/鼠标路由）——
    int hitTab(POINT clientPt) const;               // Tab 本体 → index；空白 -1
    bool hitInteractive(POINT clientPt) const;      // Tab 本体或溢出滚动区（→HTCLIENT）

    // —— 交互（main.cpp 转发；坐标为客户区）——
    void onMouseMove(POINT clientPt);
    void onMouseLeave();                            // 客户区/非客户区离开均走此（清 hover + tooltip）
    void onLButtonDown(POINT clientPt);             // 命中 Tab → 激活（down 即激活）
    void onRButtonDown(POINT clientPt);             // 命中 Tab → 右键菜单（不激活该 Tab）
    void onMouseWheel(bool horizontal, int delta);  // 溢出时横向滚动（竖直轮映射为横向）
    bool onKeyDown(UINT vk, bool shiftDown);        // 键盘导航；返回是否消费
    void onTimer(UINT_PTR timerId);                 // tooltip 延迟计时
    void onWindowKillFocus();                       // 窗口失焦：隐藏键盘焦点环
    void onDpiChanged();
    void onLayoutChanged();                         // 窗口尺寸变化：钳制滚动 + 重绘
    void onThemeChanged();                          // E5：主题切换（tooltip 换肤 + 重绘）

    // —— 绘制（WM_PAINT；标题栏底色已由 TitleBar 画好，本层只画 Tab 内容）——
    void paint(HDC dc);

    // —— 挂接点（main.cpp 组装）——
    std::function<void(int)> onTabActivated;                // 激活回调（鼠标/键盘）
    std::function<void(int, Command)> onTabContextCommand;  // 右键菜单命令（index + 命令）

private:
    void markDirtyAndInvalidate();
    void recomputeNatural() const;   // 惰性：测量文字/标记宽 → naturalWidths_
    void ensureFonts() const;
    void activate(int index);        // 激活 + 回调
    void moveFocus(int delta);       // 循环移动键盘焦点
    void scrollIntoView(int index);  // 滚动使指定 Tab 完整可见（QML Contain 语义）
    void openContextMenu(int index, POINT clientPt);
    void openContextMenuAtFocus();   // Menu 键 / Shift+F10
    void armTooltipTimer();
    void hideTooltip();
    void showTooltip();
    TOOLINFO makeToolInfo() const;
    void invalidateArea() const;

    HWND hwnd_{};
    MenuHost* menuHost_{};
    std::vector<TabItem> items_;
    int activeIndex_ = -1;
    int focusIndex_ = -1;
    int hoverIndex_ = -1;
    // 键盘焦点环可见性（对齐 QML activeFocus 语义）：仅键盘导航后显示，鼠标点击/
    // 窗口失焦隐藏；focusIndex 本身始终维护（键盘导航的起点状态）
    bool focusVisible_ = false;
    mutable std::vector<int> naturalWidths_;
    mutable int markWidthPx_ = 0;
    mutable bool dirty_ = true;        // 数据/DPI 变化 → 重测自然宽
    mutable HFONT labelFont_ = nullptr;
    mutable HFONT markFont_ = nullptr;
    mutable UINT fontDpi_ = 0;
    mutable int scrollOffset_ = 0;
    HWND tooltip_ = nullptr;
    bool tipTimerArmed_ = false;
    bool tipVisible_ = false;
    std::wstring tipText_;             // tooltip 文本缓冲（控件持有指针期间须存活）
};

}  // namespace ui
