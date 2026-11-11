// qcnote 自绘弹出菜单系统（M1-2）。
//
// 三层结构（前两层纯逻辑可单测，第三层为薄封装）：
//   MenuModel —— 数据驱动的菜单描述：N 级级联（子菜单以 id 间接指向，森林
//                结构存于模型内）、勾选态/enabled/分隔线/命令 id。主菜单内容
//                定义见 mainmenu.h；Tab 右键菜单（M1-3）与后续一切上下文菜单
//                基于同一模型动态构建（程序化指定位置弹出）。
//   ComputeMenuLayout / MenuLayoutHitTest —— 布局与命中的纯函数。文本宽度由
//                调用方（GDI 按 DPI 测量）传入，函数不依赖窗口，可直接单测。
//   MenuHost   —— 弹出层：每级菜单一个 WS_POPUP 自绘窗口 + TrackPopupMenu
//                式模态消息循环。root 弹出层持有鼠标捕获（SetCapture）与键盘
//                焦点（SetFocus）；弹出层为 WS_EX_NOACTIVATE，不夺取活动窗口
//                身份——主窗口保持 active，无边框 E2 边界线不闪灰。
//
// 弹出层方案选择（为何不用系统 TrackPopupMenu + owner-draw）：
//   需求是 E1/E4 的统一自绘弹窗体系（主题 token 配色、勾选列/快捷键右对齐/
//   三级级联、圆角+投影观感）。系统菜单自绘（MF_OWNERDRAW）只能接管条目内部
//   绘制，容器外观、级联展开时机、键盘语义均归系统，无法满足 token 化视觉；
//   自建 POPUP 层与主窗口无边框方案同构，且 Tab 右键菜单/后续自绘对话框
//   （E4）复用同一视觉与交互语言。焦点/模态处理见 MenuHost::track 注释。
#pragma once

#include <windows.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "commands.h"
#include "theme.h"

namespace ui {

// —— 菜单尺寸 token（逻辑像素，96 DPI 基准；使用处经 Scale(dpi) 换算）——
namespace menu_metrics {

constexpr int ItemHeight = 32;      // 条目行高（需求：≈32 逻辑像素）
constexpr int SeparatorHeight = 9;  // 分隔线行高（上下留白 + 1 物理像素线）
constexpr int SeparatorInset = 12;  // 分隔线左右缩进
constexpr int PadLeft = 10;         // 左缘 → 勾选列
constexpr int CheckColumn = 20;     // 勾选列宽（✓ 居中；勾选为形状线索非颜色）
constexpr int LabelGap = 6;         // 勾选列 → 标签
constexpr int ShortcutGap = 28;     // 标签 → 快捷键列的最小弹性间隔
constexpr int ArrowColumn = 18;     // 子菜单 ▸ 箭头列
constexpr int PadRight = 10;        // 右缘留白
constexpr int MinWidth = 132;       // 最小菜单宽（空/极窄内容场景）
constexpr int MaxWidth = 320;       // 最大菜单宽（超宽标签省略号截断）
constexpr int FontPt = 9;           // Segoe UI 9pt（与标题栏一致，系统菜单惯例）

}  // namespace menu_metrics

// ---------------------------------------------------------------------------
// 数据模型
// ---------------------------------------------------------------------------

// 单个菜单条目。Action=可触发命令；Separator=分隔线；Submenu=级联展开。
// Submenu 条目的 submenu 字段指向 MenuModel 内 addMenu 返回的子菜单 id。
struct MenuItem {
    enum class Kind { Action, Separator, Submenu };

    Kind kind = Kind::Action;
    std::wstring text;       // 显示文本（Separator 为空）
    std::wstring shortcut;   // 快捷键显示文本（右对齐，仅展示；实际按键 M3 接）
    Command command = Command::None;  // Action 条目的命令 id
    bool enabled = true;     // 置灰（含整组禁用：Submenu 置灰则不可展开）
    bool checked = false;    // 勾选态（左列 ✓，形状线索）
    int submenu = -1;        // Submenu 条目：子菜单 id
};

// 菜单森林：menus[0] 为主菜单（MenuHost::track 的根），其余为各级子菜单。
// 全部接口为纯数据操作，无窗口依赖。
class MenuModel {
public:
    struct ItemRef {
        int menuId = -1;
        int index = -1;
        bool valid() const { return menuId >= 0 && index >= 0; }
    };

    // 新建菜单（含标题，主菜单/无标题菜单可传空），返回菜单 id。
    // 首次调用创建的 id=0 即主菜单。
    int addMenu(std::wstring title = {});

    // 追加条目（返回引用以便链式设置 enabled/checked 等状态字段）
    MenuItem& addItem(int menuId, std::wstring text, std::wstring shortcut,
                      Command command);
    MenuItem& addSubmenuEntry(int menuId, std::wstring text, int submenuId);
    MenuItem& addSeparator(int menuId);

    // —— 结构查询 ——
    int menuCount() const;
    int itemCount(int menuId) const;
    const std::vector<MenuItem>& items(int menuId) const;
    const MenuItem& item(int menuId, int index) const;
    const std::wstring& title(int menuId) const;

    // —— 按命令查询/更新（深度优先遍历全部菜单；勾选与置灰状态更新入口）——
    std::optional<ItemRef> findCommand(Command command) const;
    bool setChecked(Command command, bool checked);   // 未找到返回 false
    bool setEnabled(Command command, bool enabled);   // 未找到返回 false
    bool isChecked(Command command) const;            // 未找到返回 false
    bool isEnabled(Command command) const;            // 未找到返回 false

private:
    struct Menu {
        std::wstring title;
        std::vector<MenuItem> items;
    };
    std::vector<Menu> menus_;
};

// ---------------------------------------------------------------------------
// 布局（纯函数；文本物理像素宽度由调用方测量后传入）
// ---------------------------------------------------------------------------

struct MenuLayout {
    struct Row {
        RECT bounds{};     // 行矩形（客户区坐标；Separator 为分隔行）
        RECT text{};       // 标签区（含省略号截断空间）
        RECT shortcut{};   // 快捷键区（无快捷键列时为空）
        bool hasCheck = false;
        bool hasArrow = false;
    };
    int width = 0;   // 含边框
    int height = 0;  // 含边框
    std::vector<Row> rows;
};

// labelWidths/shortcutWidths 与 items 等长（Separator 项取 0）。
// 列预留规则：勾选列恒预留；快捷键列/箭头列仅当本菜单存在对应条目时预留。
MenuLayout ComputeMenuLayout(const std::vector<MenuItem>& items,
                             const std::vector<int>& labelWidths,
                             const std::vector<int>& shortcutWidths, UINT dpi);

// 命中（客户区坐标）：返回条目下标（分隔线返回其下标——几何语义，是否可选中
// 由调用方按 kind/enabled 判定）；未命中返回 -1。
int MenuLayoutHitTest(const MenuLayout& layout, POINT pt);

// ---------------------------------------------------------------------------
// 弹出层（薄封装；绘制/窗口/消息循环都在实现内）
// ---------------------------------------------------------------------------

class MenuHost {
public:
    // 模态弹出（TrackPopupMenu 语义）：阻塞至菜单链整体关闭。
    //   owner  —— 宿主窗口（所有权/焦点恢复；菜单尺寸随其 DPI）
    //   model  —— 菜单数据（须在 track 返回前存活；调用方栈对象即可）
    //   anchor —— 菜单期望左上角的屏幕坐标（如菜单按钮左下角）；翻转/夹取
    //             到所在显示器工作区由组件处理
    // 返回被触发的命令；未触发（Escape/点击外部/空模型）返回 Command::None。
    //
    // 焦点/模态：弹出层窗口 WS_EX_NOACTIVATE + SetWindowPos(SWP_NOACTIVATE)——
    // 不夺取活动窗口身份与输入焦点（主窗口保持 active，E2 边界线不变灰）。
    // 键盘在模态消息循环内拦截分发（TrackPopupMenu 同款做法）：菜单打开期间
    // 宿主窗口不再收到按键（快捷键屏蔽的自然实现），关闭后键盘自然回流宿主，
    // 无需焦点恢复动作。uiLocked 显式语义由组装层经 onOpened/onClosed 挂接（E4）。
    //
    // 命令触发时机：条目触发后菜单链完全关闭、焦点恢复，onClosed 回调之后
    // 才执行 onCommand——命令处理器可安全打开对话框等（无模态循环重入）。
    Command track(HWND owner, const MenuModel& model, POINT anchor);

    bool tracking() const { return tracking_; }

    // —— 挂接点 ——
    // 命令路由（条目触发；见上文触发时机说明）
    std::function<void(Command)> onCommand;
    // 根菜单创建后（模态循环开始前）／全部关闭且焦点恢复后。
    // 组装层挂 uiLocked：菜单打开期间屏蔽主窗口快捷键（M2/M3 接入）。
    std::function<void()> onOpened;
    std::function<void()> onClosed;
    // WM_APP..WM_APP+9 消息透传（弹出层窗口收到时回调；自动化测试钩子，
    // 供程序化采样菜单内部状态，业务代码勿用）
    std::function<LRESULT(int code, WPARAM, LPARAM)> onAppMessage;

    // —— 测试钩子（自动化驱动：菜单级数/根弹出层窗口句柄）——
    int openLevelCountForTest() const { return static_cast<int>(levels_.size()); }
    HWND rootPopupForTest() const { return levels_.empty() ? nullptr : levels_[0].hwnd; }

    // 内部使用：弹出层窗口过程（注册窗口类时引用）
    static LRESULT CALLBACK MenuWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    struct Level {
        HWND hwnd{};
        int menuId = 0;
        int selectedIndex = -1;  // 高亮条目（鼠标悬停与键盘选中同一状态）
        RECT screenRect{};       // 屏幕坐标（跨层命中/子菜单定位）
        MenuLayout layout;
    };
    struct Hit {
        int level;
        int item;
    };

    // 会话
    bool openRoot(POINT anchor);
    void openSubmenu(int parentLevelIdx, int itemIdx);
    void closeDeeperThan(int levelIdx);  // 保留 levels_[0..levelIdx]
    void closeDeepest();
    void dismissAll();
    void cleanupSession();

    // 交互
    void setSelected(int levelIdx, int itemIdx);
    void clearAllSelection();
    void moveSelection(int dir);
    void expandSelected();
    void collapseOrClose();
    void activateSelected();
    void escape();
    void handleKeyMessage(UINT vk, bool sysKey);  // 键盘统一入口（模态循环拦截/窗口过程兜底；sysKey=WM_SYSKEYDOWN 形态）
    std::optional<Hit> hitTest(POINT screenPt) const;
    void onMouseMove(POINT rootClientPt);
    void onLeftDown(POINT rootClientPt);
    void onLeftUp(POINT rootClientPt);
    void scheduleExpand(int level, int item);
    void killPendingExpand();
    void onExpandTimer();

    // 绘制/资源
    void paintLevel(HWND hwnd);
    void drawItemRow(HDC dc, const MenuItem& item, const MenuLayout::Row& row,
                     bool selected);
    void drawCheckGlyph(HDC dc, int cx, int cy, COLORREF color);
    void drawSubmenuArrow(HDC dc, int cx, int cy, COLORREF color);
    void ensureSessionFont();
    void measure(const std::vector<MenuItem>& items, std::vector<int>& labels,
                 std::vector<int>& shortcuts);
    HWND createLevelWindow(const RECT& rc);
    LRESULT proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    // —— 会话状态（track 开始到结束）——
    HWND owner_{};
    const MenuModel* model_{};
    UINT dpi_ = 96;
    std::vector<Level> levels_;
    HFONT font_{};
    int pendingLevel_ = -1;  // 延迟展开目标（悬停子菜单 ~300ms）
    int pendingItem_ = -1;
    Command pendingCommand_ = Command::None;
    bool tracking_ = false;
    bool closing_ = false;  // 拆除期守卫（忽略捕获丢失/焦点丢失引发的递归关闭）
    bool done_ = false;     // 模态循环退出条件
};

}  // namespace ui
