// qcnote 文档列表面板（任务 B，对齐 Notepad++ Document List：左侧平铺列表显示
// 全部打开文档，点击切换）。
//
// 三层结构（对齐 tabs.h 范式：前两层纯函数可单测，第三层为有状态组件）：
//   ComputeDocListGeometry   —— 几何纯函数：条目行矩形数组、自绘滚动条（轨道/
//                               比例滑块/上下箭头按钮矩形）、contentHeight/
//                               viewHeight/maxScroll/overflow 判定（布局/命中/
//                               绘制三方共用同一几何源，全部物理像素，DPI 由
//                               调用方换算传入）
//   DocListHitItem           —— 条目命中纯函数（→index；空白/滚动条区/面板外 -1）
//   DocListHitSplitter       —— 右缘分隔条热区判定纯函数（SplitterWidth 逻辑像素）
//   ClampDocListScroll       —— 滚动偏移钳制 [0, maxScroll]
//   ClampDocListWidthLogical —— 面板宽钳制 [MinWidth, 上界]（逻辑像素；上界 =
//                               内容区宽/3，由调用方传入。上界小于下限的极窄
//                               窗口取下限——最小宽是硬下界，窗口收缩由组装层
//                               布局兜底）
//   AccessibleDocListName    —— 无障碍名称（未保存入名，对齐 Tab 条口径）
//   DocListPanel             —— 状态组件：数据（与 TabStrip 同源语义）+ 绘制 +
//                               交互（点击激活/右键菜单复用 BuildTabContextMenu/
//                               溢出滚轮 + 自绘滚动条箭头-轨道-滑块/分隔条拖拽
//                               调宽/tooltip 悬停 600ms）
//
// 面板本体 = 组装层传入内容区（工具栏底~状态栏顶，未收缩矩形）的左切片（宽 =
// Scale(widthLogical_, dpi)，逻辑像素 96 DPI 基准，随会话持久化跨 DPI 稳定）；
// layout() 记录宽度上界（内容区宽/3，逻辑像素）并幂等重算。
//
// 分隔条拖拽调宽（语义对齐 tabs.cpp 拖拽状态机与 CaptionPressSession 收口手法，
// 防 mouse-up 穿透）：down 于右缘热区 → 记录起始宽 + 起点并 SetCapture(主窗口)；
// move 中 newWidth = 起始宽 + (dx 物理)×96/dpi 经钳制生效、每变化一次
// onWidthChanged；真实按钮会话在 move 中防呆检测左键已松（up 丢失）按取消收口，
// 合成驱动（单测/selftest）豁免；up 提交（最终 onWidthChanged）；Esc/捕获丢失/
// 失焦统一取消回原宽（经 onWidthChanged 通知，组装层据此恢复编辑区布局）。
//
// tooltip：悬停 600ms（宿主 timer id = kDocListTooltipTimerId = 8，主窗口宿主
// id 分配表 1-7 已被会话轮询/Tab/工具栏/状态栏/转发重试/拖拽动画/文件检测占用）
// ——hasPath 显示完整路径，否则显示显示名（对齐 Tab tooltip 语义，未保存附
// 「（未保存）」）；离开/滚动/数据变化隐藏。实现参照 tabs.cpp 的
// TOOLTIPS_CLASSW 手动 TRACK 模式（组件持有独立 tooltip 控件窗口）。
//
// 命中链整合：面板本体（含条目/滚动条/分隔条/空白）归 hitInteractive（组装层
// 鼠标路由判定——面板空白不穿透到编辑区）；鼠标消息由 main.cpp 按区域转发，
// 本组件为纯 GDI 自绘非子窗口，不抢编辑器焦点。
#pragma once

#include <windows.h>
#include <commctrl.h>  // TOOLINFO / TOOLTIPS_CLASSW（面板 tooltip）

#include <functional>
#include <string>
#include <vector>

#include "commands.h"
#include "menu.h"  // MenuHost（右键菜单弹出复用）
#include "theme.h"

namespace ui {

// —— 面板数据项（与 TabStrip::TabItem 同源语义；path 供 tooltip 显示）——
struct DocListItem {
    std::wstring displayName;
    bool modified = false;
    bool hasPath = false;  // url 非空（右键菜单「文件重命名…/打开所在文件夹」可用性）
    std::wstring path;     // 完整路径（tooltip 显示；无路径为空）
};

// —— 尺寸 token（逻辑像素，96 DPI 基准；使用处经 Scale(dpi) 换算，禁止散落）——
namespace doclist_metrics {

constexpr int DefaultWidth = 200;      // 默认宽（会话缺失回退值，需求口径）
constexpr int MinWidth = 120;          // 最小宽（拖拽/恢复钳制下限）
constexpr int SplitterWidth = 4;       // 右缘分隔条热区宽（hover 光标 IDC_SIZEWE 由组装层设置）
constexpr int TooltipDelayMs = 600;    // tooltip 悬停延迟（对齐 Tab 条）
constexpr int ItemHeight = 28;         // 条目行高（比 Tab 34 略紧凑，列表密度优先；
                                       // ≥24 保证可点击目标尺寸）
constexpr int LeftMarginWithMark = 8;  // 有未保存 ● 标记时的行左边距（对齐 Tab 条）
constexpr int LeftMarginPlain = 10;    // 无标记时的行左边距
constexpr int MarkToLabelSpacing = 5;  // ● 标记与标签文字的间距
constexpr int RightPadding = 10;       // 标签文字右内边距（超宽 DT_END_ELLIPSIS 截断）
constexpr int MarkPixelSize = 9;       // ● 标记字号（与 Tab 条同值）
constexpr int LabelPixelSize = 12;     // 列表文字字号（与 Tab 条同 12px = 9pt@96dpi，
                                       // 系统 UI 字体 Segoe UI）
constexpr int ActiveIndicatorWidth = 3;   // 活动条目左侧 accent 指示条宽（形状线索，
                                          // 与背景高亮共同表达活动态，非纯颜色）
constexpr int ScrollbarWidth = 12;     // 自绘滚动条宽
constexpr int ScrollbarButton = 12;    // 上下箭头按钮边长（正方形）
constexpr int ThumbInset = 2;          // 滑块与滚动条两缘的内缩
constexpr int ThumbMinHeight = 20;     // 滑块最小高（极端比例下仍可抓取）
constexpr int WheelStepLines = 3;      // 滚轮每格滚动行数（需求「步长 = 3 行」）
constexpr int ArrowStepLines = 1;      // 箭头按钮每次点击步进行数
constexpr int EmptyHintTopInset = 12;  // 空态提示文字距面板顶的距离

}  // namespace doclist_metrics

// 面板 tooltip 宿主 timer id（主窗口宿主 id 分配：1-7 已被会话轮询/Tab tooltip/
// 工具栏/状态栏/转发重试/Tab 拖拽让位动画/文件检测占用，本组件用 8——与
// main.cpp 分配表同源约定，单窗口内互斥）。头文件公开供单测/selftest 合成
// 节拍直驱 onTimer（对齐 kDragAnimTimerId 的同源约定）。
constexpr UINT_PTR kDocListTooltipTimerId = 8;

// ---------------------------------------------------------------------------
// 几何（纯函数；全部物理像素输入输出，DPI 由调用方换算）
// ---------------------------------------------------------------------------

struct DocListGeometry {
    RECT panel{};              // 面板本体（= 输入 panelRect 原样）
    RECT listArea{};           // 条目可视区（panel 去掉滚动条占宽；无溢出时 = panel）
    std::vector<RECT> items;   // 条目行矩形（客户区坐标，已含滚动偏移；右界 = listArea.right）
    RECT scrollbar{};          // 滚动条整体（无溢出为空矩形）
    RECT sbTrack{};            // 轨道（上下箭头之间；无溢出为空矩形）
    RECT sbThumb{};            // 比例滑块（无溢出为空矩形）
    RECT sbUp{};               // 上箭头按钮（无溢出为空矩形）
    RECT sbDown{};             // 下箭头按钮（无溢出为空矩形）
    int itemHeight = 0;        // 单行高（物理像素）
    int contentHeight = 0;     // 条目总高（物理像素）
    int viewHeight = 0;        // 可视高（= 面板高）
    int maxScroll = 0;         // 最大滚动偏移（contentHeight − viewHeight，不小于 0）
    bool overflow = false;     // contentHeight > viewHeight（滚动条显隐判定）
};

// items 与 panelRect（客户区坐标）传入；scrollOffset 由调用方钳制后传入（内部
// 仍防御性钳制）。溢出时滚动条占右缘 ScrollbarWidth、条目右界收缩；滑块按
// viewHeight/contentHeight 比例取高（钳 [ThumbMinHeight, 轨道高]）、按滚动比例
// 定位；无溢出时条目自面板顶排布且不产出滚动条矩形。
DocListGeometry ComputeDocListGeometry(const std::vector<DocListItem>& items,
                                       RECT panelRect, UINT dpi, int scrollOffset);

// 命中（客户区坐标）：条目行 → 下标；空白/滚动条区/面板外 → -1。
int DocListHitItem(const DocListGeometry& geo, POINT clientPt);

// 分隔条热区判定（纯函数）：panelRect 右缘 SplitterWidth 宽、纵向全高；空/退化
// 矩形恒 false。
bool DocListHitSplitter(const RECT& panelRect, POINT clientPt, UINT dpi);

// 滚动偏移钳制 [0, geo.maxScroll]（无溢出恒 0）。
int ClampDocListScroll(int scroll, const DocListGeometry& geo);

// 面板宽钳制（逻辑像素）：[MinWidth, max(MinWidth, maxLogicalWidth)]——上界小于
// 下限的极窄窗口取下限（最小宽硬下界）。
int ClampDocListWidthLogical(int logicalWidth, int maxLogicalWidth);

// 无障碍名称：未保存状态入名（对齐 Tab 条口径：「readme.md，未保存」）。
std::wstring AccessibleDocListName(const DocListItem& item);

// ---------------------------------------------------------------------------
// 文档列表面板组件（自绘非子窗口；鼠标消息由组装层按区域转发）
// ---------------------------------------------------------------------------

class DocListPanel {
public:
    void attach(HWND hwnd, MenuHost* menuHost);
    void detach();

    void setVisible(bool v);
    bool visible() const { return visible_; }
    int widthLogical() const { return widthLogical_; }
    void setWidthLogical(int w);  // 内部钳 MinWidth（及已知布局上界）

    // 布局：main 传入完整内容区（工具栏底~状态栏顶，未收缩矩形）；组件内部
    // 计算面板本体 = 左切片（宽 = Scale(widthLogical_, dpi)），并记录宽度上界
    // （内容区宽/3，逻辑像素）。幂等、可安全重复调用；窗口收缩时上界变小自动
    // 钳制当前宽（组装层主动布局驱动，不回调 onWidthChanged）。
    void layout(const RECT& contentRect, UINT dpi);
    RECT panelRect() const;    // 面板本体（客户区坐标；不可见/未布局为空矩形）
    RECT contentRect() const { return contentRect_; }  // 最近一次 layout 的完整内容区

    // —— 数据（与 TabStrip 同源语义；数据源 = DocumentManager 快照）——
    void setItems(std::vector<DocListItem> items);  // 整表替换（active 尽量保留；钳制滚动）
    void setActiveIndex(int index);                 // 程序化激活（滚入可视区；不触发回调；越界钳制）
    bool setModified(int index, bool modified);     // 同值短路返回 false
    bool setDisplayName(int index, const std::wstring& name);   // 同值短路返回 false
    bool setHasPath(int index, bool hasPath, std::wstring fullPath);  // 同值短路返回 false
    int count() const { return static_cast<int>(items_.size()); }
    int activeIndex() const { return activeIndex_; }
    const DocListItem& item(int index) const;  // 越界空项（防御弹窗期索引失效）

    // —— 命中（客户区坐标）——
    int hitItem(POINT clientPt) const;           // 条目 → index；空白 -1
    bool hitSplitter(POINT clientPt) const;      // 分隔条热区（不可见 = false）
    bool hitInteractive(POINT clientPt) const;   // 面板本体（条目/滚动条/分隔条/空白
                                                  // 均算——面板区域不穿透到编辑区）

    // —— 交互（main 转发；坐标为客户区）——
    void onMouseMove(POINT clientPt);   // 悬停/tooltip/拖拽中（捕获持续送坐标）
    void onMouseLeave();                // 清 hover/tooltip（拖拽态不取消）
    void onLButtonDown(POINT clientPt); // 命中条目→激活+记录按下；分隔条→起拖拽
                                        // （SetCapture 主窗口）；滚动条→箭头/轨道/滑块
    void onLButtonUp(POINT clientPt);   // 结束拖拽（提交）/清按下（no-op 安全）
    void onRButtonDown(POINT clientPt); // 命中条目→右键菜单（不激活）
    bool onMouseWheel(int delta);       // 溢出滚动（步长 = 3 行）；返回是否消费
    bool onKeyDown(UINT vk);            // 拖拽中 Esc 取消回原宽；返回是否消费
    void onTimer(UINT_PTR timerId);     // tooltip 延迟（仅认 kDocListTooltipTimerId）
    void onCaptureChanged();            // 拖拽取消收口（回原宽，经 onWidthChanged 通知）
    void onWindowKillFocus();           // 取消拖拽 + 清 hover/tooltip
    void onDpiChanged();                // 字体/测量缓存失效（几何经下次 layout 重算）
    void onThemeChanged();              // tooltip 换肤 + 重绘（绘制色恒经 CurrentPalette 动态取）
    void paint(HDC dc);                 // 不可见时 no-op

    bool draggingSplitter() const { return splitterDrag_; }  // 组装层消息路由查询

    // —— 挂接点（main.cpp 组装）——
    std::function<void(int)> onDocActivated;               // 激活回调（index；点击语义，
                                                           // 同项点击也上抛——与 Tab 一致）
    std::function<void(int, Command)> onDocContextCommand; // 右键命令（index + Command）
    std::function<void(int)> onWidthChanged;               // 宽度变化（拖拽实时/提交/Esc
                                                           // 回退/捕获丢失回退；参数 = 新
                                                           // 逻辑宽；main 据此重布局编辑区）

    // —— 测试钩子（对齐 TabStrip 风格，命名 *ForTest；无窗口夹具下几何/数据/
    //     交互全部可驱动，invalidateArea 对空 hwnd 安全）——
    int scrollOffsetForTest() const;          // 惰性钳制后的当前滚动偏移
    DocListGeometry geometryForTest() const;  // 完整几何（三方共用的同一来源）
    bool tooltipVisibleForTest() const { return tipVisible_; }
    int hoverIndexForTest() const { return hoverIndex_; }
    int maxWidthLogicalForTest() const { return maxWidthLogical_; }

private:
    // 几何与滚动
    DocListGeometry geometry() const;  // 惰性钳制滚动（对齐 TabStrip::geometry 手法）
    int clampedWidth(int w) const;     // 宽度钳制（未布局时仅下限）
    void scrollIntoView(int index);    // 滚动使指定条目完整可见（QML Contain 语义）
    bool scrollBy(int deltaPx);        // 带钳制的相对滚动；返回是否实际变化

    // 字体与测量
    void ensureFonts() const;  // 惰性重建字体 + ● 标记实测宽（DPI 键控）

    // tooltip
    void armTooltipTimer();
    void hideTooltip();
    void showTooltip();
    TOOLINFO makeToolInfo() const;
    std::wstring tooltipTextFor(int index) const;

    // 右键菜单（复用 BuildTabContextMenu 命令集）
    void openContextMenu(int index, POINT clientPt);

    // 条目激活（内部）
    void activateItem(int index);

    // 分隔条拖拽状态机
    void beginSplitterDrag(POINT clientPt);
    void splitterMove(POINT clientPt);
    void endSplitterDrag(bool restore);  // true = 取消回原宽（经回调）；false = 提交
    void clearSplitterDragState();

    // 滚动条交互
    enum class SbPart { None, Up, Down, Thumb, Track };
    SbPart hitScrollbarPart(POINT clientPt) const;  // 命中滚动条子部位（无溢出 None）
    void handleScrollbarDown(SbPart part, POINT clientPt);
    void thumbDragMove(POINT clientPt);
    void endThumbDrag();

    // 绘制子程序
    void drawScrollbar(HDC dc, const DocListGeometry& g, const ThemePalette& pal, UINT dpi);

    void invalidateArea() const;

    HWND hwnd_{};
    MenuHost* menuHost_{};
    bool visible_ = false;
    int widthLogical_ = doclist_metrics::DefaultWidth;
    int maxWidthLogical_ = 0;  // layout 记录的宽度上界（内容区宽/3，逻辑像素；0 = 未布局）
    bool hasLayout_ = false;
    RECT contentRect_{};
    UINT dpi_ = 96;  // layout 传入的 DPI（几何/字体唯一权威来源）

    std::vector<DocListItem> items_;
    int activeIndex_ = -1;
    int hoverIndex_ = -1;
    int pressedIndex_ = -1;        // 左键按下的条目（按下态视觉；up/leave/失焦清）
    mutable int scrollOffset_ = 0;

    // —— 分隔条拖拽状态（对齐 tabs.cpp 拖拽状态机命名风格）——
    bool splitterDrag_ = false;        // 拖拽态：up 提交 / Esc/捕获丢失/失焦取消收口
    bool splitterHover_ = false;       // 热区悬停（视觉反馈；光标由组装层设置）
    int splitterStartWidth_ = 0;       // 拖拽起始逻辑宽（取消回退目标）
    POINT splitterStartPt_{};          // 按下位置（dx 基准点）
    bool splitterRealButton_ = false;  // 本次按下源自真实按钮（down 时刻 GetKeyState
                                       // 采样；selftest/单测合成驱动为 false——按钮
                                       // 防呆据此豁免）

    // —— 滚动条交互状态 ——
    SbPart sbHover_ = SbPart::None;
    SbPart sbPressed_ = SbPart::None;
    bool thumbDrag_ = false;         // 滑块拖拽中（捕获期间 move 直驱滚动）
    POINT thumbDragStartPt_{};       // 拖拽起点（dy 基准）
    int thumbDragStartScroll_ = 0;   // 拖拽起始滚动（锚定换算）

    mutable HFONT labelFont_ = nullptr;
    mutable HFONT markFont_ = nullptr;
    mutable UINT fontDpi_ = 0;
    mutable int markWidthPx_ = 0;    // ● 标记实测宽（ensureFonts 随字体一并测量）

    HWND tooltip_ = nullptr;
    bool tipTimerArmed_ = false;
    bool tipVisible_ = false;
    std::wstring tipText_;           // tooltip 文本缓冲（控件持有指针期间须存活）
};

}  // namespace ui
