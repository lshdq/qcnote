// 状态栏实现。段布局计算（数据变化时 update 急切重排以供差分失效；尺寸/DPI
// 变化置脏由绘制/tooltip 命中时惰性重排），绘制与 tooltip 命中共用同一布局；
// 路径段超宽以 DT_PATH_ELLIPSIS 中部省略（旧 QML ElideMiddle 语义）。
#include "statusbar.h"

#include <algorithm>

namespace ui {

// ---------------------------------------------------------------------------
// 字段文本纯函数
// ---------------------------------------------------------------------------

std::wstring StatusLineColText(const StatusData& d) {
    if (!d.hasDoc || d.cursorLine < 0 || d.cursorColumn < 0) {
        return L"行 —，列 —";
    }
    return L"行 " + std::to_wstring(d.cursorLine) + L"，列 " + std::to_wstring(d.cursorColumn);
}

std::wstring StatusSelectionText(const StatusData& d) {
    if (!d.hasDoc || d.selectedChars <= 0) return L"";
    return L"已选 " + std::to_wstring(d.selectedChars) + L" 字符";
}

std::wstring StatusCursorCountText(const StatusData& d) {
    if (!d.hasDoc || d.cursorCount <= 1) return L"";
    return L"光标 ×" + std::to_wstring(d.cursorCount);
}

std::wstring StatusTotalText(const StatusData& d) {
    if (!d.hasDoc) return L"";
    return L"共 " + std::to_wstring(d.totalChars) + L" 字符";
}

std::wstring StatusEncodingText(const StatusData& d) {
    return d.hasDoc ? d.encoding : L"";
}

std::wstring StatusLineEndingText(const StatusData& d) {
    return d.hasDoc ? d.lineEnding : L"";
}

// ---------------------------------------------------------------------------
// 段级差分失效（R2-A，纯函数）
// ---------------------------------------------------------------------------

namespace {

// RECT 全等（GDI RECT 无运算符；差分判定用）
bool RectEquals(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right &&
           a.bottom == b.bottom;
}

// R3-A：整条状态栏内存渲染表面（RAII，GDI 对象严格清理）。构造兼容
// DC + 位图并选入；失败时构造内自清理（ok() == false）。析构先复原默认
// 位图再逐个删除（选入中的位图不可 DeleteObject）。paint() 整条上屏与
// update() 差分矩形直绘共用，配合 drawBarContent 保证两路径像素一致。
// 条带面积小，per-call 创建/销毁即可（不做常驻缓存）
class MemRenderSurface {
public:
    MemRenderSurface(HDC compatibleWith, int w, int h) {
        if (compatibleWith && w > 0 && h > 0) {
            dc_ = CreateCompatibleDC(compatibleWith);
            bmp_ = dc_ ? CreateCompatibleBitmap(compatibleWith, w, h) : nullptr;
            if (ok()) {
                oldBmp_ = SelectObject(dc_, bmp_);
            } else {
                cleanup();
            }
        }
    }
    ~MemRenderSurface() { cleanup(); }
    MemRenderSurface(const MemRenderSurface&) = delete;
    MemRenderSurface& operator=(const MemRenderSurface&) = delete;
    bool ok() const { return dc_ != nullptr && bmp_ != nullptr; }
    HDC dc() const { return dc_; }

private:
    void cleanup() {
        if (dc_ && oldBmp_) SelectObject(dc_, oldBmp_);
        oldBmp_ = nullptr;
        if (bmp_) DeleteObject(bmp_);
        bmp_ = nullptr;
        if (dc_) DeleteDC(dc_);
        dc_ = nullptr;
    }
    HDC dc_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HGDIOBJ oldBmp_ = nullptr;
};

}  // namespace

std::vector<RECT> StatusDiffInvalidRects(const std::vector<StatusSegment>& oldSegments,
                                         const std::vector<StatusSegment>& newSegments) {
    std::vector<RECT> rects;
    static const StatusSegment kHidden;  // 长度不齐时越界下标按不可见空段处理（防御）
    const size_t n = std::max(oldSegments.size(), newSegments.size());
    for (size_t i = 0; i < n; ++i) {
        const StatusSegment& o = i < oldSegments.size() ? oldSegments[i] : kHidden;
        const StatusSegment& w = i < newSegments.size() ? newSegments[i] : kHidden;
        if (o.visible == w.visible && o.text == w.text && RectEquals(o.bounds, w.bounds)) {
            continue;  // 该段无变化：不失效
        }
        // 旧∪新并集：旧 visible 覆盖残影、新 visible 画新字；仅一侧可见取该侧
        RECT r{};
        bool any = false;
        if (o.visible) {
            r = o.bounds;
            any = true;
        }
        if (w.visible) {
            if (any) {
                r.left = std::min(r.left, w.bounds.left);
                r.top = std::min(r.top, w.bounds.top);
                r.right = std::max(r.right, w.bounds.right);
                r.bottom = std::max(r.bottom, w.bounds.bottom);
            } else {
                r = w.bounds;
            }
            any = true;
        }
        if (any) rects.push_back(r);
    }
    return rects;
}

// ---------------------------------------------------------------------------
// StatusBar
// ---------------------------------------------------------------------------

void StatusBar::attach(HWND hostWindow) {
    hwnd_ = hostWindow;
    tooltip_.create(hostWindow);
}

void StatusBar::detach() {
    if (tipTimerArmed_ && hwnd_) {
        KillTimer(hwnd_, tipTimerId_);
    }
    tooltip_.destroy();
    tipTimerArmed_ = false;
    tipVisible_ = false;
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    fontDpi_ = 0;
    hwnd_ = nullptr;
}

RECT StatusBar::rect(const RECT& clientRect) const {
    const UINT dpi = hwnd_ ? WindowDpi(hwnd_) : 96;
    const int h = Scale(statusbar_metrics::Height, dpi);
    return {clientRect.left, clientRect.bottom - h, clientRect.right, clientRect.bottom};
}

void StatusBar::ensureFont() const {
    const UINT dpi = hwnd_ ? WindowDpi(hwnd_) : 96;
    if (font_ && fontDpi_ == dpi) return;
    if (font_) DeleteObject(font_);
    font_ = CreateFontW(-Scale(statusbar_metrics::FontPixelSize, dpi), 0, 0, 0, FW_NORMAL,
                        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                        L"Segoe UI");
    fontDpi_ = dpi;
}

void StatusBar::measureAndLayout(const RECT& clientRect) const {
    const UINT dpi = hwnd_ ? WindowDpi(hwnd_) : 96;
    if (!layoutDirty_ && layoutDpi_ == dpi && RectEquals(clientCache_, clientRect)) return;
    ensureFont();

    segments_.assign(static_cast<size_t>(SegId::Count), StatusSegment{});
    auto textOf = [&](SegId id) -> std::wstring {
        switch (id) {
        case SegId::LineCol: return StatusLineColText(data_);
        case SegId::Selection: return StatusSelectionText(data_);
        case SegId::CursorCount: return StatusCursorCountText(data_);
        case SegId::Total: return StatusTotalText(data_);
        case SegId::Encoding: return StatusEncodingText(data_);
        case SegId::LineEnd: return StatusLineEndingText(data_);
        case SegId::Path: return data_.path;
        default: return L"";
        }
    };

    // 左组（行/列→总字符）从左排起；右组（编码/换行/路径）从右排起；
    // 路径段限宽 PathMaxWidth，剩余宽度不足时按段从左向右压缩省略。
    HDC dc = GetDC(hwnd_);
    if (!dc) return;
    const HGDIOBJ oldFont = SelectObject(dc, font_);

    struct Item { SegId id; std::wstring text; int width; };
    std::vector<Item> left, right;
    const auto addItem = [&](SegId id, std::vector<Item>& group) {
        std::wstring t = textOf(id);
        if (t.empty()) return;
        SIZE sz{};
        int w = 0;
        if (GetTextExtentPoint32W(dc, t.c_str(), static_cast<int>(t.size()), &sz)) {
            w = sz.cx;
        }
        group.push_back({id, std::move(t), w});
    };
    addItem(SegId::LineCol, left);
    addItem(SegId::Selection, left);
    addItem(SegId::CursorCount, left);
    addItem(SegId::Total, left);
    addItem(SegId::Encoding, right);
    addItem(SegId::LineEnd, right);
    addItem(SegId::Path, right);

    const int margin = Scale(statusbar_metrics::MarginH, dpi);
    const int spacing = Scale(statusbar_metrics::Spacing, dpi);
    const int availW = clientRect.right - clientRect.left - 2 * margin;
    const int barTop = clientRect.bottom - Scale(statusbar_metrics::Height, dpi);

    // 右组从右往左布置；路径段（组内最后元素）宽夹取到
    // min(PathMaxWidth, availW/2)——长路径不吞没左侧行/列字段
    const int pathMax =
        std::min(Scale(statusbar_metrics::PathMaxWidth, dpi), std::max(0, availW / 2));
    int xRight = clientRect.right - margin;
    for (size_t i = 0; i < right.size(); ++i) {
        Item& it = right[i];
        int w = it.width;
        if (it.id == SegId::Path) w = std::min(w, pathMax);
        const int leftEdge = xRight - w;
        StatusSegment& seg = segments_[static_cast<size_t>(it.id)];
        seg.text = it.text;
        seg.bounds = {leftEdge, barTop, xRight, clientRect.bottom};
        seg.visible = true;
        seg.isPath = (it.id == SegId::Path);
        xRight = leftEdge - spacing;
    }

    // 左组从左往右（超出时文本绘制阶段省略截断）
    int xLeft = clientRect.left + margin;
    const int leftStop = std::max(xLeft, xRight);  // 不与右组重叠
    for (const Item& it : left) {
        StatusSegment& seg = segments_[static_cast<size_t>(it.id)];
        int w = std::min(it.width, std::max(0, leftStop - xLeft));
        seg.text = it.text;
        seg.bounds = {xLeft, barTop, xLeft + w, clientRect.bottom};
        seg.visible = true;
        seg.isPath = (it.id == SegId::Path);
        xLeft += w + spacing;
    }

    SelectObject(dc, oldFont);
    ReleaseDC(hwnd_, dc);
    clientCache_ = clientRect;
    layoutDpi_ = dpi;
    layoutDirty_ = false;
}

void StatusBar::update(const StatusData& data) {
    // 等值跳过（DEC-004 前半）：拖拽中每次 mouse move → SCN_UPDATEUI →
    // RefreshStatusBar → 本函数，数据未变化时不置脏、不失效重绘。前提是
    // layoutDirty_ 为 false 时 segments_ 必然对应当前 data_（measureAndLayout
    // 失败路径不清脏标记，保证该不变式成立）
    if (!layoutDirty_ && data_ == data) return;

    // 段级差分失效（R2-A）：第 1 轮「任何变化整条失效 + 整条重绘」经真机
    // 复验仍闪烁——单击、列数变更等任何文字变化都把整条 BitBlt 一遍。改为
    // 只失效「发生变化的段」的旧∪新并集矩形：先快照旧布局段，置新数据后
    // 急切重排（差分需要新旧两份布局），再经纯函数求矩形。首次 update
    // （旧段空 → 全部按不可见处理）自然退化为逐段直绘，语义正确
    const std::vector<StatusSegment> oldSegments = segments_;
    data_ = data;
    layoutDirty_ = true;
    if (!hwnd_) return;  // 未附着：取不到客户区，paint 时惰性重排（整条绘制）
    RECT client{};
    GetClientRect(hwnd_, &client);
    measureAndLayout(client);  // 急切重排（成功后清除 layoutDirty_）
    if (layoutDirty_) {
        // 布局失败（GetDC 不可得，极罕见；失败路径不清脏的不变式保持）：
        // measureAndLayout 的 assign 已把 segments_ 清为全不可见空段，拿不到
        // 可靠新布局（差分缺新侧基准）→ 整条失效兜底
        invalidateBar();
        return;
    }
    const std::vector<RECT> diffs = StatusDiffInvalidRects(oldSegments, segments_);
    // 全部段无变化（如 path 变化但省略显示与宽度都不变）→ 零失效零绘制
    if (diffs.empty()) return;

    // —— R3-A 差分矩形同步直绘（不再 InvalidateRect 排队等 WM_PAINT）——
    // R2 复验：失效→WM_PAINT 之间存在系统清刷/调度间隔，用户仍见「行列
    // 小块闪」——只要走失效队列，失效与重绘之间就有可感知空白帧（参考
    // Notepad++ 原生控件式同步直绘：选区时只有行/列文字在变）。改为一次
    // 内存渲染整条后仅把差分矩形 BitBlt 上屏：同步写屏，无中间空白帧。
    // 系统暴露/尺寸（CS_HREDRAW|CS_VREDRAW）/DPI/主题全窗失效仍走
    // WM_PAINT 整条双缓冲（paint()），两条路径共用 drawBarContent。
    const RECT bar = rect(client);

    // 底缘边界线保护：E2 窗口底 1px 边界线由 TitleBar::paintWindowBorder
    // 在 WM_PAINT 末尾绘制（titlebar.cpp bottomRc = {client.bottom-1,
    // client.bottom}，落在状态栏 rect 内）。直绘若覆盖该行，下次系统暴露
    // 前无人恢复 → 差分矩形 bottom 一律钳到 bar.bottom-1（DT_VCENTER 文本
    // 不落最底行，钳制无视觉损失；最大化时边界线隐藏，钳制同样无害）。
    // 其余三边为防御性夹取：窗口尺寸/DPI 变化后旧段 bounds 可能越出新条带
    //（变窄/变矮），BitBlt 源坐标越界行为未定义，必须夹回条内
    std::vector<RECT> rects;
    rects.reserve(diffs.size());
    for (RECT r : diffs) {
        if (r.top < bar.top) r.top = bar.top;
        if (r.left < bar.left) r.left = bar.left;
        if (r.right > bar.right) r.right = bar.right;
        if (r.bottom > bar.bottom - 1) r.bottom = bar.bottom - 1;
        if (!IsRectEmpty(&r)) rects.push_back(r);
    }
    if (rects.empty()) return;  // 理论不达（段高 = 条高 > 1px），防御

    HDC wnd = GetDC(hwnd_);
    if (!wnd) {
        // 防御回退：拿不到窗口 DC（极罕见）→ 逐矩形失效，交 WM_PAINT 绘制
        for (const RECT& r : rects) InvalidateRect(hwnd_, &r, FALSE);
        return;
    }
    MemRenderSurface mem(wnd, bar.right - bar.left, bar.bottom - bar.top);
    if (mem.ok()) {
        drawBarToMemory(mem.dc(), bar);
        for (const RECT& r : rects) {
            BitBlt(wnd, r.left, r.top, r.right - r.left, r.bottom - r.top,
                   mem.dc(), r.left - bar.left, r.top - bar.top, SRCCOPY);
        }
    } else {
        // 防御回退：内存表面创建失败 → 逐矩形失效。不在窗口 DC 上直接
        // 分步绘制（drawBarContent 清底 → 写字两步间的空白帧正是要消灭
        // 的闪烁源）；失效路径经 WM_PAINT 的 paint() 双缓冲，无此问题
        for (const RECT& r : rects) InvalidateRect(hwnd_, &r, FALSE);
    }
    ReleaseDC(hwnd_, wnd);
}

void StatusBar::invalidateBar() const {
    if (!hwnd_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    RECT bar = rect(client);
    InvalidateRect(hwnd_, &bar, FALSE);
}

RECT StatusBar::pathRect(const RECT& clientRect) const {
    measureAndLayout(clientRect);
    const StatusSegment& seg = segments_[static_cast<size_t>(SegId::Path)];
    return seg.visible ? seg.bounds : RECT{};
}

void StatusBar::onMouseMove(POINT clientPt) {
    if (!hwnd_) return;
    RECT client{};
    GetClientRect(hwnd_, &client);
    const RECT path = pathRect(client);
    const bool onPath = !IsRectEmpty(&path) && PtInRect(&path, clientPt) != FALSE;
    if (!onPath) {
        if (tipTimerArmed_) {
            KillTimer(hwnd_, tipTimerId_);
            tipTimerArmed_ = false;
        }
        if (tipVisible_) {
            tooltip_.hide();
            tipVisible_ = false;
        }
        return;
    }
    if (!tipTimerArmed_ && !tipVisible_) {
        SetTimer(hwnd_, tipTimerId_, statusbar_metrics::TooltipDelayMs, nullptr);
        tipTimerArmed_ = true;
    }
}

void StatusBar::onMouseLeave() {
    if (tipTimerArmed_ && hwnd_) {
        KillTimer(hwnd_, tipTimerId_);
        tipTimerArmed_ = false;
    }
    if (tipVisible_) {
        tooltip_.hide();
        tipVisible_ = false;
    }
}

void StatusBar::onTimer(UINT_PTR timerId) {
    if (timerId != tipTimerId_ || !hwnd_) return;
    tipTimerArmed_ = false;
    KillTimer(hwnd_, tipTimerId_);
    if (data_.path.empty()) return;
    POINT cursor{};
    GetCursorPos(&cursor);
    tooltip_.show(data_.path, {cursor.x + 16, cursor.y + 20});
    tipVisible_ = true;
}

void StatusBar::onThemeChanged() {
    tooltip_.applyThemeColors();  // E5：tooltip 换肤（绘制色经 CurrentPalette 动态取）
}

// 整条内容绘制（R3-A 抽取自 paint() 原内联 lambda）：底色 + 顶部分隔线 +
// 各段文字。paint() 双缓冲与 update() 差分直绘共用——两条路径像素级一致
void StatusBar::drawBarContent(HDC target, const RECT& bar) const {
    const ThemePalette& pal = CurrentPalette();
    HBRUSH bg = CreateSolidBrush(ToColorRef(pal.statusBarBackground));
    FillRect(target, &bar, bg);
    DeleteObject(bg);
    // 顶部分隔线（旧 QML 顶部 1px border）
    HBRUSH line = CreateSolidBrush(ToColorRef(pal.border));
    RECT top{bar.left, bar.top, bar.right, bar.top + 1};
    FillRect(target, &top, line);
    DeleteObject(line);

    ensureFont();
    const HGDIOBJ oldFont = SelectObject(target, font_);
    SetBkMode(target, TRANSPARENT);
    SetTextColor(target, ToColorRef(pal.textSecondary));
    for (const StatusSegment& seg : segments_) {
        if (!seg.visible || seg.text.empty()) continue;
        RECT tr = seg.bounds;
        const UINT flags = DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX |
                           (seg.isPath ? DT_PATH_ELLIPSIS : DT_END_ELLIPSIS);
        DrawTextW(target, seg.text.c_str(), -1, &tr, flags);
    }
    SelectObject(target, oldFont);
}

// 渲染整条到内存位图（R3-A）：视口原点平移到条带左上角，drawBarContent
// 的绝对客户区坐标无需换算直接可画（含 DrawTextW 的 DT_PATH_ELLIPSIS/
// DT_END_ELLIPSIS 矩形）；完成后复位原点，位图 (0,0) 即条带左上角
void StatusBar::drawBarToMemory(HDC memDc, const RECT& bar) const {
    SetViewportOrgEx(memDc, -bar.left, -bar.top, nullptr);
    drawBarContent(memDc, bar);
    SetViewportOrgEx(memDc, 0, 0, nullptr);
}

void StatusBar::paint(HDC dc, const RECT& clientRect) {
    if (!dc) return;
    measureAndLayout(clientRect);

    const RECT bar = rect(clientRect);
    const int w = bar.right - bar.left;
    const int h = bar.bottom - bar.top;
    if (w <= 0 || h <= 0) return;

    // 内存 DC 双缓冲（DEC-004 后半）：清底与写字在内存位图上完成后一次
    // BitBlt 上屏——直接画屏幕 DC 时「FillRect 清底 → DrawTextW 写字」两步
    // 之间存在只有底色的空白帧，列选拖拽高频重绘下肉眼可见闪烁。渲染部分
    // （drawBarToMemory）与 update() 差分直绘共用（R3-A，像素一致）；创建
    // 失败回退为直接绘制路径（防御，仅覆盖 WM_PAINT 内罕见失败场景）
    MemRenderSurface mem(dc, w, h);
    if (mem.ok()) {
        drawBarToMemory(mem.dc(), bar);
        BitBlt(dc, bar.left, bar.top, w, h, mem.dc(), 0, 0, SRCCOPY);
    } else {
        drawBarContent(dc, bar);
    }
}

}  // namespace ui
