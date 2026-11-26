// 文档列表面板实现。几何为纯函数（单测直接覆盖）；条目行高恒定，无需按条目
// 测量文本（超宽 DT_END_ELLIPSIS 截断，测量仅 ● 标记一次），字体随 DPI 惰性
// 重建；绘制全 GDI，与主窗口同一 WM_PAINT 内直接落笔（WM_ERASEBKGND 已禁，
// 无闪烁）。颜色一律 CurrentPalette() 动态取（半透明 token 经 BlendOver 与面板
// 底色预合成）；全部尺寸经 Scale(dpi)（dpi 来自 layout 传入，唯一权威来源）。
//
// 分隔条拖拽（宽度语义）：down 记录起始逻辑宽 + 起点并 SetCapture（失败不阻塞
// ——headless/selftest 下捕获可能不可用，无捕获时由再次交互/取消路径自然收口）；
// move 中 newWidth = 起始宽 + (dx 物理)×96/dpi 经钳制生效、每变化一次
// onWidthChanged（main 据此实时重布局编辑区）；真实按钮会话在 move 中防呆检测
// 左键已松（up 丢失）按取消收口，合成驱动（down 时刻采样无真实按钮）豁免；
// up 提交（最终 onWidthChanged——main 的持久化时机）；Esc/捕获丢失/失焦统一
// 回原宽（经 onWidthChanged 通知）。收口先清状态再释放捕获（WM_CAPTURECHANGED
// 重入看到已复位状态，不二次处理），旧/新面板矩形一并失效防残影。
#include "doclist.h"

#include <algorithm>
#include <commctrl.h>
#include <utility>

#include "tabs.h"  // BuildTabContextMenu / TabItem（右键菜单命令集复用）

namespace ui {

namespace {
constexpr wchar_t kUnsavedMarkText[] = L"●";  // 未保存标记（形状即线索，与 Tab 条同字形）
constexpr wchar_t kEmptyHintText[] = L"无打开文档";  // 空态提示（textSecondary）

// 行内子布局（● 标记区与文字区；行矩形 + token 推导，paint 专用——行矩形本身
// 来自 ComputeDocListGeometry，子区不参与命中故不入纯函数几何）
void itemSubRects(const RECT& row, bool hasMark, int markW, UINT dpi, RECT& mark,
                  RECT& label) {
    if (hasMark) {
        const int ml = Scale(doclist_metrics::LeftMarginWithMark, dpi);
        mark = {row.left + ml, row.top, row.left + ml + markW, row.bottom};
        label = {mark.right + Scale(doclist_metrics::MarkToLabelSpacing, dpi), row.top,
                 row.right - Scale(doclist_metrics::RightPadding, dpi), row.bottom};
    } else {
        mark = {row.left, row.top, row.left, row.bottom};  // 空矩形
        label = {row.left + Scale(doclist_metrics::LeftMarginPlain, dpi), row.top,
                 row.right - Scale(doclist_metrics::RightPadding, dpi), row.bottom};
    }
}

// 点入矩形（右/下开区间，与项目命中口径一致）
bool ptInRect(const RECT& r, POINT p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

// 滚动条箭头字形（三角形，形状线索）
void drawArrowGlyph(HDC dc, const RECT& btn, bool up, COLORREF color) {
    const int w = static_cast<int>(btn.right) - static_cast<int>(btn.left);
    const int h = static_cast<int>(btn.bottom) - static_cast<int>(btn.top);
    if (w <= 0 || h <= 0) {
        return;
    }
    const LONG cx = static_cast<LONG>((static_cast<int>(btn.left) + static_cast<int>(btn.right)) / 2);
    const LONG cy = static_cast<LONG>((static_cast<int>(btn.top) + static_cast<int>(btn.bottom)) / 2);
    const LONG s = static_cast<LONG>(std::max(2, std::min(w, h) / 3));
    POINT pts[3]{};
    if (up) {
        pts[0] = {cx, cy - s};
        pts[1] = {cx - s, cy + s};
        pts[2] = {cx + s, cy + s};
    } else {
        pts[0] = {cx, cy + s};
        pts[1] = {cx - s, cy - s};
        pts[2] = {cx + s, cy - s};
    }
    HBRUSH brush = CreateSolidBrush(color);
    const HGDIOBJ old = SelectObject(dc, brush);
    Polygon(dc, pts, 3);
    SelectObject(dc, old);
    DeleteObject(brush);
}
}  // namespace

// ---------------------------------------------------------------------------
// 几何与钳制（纯函数）
// ---------------------------------------------------------------------------

DocListGeometry ComputeDocListGeometry(const std::vector<DocListItem>& items,
                                       RECT panelRect, UINT dpi, int scrollOffset) {
    DocListGeometry g;
    g.panel = panelRect;
    g.listArea = panelRect;
    const int n = static_cast<int>(items.size());
    const int w = static_cast<int>(panelRect.right) - static_cast<int>(panelRect.left);
    const int h = static_cast<int>(panelRect.bottom) - static_cast<int>(panelRect.top);
    g.itemHeight = Scale(doclist_metrics::ItemHeight, dpi);
    g.contentHeight = g.itemHeight * n;
    g.viewHeight = std::max(0, h);
    if (w <= 0 || h <= 0 || n == 0) {
        return g;  // 退化防御：无条目几何（overflow 恒 false、滚动条空矩形）
    }
    g.overflow = g.contentHeight > g.viewHeight;
    g.maxScroll = g.overflow ? g.contentHeight - g.viewHeight : 0;

    // 滚动偏移防御性钳制（调用方已钳制时无副作用）
    const int sc = std::clamp(scrollOffset, 0, g.maxScroll);

    if (g.overflow) {
        // 溢出：滚动条占右缘，条目右界收缩
        const int sbw = Scale(doclist_metrics::ScrollbarWidth, dpi);
        const int btnH = Scale(doclist_metrics::ScrollbarButton, dpi);
        g.scrollbar = {panelRect.right - sbw, panelRect.top, panelRect.right, panelRect.bottom};
        g.listArea = {panelRect.left, panelRect.top, panelRect.right - sbw, panelRect.bottom};
        g.sbUp = {g.scrollbar.left, g.scrollbar.top, g.scrollbar.right,
                  g.scrollbar.top + btnH};
        g.sbDown = {g.scrollbar.left, g.scrollbar.bottom - btnH, g.scrollbar.right,
                    g.scrollbar.bottom};
        g.sbTrack = {g.scrollbar.left, g.scrollbar.top + btnH, g.scrollbar.right,
                     g.scrollbar.bottom - btnH};
        // 比例滑块：高 = 轨道高 × viewHeight/contentHeight（MulDiv 就近取整），
        // 钳 [ThumbMinHeight, 轨道高]（极矮面板退化为整轨）
        const int trackH =
            std::max(0, static_cast<int>(g.sbTrack.bottom) - static_cast<int>(g.sbTrack.top));
        int thumbH = ::MulDiv(trackH, g.viewHeight, g.contentHeight);
        thumbH = std::max(thumbH, Scale(doclist_metrics::ThumbMinHeight, dpi));
        if (thumbH > trackH) {
            thumbH = trackH;
        }
        const int range = std::max(0, trackH - thumbH);
        int thumbTop = g.sbTrack.top;
        if (g.maxScroll > 0 && range > 0) {
            thumbTop += ::MulDiv(range, sc, g.maxScroll);
        }
        const int inset = Scale(doclist_metrics::ThumbInset, dpi);
        g.sbThumb = {g.scrollbar.left + inset, thumbTop, g.scrollbar.right - inset,
                     thumbTop + thumbH};
        const int top = static_cast<int>(panelRect.top) - sc;
        g.items.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            g.items[static_cast<size_t>(i)] = {g.listArea.left, top + i * g.itemHeight,
                                               g.listArea.right,
                                               top + (i + 1) * g.itemHeight};
        }
        return g;
    }

    // 无溢出：条目自面板顶排布，无滚动条
    g.items.resize(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        g.items[static_cast<size_t>(i)] = {panelRect.left,
                                           static_cast<int>(panelRect.top) + i * g.itemHeight,
                                           panelRect.right,
                                           static_cast<int>(panelRect.top) +
                                               (i + 1) * g.itemHeight};
    }
    return g;
}

int DocListHitItem(const DocListGeometry& geo, POINT clientPt) {
    if (geo.items.empty()) {
        return -1;
    }
    // 先限定在条目可视区（滚动条区/面板上下界外不命中）
    if (clientPt.x < geo.listArea.left || clientPt.x >= geo.listArea.right) {
        return -1;
    }
    if (clientPt.y < geo.panel.top || clientPt.y >= geo.panel.bottom) {
        return -1;
    }
    for (int i = 0; i < static_cast<int>(geo.items.size()); ++i) {
        const RECT& r = geo.items[static_cast<size_t>(i)];
        if (clientPt.y >= r.top && clientPt.y < r.bottom) {
            return i;
        }
    }
    return -1;  // 条目下方的空白
}

bool DocListHitSplitter(const RECT& panelRect, POINT clientPt, UINT dpi) {
    if (panelRect.right <= panelRect.left || panelRect.bottom <= panelRect.top) {
        return false;
    }
    const int w = Scale(doclist_metrics::SplitterWidth, dpi);
    return clientPt.x >= panelRect.right - w && clientPt.x < panelRect.right &&
           clientPt.y >= panelRect.top && clientPt.y < panelRect.bottom;
}

int ClampDocListScroll(int scroll, const DocListGeometry& geo) {
    return std::clamp(scroll, 0, geo.maxScroll);
}

int ClampDocListWidthLogical(int logicalWidth, int maxLogicalWidth) {
    const int lower = doclist_metrics::MinWidth;
    const int upper = std::max(lower, maxLogicalWidth);  // 极窄窗口上界<下限：下限硬约束
    return std::clamp(logicalWidth, lower, upper);
}

std::wstring AccessibleDocListName(const DocListItem& item) {
    std::wstring name = item.displayName;
    if (item.modified) {
        name += L"，未保存";
    }
    return name;
}

// ---------------------------------------------------------------------------
// DocListPanel：附着/拆除
// ---------------------------------------------------------------------------

void DocListPanel::attach(HWND hwnd, MenuHost* menuHost) {
    hwnd_ = hwnd;
    menuHost_ = menuHost;
    if (tooltip_) {  // 重复 attach 防御：先拆旧 tooltip（防窗口句柄泄漏）
        DestroyWindow(tooltip_);
        tooltip_ = nullptr;
    }
    // tooltip 控件（comctl32，系统 DLL）：手动 TRACK 模式，延迟自行计时
    // （与 TabStrip 同款做法；宿主 timer id = kDocListTooltipTimerId）
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

void DocListPanel::detach() {
    // 拖拽/按下状态安全终止（销毁路径，不回调；宽度保留现值不回退）
    clearSplitterDragState();
    endThumbDrag();
    thumbDrag_ = false;
    sbPressed_ = SbPart::None;
    sbHover_ = SbPart::None;
    splitterHover_ = false;
    hoverIndex_ = -1;
    pressedIndex_ = -1;
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
    markWidthPx_ = 0;
    items_.clear();
    activeIndex_ = -1;
    scrollOffset_ = 0;
    visible_ = false;
    hasLayout_ = false;
    contentRect_ = {};
    maxWidthLogical_ = 0;
    hwnd_ = nullptr;
    menuHost_ = nullptr;
}

// ---------------------------------------------------------------------------
// 显隐与布局
// ---------------------------------------------------------------------------

void DocListPanel::setVisible(bool v) {
    if (visible_ == v) {
        return;
    }
    const RECT oldRect = panelRect();  // 翻转前矩形（隐藏时清除残影的失效依据）
    visible_ = v;
    if (!v) {
        // 隐藏即终止全部交互会话；分隔条拖拽不回宽不回调（面板不可见，宽度语义
        // 保留现值——重新显示后经 layout/onWidthChanged 正常流转）
        if (splitterDrag_) {
            clearSplitterDragState();
            if (hwnd_ && ::GetCapture() == hwnd_) {
                ReleaseCapture();  // 仅释放自己持有的捕获
            }
        }
        endThumbDrag();
        sbPressed_ = SbPart::None;
        sbHover_ = SbPart::None;
        splitterHover_ = false;
        hoverIndex_ = -1;
        pressedIndex_ = -1;
        hideTooltip();
    }
    if (hwnd_) {
        if (oldRect.right > oldRect.left) {
            InvalidateRect(hwnd_, &oldRect, FALSE);
        }
        const RECT nowRect = panelRect();
        if (nowRect.right > nowRect.left) {
            InvalidateRect(hwnd_, &nowRect, FALSE);
        }
    }
}

void DocListPanel::setWidthLogical(int w) {
    const int clamped = clampedWidth(w);
    if (clamped == widthLogical_) {
        return;
    }
    const RECT oldRect = panelRect();
    widthLogical_ = clamped;
    if (hwnd_) {
        if (oldRect.right > oldRect.left) {
            InvalidateRect(hwnd_, &oldRect, FALSE);
        }
        invalidateArea();
    }
}

void DocListPanel::layout(const RECT& contentRect, UINT dpi) {
    const RECT oldRect = panelRect();
    contentRect_ = contentRect;
    dpi_ = dpi > 0 ? dpi : 96;
    hasLayout_ = true;
    // 宽度上界：内容区宽/3（逻辑像素；物理→逻辑经 MulDiv 取整）
    const int physW = std::max(0, static_cast<int>(contentRect_.right) -
                                      static_cast<int>(contentRect_.left));
    const int logicalW = ::MulDiv(physW, 96, static_cast<int>(dpi_));
    maxWidthLogical_ = logicalW / 3;
    // 窗口收缩时上界变小自动钳制当前宽（组装层主动布局驱动，不回调）
    widthLogical_ = clampedWidth(widthLogical_);
    if (hwnd_) {
        if (oldRect.right > oldRect.left) {
            InvalidateRect(hwnd_, &oldRect, FALSE);
        }
        invalidateArea();
    }
}

RECT DocListPanel::panelRect() const {
    if (!visible_ || !hasLayout_) {
        return {};
    }
    const int w = Scale(widthLogical_, dpi_);
    RECT r = contentRect_;
    r.right = r.left + w;
    if (r.right > contentRect_.right) {
        r.right = contentRect_.right;  // 防御：内容区收缩到小于面板宽（极窄窗口）
    }
    return r;
}

int DocListPanel::clampedWidth(int w) const {
    if (maxWidthLogical_ <= 0) {
        return std::max(w, doclist_metrics::MinWidth);  // 未布局：仅下限
    }
    return ClampDocListWidthLogical(w, maxWidthLogical_);
}

// ---------------------------------------------------------------------------
// 数据（与 TabStrip 同源语义）
// ---------------------------------------------------------------------------

const DocListItem& DocListPanel::item(int index) const {
    static const DocListItem kEmpty;
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return kEmpty;
    }
    return items_[static_cast<size_t>(index)];
}

void DocListPanel::setItems(std::vector<DocListItem> items) {
    // 滚动条拖拽/按下索引上下文失效——安全终止（不回调；滚动保留现值经几何钳制）
    endThumbDrag();
    sbPressed_ = SbPart::None;
    pressedIndex_ = -1;
    items_ = std::move(items);
    const int n = static_cast<int>(items_.size());
    if (activeIndex_ >= n) {
        activeIndex_ = n - 1;  // 尽量保留原 active 位置语义（组装层随后精确映射）
    }
    if (hoverIndex_ >= n) {
        hoverIndex_ = -1;
    }
    hideTooltip();  // 数据变化隐藏（需求口径）
    invalidateArea();  // 滚动钳制在 geometry() 内惰性完成
}

void DocListPanel::setActiveIndex(int index) {
    const int n = static_cast<int>(items_.size());
    int idx = index;
    if (n <= 0) {
        idx = -1;
    } else {
        idx = std::clamp(idx, 0, n - 1);  // 越界钳制
    }
    if (idx == activeIndex_) {
        return;  // 同值不重滚（防外部高频事件把用户滚走的视图拉回）
    }
    activeIndex_ = idx;
    scrollIntoView(idx);
    invalidateArea();
}

bool DocListPanel::setModified(int index, bool modified) {
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return false;
    }
    if (items_[static_cast<size_t>(index)].modified == modified) {
        return false;  // 同值短路：不置脏、不触发重绘
    }
    items_[static_cast<size_t>(index)].modified = modified;
    hideTooltip();  // 显示中的 tooltip 文本已过时
    invalidateArea();
    return true;
}

bool DocListPanel::setDisplayName(int index, const std::wstring& name) {
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return false;
    }
    if (items_[static_cast<size_t>(index)].displayName == name) {
        return false;  // 同值短路（选区拖拽每帧到达的消抖，与 Tab 条同策略）
    }
    items_[static_cast<size_t>(index)].displayName = name;
    hideTooltip();
    invalidateArea();
    return true;
}

bool DocListPanel::setHasPath(int index, bool hasPath, std::wstring fullPath) {
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return false;
    }
    DocListItem& it = items_[static_cast<size_t>(index)];
    if (it.hasPath == hasPath && it.path == fullPath) {
        return false;  // 同值短路
    }
    it.hasPath = hasPath;
    it.path = std::move(fullPath);
    hideTooltip();  // tooltip 文本可能随路径翻转（未命名→另存为）
    invalidateArea();
    return true;
}

// ---------------------------------------------------------------------------
// 几何与滚动
// ---------------------------------------------------------------------------

DocListGeometry DocListPanel::geometry() const {
    DocListGeometry g = ComputeDocListGeometry(items_, panelRect(), dpi_, scrollOffset_);
    const int clamped = ClampDocListScroll(scrollOffset_, g);
    if (clamped != scrollOffset_) {
        scrollOffset_ = clamped;  // 数据/尺寸变化后惰性钳制（滚动不越过最大值）
        g = ComputeDocListGeometry(items_, panelRect(), dpi_, clamped);
    }
    return g;
}

int DocListPanel::scrollOffsetForTest() const {
    geometry();
    return scrollOffset_;
}

DocListGeometry DocListPanel::geometryForTest() const {
    return geometry();
}

int DocListPanel::hitItem(POINT clientPt) const {
    return DocListHitItem(geometry(), clientPt);
}

bool DocListPanel::hitSplitter(POINT clientPt) const {
    if (!visible_) {
        return false;
    }
    return DocListHitSplitter(panelRect(), clientPt, dpi_);
}

bool DocListPanel::hitInteractive(POINT clientPt) const {
    return ptInRect(panelRect(), clientPt);  // 面板本体整体接管（空矩形恒 false）
}

void DocListPanel::scrollIntoView(int index) {
    const DocListGeometry g = geometry();
    if (index < 0 || index >= static_cast<int>(g.items.size())) {
        return;
    }
    // 内容坐标：撤销滚动偏移与面板顶基准（客户 y → 内容 y = 客户 y − panel.top
    // + scrollOffset；items 矩形已含滚动平移）
    const int top = static_cast<int>(g.items[static_cast<size_t>(index)].top) -
                    static_cast<int>(g.panel.top) + scrollOffset_;
    const int bottom = static_cast<int>(g.items[static_cast<size_t>(index)].bottom) -
                       static_cast<int>(g.panel.top) + scrollOffset_;
    int offset = scrollOffset_;
    if (top < offset) {
        offset = top;
    } else if (bottom > offset + g.viewHeight) {
        offset = bottom - g.viewHeight;
    }
    offset = ClampDocListScroll(offset, g);
    if (offset != scrollOffset_) {
        scrollOffset_ = offset;
        hideTooltip();  // 滚动隐藏 tooltip
        invalidateArea();
    }
}

bool DocListPanel::scrollBy(int deltaPx) {
    const DocListGeometry g = geometry();
    if (!g.overflow) {
        return false;
    }
    const int sc = ClampDocListScroll(scrollOffset_ + deltaPx, g);
    if (sc == scrollOffset_) {
        return false;
    }
    scrollOffset_ = sc;
    hideTooltip();  // 滚动隐藏 tooltip
    invalidateArea();
    return true;
}

// ---------------------------------------------------------------------------
// 交互（main 转发）
// ---------------------------------------------------------------------------

void DocListPanel::onMouseMove(POINT clientPt) {
    if (splitterDrag_) {
        splitterMove(clientPt);
        return;  // 拖拽中不做悬停/tooltip（悬停已在起拖时清除）
    }
    if (thumbDrag_) {
        thumbDragMove(clientPt);
        return;
    }
    // 条目按下态的陈旧按钮自愈：真实按钮已松而 up 未到达（无捕获时窗外释放）
    if (pressedIndex_ >= 0 && ::GetKeyState(VK_LBUTTON) >= 0) {
        pressedIndex_ = -1;
        invalidateArea();
    }
    // 悬停（条目）
    const int hit = hitItem(clientPt);
    if (hit != hoverIndex_) {
        hoverIndex_ = hit;
        invalidateArea();
        if (hit >= 0) {
            // 进入新条目：隐藏旧 tooltip + 重计 600ms 延迟（文本在触发时取当前 hover）
            hideTooltip();
            armTooltipTimer();
        } else {
            hideTooltip();
        }
    }
    // 悬停（滚动条部件 + 分隔条热区）
    const SbPart part = hitScrollbarPart(clientPt);
    if (part != sbHover_) {
        sbHover_ = part;
        invalidateArea();
    }
    const bool spHover = hitSplitter(clientPt);
    if (spHover != splitterHover_) {
        splitterHover_ = spHover;
        invalidateArea();
    }
}

void DocListPanel::onMouseLeave() {
    // 拖拽态不取消（捕获期间光标短暂离窗是正常拖拽——对齐 Tab 条）
    if (splitterDrag_ || thumbDrag_) {
        return;
    }
    if (hoverIndex_ != -1) {
        hoverIndex_ = -1;
    }
    sbHover_ = SbPart::None;
    splitterHover_ = false;
    pressedIndex_ = -1;
    hideTooltip();
    invalidateArea();
}

void DocListPanel::onLButtonDown(POINT clientPt) {
    if (!visible_ || splitterDrag_ || thumbDrag_) {
        return;  // 拖拽会话中的重复 down（防御）：忽略，保持状态一致
    }
    // 1) 分隔条：起拖拽（SetCapture 主窗口）
    if (hitSplitter(clientPt)) {
        beginSplitterDrag(clientPt);
        return;
    }
    // 2) 滚动条：箭头步进/轨道翻页/滑块拖拽
    const SbPart part = hitScrollbarPart(clientPt);
    if (part != SbPart::None) {
        handleScrollbarDown(part, clientPt);
        return;
    }
    // 3) 条目：激活 + 记录按下（按下记录在激活回调之后：防御回调内整表重建）
    const int hit = hitItem(clientPt);
    if (hit >= 0) {
        hideTooltip();
        activateItem(hit);
        pressedIndex_ = (hit < static_cast<int>(items_.size())) ? hit : -1;
        invalidateArea();
    }
}

void DocListPanel::onLButtonUp(POINT /*clientPt*/) {
    if (splitterDrag_) {
        endSplitterDrag(false);  // 松手提交（最终 onWidthChanged；up 坐标不参与）
        return;
    }
    if (thumbDrag_) {
        endThumbDrag();  // 滑块拖拽结束：滚动已实时生效，仅收口状态
        return;
    }
    if (sbPressed_ != SbPart::None) {
        sbPressed_ = SbPart::None;
        if (hwnd_ && ::GetCapture() == hwnd_) {
            ReleaseCapture();  // 仅释放自己持有的捕获
        }
        invalidateArea();
        return;
    }
    // 纯点击（或无按下）：激活已在 down 完成——仅清按下记录
    if (pressedIndex_ != -1) {
        pressedIndex_ = -1;
        invalidateArea();
    }
}

void DocListPanel::onRButtonDown(POINT clientPt) {
    if (!visible_ || splitterDrag_ || thumbDrag_) {
        return;
    }
    const int hit = hitItem(clientPt);
    if (hit >= 0) {
        openContextMenu(hit, clientPt);  // 右键不激活该条目（对齐 Tab 语义）
    }
}

bool DocListPanel::onMouseWheel(int delta) {
    if (!visible_ || items_.empty()) {
        return false;
    }
    const DocListGeometry g = geometry();
    if (!g.overflow) {
        return false;  // 无溢出不滚动（滚轮留给组装层路由）
    }
    // 步长 = 3 行；滚轮向上（正 delta）= 偏移减小（内容向上滚出）
    const int stepPx = doclist_metrics::WheelStepLines * g.itemHeight;
    scrollBy(-::MulDiv(delta, stepPx, WHEEL_DELTA));
    return true;  // 溢出即接管（已到边界也消费，防穿透到编辑区）
}

bool DocListPanel::onKeyDown(UINT vk) {
    if (splitterDrag_ && vk == VK_ESCAPE) {
        endSplitterDrag(true);  // 拖拽中 Esc：取消回原宽（经回调）并消费
        return true;
    }
    return false;  // 上下键导航记为后续扩展（需求边界）
}

void DocListPanel::onTimer(UINT_PTR timerId) {
    if (timerId != kDocListTooltipTimerId) {
        return;
    }
    tipTimerArmed_ = false;  // 一次性
    if (visible_ && hoverIndex_ >= 0 && hoverIndex_ < static_cast<int>(items_.size())) {
        showTooltip();
    }
}

void DocListPanel::onCaptureChanged() {
    if (splitterDrag_) {
        endSplitterDrag(true);  // 捕获被夺走/释放：回原宽（经 onWidthChanged）
    }
    if (thumbDrag_) {
        endThumbDrag();  // 滑块拖拽中断：滚动已实时生效，保留现值收口
    }
    if (sbPressed_ != SbPart::None) {
        sbPressed_ = SbPart::None;
        invalidateArea();
    }
}

void DocListPanel::onWindowKillFocus() {
    if (splitterDrag_) {
        endSplitterDrag(true);  // 失焦取消：回原宽（经回调）
    }
    if (thumbDrag_) {
        endThumbDrag();
    }
    sbPressed_ = SbPart::None;
    sbHover_ = SbPart::None;
    splitterHover_ = false;
    hoverIndex_ = -1;
    pressedIndex_ = -1;
    hideTooltip();
    invalidateArea();
}

void DocListPanel::onDpiChanged() {
    fontDpi_ = 0;  // 字体/测量缓存失效（ensureFonts 惰性按当前 dpi_ 重建；
                   // dpi_ 由组装层随 WM_DPICHANGED 的下一次 layout 更新）
    invalidateArea();
}

void DocListPanel::onThemeChanged() {
    // tooltip 控件换肤（组件持有独立 tooltip 窗口，非 ManualTooltip）；绘制颜色
    // 全部经 CurrentPalette 动态取，整窗重绘由组装层统一触发
    if (tooltip_) {
        const ThemePalette& pal = CurrentPalette();
        SendMessageW(tooltip_, TTM_SETTIPBKCOLOR,
                     static_cast<WPARAM>(ToColorRef(pal.toolbarBackground)), 0);
        SendMessageW(tooltip_, TTM_SETTIPTEXTCOLOR,
                     static_cast<WPARAM>(ToColorRef(pal.textPrimary)), 0);
    }
}

void DocListPanel::activateItem(int index) {
    setActiveIndex(index);
    if (onDocActivated) {
        onDocActivated(index);  // 点击语义：同项也上抛（与 Tab 条一致，组装层可同值短路）
    }
}

// ---------------------------------------------------------------------------
// 分隔条拖拽状态机
// ---------------------------------------------------------------------------

void DocListPanel::beginSplitterDrag(POINT clientPt) {
    splitterDrag_ = true;
    splitterStartWidth_ = widthLogical_;
    splitterStartPt_ = clientPt;
    // 真实按钮采样：消息驱动的 down 此刻 GetKeyState 为按下态；selftest/单测的
    // 合成驱动为松开态——按钮防呆据此区分会话来源（见 splitterMove）
    splitterRealButton_ = (::GetKeyState(VK_LBUTTON) < 0);
    hoverIndex_ = -1;  // 悬停让位给拖拽反馈
    splitterHover_ = true;
    hideTooltip();
    if (hwnd_) {
        // 捕获保证窗外坐标继续送达；失败不阻塞进入拖拽态（headless/selftest
        // 下捕获可能不可用），无捕获时由再次交互/取消路径自然收口
        SetCapture(hwnd_);
    }
    invalidateArea();
}

void DocListPanel::splitterMove(POINT clientPt) {
    // 防呆：真实按钮发起的会话中检测到左键已松（up 消息丢失，如无捕获时窗外
    // 释放）→ 按取消收口；合成驱动（down 时刻采样无真实按钮）豁免该检测
    if (splitterRealButton_ && ::GetKeyState(VK_LBUTTON) >= 0) {
        endSplitterDrag(true);
        return;
    }
    // newWidth = 起始宽 + (dx 物理)×96/dpi（MulDiv 就近取整），经钳制生效
    const int dx = static_cast<int>(clientPt.x) - static_cast<int>(splitterStartPt_.x);
    const int newWidth = clampedWidth(
        splitterStartWidth_ + ::MulDiv(dx, 96, static_cast<int>(dpi_)));
    if (newWidth == widthLogical_) {
        return;
    }
    const RECT oldRect = panelRect();
    widthLogical_ = newWidth;
    if (onWidthChanged) {
        onWidthChanged(widthLogical_);  // 拖拽实时（main 据此重布局编辑区）
    }
    if (hwnd_) {
        if (oldRect.right > oldRect.left) {
            InvalidateRect(hwnd_, &oldRect, FALSE);  // 收缩侧旧区域重绘
        }
        invalidateArea();
    }
}

void DocListPanel::endSplitterDrag(bool restore) {
    if (!splitterDrag_) {
        return;
    }
    const RECT oldRect = panelRect();
    const int startWidth = splitterStartWidth_;
    // 先清状态再释放捕获：WM_CAPTURECHANGED 重入看到已复位状态，不二次处理
    clearSplitterDragState();
    if (hwnd_ && ::GetCapture() == hwnd_) {
        ReleaseCapture();  // 仅释放自己持有的捕获（防误伤他组件捕获）
    }
    if (restore) {
        const int back = clampedWidth(startWidth);
        if (back != widthLogical_) {
            widthLogical_ = back;
            if (onWidthChanged) {
                onWidthChanged(back);  // 回退也通知（编辑区恢复原布局）
            }
        }
    } else if (onWidthChanged) {
        onWidthChanged(widthLogical_);  // 提交：最终宽（main 的持久化时机）
    }
    if (hwnd_) {
        if (oldRect.right > oldRect.left) {
            InvalidateRect(hwnd_, &oldRect, FALSE);
        }
        invalidateArea();
    }
}

void DocListPanel::clearSplitterDragState() {
    splitterDrag_ = false;
    splitterStartWidth_ = 0;
    splitterStartPt_ = {};
    splitterRealButton_ = false;
}

// ---------------------------------------------------------------------------
// 滚动条交互
// ---------------------------------------------------------------------------

DocListPanel::SbPart DocListPanel::hitScrollbarPart(POINT clientPt) const {
    const DocListGeometry g = geometry();
    if (!g.overflow) {
        return SbPart::None;
    }
    if (ptInRect(g.sbUp, clientPt)) {
        return SbPart::Up;
    }
    if (ptInRect(g.sbDown, clientPt)) {
        return SbPart::Down;
    }
    if (ptInRect(g.sbThumb, clientPt)) {
        return SbPart::Thumb;
    }
    if (ptInRect(g.sbTrack, clientPt)) {
        return SbPart::Track;
    }
    return SbPart::None;
}

void DocListPanel::handleScrollbarDown(SbPart part, POINT clientPt) {
    sbPressed_ = part;
    if (hwnd_) {
        SetCapture(hwnd_);  // 按下期间捕获：up 可靠送达（防 mouse-up 穿透）
    }
    switch (part) {
    case SbPart::Up:
        scrollBy(-doclist_metrics::ArrowStepLines *
                 Scale(doclist_metrics::ItemHeight, dpi_));
        break;
    case SbPart::Down:
        scrollBy(+doclist_metrics::ArrowStepLines *
                 Scale(doclist_metrics::ItemHeight, dpi_));
        break;
    case SbPart::Thumb:
        thumbDrag_ = true;
        thumbDragStartPt_ = clientPt;
        thumbDragStartScroll_ = scrollOffset_;
        break;
    case SbPart::Track: {
        // 点击位置在滑块上方 → 向上翻页；下方 → 向下翻页（单次一页）
        const DocListGeometry g = geometry();
        if (clientPt.y < g.sbThumb.top) {
            scrollBy(-g.viewHeight);
        } else if (clientPt.y >= g.sbThumb.bottom) {
            scrollBy(+g.viewHeight);
        }
        break;
    }
    default:
        break;
    }
    hideTooltip();
    invalidateArea();
}

void DocListPanel::thumbDragMove(POINT clientPt) {
    const DocListGeometry g = geometry();
    if (!g.overflow) {
        thumbDrag_ = false;  // 防御：数据变化后无溢出（拖拽锚点失效）
        return;
    }
    const int trackH =
        std::max(0, static_cast<int>(g.sbTrack.bottom) - static_cast<int>(g.sbTrack.top));
    const int thumbH =
        std::max(0, static_cast<int>(g.sbThumb.bottom) - static_cast<int>(g.sbThumb.top));
    const int range = trackH - thumbH;
    if (range <= 0) {
        return;  // 滑块占满轨道（比例退化）：无拖拽自由度
    }
    const int dy = static_cast<int>(clientPt.y) - static_cast<int>(thumbDragStartPt_.y);
    const int sc = ClampDocListScroll(
        thumbDragStartScroll_ + ::MulDiv(dy, g.maxScroll, range), g);
    if (sc != scrollOffset_) {
        scrollOffset_ = sc;
        hideTooltip();
        invalidateArea();
    }
}

void DocListPanel::endThumbDrag() {
    if (!thumbDrag_) {
        return;
    }
    thumbDrag_ = false;
    sbPressed_ = SbPart::None;
    if (hwnd_ && ::GetCapture() == hwnd_) {
        ReleaseCapture();
    }
    invalidateArea();
}

// ---------------------------------------------------------------------------
// 右键菜单（复用 Tab 右键命令集）
// ---------------------------------------------------------------------------

void DocListPanel::openContextMenu(int index, POINT clientPt) {
    if (!hwnd_ || !menuHost_) {
        return;
    }
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return;  // 防御：弹出期间模型被外部变动导致索引失效
    }
    hideTooltip();
    const DocListItem& it = items_[static_cast<size_t>(index)];
    // 复用 Tab 右键命令集：同一 Command 枚举、同一置灰规则（数量动态文本 +
    // hasPath 控制「文件重命名…/打开所在文件夹」可用性）
    MenuModel model = BuildTabContextMenu(
        TabItem{it.displayName, it.modified, it.hasPath}, index,
        static_cast<int>(items_.size()));
    POINT anchor = clientPt;
    ClientToScreen(hwnd_, &anchor);
    // 命令经 track 返回值 → onDocContextCommand 上抛（携带被右键条目 index）；
    // 暂时摘除全局 onCommand 防止同命令双路由（与 TabStrip openContextMenu
    // 同手法）；uiLocked 语义由 MenuHost onOpened/onClosed 承担（组装层接线）
    auto saved = menuHost_->onCommand;
    menuHost_->onCommand = nullptr;
    const Command cmd = menuHost_->track(hwnd_, model, anchor);
    menuHost_->onCommand = saved;
    if (cmd != Command::None && onDocContextCommand) {
        onDocContextCommand(index, cmd);
    }
}

// ---------------------------------------------------------------------------
// tooltip（完整路径/显示名 + 未保存；TOOLTIPS_CLASSW 手动 TRACK，同 TabStrip）
// ---------------------------------------------------------------------------

std::wstring DocListPanel::tooltipTextFor(int index) const {
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return {};
    }
    const DocListItem& it = items_[static_cast<size_t>(index)];
    // hasPath 显示完整 path，否则 displayName（对齐 Tab tooltip 语义：未保存附注）
    std::wstring text = (it.hasPath && !it.path.empty()) ? it.path : it.displayName;
    if (it.modified) {
        text += L"（未保存）";
    }
    return text;
}

TOOLINFO DocListPanel::makeToolInfo() const {
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

void DocListPanel::armTooltipTimer() {
    if (hwnd_ && !tipTimerArmed_) {
        SetTimer(hwnd_, kDocListTooltipTimerId, doclist_metrics::TooltipDelayMs, nullptr);
        tipTimerArmed_ = true;
    }
}

void DocListPanel::hideTooltip() {
    if (tipTimerArmed_ && hwnd_) {
        KillTimer(hwnd_, kDocListTooltipTimerId);
        tipTimerArmed_ = false;
    }
    if (tipVisible_ && tooltip_) {
        TOOLINFO ti = makeToolInfo();
        SendMessageW(tooltip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&ti));
        tipVisible_ = false;
    }
}

void DocListPanel::showTooltip() {
    if (!tooltip_ || !hwnd_) {
        return;
    }
    if (hoverIndex_ < 0 || hoverIndex_ >= static_cast<int>(items_.size())) {
        return;
    }
    tipText_ = tooltipTextFor(hoverIndex_);
    // 定位：光标右下（屏幕坐标，钳制在所在显示器工作区内）
    POINT cursor{};
    GetCursorPos(&cursor);
    const UINT dpi = dpi_;
    int x = cursor.x + Scale(12, dpi);
    int y = cursor.y + Scale(16, dpi);
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
// 字体与测量
// ---------------------------------------------------------------------------

void DocListPanel::ensureFonts() const {
    if (labelFont_ && markFont_ && fontDpi_ == dpi_) {
        return;
    }
    if (labelFont_) {
        DeleteObject(labelFont_);
        labelFont_ = nullptr;
    }
    if (markFont_) {
        DeleteObject(markFont_);
        markFont_ = nullptr;
    }
    // 列表文字用系统 UI 字体（对齐 tabs.cpp 的 label 字体创建方式；pixelSize N
    // 逻辑像素 ≙ 字符高度 -Scale(N)，随 DPI 缩放）
    labelFont_ = CreateFontW(-Scale(doclist_metrics::LabelPixelSize, dpi_), 0, 0, 0, FW_NORMAL,
                             FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    markFont_ = CreateFontW(-Scale(doclist_metrics::MarkPixelSize, dpi_), 0, 0, 0, FW_NORMAL,
                            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    fontDpi_ = dpi_;
    // ● 标记实测宽（与渲染同字体；测量失败回退字号逻辑宽换算）
    markWidthPx_ = Scale(doclist_metrics::MarkPixelSize, dpi_);
    HDC dc = GetDC(nullptr);  // 屏幕 DC 测量（与渲染同字体，无需窗口 DC）
    if (dc && markFont_) {
        SIZE sz{};
        if (GetTextExtentPoint32W(dc, kUnsavedMarkText, 1, &sz)) {
            markWidthPx_ = sz.cx;
        }
    }
    if (dc) {
        ReleaseDC(nullptr, dc);
    }
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------

void DocListPanel::paint(HDC dc) {
    if (!visible_ || !dc) {
        return;
    }
    const DocListGeometry g = geometry();
    const RECT& pr = g.panel;
    if (pr.right <= pr.left || pr.bottom <= pr.top) {
        return;
    }
    const ThemePalette& pal = CurrentPalette();
    const UINT dpi = dpi_;
    ensureFonts();

    // —— 面板底色（titleBarBackground：比内容区深一档的侧栏表面，与标题栏同源）——
    HBRUSH bg = CreateSolidBrush(ToColorRef(pal.titleBarBackground));
    FillRect(dc, &pr, bg);
    DeleteObject(bg);

    SaveDC(dc);
    // 裁剪到面板本体：滚动半出条目不溢画到编辑区/工具栏
    IntersectClipRect(dc, pr.left, pr.top, pr.right, pr.bottom);

    // —— 滚动条（轨道+按钮+滑块；先画，条目区与其无重叠）——
    if (g.overflow) {
        drawScrollbar(dc, g, pal, dpi);
    }

    // —— 条目 / 空态 ——
    if (items_.empty()) {
        // 空面板照常绘制（对齐 Notepad++ 常驻语义）：textSecondary 提示文字
        if (labelFont_) {
            SelectObject(dc, labelFont_);
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ToColorRef(pal.textSecondary));
        const int inset = Scale(doclist_metrics::EmptyHintTopInset, dpi);
        RECT hint{g.listArea.left, g.listArea.top + inset, g.listArea.right,
                  g.listArea.top + inset + g.itemHeight};
        DrawTextW(dc, kEmptyHintText, -1, &hint,
                  DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    } else {
        const int n = static_cast<int>(items_.size());
        const int indW = Scale(doclist_metrics::ActiveIndicatorWidth, dpi);
        const HGDIOBJ oldFont = labelFont_ ? SelectObject(dc, labelFont_) : nullptr;
        SetBkMode(dc, TRANSPARENT);
        for (int i = 0; i < n; ++i) {
            const RECT row = g.items[static_cast<size_t>(i)];
            if (row.top >= pr.bottom || row.bottom <= pr.top) {
                continue;  // 可视区外（滚动裁剪）
            }
            const DocListItem& it = items_[static_cast<size_t>(i)];
            const bool active = (i == activeIndex_);
            const bool pressed = (i == pressedIndex_);
            // 行底色：活动 = toolbarBackground（与活动 Tab 同色语义）；悬停 =
            // hover 半透明预合成；再叠 pressed（按下反馈，桌面惯例三态）
            uint32_t rowColor = pal.titleBarBackground;
            if (active) {
                rowColor = pal.toolbarBackground;
            } else if (i == hoverIndex_) {
                rowColor = BlendOver(pal.titleBarBackground, pal.hover);
            }
            if (pressed) {
                rowColor = BlendOver(rowColor, pal.pressed);
            }
            HBRUSH rb = CreateSolidBrush(ToColorRef(rowColor));
            FillRect(dc, &row, rb);
            DeleteObject(rb);
            // 活动指示条（accent 左缘竖条：形状线索，与背景高亮共同表达活动态）
            if (active && indW > 0) {
                RECT ind{row.left, row.top, row.left + indW, row.bottom};
                HBRUSH ib = CreateSolidBrush(ToColorRef(pal.accent));
                FillRect(dc, &ind, ib);
                DeleteObject(ib);
            }
            // ● 标记 + 标签（超宽 DT_END_ELLIPSIS；markFont 仅画标记期间切入）
            RECT mark{}, label{};
            itemSubRects(row, it.modified, markWidthPx_, dpi, mark, label);
            if (it.modified && markFont_) {
                SelectObject(dc, markFont_);
                SetTextColor(dc, ToColorRef(pal.unsavedMark));
                DrawTextW(dc, kUnsavedMarkText, -1, &mark,
                          DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
                if (labelFont_) {
                    SelectObject(dc, labelFont_);
                }
            }
            SetTextColor(dc, ToColorRef(active ? pal.textPrimary : pal.textSecondary));
            DrawTextW(dc, it.displayName.c_str(), -1, &label,
                      DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        if (oldFont) {
            SelectObject(dc, oldFont);
        }
    }

    // —— 分隔条热区反馈（hover 高亮整条热区；拖拽中再叠 accent 竖线）——
    if (splitterHover_ || splitterDrag_) {
        const int zw = Scale(doclist_metrics::SplitterWidth, dpi);
        RECT zone{pr.right - zw, pr.top, pr.right, pr.bottom};
        HBRUSH zb = CreateSolidBrush(ToColorRef(BlendOver(pal.titleBarBackground, pal.hover)));
        FillRect(dc, &zone, zb);
        DeleteObject(zb);
        if (splitterDrag_) {
            const int lw = std::max(1, Scale(2, dpi));
            RECT line{pr.right - lw, pr.top, pr.right, pr.bottom};
            HBRUSH ab = CreateSolidBrush(ToColorRef(pal.accent));
            FillRect(dc, &line, ab);
            DeleteObject(ab);
        }
    }

    // —— 右缘分隔线（border 1px：面板与编辑区的常驻视觉边界，最后画保持清晰）——
    {
        const int lw = std::max(1, Scale(1, dpi));
        RECT sep{pr.right - lw, pr.top, pr.right, pr.bottom};
        HBRUSH sb = CreateSolidBrush(ToColorRef(pal.border));
        FillRect(dc, &sep, sb);
        DeleteObject(sb);
    }

    RestoreDC(dc, -1);
}

void DocListPanel::drawScrollbar(HDC dc, const DocListGeometry& g, const ThemePalette& pal,
                                 UINT /*dpi*/) {
    // 轨道底（含箭头区）：hover 半透明叠加（比面板底深一档的浅轨道）
    const COLORREF track = ToColorRef(BlendOver(pal.titleBarBackground, pal.hover));
    HBRUSH tb = CreateSolidBrush(track);
    FillRect(dc, &g.scrollbar, tb);
    DeleteObject(tb);

    // 上下箭头按钮：hover/pressed 叠加 + 三角字形（到边界时字形 textDisabled）
    const auto drawButton = [&](const RECT& btn, bool up, SbPart part, bool atEdge) {
        const bool hovered = (sbHover_ == part);
        const bool pressed = (sbPressed_ == part);
        if (hovered || pressed) {
            const uint32_t overlay = pressed ? pal.pressed : pal.hover;
            HBRUSH b =
                CreateSolidBrush(ToColorRef(BlendOver(BlendOver(pal.titleBarBackground,
                                                                pal.hover),
                                                      overlay)));
            FillRect(dc, &btn, b);
            DeleteObject(b);
        }
        const COLORREF glyph = atEdge
                                   ? ToColorRef(pal.textDisabled)
                                   : ToColorRef((hovered || pressed) ? pal.textPrimary
                                                                     : pal.textSecondary);
        drawArrowGlyph(dc, btn, up, glyph);
    };
    drawButton(g.sbUp, true, SbPart::Up, scrollOffset_ <= 0);
    drawButton(g.sbDown, false, SbPart::Down, scrollOffset_ >= g.maxScroll);

    // 比例滑块：正常 pressed 叠加色；hover 加深；拖拽中 accent（即时反馈）
    uint32_t thumbColor = BlendOver(pal.titleBarBackground, pal.pressed);
    if (sbPressed_ == SbPart::Thumb) {
        thumbColor = pal.accent;
    } else if (sbHover_ == SbPart::Thumb) {
        thumbColor = BlendOver(BlendOver(pal.titleBarBackground, pal.pressed), pal.hover);
    }
    HBRUSH thb = CreateSolidBrush(ToColorRef(thumbColor));
    FillRect(dc, &g.sbThumb, thb);
    DeleteObject(thb);
}

void DocListPanel::invalidateArea() const {
    if (!hwnd_) {
        return;
    }
    const RECT r = panelRect();
    if (r.right > r.left && r.bottom > r.top) {
        InvalidateRect(hwnd_, &r, FALSE);
    }
}

}  // namespace ui
