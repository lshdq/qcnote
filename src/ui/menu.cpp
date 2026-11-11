// 自绘弹出菜单实现。布局/数据模型为纯函数，窗口与消息循环在 MenuHost 内。
//
// 绘制全部 GDI（纯色块+线段+Polygon），弹层窗口内容经内存 DC 双缓冲一次
// BitBlt；圆角经 DWMWA_WINDOW_CORNER_PREFERENCE（Win11，Win10 静默回退直角），
// 投影由窗口类 CS_DROPSHADOW 提供，容器边缘以 border token 描 1 物理像素线。
#include "menu.h"

#include <algorithm>
#include <cassert>
#include <cwchar>

#include <dwmapi.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

namespace ui {

namespace {

constexpr wchar_t kMenuClassName[] = L"QcnoteMenuPopup";
constexpr UINT_PTR kExpandTimerId = 1;
constexpr UINT kExpandDelayMs = 300;  // 悬停展开子菜单延迟（需求 ~300ms）

void RegisterMenuWindowClass() {
    static const bool registered = [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        // CS_SAVEBITS：弹出层移走后系统按保存位图恢复底面；CS_DROPSHADOW：
        // 菜单式右下投影（与 DWM 圆角叠加，Win11 观感对齐系统菜单）
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_SAVEBITS | CS_DROPSHADOW;
        wc.lpfnWndProc = &MenuHost::MenuWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;  // WM_PAINT 全覆盖（WM_ERASEBKGND 返回 1）
        wc.lpszClassName = kMenuClassName;
        return RegisterClassExW(&wc) != 0;
    }();
    (void)registered;
}

RECT WorkAreaOf(HMONITOR mon, const RECT& fallback) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        return mi.rcWork;
    }
    return fallback;
}

}  // namespace

// ---------------------------------------------------------------------------
// MenuModel
// ---------------------------------------------------------------------------

int MenuModel::addMenu(std::wstring title) {
    menus_.push_back(Menu{std::move(title), {}});
    return static_cast<int>(menus_.size()) - 1;
}

MenuItem& MenuModel::addItem(int menuId, std::wstring text, std::wstring shortcut,
                             Command command) {
    assert(menuId >= 0 && menuId < static_cast<int>(menus_.size()));
    MenuItem it;
    it.kind = MenuItem::Kind::Action;
    it.text = std::move(text);
    it.shortcut = std::move(shortcut);
    it.command = command;
    menus_[menuId].items.push_back(std::move(it));
    return menus_[menuId].items.back();
}

MenuItem& MenuModel::addSubmenuEntry(int menuId, std::wstring text, int submenuId) {
    assert(menuId >= 0 && menuId < static_cast<int>(menus_.size()));
    assert(submenuId >= 0 && submenuId < static_cast<int>(menus_.size()));
    assert(submenuId != menuId);  // 禁止自引用环
    MenuItem it;
    it.kind = MenuItem::Kind::Submenu;
    it.text = std::move(text);
    it.submenu = submenuId;
    menus_[menuId].items.push_back(std::move(it));
    return menus_[menuId].items.back();
}

MenuItem& MenuModel::addSeparator(int menuId) {
    assert(menuId >= 0 && menuId < static_cast<int>(menus_.size()));
    MenuItem it;
    it.kind = MenuItem::Kind::Separator;
    menus_[menuId].items.push_back(std::move(it));
    return menus_[menuId].items.back();
}

int MenuModel::menuCount() const {
    return static_cast<int>(menus_.size());
}

int MenuModel::itemCount(int menuId) const {
    assert(menuId >= 0 && menuId < static_cast<int>(menus_.size()));
    if (menuId < 0 || menuId >= static_cast<int>(menus_.size())) return 0;
    return static_cast<int>(menus_[menuId].items.size());
}

const std::vector<MenuItem>& MenuModel::items(int menuId) const {
    assert(menuId >= 0 && menuId < static_cast<int>(menus_.size()));
    return menus_[menuId].items;
}

const MenuItem& MenuModel::item(int menuId, int index) const {
    const auto& list = items(menuId);
    assert(index >= 0 && index < static_cast<int>(list.size()));
    if (index < 0 || index >= static_cast<int>(list.size())) {
        static const MenuItem kEmpty{};
        return kEmpty;
    }
    return list[static_cast<size_t>(index)];
}

const std::wstring& MenuModel::title(int menuId) const {
    assert(menuId >= 0 && menuId < static_cast<int>(menus_.size()));
    if (menuId < 0 || menuId >= static_cast<int>(menus_.size())) {
        static const std::wstring kEmpty;
        return kEmpty;
    }
    return menus_[menuId].title;
}

std::optional<MenuModel::ItemRef> MenuModel::findCommand(Command command) const {
    for (int m = 0; m < static_cast<int>(menus_.size()); ++m) {
        const auto& list = menus_[m].items;
        for (int i = 0; i < static_cast<int>(list.size()); ++i) {
            if (list[i].command == command) {
                return ItemRef{m, i};
            }
        }
    }
    return std::nullopt;
}

bool MenuModel::setChecked(Command command, bool checked) {
    const auto ref = findCommand(command);
    if (!ref) return false;
    menus_[ref->menuId].items[static_cast<size_t>(ref->index)].checked = checked;
    return true;
}

bool MenuModel::setEnabled(Command command, bool enabled) {
    const auto ref = findCommand(command);
    if (!ref) return false;
    menus_[ref->menuId].items[static_cast<size_t>(ref->index)].enabled = enabled;
    return true;
}

bool MenuModel::isChecked(Command command) const {
    const auto ref = findCommand(command);
    return ref && item(ref->menuId, ref->index).checked;
}

bool MenuModel::isEnabled(Command command) const {
    const auto ref = findCommand(command);
    return ref && item(ref->menuId, ref->index).enabled;
}

// ---------------------------------------------------------------------------
// 布局纯函数
// ---------------------------------------------------------------------------

MenuLayout ComputeMenuLayout(const std::vector<MenuItem>& items,
                             const std::vector<int>& labelWidths,
                             const std::vector<int>& shortcutWidths, UINT dpi) {
    MenuLayout out;
    const int n = static_cast<int>(items.size());
    assert(labelWidths.size() == items.size() && shortcutWidths.size() == items.size());
    if (n == 0) {
        out.width = Scale(menu_metrics::MinWidth, dpi);
        out.height = 2;  // 上下边框
        return out;
    }

    bool hasShortcutColumn = false;
    bool hasArrowColumn = false;
    int labelCol = 0;
    int shortcutCol = 0;
    for (int i = 0; i < n; ++i) {
        const MenuItem& it = items[static_cast<size_t>(i)];
        if (it.kind == MenuItem::Kind::Submenu) hasArrowColumn = true;
        if (!it.shortcut.empty()) {
            hasShortcutColumn = true;
            shortcutCol = std::max(shortcutCol, shortcutWidths[static_cast<size_t>(i)]);
        }
        if (it.kind != MenuItem::Kind::Separator) {
            labelCol = std::max(labelCol, labelWidths[static_cast<size_t>(i)]);
        }
    }

    // 左段：边距 + 勾选列 + 间隙；右段：快捷键（间隙+列）+ 箭头列 + 边距
    const int left = Scale(menu_metrics::PadLeft, dpi) +
                     Scale(menu_metrics::CheckColumn, dpi) +
                     Scale(menu_metrics::LabelGap, dpi);
    const int right =
        (hasShortcutColumn ? Scale(menu_metrics::ShortcutGap, dpi) + shortcutCol : 0) +
        (hasArrowColumn ? Scale(menu_metrics::ArrowColumn, dpi) : 0) +
        Scale(menu_metrics::PadRight, dpi);
    int width = std::clamp(left + labelCol + right, Scale(menu_metrics::MinWidth, dpi),
                           Scale(menu_metrics::MaxWidth, dpi));

    out.rows.resize(items.size());
    int y = 1;  // 顶边框（1 物理像素，不随 DPI——对齐系统菜单/窗口边框惯例）
    for (int i = 0; i < n; ++i) {
        const MenuItem& it = items[static_cast<size_t>(i)];
        const int h = Scale(it.kind == MenuItem::Kind::Separator
                                ? menu_metrics::SeparatorHeight
                                : menu_metrics::ItemHeight,
                            dpi);
        MenuLayout::Row& row = out.rows[static_cast<size_t>(i)];
        row.bounds = {0, y, width, y + h};
        // 标签区右界 = 快捷键列起点（超宽时 DrawText 省略号截断）
        row.text = {left, y, width - right, y + h};
        if (hasShortcutColumn) {
            const int scLeft = width - right + Scale(menu_metrics::ShortcutGap, dpi);
            row.shortcut = {scLeft, y, scLeft + shortcutCol, y + h};
        }
        row.hasCheck = it.checked;
        row.hasArrow = (it.kind == MenuItem::Kind::Submenu);
        y += h;
    }
    out.width = width;
    out.height = y + 1;  // 底边框
    return out;
}

int MenuLayoutHitTest(const MenuLayout& layout, POINT pt) {
    for (int i = 0; i < static_cast<int>(layout.rows.size()); ++i) {
        const RECT& r = layout.rows[static_cast<size_t>(i)].bounds;
        if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom) {
            return i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// MenuHost：弹出层
// ---------------------------------------------------------------------------

Command MenuHost::track(HWND owner, const MenuModel& model, POINT anchor) {
    if (tracking_ || !owner || !IsWindow(owner)) {
        return Command::None;
    }
    if (model.menuCount() == 0 || model.itemCount(0) == 0) {
        return Command::None;
    }

    tracking_ = true;
    pendingCommand_ = Command::None;
    owner_ = owner;
    model_ = &model;
    dpi_ = WindowDpi(owner);
    ensureSessionFont();

    if (!openRoot(anchor)) {
        cleanupSession();
        tracking_ = false;
        return Command::None;
    }
    HWND root = levels_[0].hwnd;
    // 鼠标捕获挂根弹出层：所有鼠标消息（含越过子菜单/主窗口时）统一路由到根层，
    // 由本类按各层窗口矩形做跨层命中。
    //
    // 键盘不夺取焦点：TrackPopupMenu 式菜单在模态循环内拦截键盘消息（见循环体），
    // 不依赖弹出层获得输入焦点——避免 SetFocus 引发激活/焦点转移（主窗口被
    // 去激活会闪灰 E2 边界线），也规避 NOACTIVATE 窗口的焦点时序竞态。
    // 菜单打开期间宿主窗口按键被循环吞掉（模态语义）。
    SetCapture(root);
    if (onOpened) onOpened();

    done_ = false;
    MSG msg{};
    while (!done_) {
        const BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r <= 0) {
            if (r == 0) {
                // WM_QUIT：清理后原样交还外层消息循环（TrackPopupMenu 同款约定）
                PostQuitMessage(static_cast<int>(msg.wParam));
            }
            break;
        }
        if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
            // 键盘模态拦截：焦点窗口（宿主或本组件窗口）的按键在此统一处理，
            // 不再向宿主窗口分发（菜单打开期间屏蔽主窗口快捷键的自然实现）
            handleKeyMessage(static_cast<UINT>(msg.wParam), msg.message == WM_SYSKEYDOWN);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    const Command result = pendingCommand_;
    cleanupSession();
    // 焦点从未夺取（键盘经模态循环拦截，焦点始终在宿主窗口），无需恢复；
    // 防御：若焦点意外落在本组件已销毁窗口，归还宿主
    if (IsWindow(owner_) && GetFocus() && !IsWindow(GetFocus())) {
        SetFocus(owner_);
    }
    owner_ = nullptr;
    tracking_ = false;
    if (onClosed) onClosed();
    // 命令在模态循环退出、焦点恢复、closed 回调之后才分发——命令处理器打开
    // 对话框/关闭应用均不会重入本菜单循环
    if (result != Command::None && onCommand) onCommand(result);
    return result;
}

bool MenuHost::openRoot(POINT anchor) {
    const auto& rootItems = model_->items(0);
    std::vector<int> labels, shortcuts;
    measure(rootItems, labels, shortcuts);
    MenuLayout layout = ComputeMenuLayout(rootItems, labels, shortcuts, dpi_);

    const RECT fallback{0, 0, 4096, 2048};
    const RECT work = WorkAreaOf(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), fallback);
    const int w = layout.width;
    const int h = layout.height;
    int x = anchor.x;
    int y = anchor.y;
    if (x + w > work.right) x = std::max(work.left, work.right - w);  // 右溢出左移
    if (y + h > work.bottom) y = anchor.y - h;  // 下方放不下：翻转到锚点上方
    if (y < work.top) y = work.top;

    HWND hwnd = createLevelWindow({x, y, x + w, y + h});
    if (!hwnd) return false;
    levels_.push_back(Level{hwnd, 0, -1, RECT{x, y, x + w, y + h}, std::move(layout)});
    return true;
}

void MenuHost::openSubmenu(int parentLevelIdx, int itemIdx) {
    if (parentLevelIdx < 0 || parentLevelIdx >= static_cast<int>(levels_.size())) return;
    const MenuItem& parentItem = model_->item(levels_[parentLevelIdx].menuId, itemIdx);
    if (parentItem.kind != MenuItem::Kind::Submenu || !parentItem.enabled) return;
    if (parentItem.submenu < 0 || parentItem.submenu >= model_->menuCount()) return;
    const auto& subItems = model_->items(parentItem.submenu);
    if (subItems.empty()) return;  // 空子菜单不弹窗

    closeDeeperThan(parentLevelIdx);

    std::vector<int> labels, shortcuts;
    measure(subItems, labels, shortcuts);
    MenuLayout layout = ComputeMenuLayout(subItems, labels, shortcuts, dpi_);

    // 定位：父级条目右缘右侧、顶边对齐条目；右缘放不下翻到左侧；底部溢出上移。
    const Level& parent = levels_[parentLevelIdx];
    const RECT& row = parent.layout.rows[static_cast<size_t>(itemIdx)].bounds;
    const RECT fallback{0, 0, 4096, 2048};
    const RECT work = WorkAreaOf(MonitorFromRect(&parent.screenRect, MONITOR_DEFAULTTONEAREST),
                                 fallback);
    const int w = layout.width;
    const int h = layout.height;
    int x = parent.screenRect.right;
    if (x + w > work.right) x = parent.screenRect.left - w;
    if (x < work.left) x = work.left;
    int y = parent.screenRect.top + row.top;
    if (y + h > work.bottom) y = work.bottom - h;
    if (y < work.top) y = work.top;

    HWND hwnd = createLevelWindow({x, y, x + w, y + h});
    if (!hwnd) return;
    levels_.push_back(
        Level{hwnd, parentItem.submenu, -1, RECT{x, y, x + w, y + h}, std::move(layout)});
}

void MenuHost::closeDeeperThan(int levelIdx) {
    while (static_cast<int>(levels_.size()) > levelIdx + 1) {
        Level& lv = levels_.back();
        if (IsWindow(lv.hwnd)) {
            const bool reopenGuard = closing_;
            closing_ = true;
            DestroyWindow(lv.hwnd);
            closing_ = reopenGuard;
        }
        levels_.pop_back();
    }
}

void MenuHost::closeDeepest() {
    if (levels_.size() <= 1) return;
    closeDeeperThan(static_cast<int>(levels_.size()) - 2);
}

void MenuHost::dismissAll() {
    if (!closing_) {
        closing_ = true;
        killPendingExpand();
        closeDeeperThan(-1);  // closing_ 已置位，closeDeeperThan 内的守卫不再重复
        closing_ = false;
    }
    done_ = true;
}

void MenuHost::cleanupSession() {
    closing_ = true;
    killPendingExpand();
    if (!levels_.empty() && IsWindow(levels_[0].hwnd) && GetCapture() == levels_[0].hwnd) {
        ReleaseCapture();  // 触发 WM_CAPTURECHANGED，closing_ 守卫忽略
    }
    for (auto& lv : levels_) {
        if (IsWindow(lv.hwnd)) DestroyWindow(lv.hwnd);
    }
    levels_.clear();
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    fontDpi_ = 0;
    model_ = nullptr;
    pendingLevel_ = pendingItem_ = -1;
    pendingCommand_ = Command::None;
    closing_ = false;
}

// ---------------------------------------------------------------------------
// 交互
// ---------------------------------------------------------------------------

void MenuHost::setSelected(int levelIdx, int itemIdx) {
    if (levelIdx < 0 || levelIdx >= static_cast<int>(levels_.size())) return;
    Level& lv = levels_[static_cast<size_t>(levelIdx)];
    if (lv.selectedIndex == itemIdx) return;
    lv.selectedIndex = itemIdx;
    if (IsWindow(lv.hwnd)) InvalidateRect(lv.hwnd, nullptr, FALSE);
}

void MenuHost::clearAllSelection() {
    for (int i = 0; i < static_cast<int>(levels_.size()); ++i) {
        setSelected(i, -1);
    }
}

void MenuHost::moveSelection(int dir) {
    if (levels_.empty()) return;
    const int li = static_cast<int>(levels_.size()) - 1;  // 键盘作用于最深展开级
    const auto& items = model_->items(levels_[static_cast<size_t>(li)].menuId);
    const int n = static_cast<int>(items.size());
    if (n == 0) return;
    int i = levels_[static_cast<size_t>(li)].selectedIndex;
    for (int step = 0; step < n; ++step) {
        i = (i + dir + n) % n;
        const MenuItem& it = items[static_cast<size_t>(i)];
        // 分隔线与置灰条目不可键盘选中（系统菜单惯例）
        if (it.kind == MenuItem::Kind::Separator || !it.enabled) continue;
        if (i == levels_[static_cast<size_t>(li)].selectedIndex) break;  // 仅自身可选
        setSelected(li, i);
        closeDeeperThan(li);
        // 键盘落在子菜单条目：立即展开（系统菜单惯例，无悬停延迟）
        if (it.kind == MenuItem::Kind::Submenu) openSubmenu(li, i);
        return;
    }
}

void MenuHost::expandSelected() {
    if (levels_.empty()) return;
    const int li = static_cast<int>(levels_.size()) - 1;
    const int i = levels_[static_cast<size_t>(li)].selectedIndex;
    if (i < 0) return;
    const MenuItem& it =
        model_->item(levels_[static_cast<size_t>(li)].menuId, i);
    if (it.kind == MenuItem::Kind::Submenu && it.enabled) {
        if (static_cast<int>(levels_.size()) > li + 1 &&
            levels_[static_cast<size_t>(li + 1)].menuId == it.submenu) {
            return;  // 已展开
        }
        openSubmenu(li, i);
    }
}

void MenuHost::collapseOrClose() {
    if (levels_.size() > 1) {
        closeDeepest();  // 逐级收起；根级无左向语义（非菜单栏）
    }
}

void MenuHost::activateSelected() {
    if (levels_.empty()) return;
    const int li = static_cast<int>(levels_.size()) - 1;
    const int i = levels_[static_cast<size_t>(li)].selectedIndex;
    if (i < 0) return;
    const MenuItem& it =
        model_->item(levels_[static_cast<size_t>(li)].menuId, i);
    if (!it.enabled) return;
    if (it.kind == MenuItem::Kind::Submenu) {
        expandSelected();  // Enter 在子菜单条目上=展开
        return;
    }
    if (it.kind == MenuItem::Kind::Action) {
        pendingCommand_ = it.command;
        dismissAll();
    }
}

void MenuHost::escape() {
    if (levels_.size() > 1) {
        closeDeepest();  // 逐级关闭
    } else {
        dismissAll();    // 根级：整体关闭（焦点恢复由 track 收尾统一处理）
    }
}

std::optional<MenuHost::Hit> MenuHost::hitTest(POINT screenPt) const {
    for (int li = static_cast<int>(levels_.size()) - 1; li >= 0; --li) {
        RECT r = levels_[static_cast<size_t>(li)].screenRect;  // PtInRect 需可写指针
        if (!PtInRect(&r, screenPt)) continue;
        POINT c = screenPt;
        ScreenToClient(levels_[static_cast<size_t>(li)].hwnd, &c);
        const int idx = MenuLayoutHitTest(levels_[static_cast<size_t>(li)].layout, c);
        if (idx >= 0) return Hit{li, idx};
        break;  // 命中该层窗口但落在留白/分隔线上：按无条目处理
    }
    return std::nullopt;
}

void MenuHost::onMouseMove(POINT rootClientPt) {
    if (levels_.empty()) return;
    POINT s = rootClientPt;
    ClientToScreen(levels_[0].hwnd, &s);
    const auto hit = hitTest(s);
    if (!hit) {
        // 菜单外/留白：清高亮（菜单保持打开，点击外部才关闭——系统惯例）
        clearAllSelection();
        killPendingExpand();
        return;
    }
    const MenuItem& it =
        model_->item(levels_[static_cast<size_t>(hit->level)].menuId, hit->item);
    if (it.kind == MenuItem::Kind::Separator || !it.enabled) {
        setSelected(hit->level, -1);
        closeDeeperThan(hit->level);
        killPendingExpand();
        return;
    }
    setSelected(hit->level, hit->item);
    if (it.kind == MenuItem::Kind::Submenu) {
        const int lvl = hit->level;
        // 已展开同一子菜单：保留（收拢更深层），否则挂 300ms 延迟展开
        if (static_cast<int>(levels_.size()) > lvl + 1 &&
            levels_[static_cast<size_t>(lvl + 1)].menuId == it.submenu) {
            closeDeeperThan(lvl + 1);
            killPendingExpand();
            return;
        }
        closeDeeperThan(lvl);
        scheduleExpand(lvl, hit->item);
    } else {
        closeDeeperThan(hit->level);  // 悬停普通条目：收起本级子菜单链
        killPendingExpand();
    }
}

void MenuHost::onLeftDown(POINT rootClientPt) {
    if (levels_.empty()) return;
    POINT s = rootClientPt;
    ClientToScreen(levels_[0].hwnd, &s);
    const auto hit = hitTest(s);
    if (!hit) {
        dismissAll();  // 点击菜单外：整体关闭（本组件吞掉该点击，不透传）
        return;
    }
    const MenuItem& it =
        model_->item(levels_[static_cast<size_t>(hit->level)].menuId, hit->item);
    if (it.kind == MenuItem::Kind::Separator || !it.enabled) return;
    setSelected(hit->level, hit->item);
    closeDeeperThan(hit->level);  // 点击任何条目先收起更深层（含 Action 条目）
    killPendingExpand();
    if (it.kind == MenuItem::Kind::Submenu) {
        openSubmenu(hit->level, hit->item);  // 点击子菜单条目：立即展开（不等延迟）
    }
    // Action 条目在抬起时触发（见 onLeftUp）
}

void MenuHost::onLeftUp(POINT rootClientPt) {
    if (levels_.empty()) return;
    POINT s = rootClientPt;
    ClientToScreen(levels_[0].hwnd, &s);
    const auto hit = hitTest(s);
    if (!hit) return;
    const MenuItem& it =
        model_->item(levels_[static_cast<size_t>(hit->level)].menuId, hit->item);
    if (it.kind == MenuItem::Kind::Action && it.enabled) {
        pendingCommand_ = it.command;
        dismissAll();
    }
}

void MenuHost::scheduleExpand(int level, int item) {
    pendingLevel_ = level;
    pendingItem_ = item;
    if (!levels_.empty() && IsWindow(levels_[0].hwnd)) {
        SetTimer(levels_[0].hwnd, kExpandTimerId, kExpandDelayMs, nullptr);
    }
}

void MenuHost::killPendingExpand() {
    if (pendingLevel_ >= 0 && !levels_.empty() && IsWindow(levels_[0].hwnd)) {
        KillTimer(levels_[0].hwnd, kExpandTimerId);
    }
    pendingLevel_ = pendingItem_ = -1;
}

void MenuHost::onExpandTimer() {
    if (pendingLevel_ < 0 || pendingItem_ < 0) return;
    const int lvl = pendingLevel_;
    const int item = pendingItem_;
    killPendingExpand();  // 一次性
    if (lvl < 0 || lvl >= static_cast<int>(levels_.size())) return;
    setSelected(lvl, item);  // 悬停可能已移走；确保延迟目标仍高亮再展开
    const MenuItem& it = model_->item(levels_[static_cast<size_t>(lvl)].menuId, item);
    if (it.kind == MenuItem::Kind::Submenu && it.enabled) {
        openSubmenu(lvl, item);
    }
}

// ---------------------------------------------------------------------------
// 窗口/绘制
// ---------------------------------------------------------------------------

HWND MenuHost::createLevelWindow(const RECT& rc) {
    RegisterMenuWindowClass();
    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kMenuClassName, L"",
        WS_POPUP, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, owner_,
        nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return nullptr;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    // Win11 圆角（DWMWCP_ROUNDSMALL）；Win10 无此属性调用失败→直角，边框仍在。
    // 深色主题：DWM 沉浸式暗色（投影/边框观感随动；菜单每次弹出前按当前主题）
    DWORD corner = DWMWCP_ROUNDSMALL;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    SetImmersiveDark(hwnd, IsDarkTheme());
    // SetWindowPos(SWP_NOACTIVATE|SWP_SHOWWINDOW) 是可靠的不激活显示路径：
    // ShowWindow(SW_SHOWNA) 在部分形态（有 owner 的弹出层）下仍会触发激活
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER);
    return hwnd;
}

LRESULT CALLBACK MenuHost::MenuWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // GWLP_USERDATA 在 CreateWindowEx 返回后才写入；创建期消息走默认处理
    auto* self = reinterpret_cast<MenuHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->proc(hwnd, msg, wp, lp);
}

LRESULT MenuHost::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;  // WM_PAINT 全覆盖（防闪白）
    case WM_PAINT:
        paintLevel(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        // 鼠标捕获在根层：子菜单窗口上的移动也路由到根层（坐标按根层换算）
        onMouseMove({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        return 0;
    case WM_LBUTTONDOWN:
        onLeftDown({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        return 0;
    case WM_LBUTTONUP:
        onLeftUp({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        return 0;
    case WM_RBUTTONDOWN: {
        // 菜单外右键=关闭；菜单内右键忽略（对齐系统菜单）
        POINT s{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(hwnd, &s);
        if (!hitTest(s)) dismissAll();
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        // 兜底路径：正常键盘已在模态循环拦截；直接 post 到本窗口的消息走此
        //（F4 须区分：Alt+F4 仅以 WM_SYSKEYDOWN 形态到达，单按 F4 不触发退出）
        handleKeyMessage(static_cast<UINT>(wp), msg == WM_SYSKEYDOWN);
        return 0;
    case WM_TIMER:
        if (wp == kExpandTimerId) onExpandTimer();
        return 0;
    case WM_CAPTURECHANGED:
        // 捕获被系统/他窗夺走（如激活切换）：菜单失去鼠标路由，整体关闭
        if (!closing_) dismissAll();
        return 0;
    case WM_CANCELMODE:
        dismissAll();
        return 0;
    case WM_ACTIVATEAPP:
        // 应用失活（Alt+Tab 切走）：键盘/鼠标均失去，整体关闭
        if (wp == 0 && !closing_ && !done_) dismissAll();
        return 0;
    default:
        if (msg >= WM_APP && msg <= WM_APP + 9 && onAppMessage) {
            return onAppMessage(static_cast<int>(msg - WM_APP), wp, lp);
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 键盘统一入口（模态循环拦截调用；弹出层窗口过程的 WM_KEYDOWN 分支为
// posted 消息兜底路径，逻辑同此）。sysKey：是否 WM_SYSKEYDOWN 形态（Alt
// 组合键仅经该形态到达——F4 退出只在 Alt 按下时成立，单按 F4 忽略）
void MenuHost::handleKeyMessage(UINT vk, bool sysKey) {
    switch (vk) {
    case VK_DOWN: moveSelection(+1); break;
    case VK_UP: moveSelection(-1); break;
    case VK_RIGHT: expandSelected(); break;
    case VK_LEFT: collapseOrClose(); break;
    case VK_RETURN:
    case VK_SPACE: activateSelected(); break;
    case VK_ESCAPE: escape(); break;
    case VK_MENU:
    case VK_F10: dismissAll(); break;  // Alt / F10：整体关闭（系统菜单惯例）
    case VK_F4:
        // Alt+F4：关菜单并按退出语义交命令路由（仅 WM_SYSKEYDOWN 形态；
        // 单按 F4 不是退出，交 default 忽略）
        if (!sysKey) break;
        pendingCommand_ = Command::Quit;
        dismissAll();
        break;
    default: break;
    }
}

void MenuHost::paintLevel(HWND hwnd) {
    const auto it = std::find_if(levels_.begin(), levels_.end(),
                                 [hwnd](const Level& l) { return l.hwnd == hwnd; });
    if (it == levels_.end()) return;
    const Level& lv = *it;

    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int w = rc.right;
    const int h = rc.bottom;
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);

    // 双缓冲
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    const ThemePalette& pal = CurrentPalette();
    HBRUSH bg = CreateSolidBrush(ToColorRef(pal.contentBackground));
    RECT all{0, 0, w, h};
    FillRect(mem, &all, bg);
    DeleteObject(bg);

    const auto& items = model_->items(lv.menuId);
    const HGDIOBJ oldFont = SelectObject(mem, font_);
    SetBkMode(mem, TRANSPARENT);
    for (int i = 0; i < static_cast<int>(items.size()) &&
                    i < static_cast<int>(lv.layout.rows.size());
         ++i) {
        const MenuItem& item = items[static_cast<size_t>(i)];
        const MenuLayout::Row& row = lv.layout.rows[static_cast<size_t>(i)];
        if (item.kind == MenuItem::Kind::Separator) {
            const int y = (row.bounds.top + row.bounds.bottom) / 2;
            const int inset = Scale(menu_metrics::SeparatorInset, dpi_);
            RECT line{row.bounds.left + inset, y, row.bounds.right - inset, y + 1};
            HBRUSH b = CreateSolidBrush(ToColorRef(pal.border));
            FillRect(mem, &line, b);
            DeleteObject(b);
            continue;
        }
        drawItemRow(mem, item, row, lv.selectedIndex == i);
    }
    SelectObject(mem, oldFont);

    // 容器边缘：1 物理像素 border token（投影由 CS_DROPSHADOW、圆角由 DWM 提供）
    HBRUSH border = CreateSolidBrush(ToColorRef(pal.border));
    RECT top{0, 0, w, 1}, bottom{0, h - 1, w, h}, left{0, 0, 1, h}, right{w - 1, 0, w, h};
    FillRect(mem, &top, border);
    FillRect(mem, &bottom, border);
    FillRect(mem, &left, border);
    FillRect(mem, &right, border);
    DeleteObject(border);

    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

void MenuHost::drawItemRow(HDC dc, const MenuItem& item, const MenuLayout::Row& row,
                           bool selected) {
    const ThemePalette& pal = CurrentPalette();

    // 悬停/键盘选中高亮：hover token 与白底预合成（禁用条目不高亮）
    if (selected && item.enabled) {
        HBRUSH b =
            CreateSolidBrush(ToColorRef(BlendOver(pal.contentBackground, pal.hover)));
        FillRect(dc, &row.bounds, b);
        DeleteObject(b);
    }

    const COLORREF labelColor =
        ToColorRef(item.enabled ? pal.textPrimary : pal.textDisabled);
    const COLORREF shortcutColor =
        ToColorRef(item.enabled ? pal.textSecondary : pal.textDisabled);

    // 标签（超宽省略号）
    SetTextColor(dc, labelColor);
    RECT text = row.text;
    DrawTextW(dc, item.text.c_str(), -1, &text,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

    // 快捷键（右对齐，textSecondary）
    if (!item.shortcut.empty() && row.shortcut.right > row.shortcut.left) {
        SetTextColor(dc, shortcutColor);
        RECT sc = row.shortcut;
        DrawTextW(dc, item.shortcut.c_str(), -1, &sc,
                  DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    }

    // 勾选列 ✓（形状线索，非颜色依赖）
    if (row.hasCheck) {
        const int cx = Scale(menu_metrics::PadLeft, dpi_) +
                       Scale(menu_metrics::CheckColumn, dpi_) / 2;
        const int cy = (row.bounds.top + row.bounds.bottom) / 2;
        drawCheckGlyph(dc, cx, cy, labelColor);
    }

    // 子菜单 ▸ 箭头（右缘列居中）
    if (row.hasArrow) {
        const int cx = row.bounds.right - Scale(menu_metrics::PadRight, dpi_) -
                       Scale(menu_metrics::ArrowColumn, dpi_) / 2;
        const int cy = (row.bounds.top + row.bounds.bottom) / 2;
        drawSubmenuArrow(dc, cx, cy, labelColor);
    }
}

void MenuHost::drawCheckGlyph(HDC dc, int cx, int cy, COLORREF color) {
    // 10×10 逻辑网格 ✓：两段折线（与标题栏字形同风格的自绘，不依赖字体字形）
    const int s = Scale(1, dpi_);
    const auto px = [&](int lx, int ly) {
        return POINT{cx + Scale(lx - 5, dpi_), cy + Scale(ly - 5, dpi_)};
    };
    const POINT a = px(0, 4), b = px(3, 7), c = px(9, 1);
    HPEN pen = CreatePen(PS_SOLID, s, color);
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, a.x, a.y, nullptr);
    LineTo(dc, b.x, b.y);
    LineTo(dc, c.x, c.y);
    SelectObject(dc, old);
    DeleteObject(pen);
}

void MenuHost::drawSubmenuArrow(HDC dc, int cx, int cy, COLORREF color) {
    // ▸ 实心小三角（高 10 逻辑、宽 6 逻辑，垂直居中）
    const int hh = Scale(5, dpi_);
    const int hw = Scale(3, dpi_);
    POINT pts[3] = {{cx - hw, cy - hh}, {cx - hw, cy + hh}, {cx + hw, cy}};
    HBRUSH b = CreateSolidBrush(color);
    HGDIOBJ old = SelectObject(dc, b);
    Polygon(dc, pts, 3);
    SelectObject(dc, old);
    DeleteObject(b);
}

void MenuHost::ensureSessionFont() {
    // 字体按 DPI 惰性重建：DPI 变化后旧字体尺寸失配，须删除并按新 DPI 重创
    // （同 TabStrip::ensureFonts 范式），否则跨缩放显示器弹出菜单文字大小错误。
    if (font_ && dpi_ == fontDpi_) return;
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    // 优先使用系统菜单字体（lfMenuFont）：与系统菜单观感一致（中文环境为
    // Microsoft YaHei UI 类，Segoe UI 回退对中文渲染明显劣化）。
    NONCLIENTMETRICS ncm{};
    ncm.cbSize = sizeof(NONCLIENTMETRICS);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICS),
                              &ncm, 0) && ncm.lfMenuFont.lfHeight < 0) {
        // SPI 返回的 lfHeight 按主监视器 DPI 取值，需换算到本弹出层宿主 DPI
        // （Per-Monitor DPI 下两者可能不同）；负高度=字符高度语义，保持符号。
        NONCLIENTMETRICS scaled = ncm;
        scaled.lfMenuFont.lfHeight =
            -MulDiv(-ncm.lfMenuFont.lfHeight, static_cast<int>(dpi_),
                    static_cast<int>(WindowDpi(nullptr)));
        font_ = CreateFontIndirectW(&scaled.lfMenuFont);
    }
    if (!font_) {
        // 回退：lfMenuFont 不可用时按 pt→px 正确换算创建（FontPt 单位是 pt，
        // 必须经 MulDiv(pt, dpi, 72)；此前误用 Scale() 当像素换算是本次 bug 根因）
        font_ = CreateFontW(-MulDiv(menu_metrics::FontPt, static_cast<int>(dpi_), 72),
                            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }
    if (font_) fontDpi_ = dpi_;  // 记录字体 DPI，供下次比对决定是否重建
}

void MenuHost::measure(const std::vector<MenuItem>& items, std::vector<int>& labels,
                       std::vector<int>& shortcuts) {
    labels.assign(items.size(), 0);
    shortcuts.assign(items.size(), 0);
    if (!font_) return;
    HDC dc = GetDC(nullptr);
    if (!dc) return;
    HGDIOBJ old = SelectObject(dc, font_);
    for (size_t i = 0; i < items.size(); ++i) {
        SIZE sz{};
        if (!items[i].text.empty() && GetTextExtentPoint32W(dc, items[i].text.c_str(),
                                                            static_cast<int>(items[i].text.size()),
                                                            &sz)) {
            labels[i] = sz.cx;
        }
        if (!items[i].shortcut.empty() &&
            GetTextExtentPoint32W(dc, items[i].shortcut.c_str(),
                                  static_cast<int>(items[i].shortcut.size()), &sz)) {
            shortcuts[i] = sz.cx;
        }
    }
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
}

}  // namespace ui
