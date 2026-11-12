// qcnote 状态栏（M2-B，E3）：客户区底部条（statusBarBackground）。
//
// 字段（旧 QML StatusBar.qml 对等，显隐语义一致）：
//   行 N，列 M（无文档「行 —，列 —」）｜已选 N 字符（有选区时）｜
//   [光标 ×N（多光标 >1 时；B4 前占位恒不显示）]｜共 N 字符（有文档时）｜
//   ……弹性空档……｜编码（UTF-8 / UTF-8 BOM / GBK）｜换行风格（LF/CRLF）｜
//   文件完整路径（超长 DT_PATH_ELLIPSIS 中部省略，悬停 tooltip 显示完整路径）
// 字体小号 textSecondary（旧 QML pointSize 9 → 12px@96dpi）。
// 文本构建为纯函数（BuildStatusTexts，单测覆盖）；布局测量/绘制/tooltip 为组件。
#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

#include "theme.h"
#include "tooltip.h"

namespace ui {

namespace statusbar_metrics {

constexpr int Height = 28;       // 旧 QML implicitHeight 28
constexpr int MarginH = 10;      // 左右边距（旧 QML leftMargin/rightMargin 10）
constexpr int Spacing = 16;      // 字段间距（旧 QML RowLayout spacing 16）
constexpr int FontPixelSize = 12; // 9pt ≈ 12px@96dpi
constexpr int PathMaxWidth = 320; // 路径段最大宽（旧 QML maximumWidth 320）
constexpr int TooltipDelayMs = 400; // 路径 tooltip 延迟（旧 QML ToolTip.delay 400）

}  // namespace statusbar_metrics

// 状态数据快照（组装层从活动文档 + EditorHost 采样注入）
struct StatusData {
    bool hasDoc = false;
    int cursorLine = -1;         // -1 = 无文档（「行 —」）
    int cursorColumn = -1;
    long long selectedChars = 0; // 0 = 无选区（字段隐藏）
    long long totalChars = 0;
    int cursorCount = 1;         // >1 显示「光标 ×N」（B4 前恒 1）
    std::wstring encoding;       // "UTF-8" / "UTF-8 BOM" / "GBK"（空 = 隐藏）
    std::wstring lineEnding;     // "LF" / "CRLF"（空 = 隐藏）
    std::wstring path;           // 完整路径（空 = 隐藏/无 tooltip）

    // 全值类型 defaulted 等值比较（C++20+）：update() 数据未变化时跳过
    // 置脏与失效（拖拽中 SCN_UPDATEUI 高频刷新，等值跳过减少无谓重绘）
    bool operator==(const StatusData&) const = default;
};

// 各字段显示文本（纯函数，单测覆盖；返回空串 = 该字段隐藏）
std::wstring StatusLineColText(const StatusData& d);      // 恒显示（无文档占位）
std::wstring StatusSelectionText(const StatusData& d);    // 已选 N 字符（选区>0）
std::wstring StatusCursorCountText(const StatusData& d);  // 光标 ×N（>1）
std::wstring StatusTotalText(const StatusData& d);        // 共 N 字符（有文档）
std::wstring StatusEncodingText(const StatusData& d);     // 编码（有文档）
std::wstring StatusLineEndingText(const StatusData& d);   // LF/CRLF（有文档）

// 布局段（measureAndLayout 产物，下标与 SegId 一一对应；R2-A 段级差分失效
// 的比对单元）。isPath 只影响绘制省略样式，不参与差分判定
struct StatusSegment {
    std::wstring text;  // 显示文本（省略前的完整文本）
    RECT bounds{};      // 客户区坐标
    bool visible = false;
    bool isPath = false;
};

// 段级差分失效矩形（纯函数，无 HWND 依赖，Catch2 直接构造向量单测）：
// 逐下标比较旧/新段，visible / text / bounds 任一变化 → 该段需要重绘；
// 失效矩形 = 旧 bounds（若旧 visible）∪ 新 bounds（若新 visible）的并集
//（位移联动覆盖：段宽或位置变化时新旧两块区域都要重画，不留残影）。
// 段完全相同 → 不产生矩形；全部段无变化返回空（update 调用方完全不失效）
std::vector<RECT> StatusDiffInvalidRects(const std::vector<StatusSegment>& oldSegments,
                                         const std::vector<StatusSegment>& newSegments);

class StatusBar {
public:
    void attach(HWND hostWindow);
    void detach();

    // 状态栏区域（客户区坐标）：[client.bottom - Height, client.bottom)
    RECT rect(const RECT& clientRect) const;

    // 数据更新（触发重绘）；path tooltip 文本随 path 字段
    void update(const StatusData& data);
    const StatusData& data() const { return data_; }

    // —— 交互（组装层转发）——
    void onMouseMove(POINT clientPt);  // 路径段悬停 → tooltip 计时
    void onMouseLeave();
    void onTimer(UINT_PTR timerId);
    void onThemeChanged();             // E5：主题切换（tooltip 换肤；绘制色动态取）

    // —— 绘制 ——
    void paint(HDC dc, const RECT& clientRect);

    // —— 测试钩子 ——
    RECT pathRect(const RECT& clientRect) const;  // 路径段矩形（空 = 无路径）

private:
    enum class SegId { LineCol = 0, Selection, CursorCount, Total, Encoding, LineEnd, Path, Count };

    void ensureFont() const;
    void measureAndLayout(const RECT& clientRect) const;  // 惰性段布局
    void invalidateBar() const;

    // —— R3-A 渲染辅助（paint() 与 update() 直绘共用，防两份绘制代码漂移）——
    // 整条内容绘制（底色 + 顶部分隔线 + 各段文字），坐标为绝对客户区坐标，
    // segments_ 布局结果直接可用。WM_PAINT 整条双缓冲与 update() 差分矩形
    // 同步直绘共用同一实现，保证两条路径像素级一致（theme selftest 的
    // 状态栏像素采样依赖该一致性）
    void drawBarContent(HDC target, const RECT& bar) const;
    // 渲染整条到内存位图：视口原点平移到条带左上角后 drawBarContent，
    // 完成后复位原点（位图 (0,0) = 条带左上角，供 BitBlt 源坐标换算）
    void drawBarToMemory(HDC memDc, const RECT& bar) const;

    HWND hwnd_ = nullptr;
    StatusData data_;
    mutable HFONT font_ = nullptr;
    mutable UINT fontDpi_ = 0;
    mutable std::vector<StatusSegment> segments_;  // 与 SegId 一一对应（R2-A 差分基准/产物）
    mutable RECT clientCache_{};
    mutable UINT layoutDpi_ = 0;
    mutable bool layoutDirty_ = true;
    UINT_PTR tipTimerId_ = 4;  // 主窗口宿主 id 分配：1 会话轮询/2 TabStrip/3 工具栏/4 状态栏/5 转发重试（见 main.cpp）
    bool tipTimerArmed_ = false;
    bool tipVisible_ = false;
    ManualTooltip tooltip_;
};

}  // namespace ui
