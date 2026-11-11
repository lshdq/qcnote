// 状态栏实现。段布局惰性计算（数据/尺寸/DPI 变化置脏），绘制与 tooltip 命中共用
// 同一布局；路径段超宽以 DT_PATH_ELLIPSIS 中部省略（旧 QML ElideMiddle 语义）。
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
    const auto rectEq = [](const RECT& a, const RECT& b) {
        return a.left == b.left && a.top == b.top && a.right == b.right &&
               a.bottom == b.bottom;
    };
    if (!layoutDirty_ && layoutDpi_ == dpi && rectEq(clientCache_, clientRect)) return;
    ensureFont();

    segments_.assign(static_cast<size_t>(SegId::Count), Segment{});
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
        Segment& seg = segments_[static_cast<size_t>(it.id)];
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
        Segment& seg = segments_[static_cast<size_t>(it.id)];
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
    data_ = data;
    layoutDirty_ = true;
    invalidateBar();
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
    const Segment& seg = segments_[static_cast<size_t>(SegId::Path)];
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

void StatusBar::paint(HDC dc, const RECT& clientRect) {
    if (!dc) return;
    const ThemePalette& pal = CurrentPalette();
    measureAndLayout(clientRect);

    const RECT bar = rect(clientRect);
    HBRUSH bg = CreateSolidBrush(ToColorRef(pal.statusBarBackground));
    FillRect(dc, &bar, bg);
    DeleteObject(bg);
    // 顶部分隔线（旧 QML 顶部 1px border）
    HBRUSH line = CreateSolidBrush(ToColorRef(pal.border));
    RECT top{bar.left, bar.top, bar.right, bar.top + 1};
    FillRect(dc, &top, line);
    DeleteObject(line);

    ensureFont();
    const HGDIOBJ oldFont = SelectObject(dc, font_);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, ToColorRef(pal.textSecondary));
    for (const Segment& seg : segments_) {
        if (!seg.visible || seg.text.empty()) continue;
        RECT tr = seg.bounds;
        const UINT flags = DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX |
                           (seg.isPath ? DT_PATH_ELLIPSIS : DT_END_ELLIPSIS);
        DrawTextW(dc, seg.text.c_str(), -1, &tr, flags);
    }
    SelectObject(dc, oldFont);
}

}  // namespace ui
