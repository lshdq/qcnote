// Tab 条实现。宽度自适应与几何为纯函数（单测直接覆盖），文字宽度经 GDI
// GetTextExtentPoint32 实测（与标签渲染同一字体，随 DPI 重建）；绘制全 GDI，
// 与标题栏同一 WM_PAINT 内直接落笔（WM_ERASEBKGND 已禁，无闪烁）。
#include "tabs.h"

#include <algorithm>
#include <cmath>
#include <commctrl.h>

#include "titlebar.h"  // ComputeTitleBarLayout（tabArea 几何单一来源）

namespace ui {

namespace {
constexpr wchar_t kUnsavedMarkText[] = L"●";  // 未保存标记（形状即线索）
constexpr UINT_PTR kTipTimerId = 2;           // tooltip 延迟计时器 id（主窗口宿主 id 分配：1 会话轮询 / 2 Tab / 3 工具栏 / 4 状态栏 / 5 转发重试，见 main.cpp）
}  // namespace

// ---------------------------------------------------------------------------
// 宽度自适应（纯函数）
// ---------------------------------------------------------------------------

std::vector<int> DistributeTabWidths(const std::vector<int>& naturalWidths, int areaWidth,
                                     UINT dpi) {
    const int n = static_cast<int>(naturalWidths.size());
    if (n == 0) {
        return {};
    }
    const int minW = Scale(tab_metrics::MinWidth, dpi);
    const int gap = Scale(tab_metrics::Spacing, dpi);
    const int gaps = gap * (n - 1);
    const int W = std::max(0, areaWidth);

    // 单 Tab 动态上限：cap = max(min, floor(W/4))；正整数除法即 floor
    const int cap = std::max(minW, W / 4);

    std::vector<int> capped(static_cast<size_t>(n));
    long long sum = 0;
    for (int i = 0; i < n; ++i) {
        capped[static_cast<size_t>(i)] =
            std::max(minW, std::min(naturalWidths[static_cast<size_t>(i)], cap));
        sum += capped[static_cast<size_t>(i)];
    }
    if (sum + gaps <= W) {
        return capped;  // 空间充足：各取自然宽
    }
    const long long deficit = sum + gaps - W;
    long long shrinkable = 0;
    for (int i = 0; i < n; ++i) {
        shrinkable += capped[static_cast<size_t>(i)] - minW;
    }
    if (deficit >= shrinkable) {
        return std::vector<int>(static_cast<size_t>(n), minW);  // 饱和：全部最小宽 + 滚动
    }
    const double ratio = static_cast<double>(deficit) / static_cast<double>(shrinkable);
    std::vector<int> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const int c = capped[static_cast<size_t>(i)];
        // 向下取整作用于分配结果（w_i），恒有 w_i ≥ minW 且 Σw_i + gaps ≤ W
        out[static_cast<size_t>(i)] = static_cast<int>(std::floor(c - ratio * (c - minW)));
    }
    return out;
}

// ---------------------------------------------------------------------------
// 几何（纯函数）
// ---------------------------------------------------------------------------

TabStripGeometry ComputeTabStripGeometry(const std::vector<TabItem>& items, int activeIndex,
                                         const std::vector<int>& widths, int markWidthPx,
                                         RECT tabArea, UINT dpi, int scrollOffset) {
    TabStripGeometry g;
    g.area = tabArea;
    g.viewWidth = std::max(0, static_cast<int>(tabArea.right) - static_cast<int>(tabArea.left));
    const int n = static_cast<int>(std::min(items.size(), widths.size()));
    if (n == 0) {
        return g;
    }
    const int h = Scale(tab_metrics::Height, dpi);
    // 贴底：Tab 底边与标题栏底边（tabArea.bottom）重合——连通式约束
    const int top = tabArea.bottom - h;
    const int gap = Scale(tab_metrics::Spacing, dpi);
    int x = tabArea.left - scrollOffset;
    int totalWidth = 0;
    g.tabs.resize(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const int w = std::max(1, widths[static_cast<size_t>(i)]);
        const bool active = (i == activeIndex);
        TabGeometry& t = g.tabs[static_cast<size_t>(i)];
        t.bounds = {x, top, x + w, top + h};
        t.hasMark = items[static_cast<size_t>(i)].modified;
        int textLeft;
        if (t.hasMark) {
            const int ml = Scale(tab_metrics::LeftMarginWithMark, dpi);
            t.mark = {x + ml, top, x + ml + markWidthPx, top + h};
            textLeft = t.mark.right + Scale(tab_metrics::MarkToLabelSpacing, dpi);
        } else {
            t.mark = {x, top, x, top + h};  // 空矩形
            textLeft = x + Scale(tab_metrics::LeftMarginPlain, dpi);
        }
        t.label = {textLeft, top, x + w - Scale(tab_metrics::RightPadding, dpi), top + h};
        // 分隔线规则（DEC-104）：index>0 且本标签非活动且左邻非活动；
        // 活动标签为凸起卡片，其两侧不画线（本标签活动 → 自身无线）
        t.separatorLeft = (i > 0 && !active && (i - 1) != activeIndex);
        totalWidth += w;
        x += w + gap;
    }
    g.contentWidth = totalWidth + gap * (n - 1);
    g.overflow = g.contentWidth > g.viewWidth;
    g.maxScroll = g.overflow ? g.contentWidth - g.viewWidth : 0;
    return g;
}

int TabStripHitTest(const TabStripGeometry& geo, POINT clientPt) {
    for (int i = 0; i < static_cast<int>(geo.tabs.size()); ++i) {
        const RECT& r = geo.tabs[static_cast<size_t>(i)].bounds;
        if (clientPt.x >= r.left && clientPt.x < r.right && clientPt.y >= r.top &&
            clientPt.y < r.bottom) {
            return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// 右键菜单与无障碍（纯函数）
// ---------------------------------------------------------------------------

MenuModel BuildTabContextMenu(int index, int totalCount) {
    MenuModel m;
    const int menu = m.addMenu();
    m.addItem(menu, L"关闭当前文件", {}, Command::CloseCurrentTab);  // 恒可用
    const int right = std::max(0, totalCount - 1 - index);
    const int left = std::max(0, index);
    const int others = std::max(0, totalCount - 1);
    m.addItem(menu, L"关闭右边所有 (" + std::to_wstring(right) + L")", {},
              Command::CloseTabsToRight).enabled = right > 0;
    m.addItem(menu, L"关闭左边所有 (" + std::to_wstring(left) + L")", {},
              Command::CloseTabsToLeft).enabled = left > 0;
    m.addItem(menu, L"关闭其他文件 (" + std::to_wstring(others) + L")", {},
              Command::CloseOtherTabs).enabled = others > 0;
    return m;
}

std::wstring AccessibleTabName(const TabItem& item) {
    std::wstring name = item.displayName;
    if (item.modified) {
        name += L"，未保存";
    }
    return name;
}

// ---------------------------------------------------------------------------
// TabStrip
// ---------------------------------------------------------------------------

void TabStrip::attach(HWND hwnd, MenuHost* menuHost) {
    hwnd_ = hwnd;
    menuHost_ = menuHost;
    // tooltip 控件（comctl32，系统 DLL）：手动 TRACK 模式，延迟自行计时
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               TOOLTIPS_CLASSW, nullptr,
                               WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd_, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    if (tooltip_) {
        SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 400);
        TOOLINFO ti = makeToolInfo();
        SendMessageW(tooltip_, TTM_ADDTOOL, 0, reinterpret_cast<LPARAM>(&ti));
    }
}

void TabStrip::detach() {
    hideTooltip();
    if (tooltip_) {
        DestroyWindow(tooltip_);
        tooltip_ = nullptr;
    }
    if (labelFont_) {
        DeleteObject(labelFont_);
        labelFont_ = nullptr;
    }
    if (markFont_) {
        DeleteObject(markFont_);
        markFont_ = nullptr;
    }
    fontDpi_ = 0;
    items_.clear();
    naturalWidths_.clear();
    activeIndex_ = focusIndex_ = hoverIndex_ = -1;
    scrollOffset_ = 0;
    hwnd_ = nullptr;
    menuHost_ = nullptr;
}

const TabItem& TabStrip::item(int index) const {
    static const TabItem kEmpty;
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return kEmpty;
    }
    return items_[static_cast<size_t>(index)];
}

void TabStrip::setItems(std::vector<TabItem> items) {
    items_ = std::move(items);
    const int n = static_cast<int>(items_.size());
    if (activeIndex_ >= n) {
        activeIndex_ = n - 1;  // 尽量保留原 active 位置语义（M2 由文档系统精确映射）
    }
    focusIndex_ = activeIndex_;
    markDirtyAndInvalidate();
}

int TabStrip::addTab(const TabItem& tab) {
    items_.push_back(tab);
    if (activeIndex_ < 0) {
        activeIndex_ = 0;  // 空表 → 首项即活动（QML DocumentManager 语义）
        focusIndex_ = 0;
    }
    markDirtyAndInvalidate();
    return static_cast<int>(items_.size()) - 1;
}

void TabStrip::removeTab(int index) {
    removeRange(index, index + 1);
}

void TabStrip::removeRange(int from, int to) {
    const int n = static_cast<int>(items_.size());
    from = std::max(0, from);
    to = std::min(to, n);
    if (from >= to) {
        return;
    }
    items_.erase(items_.begin() + from, items_.begin() + to);
    const int removed = to - from;
    const int count = static_cast<int>(items_.size());
    // active/focus 钳制：命中删除区 → 落到删除区起点（或末尾）；在其后 → 前移
    const auto clamp = [count, from, to, removed](int idx) {
        if (idx < 0) return -1;
        if (count == 0) return -1;
        if (idx >= to) return idx - removed;
        if (idx >= from) return std::min(from, count - 1);
        return idx;
    };
    activeIndex_ = clamp(activeIndex_);
    focusIndex_ = clamp(focusIndex_);
    hoverIndex_ = -1;
    hideTooltip();
    markDirtyAndInvalidate();
}

void TabStrip::setModified(int index, bool modified) {
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return;
    }
    items_[static_cast<size_t>(index)].modified = modified;
    markDirtyAndInvalidate();
}

void TabStrip::setDisplayName(int index, const std::wstring& name) {
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return;
    }
    items_[static_cast<size_t>(index)].displayName = name;
    markDirtyAndInvalidate();
}

bool TabStrip::setActiveIndex(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return false;
    }
    if (activeIndex_ == index) {
        focusIndex_ = index;  // 焦点跟随（QML activateIndex 同步 focusIndex）
        return true;
    }
    activeIndex_ = index;
    focusIndex_ = index;
    scrollIntoView(index);
    invalidateArea();
    return true;
}

// ---------------------------------------------------------------------------
// 几何与测量
// ---------------------------------------------------------------------------

RECT TabStrip::areaRect() const {
    RECT client{};
    if (hwnd_) {
        GetClientRect(hwnd_, &client);
    }
    return ComputeTitleBarLayout(client.right, WindowDpi(hwnd_)).tabArea;
}

TabStripGeometry TabStrip::geometry() const {
    recomputeNatural();
    const RECT area = areaRect();
    const UINT dpi = WindowDpi(hwnd_);
    const std::vector<int> widths =
        DistributeTabWidths(naturalWidths_, area.right - area.left, dpi);
    TabStripGeometry g = ComputeTabStripGeometry(items_, activeIndex_, widths, markWidthPx_,
                                                 area, dpi, scrollOffset_);
    const int clamped = std::clamp(scrollOffset_, 0, g.maxScroll);
    if (clamped != scrollOffset_) {
        scrollOffset_ = clamped;  // 数据/尺寸变化后惰性钳制（滚动不越过最大值）
        g = ComputeTabStripGeometry(items_, activeIndex_, widths, markWidthPx_, area, dpi,
                                    clamped);
    }
    return g;
}

int TabStrip::hitTab(POINT clientPt) const {
    return TabStripHitTest(geometry(), clientPt);
}

bool TabStrip::hitInteractive(POINT clientPt) const {
    const RECT area = areaRect();
    if (clientPt.x < area.left || clientPt.x >= area.right) {
        return false;
    }
    if (clientPt.y < std::max(0L, area.top) || clientPt.y >= area.bottom) {
        return false;
    }
    if (overflow()) {
        return true;  // 溢出：整个 tabArea 为滚动交互区（旧 QML tabsOverflow 语义）
    }
    return hitTab(clientPt) >= 0;  // 未溢出：仅 Tab 本体接管，空白区留给 HTCAPTION
}

void TabStrip::ensureFonts() const {
    const UINT dpi = WindowDpi(hwnd_);
    if (labelFont_ && markFont_ && fontDpi_ == dpi) {
        return;
    }
    if (labelFont_) {
        DeleteObject(labelFont_);
    }
    if (markFont_) {
        DeleteObject(markFont_);
    }
    // pixelSize N 逻辑像素 ≙ 字符高度 -Scale(N)（负高度=字符高度基准），随 DPI 缩放
    labelFont_ = CreateFontW(-Scale(tab_metrics::LabelPixelSize, dpi), 0, 0, 0, FW_NORMAL,
                             FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    markFont_ = CreateFontW(-Scale(tab_metrics::MarkPixelSize, dpi), 0, 0, 0, FW_NORMAL, FALSE,
                            FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    fontDpi_ = dpi;
}

void TabStrip::recomputeNatural() const {
    if (!dirty_) {
        return;
    }
    ensureFonts();
    const UINT dpi = fontDpi_;
    naturalWidths_.assign(items_.size(), Scale(tab_metrics::MinWidth, dpi));
    markWidthPx_ = 0;
    HDC dc = GetDC(nullptr);  // 屏幕 DC 测量（与渲染同字体，无需窗口 DC）
    if (!dc) {
        dirty_ = false;
        return;  // 测量不可用：自然宽退化为最小宽（分配算法仍成立）
    }
    SIZE sz{};
    if (markFont_ &&
        GetTextExtentPoint32W(dc, kUnsavedMarkText, 1, &sz)) {
        markWidthPx_ = sz.cx;
    }
    const HGDIOBJ old = SelectObject(dc, labelFont_);
    for (size_t i = 0; i < items_.size(); ++i) {
        int textW = 0;
        SIZE tsz{};
        const std::wstring& name = items_[i].displayName;
        if (!name.empty() && GetTextExtentPoint32W(dc, name.c_str(),
                                                   static_cast<int>(name.size()), &tsz)) {
            textW = tsz.cx;
        }
        // 装饰宽：● 标记出现时（左边距 + 标记宽 + 间距）或纯左边距，再加右内边距；
        // 无每标签关闭按钮（A2）
        const int decor =
            Scale(items_[i].modified ? tab_metrics::LeftMarginWithMark
                                     : tab_metrics::LeftMarginPlain,
                  dpi) +
            (items_[i].modified
                 ? markWidthPx_ + Scale(tab_metrics::MarkToLabelSpacing, dpi)
                 : 0) +
            Scale(tab_metrics::RightPadding, dpi);
        naturalWidths_[i] = textW + decor;
    }
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    dirty_ = false;
}

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------

void TabStrip::onMouseMove(POINT clientPt) {
    const int hit = hitTab(clientPt);
    if (hit == hoverIndex_) {
        return;
    }
    hoverIndex_ = hit;
    invalidateArea();
    if (hit >= 0) {
        armTooltipTimer();  // 进入新 Tab：重新计 600ms 延迟
    } else {
        hideTooltip();
    }
}

void TabStrip::onMouseLeave() {
    if (hoverIndex_ != -1) {
        hoverIndex_ = -1;
        invalidateArea();
    }
    hideTooltip();
}

void TabStrip::onLButtonDown(POINT clientPt) {
    const int hit = hitTab(clientPt);
    if (hit >= 0) {
        focusVisible_ = false;  // 鼠标路径隐藏焦点环（对齐 QML activeFocus 语义）
        activate(hit);          // down 即激活（Win32 惯例；旧 QML 在 release，差异见交付报告）
    }
}

void TabStrip::onRButtonDown(POINT clientPt) {
    const int hit = hitTab(clientPt);
    if (hit >= 0) {
        openContextMenu(hit, clientPt);  // 右键不激活该 Tab（A3）
    }
}

void TabStrip::onMouseWheel(bool horizontal, int delta) {
    if (items_.empty()) {
        return;
    }
    const TabStripGeometry g = geometry();
    if (!g.overflow) {
        return;  // 未溢出不滚动（空白区滚轮走 HTCAPTION，不会到达此处）
    }
    int step = ::MulDiv(delta, Scale(tab_metrics::WheelStep, WindowDpi(hwnd_)), WHEEL_DELTA);
    if (!horizontal) {
        step = -step;  // 竖直轮：向上（正 delta）= 向左滚（QML 横向 Flickable 语义）
    }
    scrollOffset_ = std::clamp(scrollOffset_ + step, 0, g.maxScroll);
    invalidateArea();
}

bool TabStrip::onKeyDown(UINT vk, bool shiftDown) {
    if (items_.empty()) {
        return false;
    }
    const auto consume = [this] {
        focusVisible_ = true;  // 键盘导航后焦点环可见
        return true;
    };
    switch (vk) {
    case VK_LEFT:
        moveFocus(-1);
        return consume();
    case VK_RIGHT:
        moveFocus(+1);
        return consume();
    case VK_RETURN:
    case VK_SPACE:
        if (focusIndex_ < 0) {
            focusIndex_ = std::max(0, activeIndex_);  // 首次按键：焦点从活动 Tab 起
        }
        activate(focusIndex_);
        return consume();
    case VK_APPS:  // Menu 键
        openContextMenuAtFocus();
        return consume();
    case VK_F10:  // Shift+F10 兜底（平台未映射为 VK_APPS 时；两条路径互斥）
        if (shiftDown) {
            openContextMenuAtFocus();
            return consume();
        }
        return false;
    default:
        return false;
    }
}

void TabStrip::onWindowKillFocus() {
    if (focusVisible_) {
        focusVisible_ = false;
        invalidateArea();
    }
}

void TabStrip::onTimer(UINT_PTR timerId) {
    if (timerId != kTipTimerId) {
        return;
    }
    tipTimerArmed_ = false;  // 一次性
    if (hoverIndex_ >= 0 && hoverIndex_ < static_cast<int>(items_.size())) {
        showTooltip();
    }
}

void TabStrip::onDpiChanged() {
    dirty_ = true;  // 字体与自然宽按新 DPI 惰性重建；滚动偏移经 geometry 钳制
    invalidateArea();
}

void TabStrip::onLayoutChanged() {
    invalidateArea();  // 滚动钳制在 geometry() 内惰性完成
}

void TabStrip::onThemeChanged() {
    // E5：tooltip 控件换肤（本条持有独立 tooltip 窗口，非 ManualTooltip）；
    // 绘制颜色全部经 CurrentPalette 动态取，整窗重绘由组装层统一触发
    if (tooltip_) {
        const ThemePalette& pal = CurrentPalette();
        SendMessageW(tooltip_, TTM_SETTIPBKCOLOR,
                     static_cast<WPARAM>(ToColorRef(pal.toolbarBackground)), 0);
        SendMessageW(tooltip_, TTM_SETTIPTEXTCOLOR,
                     static_cast<WPARAM>(ToColorRef(pal.textPrimary)), 0);
    }
}

void TabStrip::activate(int index) {
    if (setActiveIndex(index) && onTabActivated) {
        onTabActivated(index);
    }
}

void TabStrip::moveFocus(int delta) {
    const int n = static_cast<int>(items_.size());
    if (n == 0) {
        return;
    }
    if (focusIndex_ < 0) {
        focusIndex_ = activeIndex_ >= 0 ? activeIndex_ : 0;
    }
    focusIndex_ = ((focusIndex_ + delta) % n + n) % n;  // 循环
    scrollIntoView(focusIndex_);
    invalidateArea();
}

void TabStrip::scrollIntoView(int index) {
    const TabStripGeometry g = geometry();
    if (index < 0 || index >= static_cast<int>(g.tabs.size())) {
        return;
    }
    const RECT& b = g.tabs[static_cast<size_t>(index)].bounds;
    const int contentLeft = b.left - g.area.left + scrollOffset_;   // 撤销平移回内容坐标
    const int contentRight = b.right - g.area.left + scrollOffset_;
    int offset = scrollOffset_;
    if (contentLeft < offset) {
        offset = contentLeft;
    } else if (contentRight > offset + g.viewWidth) {
        offset = contentRight - g.viewWidth;
    }
    offset = std::clamp(offset, 0, g.maxScroll);
    if (offset != scrollOffset_) {
        scrollOffset_ = offset;
        invalidateArea();
    }
}

// ---------------------------------------------------------------------------
// 右键菜单（A3）
// ---------------------------------------------------------------------------

void TabStrip::openContextMenu(int index, POINT clientPt) {
    if (!hwnd_ || !menuHost_) {
        return;
    }
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return;  // 防御：弹出期间模型被外部变动导致索引失效
    }
    hideTooltip();
    MenuModel model = BuildTabContextMenu(index, static_cast<int>(items_.size()));
    POINT anchor = clientPt;
    ClientToScreen(hwnd_, &anchor);
    // 命令经 track 返回值 → onTabContextCommand 上抛（携带被右键 Tab 的 index）；
    // 暂时摘除全局 onCommand 防止同命令双路由（RouteCommand 侧无 index 上下文）。
    // uiLocked 语义由 MenuHost onOpened/onClosed 承担（组装层已接线）。
    auto saved = menuHost_->onCommand;
    menuHost_->onCommand = nullptr;
    const Command cmd = menuHost_->track(hwnd_, model, anchor);
    menuHost_->onCommand = saved;
    if (cmd != Command::None && onTabContextCommand) {
        onTabContextCommand(index, cmd);
    }
}

void TabStrip::openContextMenuAtFocus() {
    if (focusIndex_ < 0 || focusIndex_ >= static_cast<int>(items_.size())) {
        return;
    }
    const TabStripGeometry g = geometry();
    const RECT& b = g.tabs[static_cast<size_t>(focusIndex_)].bounds;
    // 标签左下角弹出（左缘对齐标签、菜单顶边贴标签底边；翻转/夹取由 MenuHost 处理）
    openContextMenu(focusIndex_, {b.left + Scale(2, WindowDpi(hwnd_)), b.bottom});
}

// ---------------------------------------------------------------------------
// tooltip（完整名 + 未保存状态；同旧 QML ToolTip 文本）
// ---------------------------------------------------------------------------

TOOLINFO TabStrip::makeToolInfo() const {
    TOOLINFO ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    ti.hwnd = hwnd_;
    ti.uId = 1;
    // const 方法内取可写指针：tooltip 消息同步拷贝文本，缓冲在下次更新前存活
    ti.lpszText = tipText_.empty() ? const_cast<LPWSTR>(L"")
                                   : const_cast<LPWSTR>(tipText_.c_str());
    return ti;
}

void TabStrip::armTooltipTimer() {
    if (hwnd_ && !tipTimerArmed_) {
        SetTimer(hwnd_, kTipTimerId, tab_metrics::TooltipDelayMs, nullptr);
        tipTimerArmed_ = true;
    }
}

void TabStrip::hideTooltip() {
    if (tipTimerArmed_ && hwnd_) {
        KillTimer(hwnd_, kTipTimerId);
        tipTimerArmed_ = false;
    }
    if (tipVisible_ && tooltip_) {
        TOOLINFO ti = makeToolInfo();
        SendMessageW(tooltip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
        tipVisible_ = false;
    }
}

void TabStrip::showTooltip() {
    if (!tooltip_ || !hwnd_) {
        return;
    }
    tipText_ = items_[static_cast<size_t>(hoverIndex_)].displayName;
    if (items_[static_cast<size_t>(hoverIndex_)].modified) {
        tipText_ += L"（未保存）";
    }
    // 定位：光标右侧、标签条下缘之下（屏幕坐标，钳制在所在显示器工作区内）
    POINT cursor{};
    GetCursorPos(&cursor);
    const TabStripGeometry g = geometry();
    const UINT dpi = WindowDpi(hwnd_);
    POINT bottom{0, g.area.bottom};
    ClientToScreen(hwnd_, &bottom);
    int x = cursor.x + Scale(12, dpi);
    int y = bottom.y + Scale(2, dpi);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi)) {
        const int workLeft = static_cast<int>(mi.rcWork.left);
        const int workRight = static_cast<int>(mi.rcWork.right);
        const int workBottom = static_cast<int>(mi.rcWork.bottom);
        x = std::clamp(x, workLeft, std::max(workLeft, workRight - 300));
        y = std::min(y, workBottom - Scale(24, dpi));
    }
    TOOLINFO ti = makeToolInfo();
    SendMessageW(tooltip_, TTM_UPDATETIPTEXT, 0, reinterpret_cast<LPARAM>(&ti));
    SendMessageW(tooltip_, TTM_TRACKPOSITION, 0, MAKELPARAM(x, y));
    SendMessageW(tooltip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&ti));
    tipVisible_ = true;
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------

void TabStrip::paint(HDC dc) {
    const TabStripGeometry g = geometry();
    if (g.tabs.empty()) {
        return;
    }
    const ThemePalette& pal = CurrentPalette();
    const UINT dpi = WindowDpi(hwnd_);
    const int radius = Scale(tab_metrics::Radius, dpi);
    const int hoverRadius = Scale(tab_metrics::HoverRadius, dpi);
    const int sepWidth = Scale(tab_metrics::SeparatorWidth, dpi);
    const int sepHalf = Scale(tab_metrics::SeparatorHeight, dpi) / 2;

    // 裁剪到 tabArea：滚动半出区的 Tab 不得溢画到菜单/窗口控制按钮上
    SaveDC(dc);
    IntersectClipRect(dc, g.area.left, g.area.top, g.area.right, g.area.bottom);

    // —— 背景层：活动卡片 / 悬停高亮 / 竖分隔线 ——
    for (int i = 0; i < static_cast<int>(g.tabs.size()); ++i) {
        const TabGeometry& t = g.tabs[static_cast<size_t>(i)];
        const RECT& b = t.bounds;
        if (i == activeIndex_) {
            // Win11 连通式：顶圆角卡片（activeTab=toolbarBackground，无描边）+
            // 同色补角矩形直化底边两圆角；底边与标题栏底边重合（与工具栏无缝）
            const COLORREF card = ToColorRef(pal.activeTab);
            HBRUSH brush = CreateSolidBrush(card);
            HGDIOBJ oldBrush = SelectObject(dc, brush);
            HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, b.left, b.top, b.right - 1, b.bottom - 1, radius * 2, radius * 2);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(brush);
            RECT patch{b.left, b.bottom - radius - 1, b.right, b.bottom};
            HBRUSH pb = CreateSolidBrush(card);
            FillRect(dc, &patch, pb);
            DeleteObject(pb);
        } else if (i == hoverIndex_) {
            // 非活动悬停：半透明高亮与标题栏底色预合成（透明底透出 titleBarBackground）
            const COLORREF hover =
                ToColorRef(BlendOver(pal.titleBarBackground, pal.hover));
            HBRUSH brush = CreateSolidBrush(hover);
            HGDIOBJ oldBrush = SelectObject(dc, brush);
            HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
            RoundRect(dc, b.left, b.top, b.right - 1, b.bottom - 1, hoverRadius * 2,
                      hoverRadius * 2);
            SelectObject(dc, oldPen);
            SelectObject(dc, oldBrush);
            DeleteObject(brush);
        }
        if (t.separatorLeft) {
            // 竖分隔线：左缘、不占宽度（宽度公式/文字左边距均不动）
            const int cy = (b.top + b.bottom) / 2;
            RECT line{b.left, cy - sepHalf, b.left + sepWidth, cy + sepHalf};
            HBRUSH lb = CreateSolidBrush(ToColorRef(pal.border));
            FillRect(dc, &line, lb);
            DeleteObject(lb);
        }
    }

    // —— 内容层：● 标记 + 标签名（超长省略号）——
    const HGDIOBJ oldFont = SelectObject(dc, labelFont_);
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < static_cast<int>(g.tabs.size()); ++i) {
        const TabGeometry& t = g.tabs[static_cast<size_t>(i)];
        if (t.hasMark) {
            SelectObject(dc, markFont_);
            SetTextColor(dc, ToColorRef(pal.unsavedMark));
            RECT mr = t.mark;
            DrawTextW(dc, kUnsavedMarkText, -1, &mr,
                      DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
            SelectObject(dc, labelFont_);
        }
        SetTextColor(dc, ToColorRef(i == activeIndex_ ? pal.textPrimary : pal.textSecondary));
        RECT lr = t.label;
        DrawTextW(dc, items_[static_cast<size_t>(i)].displayName.c_str(), -1, &lr,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    SelectObject(dc, oldFont);

    // —— 键盘焦点环（accent 双线框；仅键盘导航后显示，活动/非活动底上均可见，
    //    非颜色线索）——
    if (focusVisible_ && focusIndex_ >= 0 && focusIndex_ < static_cast<int>(g.tabs.size())) {
        const RECT& b = g.tabs[static_cast<size_t>(focusIndex_)].bounds;
        const int inset = Scale(tab_metrics::FocusInset, dpi);
        const int pw = Scale(tab_metrics::FocusPenWidth, dpi);
        const int fr = Scale(tab_metrics::FocusRadius, dpi);
        HPEN pen = CreatePen(PS_SOLID, pw, ToColorRef(pal.focus));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, b.left + inset, b.top + inset, b.right - inset - 1,
                  b.bottom - inset - 1, fr * 2, fr * 2);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    RestoreDC(dc, -1);
}

void TabStrip::markDirtyAndInvalidate() {
    dirty_ = true;
    invalidateArea();
}

void TabStrip::invalidateArea() const {
    if (!hwnd_) {
        return;
    }
    const RECT area = areaRect();
    InvalidateRect(hwnd_, &area, FALSE);
}

}  // namespace ui
