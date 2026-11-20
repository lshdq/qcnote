// Tab 条实现。宽度自适应与几何为纯函数（单测直接覆盖），文字宽度经 GDI
// GetTextExtentPoint32 实测（与标签渲染同一字体，随 DPI 重建）；绘制全 GDI，
// 与标题栏同一 WM_PAINT 内直接落笔（WM_ERASEBKGND 已禁，无闪烁）。
// 拖拽重排（DEC-001/002/003，Chrome 式视觉跟随）：位移越过系统拖拽阈值
// （SM_CXDRAG/SM_CYDRAG 最大值，0 退 4px）进入拖拽并 SetCapture；拖拽期间
// items_/activeIndex_/focusIndex_ 零改动——被拖 Tab 仅在绘制层水平跟随光标
// （抓取点锚定光标下，钳制条内，仅横向），插入槽位由 TabDragInsertIndex 压缩流
// 中点判定（无状态单阈值无振荡），起始/插入槽位之间的邻居经 WM_TIMER（id 6，
// 10ms 请求节拍 + ApproachPx 指数趋近 + 1px 精确落位）缓动滑入让位/回位空位，
// 收敛即停表；松手顺序有变才一次 onTabReordered(from, insert) 提交（直接落位
// 无收尾动画，DEC-005）；Esc/捕获丢失/失焦经 cancelDrag 统一收口（数据从未
// 变动——清状态 + 停表 + 重绘即回原位，不回调），外部 setItems/removeRange 到达
// 时安全终止（不回调）。真实按钮会话在 move 中防呆检测左键已松（up 丢失）按取消
// 收口；selftest/单测的合成驱动（down 时刻采样无真实按钮）豁免该检测，由显式
// up/Esc/捕获丢失收口。
#include "tabs.h"

#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <utility>

#include "titlebar.h"  // ComputeTitleBarLayout（tabArea 几何单一来源）

namespace ui {

namespace {
constexpr wchar_t kUnsavedMarkText[] = L"●";  // 未保存标记（形状即线索）
constexpr UINT_PTR kTipTimerId = 2;           // tooltip 延迟计时器 id（主窗口宿主 id 分配：1 会话轮询 / 2 Tab / 3 工具栏 / 4 状态栏 / 5 转发重试 / 6 Tab 拖拽让位动画（kDragAnimTimerId，tabs.h 公开），见 main.cpp）
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

int TabDragSwapIndex(const TabStripGeometry& geo, int dragIndex, int clientX) {
    const int n = static_cast<int>(geo.tabs.size());
    if (dragIndex < 0 || dragIndex >= n) {
        return -1;
    }
    // 左邻：光标严格越过其 bounds 中点（clientX < mid，含滚动偏移）→ 与左邻交换
    if (dragIndex > 0) {
        const RECT& r = geo.tabs[static_cast<size_t>(dragIndex - 1)].bounds;
        const int mid = static_cast<int>(r.left + r.right) / 2;
        if (clientX < mid) {
            return dragIndex - 1;
        }
    }
    // 右邻：光标严格越过其 bounds 中点（clientX > mid）→ 与右邻交换
    if (dragIndex + 1 < n) {
        const RECT& r = geo.tabs[static_cast<size_t>(dragIndex + 1)].bounds;
        const int mid = static_cast<int>(r.left + r.right) / 2;
        if (clientX > mid) {
            return dragIndex + 1;
        }
    }
    return -1;  // 同位（未严格越过任一侧中点）/ 首向左 / 末向右
}

int TabDragInsertIndex(const TabStripGeometry& geo, int dragIndex, int clientX) {
    const int n = static_cast<int>(geo.tabs.size());
    if (n <= 0) {
        return 0;  // 空几何：无有效槽位，返回 0（纯防御）
    }
    if (dragIndex < 0 || dragIndex >= n) {
        return std::clamp(dragIndex, 0, n - 1);  // 越界：直接返回钳制后的 dragIndex（不判定）
    }
    const int idx = dragIndex;
    const RECT& drag = geo.tabs[static_cast<size_t>(idx)].bounds;
    const int dragW = std::max(0, static_cast<int>(drag.right) - static_cast<int>(drag.left));
    // 间距从几何推导（布局恒等距）：任意相邻对 bounds[i+1].left − bounds[i].right
    const int gap =
        n >= 2 ? std::max(0, static_cast<int>(geo.tabs[1].bounds.left) -
                                 static_cast<int>(geo.tabs[0].bounds.right))
               : 0;
    int insert = 0;
    for (int i = 0; i < n; ++i) {
        if (i == idx) {
            continue;
        }
        // 压缩流静息位置：被拖槽位左侧的邻居不动，右侧邻居左移 (被拖宽+间距)
        int restLeft = static_cast<int>(geo.tabs[static_cast<size_t>(i)].bounds.left);
        if (i > idx) {
            restLeft -= dragW + gap;
        }
        const RECT& r = geo.tabs[static_cast<size_t>(i)].bounds;
        const int mid =
            restLeft + std::max(0, static_cast<int>(r.right) - static_cast<int>(r.left)) / 2;
        if (mid < clientX) {
            ++insert;  // 光标严格越过该压缩中点
        }
    }
    return insert;
}

std::vector<int> ComputeTabDragOffsets(const TabStripGeometry& geo, int fromIndex,
                                       int insertIndex, int followDx) {
    const int n = static_cast<int>(geo.tabs.size());
    std::vector<int> out(static_cast<size_t>(std::max(0, n)), 0);
    if (n <= 0) {
        return out;
    }
    const int from = std::clamp(fromIndex, 0, n - 1);
    const int insert = std::clamp(insertIndex, 0, n - 1);
    // 让位量 = 被拖宽 + 间距（均从 geo 推导，随 DPI 自动正确）
    const RECT& b = geo.tabs[static_cast<size_t>(from)].bounds;
    const int shift = std::max(0, static_cast<int>(b.right) - static_cast<int>(b.left)) +
                      (n >= 2 ? std::max(0, static_cast<int>(geo.tabs[1].bounds.left) -
                                               static_cast<int>(geo.tabs[0].bounds.right))
                              : 0);
    out[static_cast<size_t>(from)] = followDx;  // 被拖 Tab：跟随偏移本身（不缓动）
    if (insert > from) {
        for (int i = from + 1; i <= insert; ++i) {
            out[static_cast<size_t>(i)] = -shift;  // 右侧邻居左移让位
        }
    } else if (insert < from) {
        for (int i = insert; i < from; ++i) {
            out[static_cast<size_t>(i)] = shift;  // 左侧邻居右移让位
        }
    }
    return out;
}

int ApproachPx(int current, int target, double ratio) {
    const long long delta = static_cast<long long>(target) - static_cast<long long>(current);
    const long long mag = delta < 0 ? -delta : delta;
    if (mag <= tab_metrics::DragAnimSnapPx) {
        return target;  // 落位阈值内：精确取目标（终止性）
    }
    const double r = std::clamp(ratio, 0.0, 1.0);
    long long step = std::llround(static_cast<double>(delta) * r);
    if (step == 0) {
        step = delta > 0 ? 1 : -1;  // 极小比例步进取整退化：至少前进 1px（防停滞）
    }
    return static_cast<int>(static_cast<long long>(current) + step);
}

// ---------------------------------------------------------------------------
// 右键菜单与无障碍（纯函数）
// ---------------------------------------------------------------------------

MenuModel BuildTabContextMenu(const TabItem& item, int index, int totalCount) {
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
    // 尾部分隔线 + 文件定位类新项：仅磁盘文件可用（未命名置灰不隐藏）
    m.addSeparator(menu);
    m.addItem(menu, L"文件重命名…", {}, Command::RenameFile).enabled = item.hasPath;
    m.addItem(menu, L"打开所在文件夹", {}, Command::OpenContainingFolder).enabled =
        item.hasPath;
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
    resetPressAndDragState();  // 拖拽/按下状态安全终止（销毁路径，不回调）
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
    // 拖拽/按住中的外部整表重建：索引上下文失效——安全终止（外部模型已变，
    // 回滚无意义；不回调——DEC-002），press/drag 状态全部复位
    resetPressAndDragState();
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
    // 同 setItems：删除使拖拽/按下索引失效——安全终止，不回滚不回调
    resetPressAndDragState();
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

bool TabStrip::setHasPath(int index, bool hasPath) {
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return false;
    }
    if (items_[static_cast<size_t>(index)].hasPath == hasPath) {
        return false; // 同值短路：不置脏、不触发重绘（与 setDisplayName 同策略）
    }
    items_[static_cast<size_t>(index)].hasPath = hasPath;
    markDirtyAndInvalidate();
    return true;
}

bool TabStrip::setDisplayName(int index, const std::wstring& name) {
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return false;
    }
    if (items_[static_cast<size_t>(index)].displayName == name) {
        return false; // 同值短路：不置脏、不触发重绘（拖拽选区每帧调用的消抖）
    }
    items_[static_cast<size_t>(index)].displayName = name;
    markDirtyAndInvalidate();
    return true;
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

std::vector<int> TabStrip::dragVisualOffsets() const {
    // DEC-004 只读测试缝：长度恒 = Tab 数；非拖拽态全 0（空表为空数组）
    const int n = static_cast<int>(items_.size());
    std::vector<int> out(static_cast<size_t>(std::max(0, n)), 0);
    if (!dragging_) {
        return out;
    }
    for (int i = 0; i < n; ++i) {
        if (i == dragFromIndex_) {
            out[static_cast<size_t>(i)] = followDx_;  // 被拖项即时跟随（不缓动）
        } else if (i < static_cast<int>(dragVisualDx_.size())) {
            out[static_cast<size_t>(i)] = dragVisualDx_[static_cast<size_t>(i)];
        }
    }
    return out;
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
    if (dragging_) {
        dragMove(clientPt);
        return;  // 拖拽中不做悬停/tooltip（悬停已在进入拖拽时清除）
    }
    if (pressIndex_ >= 0) {
        if (pressRealButton_ && ::GetKeyState(VK_LBUTTON) >= 0) {
            // 陈旧按下清理：真实按钮已松而 up 未到达（无捕获时窗外释放）——静默
            // 清理，不影响后续键盘/滚轮/再次 down（合成驱动会话不走此分支）
            pressIndex_ = -1;
            pressPt_ = {};
            pressRealButton_ = false;
        } else {
            // 按住未拖：位移超过系统拖拽阈值（两轴取最大值；度量不可用退 4px）
            // 才进入拖拽——未越过即为纯点击的微动
            const LONG dx = clientPt.x - pressPt_.x;
            const LONG dy = clientPt.y - pressPt_.y;
            int threshold =
                std::max(GetSystemMetrics(SM_CXDRAG), GetSystemMetrics(SM_CYDRAG));
            if (threshold <= 0) {
                threshold = 4;
            }
            const long long dist2 =
                static_cast<long long>(dx) * dx + static_cast<long long>(dy) * dy;
            if (dist2 > static_cast<long long>(threshold) * threshold) {
                beginDrag();
                dragMove(clientPt);  // 本次 move 立即参与交换/边缘滚动判定
                return;
            }
        }
    }
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
    if (dragging_) {
        return;  // 拖拽会话中的重复 down（防御）：忽略，保持状态一致
    }
    const int hit = hitTab(clientPt);
    if (hit >= 0) {
        focusVisible_ = false;  // 鼠标路径隐藏焦点环（对齐 QML activeFocus 语义）
        activate(hit);          // down 即激活（Win32 惯例；语义不变）
        // 按下记录在激活回调之后：防御回调内整表重建（setItems）清除新鲜 press
        pressIndex_ = hit;
        pressPt_ = clientPt;
        // 真实按钮采样：消息驱动的 down 此刻 GetKeyState 为按下态；selftest/
        // 单测的合成驱动为松开态——按钮防呆据此区分会话来源（见 dragMove）
        pressRealButton_ = (::GetKeyState(VK_LBUTTON) < 0);
    }
}

void TabStrip::onLButtonUp(POINT /*clientPt*/) {
    if (!dragging_) {
        // 纯点击（或无按下）：激活已在 down 完成——仅清按下记录
        pressIndex_ = -1;
        pressPt_ = {};
        pressRealButton_ = false;
        return;
    }
    const int n = static_cast<int>(items_.size());
    const int from = dragFromIndex_;
    int to = dragInsertIndex_;
    if (to < 0 || to >= n) {
        to = from;  // 防御：插入槽位失效视为未移动（from==to 不回调）
    }
    // 先收口再回调：回调内的整表重建（setItems）与捕获变更重入都应看到已复位
    // 的拖拽态（up 坐标不再参与判定——提交的是既成插入槽位）。清状态即直接
    // 落位（DEC-005：无收尾动画、不引入过渡态）
    clearDragState();
    if (hwnd_ && ::GetCapture() == hwnd_) {
        ReleaseCapture();  // 仅释放自己持有的捕获；未捕获时无副作用
    }
    invalidateArea();
    if (from != to && onTabReordered) {
        onTabReordered(from, to);  // 松手一次提交（DEC-001；真实换序由 moveDocument 完成）
    }
}

void TabStrip::onCaptureChanged() {
    if (dragging_) {
        cancelDrag();  // 捕获被夺走/释放：清状态+停表+重绘回原位，不回调
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
    if (dragging_ && vk == VK_ESCAPE) {
        cancelDrag();  // 拖拽中 Esc：取消并消费（不进入键盘导航）
        return true;
    }
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
    if (dragging_) {
        cancelDrag();  // 失焦取消：清状态+停表+重绘回原位，不回调（需求取消路径 3）
    }
    if (focusVisible_) {
        focusVisible_ = false;
        invalidateArea();
    }
}

void TabStrip::onTimer(UINT_PTR timerId) {
    if (timerId == kDragAnimTimerId) {
        // 让位动画节拍（DEC-003）：全收敛 → 停表；否则逐项指数趋近 + tabArea 局部
        // 重绘。被拖项 visual==target==followDx_（dragMove 直写），趋近自然 no-op
        if (!dragging_) {
            killDragAnimTimer();  // 陈旧节拍防御（收口路径已停表，双保险）
            return;
        }
        bool settled = true;
        for (size_t i = 0; i < dragVisualDx_.size() && i < dragTargetDx_.size(); ++i) {
            if (dragVisualDx_[i] != dragTargetDx_[i]) {
                settled = false;
                break;
            }
        }
        if (settled) {
            killDragAnimTimer();
            return;
        }
        for (size_t i = 0; i < dragVisualDx_.size() && i < dragTargetDx_.size(); ++i) {
            dragVisualDx_[i] = ApproachPx(dragVisualDx_[i], dragTargetDx_[i],
                                          tab_metrics::DragAnimApproachRatio);
        }
        invalidateArea();
        return;
    }
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
// 拖拽重排状态机（DEC-001/002/003：视觉偏移式 + 松手一次提交）
// ---------------------------------------------------------------------------

void TabStrip::beginDrag() {
    dragging_ = true;
    dragFromIndex_ = pressIndex_;
    dragInsertIndex_ = pressIndex_;  // 首次 dragMove 即按压缩流中点重判（防御初值）
    const TabStripGeometry g = geometry();
    const int n = static_cast<int>(g.tabs.size());
    if (dragFromIndex_ >= 0 && dragFromIndex_ < n) {
        // 抓取点距槽左缘的物理偏移：跟随锚定（抓取点保持在光标下）
        dragGrabX_ =
            static_cast<int>(pressPt_.x) - static_cast<int>(g.tabs[static_cast<size_t>(dragFromIndex_)].bounds.left);
    } else {
        dragGrabX_ = 0;
    }
    followDx_ = 0;
    dragVisualDx_.assign(items_.size(), 0);
    dragTargetDx_.assign(items_.size(), 0);
    if (hwnd_) {
        // 捕获保证条外/窗外坐标继续送达；失败不阻塞进入拖拽态（headless/
        // selftest 下捕获可能不可用），无捕获时由再次交互/取消路径自然收口
        SetCapture(hwnd_);
    }
    hoverIndex_ = -1;  // 悬停高亮让位给抓起描边
    hideTooltip();
    armDragAnimTimer();
    invalidateArea();
}

void TabStrip::dragMove(POINT clientPt) {
    // 防呆：真实按钮发起的会话中检测到左键已松（up 消息丢失，如无捕获时窗外
    // 释放）→ 按取消收口。合成驱动（selftest/单测，down 时刻采样无真实按钮）
    // 豁免该检测——由显式 up/Esc/捕获丢失收口
    if (pressRealButton_ && ::GetKeyState(VK_LBUTTON) >= 0) {
        cancelDrag();
        return;
    }
    TabStripGeometry g = geometry();
    LONG x = clientPt.x;
    if (g.area.right > g.area.left) {
        // x 钳制到 [area.left, area.right)：捕获期间条外/窗外坐标继续参与判定；
        // 区域退化（宽 0，如无窗口夹具）不钳制
        x = std::clamp<LONG>(clientPt.x, g.area.left, g.area.right - 1);
        // 溢出边缘步进滚动（移动驱动，无定时器：光标停住即停）；
        // 左右边缘带重叠时左带优先（极窄区防御）
        if (g.overflow) {
            const UINT dpi = WindowDpi(hwnd_);
            const int zone = Scale(tab_metrics::DragEdgeZone, dpi);
            const int step = Scale(tab_metrics::DragEdgeScrollStep, dpi);
            int offset = scrollOffset_;
            if (x < g.area.left + zone) {
                offset -= step;  // 左边缘带：向左滚，露出左侧内容
            } else if (x >= g.area.right - zone) {
                offset += step;  // 右边缘带：向右滚
            }
            const int clamped = std::clamp(offset, 0, g.maxScroll);
            if (clamped != scrollOffset_) {
                scrollOffset_ = clamped;
                invalidateArea();
                g = geometry();  // 滚动后 bounds 平移，按新几何继续判定/跟随
            }
        }
    }
    const int n = static_cast<int>(items_.size());
    if (dragFromIndex_ < 0 || dragFromIndex_ >= n ||
        dragFromIndex_ >= static_cast<int>(g.tabs.size())) {
        return;  // 防御：数据/几何不一致（正常路径由安全终止排除）
    }
    // 插入槽位：压缩流中点判定（DEC-002，无状态单阈值——同点跨入跨出无振荡）
    dragInsertIndex_ = TabDragInsertIndex(g, dragFromIndex_, static_cast<int>(x));
    // 跟随：抓取点保持在光标下（仅横向；竖直位移只参与进入拖拽的阈值判定），
    // 视觉矩形钳制条内（不遮菜单按钮/窗口控制）。槽左缘取自重取后的 g——
    // 滚动时光标下不漂移
    const RECT& slot = g.tabs[static_cast<size_t>(dragFromIndex_)].bounds;
    const int w = static_cast<int>(slot.right) - static_cast<int>(slot.left);
    int followLeft = static_cast<int>(x) - dragGrabX_;
    const int maxLeft = static_cast<int>(g.area.right) - w;
    if (maxLeft >= static_cast<int>(g.area.left)) {
        followLeft = std::clamp(followLeft, static_cast<int>(g.area.left), maxLeft);
    } else {
        followLeft = static_cast<int>(g.area.left);  // 条比 Tab 窄的退化防御
    }
    followDx_ = followLeft - static_cast<int>(slot.left);
    // 邻居让位目标：被拖项目标即当前跟随值（不缓动，直接生效）
    dragTargetDx_ = ComputeTabDragOffsets(g, dragFromIndex_, dragInsertIndex_, followDx_);
    if (dragVisualDx_.size() != dragTargetDx_.size()) {
        dragVisualDx_.assign(dragTargetDx_.size(), 0);  // 防御：长度漂移重建
    }
    dragVisualDx_[static_cast<size_t>(dragFromIndex_)] = followDx_;
    // 起表：仅当确有未到位的让位偏移（目标变化但已到位/纯跟随移动时不空转）
    bool pending = false;
    for (size_t i = 0; i < dragVisualDx_.size() && i < dragTargetDx_.size(); ++i) {
        if (dragVisualDx_[i] != dragTargetDx_[i]) {
            pending = true;
            break;
        }
    }
    if (pending) {
        armDragAnimTimer();
    }
    invalidateArea();  // 被拖 Tab 即时跟随 → 每次 move 都需重绘
}

void TabStrip::cancelDrag() {
    if (!dragging_) {
        return;
    }
    // 视觉偏移式（DEC-001）：拖拽期间数据从未变动——清状态 + 停表 + 重绘即回
    // 原位，无需回滚；不触发任何回调（DEC-002）
    clearDragState();
    if (hwnd_ && ::GetCapture() == hwnd_) {
        ReleaseCapture();  // 仅释放自己持有的捕获（防误伤他组件捕获）
    }
    invalidateArea();
}

void TabStrip::clearDragState() {
    killDragAnimTimer();
    dragging_ = false;
    dragFromIndex_ = -1;
    dragInsertIndex_ = -1;
    dragGrabX_ = 0;
    followDx_ = 0;
    dragVisualDx_.clear();
    dragTargetDx_.clear();
    pressIndex_ = -1;
    pressPt_ = {};
    pressRealButton_ = false;
}

void TabStrip::armDragAnimTimer() {
    if (hwnd_ && !dragAnimArmed_) {
        SetTimer(hwnd_, kDragAnimTimerId, tab_metrics::DragAnimTimerMs, nullptr);
        dragAnimArmed_ = true;
    }
}

void TabStrip::killDragAnimTimer() {
    if (dragAnimArmed_ && hwnd_) {
        KillTimer(hwnd_, kDragAnimTimerId);  // 仅杀自己持有的节拍（防误伤他组件计时器）
    }
    dragAnimArmed_ = false;
}

void TabStrip::resetPressAndDragState() {
    if (!dragging_ && pressIndex_ < 0) {
        return;
    }
    const bool wasDragging = dragging_;
    clearDragState();
    if (wasDragging && hwnd_ && ::GetCapture() == hwnd_) {
        ReleaseCapture();
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
    MenuModel model = BuildTabContextMenu(items_[static_cast<size_t>(index)],
                                           index, static_cast<int>(items_.size()));
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
    const int n = static_cast<int>(g.tabs.size());
    // 拖拽视觉偏移（DEC-001）：被拖 Tab = followDx_（直接生效），邻居 = 动画当前值；
    // 各 Tab 的 bounds/label/mark 统一 OffsetRect 平移后再画（仅横向，视觉 y 不变）
    const bool dragVis = dragging_ && dragFromIndex_ >= 0 && dragFromIndex_ < n;
    const auto dxOf = [&](int i) -> int {
        if (!dragVis) {
            return 0;
        }
        if (i == dragFromIndex_) {
            return followDx_;
        }
        return i >= 0 && i < static_cast<int>(dragVisualDx_.size())
                   ? dragVisualDx_[static_cast<size_t>(i)]
                   : 0;
    };
    const auto offset = [](RECT r, int dx) {
        OffsetRect(&r, dx, 0);  // 仅横向平移（视觉 y 不变）
        return r;
    };

    // 裁剪到 tabArea：滚动半出区的 Tab 不得溢画到菜单/窗口控制按钮上
    SaveDC(dc);
    IntersectClipRect(dc, g.area.left, g.area.top, g.area.right, g.area.bottom);

    // —— 背景层单 Tab：活动卡片 / 悬停高亮 / 竖分隔线（分隔线按平移后左缘）——
    const auto drawBackground = [&](int i, const TabGeometry& t, int dx) {
        const RECT b = offset(t.bounds, dx);
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
    };

    // —— 内容层单 Tab：● 标记 + 标签名（超长省略号；字体选择沿用原约定：
    //    markFont 仅画标记期间切入，画完切回 labelFont）——
    const auto drawContent = [&](int i, const TabGeometry& t, int dx) {
        if (t.hasMark) {
            SelectObject(dc, markFont_);
            SetTextColor(dc, ToColorRef(pal.unsavedMark));
            RECT mr = offset(t.mark, dx);
            DrawTextW(dc, kUnsavedMarkText, -1, &mr,
                      DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
            SelectObject(dc, labelFont_);
        }
        SetTextColor(dc, ToColorRef(i == activeIndex_ ? pal.textPrimary : pal.textSecondary));
        RECT lr = offset(t.label, dx);
        DrawTextW(dc, items_[static_cast<size_t>(i)].displayName.c_str(), -1, &lr,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    };

    // 画序：先画全部非被拖 Tab（背景层 + 内容层），被拖 Tab 最后置顶画（防叠压）
    for (int i = 0; i < n; ++i) {
        if (dragVis && i == dragFromIndex_) {
            continue;
        }
        drawBackground(i, g.tabs[static_cast<size_t>(i)], dxOf(i));
    }
    const HGDIOBJ oldFont = SelectObject(dc, labelFont_);
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < n; ++i) {
        if (dragVis && i == dragFromIndex_) {
            continue;
        }
        drawContent(i, g.tabs[static_cast<size_t>(i)], dxOf(i));
    }

    // —— 键盘焦点环（accent 双线框；仅键盘导航后显示，活动/非活动底上均可见，
    //    非颜色线索。拖拽态平移量随 Tab——常态鼠标路径不显示，防御性跟随）——
    if (focusVisible_ && focusIndex_ >= 0 && focusIndex_ < n) {
        const RECT b = offset(g.tabs[static_cast<size_t>(focusIndex_)].bounds,
                              dxOf(focusIndex_));
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

    // —— 被拖 Tab 置顶：背景 + 内容 + accent 抓起描边（按跟随平移后 bounds）。
    //    描边复用焦点环手法与 Focus* 尺寸 token；描边即「抓起」的形状线索，满足
    //    非「仅颜色」规范。常态为活动 Tab（down 即激活）圆角对齐 Radius，防御
    //    非活动抓起（合成驱动）时对齐 HoverRadius ——
    if (dragVis) {
        const TabGeometry& t = g.tabs[static_cast<size_t>(dragFromIndex_)];
        drawBackground(dragFromIndex_, t, followDx_);
        drawContent(dragFromIndex_, t, followDx_);
        const RECT b = offset(t.bounds, followDx_);
        const int inset = Scale(tab_metrics::FocusInset, dpi);
        const int pw = Scale(tab_metrics::FocusPenWidth, dpi);
        const int dr = Scale(dragFromIndex_ == activeIndex_ ? tab_metrics::Radius
                                                            : tab_metrics::HoverRadius,
                             dpi);
        HPEN pen = CreatePen(PS_SOLID, pw, ToColorRef(pal.focus));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, b.left + inset, b.top + inset, b.right - inset - 1,
                  b.bottom - inset - 1, dr * 2, dr * 2);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    SelectObject(dc, oldFont);
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
