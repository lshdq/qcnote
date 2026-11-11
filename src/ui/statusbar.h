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
};

// 各字段显示文本（纯函数，单测覆盖；返回空串 = 该字段隐藏）
std::wstring StatusLineColText(const StatusData& d);      // 恒显示（无文档占位）
std::wstring StatusSelectionText(const StatusData& d);    // 已选 N 字符（选区>0）
std::wstring StatusCursorCountText(const StatusData& d);  // 光标 ×N（>1）
std::wstring StatusTotalText(const StatusData& d);        // 共 N 字符（有文档）
std::wstring StatusEncodingText(const StatusData& d);     // 编码（有文档）
std::wstring StatusLineEndingText(const StatusData& d);   // LF/CRLF（有文档）

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
    struct Segment {
        std::wstring text;  // 显示文本（含省略）
        RECT bounds{};      // 客户区坐标
        bool visible = false;
        bool isPath = false;
    };
    enum class SegId { LineCol = 0, Selection, CursorCount, Total, Encoding, LineEnd, Path, Count };

    void ensureFont() const;
    void measureAndLayout(const RECT& clientRect) const;  // 惰性段布局
    void invalidateBar() const;

    HWND hwnd_ = nullptr;
    StatusData data_;
    mutable HFONT font_ = nullptr;
    mutable UINT fontDpi_ = 0;
    mutable std::vector<Segment> segments_;  // 与 SegId 一一对应
    mutable RECT clientCache_{};
    mutable UINT layoutDpi_ = 0;
    mutable bool layoutDirty_ = true;
    UINT_PTR tipTimerId_ = 4;  // 主窗口宿主 id 分配：1 会话轮询/2 TabStrip/3 工具栏/4 状态栏/5 转发重试（见 main.cpp）
    bool tipTimerArmed_ = false;
    bool tipVisible_ = false;
    ManualTooltip tooltip_;
};

}  // namespace ui
