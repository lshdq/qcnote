// 手动 TRACK tooltip 实现（对齐 TabStrip 既有行为：TTM_SETMAXTIPWIDTH 限宽换行、
// TTF_TRACK|TTF_ABSOLUTE 手动定位、TTM_UPDATETIPTEXT 热更新）。
#include "tooltip.h"

#include "theme.h"

namespace ui {

ManualTooltip::~ManualTooltip() {
    destroy();
}

void ManualTooltip::applyThemeColors() {
    if (!tooltip_) return;
    const ThemePalette& pal = CurrentPalette();
    // tip 面板取工具栏表面色（与宿主条带同族的「浮层」语义；浅 #f2f4f8 /
    // 深 #232833），文字 textPrimary；边框为系统 1px 亮边（不可定制，两主题
    // 下均可辨）。
    // 已知限制（DEC-001 伴随）：进程绑定 comctl32 v6（Common-Controls 6.0
    // manifest，tooltip 根因修复的前提）后，视觉样式开启时系统忽略
    // TTM_SETTIPBKCOLOR/TTM_SETTIPTEXTCOLOR（MSDN 确定行为），E5 tooltip
    // 换肤为死路径——深/浅主题下 tooltip 均呈系统标准样式（视觉样式服务保证
    // 对比度，可读性无损）。消息保留：未来回退 v5 或系统放开限制时无需改代码。
    // TabStrip 自建 tooltip（tabs.cpp）同源同限制
    SendMessageW(tooltip_, TTM_SETTIPBKCOLOR,
                 static_cast<WPARAM>(ToColorRef(pal.toolbarBackground)), 0);
    SendMessageW(tooltip_, TTM_SETTIPTEXTCOLOR,
                 static_cast<WPARAM>(ToColorRef(pal.textPrimary)), 0);
}

void ManualTooltip::create(HWND hostWindow) {
    if (tooltip_ || !hostWindow) return;
    host_ = hostWindow;
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                               hostWindow, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!tooltip_) return;
    SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 600);
    TOOLINFO ti = makeToolInfo();
    SendMessageW(tooltip_, TTM_ADDTOOL, 0, reinterpret_cast<LPARAM>(&ti));
    applyThemeColors();  // E5：创建即按当前主题着色
    SetWindowPos(tooltip_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void ManualTooltip::destroy() {
    if (tooltip_) {
        DestroyWindow(tooltip_);
        tooltip_ = nullptr;
    }
    host_ = nullptr;
}

void ManualTooltip::show(const std::wstring& text, POINT screenPt) {
    if (!tooltip_ || text.empty()) return;
    text_ = text;
    TOOLINFO ti = makeToolInfo();
    SendMessageW(tooltip_, TTM_UPDATETIPTEXT, 0, reinterpret_cast<LPARAM>(&ti));
    SendMessageW(tooltip_, TTM_TRACKPOSITION, 0, MAKELPARAM(screenPt.x, screenPt.y));
    SendMessageW(tooltip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));
}

void ManualTooltip::hide() {
    if (!tooltip_) return;
    TOOLINFO ti = makeToolInfo();
    SendMessageW(tooltip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
}

TOOLINFO ManualTooltip::makeToolInfo() const {
    TOOLINFO ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    ti.hwnd = host_;
    ti.uId = 1;  // 单工具实例（手动 TRACK 不需要多工具）
    ti.lpszText = text_.empty() ? const_cast<LPWSTR>(L"")
                                : const_cast<LPWSTR>(text_.c_str());
    return ti;
}

}  // namespace ui
