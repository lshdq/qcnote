// qcnote Win11 连通式 Tab 条（M1-3，A2/A3）。
//
// 三层结构（前两层纯函数可单测，第三层为有状态组件）：
//   DistributeTabWidths        —— 宽度自适应算法（照旧 QML DocumentTabs 的 DEC 规格移植：
//                                  下限 100、单 Tab 上限 max(100, W/4)、按可收缩量等比收缩、
//                                  结果向下取整、饱和回退最小宽 + 横向滚动）
//   ComputeTabStripGeometry    —— 几何纯函数：各 Tab 本体/文字/● 标记矩形、分隔线可见性、
//                                  内容宽/溢出判定/最大滚动量（布局/命中/绘制三方共用）
//   TabDragSwapIndex           —— 拖拽中点交换判定纯函数（链式相邻交换的历史口径，保留供
//                                  原单测覆盖；拖拽路径已改用 TabDragInsertIndex——DEC-002）
//   TabDragInsertIndex         —— 拖拽插入槽位判定纯函数（DEC-002 压缩流中点）：抽掉被拖
//                                  槽位后的邻居静息位置，光标严格越过压缩中点即计入
//   ComputeTabDragOffsets      —— 拖拽让位偏移纯函数：被拖 Tab = 跟随偏移；起始/插入槽位
//                                  之间的邻居 ±(被拖宽+间距)，区间外 0
//   ApproachPx                 —— 指数趋近缓动纯函数：每帧趋近剩余距离的固定比例，
//                                  |Δ|≤落位阈值时精确取目标（单调不越目标、有限帧收敛）
//   TabStrip                   —— 数据（TabItem mock，M2 换文档系统）+ 绘制 + 交互
//                                  （激活点击/右键菜单/键盘导航/滚轮滚动/tooltip/
//                                  拖拽重排：视觉偏移式（DEC-001，拖拽期间 items_/
//                                  active/focus 零改动）——越阈值拖起后被拖 Tab 水平
//                                  跟随光标（钳制条内），邻居经 WM_TIMER 缓动让位
//                                  （DEC-003），松手顺序有变才一次 onTabReordered(from,
//                                  insert) 提交（直接落位）；Esc/捕获丢失/失焦统一取消
//                                  收口（清状态 + 停表 + 重绘回原位，不回调）——详见
//                                  tabs.cpp 头注释）
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
    bool hasPath = false;  // 文档有磁盘路径（url 非空）——右键菜单「文件重命名…/
                           // 打开所在文件夹」可用性；新增字段带默认值，既有
                           // {name, modified} 聚合初始化不受影响
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
constexpr int LabelPixelSize = 12;     // Tab 标签字号：12px = 9pt@96dpi（系统标准；
                                       // 中文全 em 字形视觉更重，13 偏大）。测量与渲染共用
constexpr int Radius = 5;              // 活动 Tab 顶部圆角半径
constexpr int HoverRadius = 4;         // 非活动 Tab 悬停高亮圆角
constexpr int FocusRadius = 4;         // 键盘焦点环圆角
constexpr int FocusInset = 1;          // 焦点环内缩
constexpr int FocusPenWidth = 2;       // 焦点环线宽
constexpr int SeparatorHeight = 20;    // 标签间竖分隔线高（≈标签高 34 的 60%，垂直居中）
constexpr int SeparatorWidth = 1;      // 竖分隔线宽
constexpr int TooltipDelayMs = 600;    // tooltip 悬停延迟（旧 QML ToolTip.delay）
constexpr int WheelStep = 60;          // 滚轮每格滚动步长（逻辑像素）
constexpr int DragEdgeZone = 20;       // 拖拽溢出自动滚动的条区左右边缘带宽（逻辑像素；
                                       // 需求「约 20」：约 1/5 最小宽 Tab，光标进入即
                                       // 步进一级，停住即停——移动驱动，无定时器）
constexpr int DragEdgeScrollStep = 60; // 边缘带内每次鼠标移动的滚动步长（逻辑像素）。
                                       // 取滚轮 WheelStep 同量级（≈2/3 个最小宽 Tab）：
                                       // 每步可感知又不跳跃

constexpr double DragAnimApproachRatio = 0.45;  // 邻居让位动画每帧指数趋近比例（DEC-003）：
                                                // 10ms 请求节拍下 ~7 帧收敛到 1px 阈值
                                                // （≈100-150ms，对齐 Chrome 手感）；帧率波动
                                                // 只影响收敛快慢、不影响终点
constexpr int DragAnimTimerMs = 10;     // 让位动画 WM_TIMER 请求间隔（系统实际派发
                                        // ~15.6ms ≈ 64fps；收敛即停表，无残留计时器）
constexpr int DragAnimSnapPx = 1;       // 让位动画精确落位阈值（|Δ|≤1px 直接取目标，
                                        // 保证有限帧内终止——ApproachPx 共用）
}  // namespace tab_metrics

// 拖拽让位动画计时器 id（主窗口宿主 id 分配：1 会话轮询 / 2 Tab tooltip / 3 工具栏 /
// 4 状态栏 / 5 转发重试 / 6 本表——单窗口内互斥，见 main.cpp 分配表）。头文件公开供
// 单测/selftest 合成节拍直驱 onTimer（DEC-004 测试缝的同源约定）。
constexpr UINT_PTR kDragAnimTimerId = 6;

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

// 拖拽中点交换判定（纯函数，链式相邻交换的历史口径——DEC-002 保留：原单测继续
// 覆盖，拖拽路径已改用下方 TabDragInsertIndex）：dragIndex 为被拖 Tab 当前下标，
// clientX 为光标 x（窗口客户区坐标）。光标严格越过相邻 Tab bounds（已含滚动偏移）
// 中点 → 返回应与之交换的相邻下标（dragIndex-1 / dragIndex+1）；否则 -1（同位
// 未越过、首下标向左、末下标向右、dragIndex 越界、空几何均不交换）。
int TabDragSwapIndex(const TabStripGeometry& geo, int dragIndex, int clientX);

// 拖拽插入槽位判定（纯函数，DEC-002 压缩流中点）：dragIndex 为被拖 Tab 下标
// （视觉偏移式下数据零改动，恒为按下时下标），clientX 为光标 x（窗口客户区
// 坐标）。把被拖槽位从流中抽掉：i<dragIndex 的邻居静息位置即 bounds[i] 本身；
// i>dragIndex 的邻居左移 (被拖宽+间距)。插入槽位 = 压缩中点严格小于 clientX
// 的邻居个数（无状态单阈值跨线，同点跨入跨出天然无振荡）。dragIndex 越界直接
// 返回钳制到 [0, n-1] 的 dragIndex（不做判定）；空几何返回 0（纯防御——正常
// 路径 dragFromIndex_ 恒有效）。
int TabDragInsertIndex(const TabStripGeometry& geo, int dragIndex, int clientX);

// 拖拽让位偏移（纯函数）：返回长度 n 的每 Tab 视觉目标偏移——i==fromIndex 为
// followDx（被拖 Tab 跟随光标的偏移，直接生效不缓动）；i∈(from, insert] 的邻居
// 为 -(被拖宽+间距)（右邻左移让位）；i∈[insert, from) 的邻居为 +(被拖宽+间距)
// （左邻右移让位）；其余 0。被拖宽与间距均从 geo 推导；from/insert 越界钳制到
// [0, n-1]（空几何返回空数组）。
std::vector<int> ComputeTabDragOffsets(const TabStripGeometry& geo, int fromIndex,
                                       int insertIndex, int followDx);

// 指数趋近缓动（纯函数）：|target−current| ≤ tab_metrics::DragAnimSnapPx 时精确
// 返回 target（终止性）；否则 current + round(Δ×ratio)。ratio∈(0,1)（越界钳制）
// 单调收敛不越目标；步进取整退化为 0 时至少前进 1px——任意合法比例均有限帧收敛。
int ApproachPx(int current, int target, double ratio);

// ---------------------------------------------------------------------------
// 右键菜单与无障碍（纯函数）
// ---------------------------------------------------------------------------

// A3：关闭当前文件 / 关闭右边所有 (N) / 关闭左边所有 (N) / 关闭其他文件 (N)。
// 数量为动态文本；无可关闭对象时置灰不隐藏（对齐旧 QML enabled 规则）。
// 尾部分隔线后接「文件重命名…」「打开所在文件夹」——仅当该标签有磁盘路径
// （item.hasPath）时可用，未命名文档置灰不隐藏（与批量关闭置灰约定一致）。
MenuModel BuildTabContextMenu(const TabItem& item, int index, int totalCount);

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
    /// hasPath 写入（文档 url 空↔非空翻转，如未命名→另存为）：越界防御；
    /// 变化频率低，仅影响右键菜单可用性不参与测量，但同值短路避免无效置脏重绘
    /// @return true = 实际变化（已置脏失效），false = 同值或越界（无操作）
    bool setHasPath(int index, bool hasPath);
    /// 显示名写入；同值短路返回 false（不置脏不重绘）——选区拖拽路径每帧经
    /// OnDocumentMetadataChanged 到达此处，同值必须消除无效置脏
    /// @return true = 实际变化（已置脏失效），false = 同值或越界（无操作）
    bool setDisplayName(int index, const std::wstring& name);
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
    bool dragging() const { return dragging_; }     // 拖拽态（组装层消息路由查询）
    int dragInsertIndex() const {                  // 插入槽位（压缩流中点判定；提交 to；
        return dragInsertIndex_;                    // 非拖拽态 -1——DEC-004 只读测试缝）
    }
    std::vector<int> dragVisualOffsets() const;     // 每 Tab 当前视觉偏移完整数组（被拖=
                                                    // 跟随值，邻居=动画当前值；非拖拽态全 0）

    // —— 命中（窗口客户区坐标；供 main.cpp 的 NCHITTEST/鼠标路由）——
    int hitTab(POINT clientPt) const;               // Tab 本体 → index；空白 -1
    bool hitInteractive(POINT clientPt) const;      // Tab 本体或溢出滚动区（→HTCLIENT）

    // —— 交互（main.cpp 转发；坐标为客户区）——
    void onMouseMove(POINT clientPt);
    void onMouseLeave();                            // 客户区/非客户区离开均走此（清 hover + tooltip；
                                                    // 拖拽态不取消——捕获期间光标短暂离窗是正常拖拽）
    void onLButtonDown(POINT clientPt);             // 命中 Tab → 激活（down 即激活）+ 记录按下
    void onLButtonUp(POINT clientPt);               // 松手：拖拽结束、顺序有变才一次提交；
                                                    // 未拖拽仅清按下记录（no-op 安全）
    void onRButtonDown(POINT clientPt);             // 命中 Tab → 右键菜单（不激活该 Tab）
    void onMouseWheel(bool horizontal, int delta);  // 溢出时横向滚动（竖直轮映射为横向）
    bool onKeyDown(UINT vk, bool shiftDown);        // 键盘导航；返回是否消费
    void onTimer(UINT_PTR timerId);                 // tooltip 延迟计时
    void onCaptureChanged();                        // 鼠标捕获丢失：取消拖拽（清状态+停表+重绘回原位，不回调）
    void onWindowKillFocus();                       // 窗口失焦：取消拖拽（如在拖）+ 隐藏键盘焦点环
    void onDpiChanged();
    void onLayoutChanged();                         // 窗口尺寸变化：钳制滚动 + 重绘
    void onThemeChanged();                          // E5：主题切换（tooltip 换肤 + 重绘）

    // —— 绘制（WM_PAINT；标题栏底色已由 TitleBar 画好，本层只画 Tab 内容）——
    void paint(HDC dc);

    // —— 挂接点（main.cpp 组装）——
    std::function<void(int)> onTabActivated;                // 激活回调（鼠标/键盘）
    std::function<void(int, Command)> onTabContextCommand;  // 右键菜单命令（index + 命令）
    std::function<void(int, int)> onTabReordered;           // 拖拽重排提交：松手时顺序有变才触发
                                                            // 一次（from=按下时下标，to=最终插入槽位）

private:
    void markDirtyAndInvalidate();
    void recomputeNatural() const;   // 惰性：测量文字/标记宽 → naturalWidths_
    void ensureFonts() const;
    void activate(int index);        // 激活 + 回调
    void moveFocus(int delta);       // 循环移动键盘焦点
    void scrollIntoView(int index);  // 滚动使指定 Tab 完整可见（QML Contain 语义）
    // —— 拖拽重排状态机（视觉偏移式 + 松手一次提交，DEC-001/002/003）——
    void beginDrag();                   // 位移越阈值：进入拖拽（捕获 + 悬停/tooltip 让位 +
                                        // 初始化跟随/让位状态 + 起动画表）
    void dragMove(POINT clientPt);      // 拖拽中 move：按钮防呆收口/x 钳制/边缘滚动/
                                        // 插入槽位判定/跟随钳制/让位目标更新
    void cancelDrag();                  // 统一取消收口：清状态 + 停表 + 释放捕获，不回调
    void clearDragState();              // 清全部拖拽/按下状态与动画表（不触捕获、不回调）
    void resetPressAndDragState();      // 外部数据重建：安全终止（不回调）
    void armDragAnimTimer();            // 起让位动画表（10ms；无 hwnd 跳过）
    void killDragAnimTimer();           // 停让位动画表（仅自己持有才杀）
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
    // —— 拖拽重排状态（命名对齐 hoverIndex_ 风格；视觉偏移式：数据零改动）——
    int pressIndex_ = -1;           // 左键按下命中的 Tab 下标（拖拽判定起点；-1 无按下）
    POINT pressPt_{};               // 按下位置（位移阈值基准点）
    bool pressRealButton_ = false;  // 本次按下源自真实按钮（down 时刻 GetKeyState 采样；
                                    // selftest/单测的合成驱动为 false——按钮防呆据此豁免）
    bool dragging_ = false;         // 拖拽态：位移越阈值进入，松手提交/取消收口退出
    int dragFromIndex_ = -1;        // 拖拽起始下标（= 被拖 Tab 下标：数据零改动不随迁；
                                    // 提交 from / 跟随槽位锚点）
    int dragInsertIndex_ = -1;      // 插入槽位（压缩流中点判定；提交 to；非拖拽 -1）
    int dragGrabX_ = 0;             // beginDrag 时抓取点距槽左缘的物理偏移（跟随锚定）
    int followDx_ = 0;              // 被拖 Tab 跟随偏移（直接生效，不缓动；仅横向）
    std::vector<int> dragVisualDx_; // 邻居当前视觉偏移（动画值；被拖项同步为 followDx_）
    std::vector<int> dragTargetDx_; // 邻居目标偏移（让位/回位终点）
    bool dragAnimArmed_ = false;    // 让位动画表在跑（仅自己持有才杀）
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
