// M1-2 菜单系统单测：MenuModel 数据模型（结构构建/级联/勾选/enabled 查询/
// 动态条目）、ComputeMenuLayout 布局数学（列预留/DPI 换算/极值夹取/分隔行）、
// 主菜单内容构建（E1 结构与状态接线）。全部纯逻辑，不依赖窗口存在。
#include <catch2/catch_test_macros.hpp>

#include "ui/commands.h"
#include "ui/mainmenu.h"
#include "ui/menu.h"

using namespace ui;

// ---------------------------------------------------------------------------
// MenuModel：结构构建与 N 级级联
// ---------------------------------------------------------------------------

TEST_CASE("MenuModel 结构构建与三级级联", "[menu]") {
    MenuModel m;
    CHECK(m.menuCount() == 0);

    const int root = m.addMenu();            // id=0 主菜单
    const int file = m.addMenu(L"文件");     // 一级
    const int enc = m.addMenu(L"编码");      // 二级
    const int encReload = m.addMenu(L"通过编码重新加载");  // 三级
    CHECK(m.menuCount() == 4);
    CHECK(m.title(file) == L"文件");
    CHECK(m.title(root).empty());  // 主菜单无标题

    m.addItem(file, L"新建", L"Ctrl+N", Command::NewDocument);
    m.addSubmenuEntry(file, L"编码", enc);
    m.addSubmenuEntry(enc, L"通过编码重新加载", encReload);
    m.addItem(encReload, L"UTF-8", {}, Command::ReloadWithEncodingUtf8);
    m.addItem(encReload, L"GBK", {}, Command::ReloadWithEncodingGbk);

    // 文件菜单：条目 → 编码条目（指向子菜单 id）
    REQUIRE(m.itemCount(file) == 2);
    const MenuItem& newCmd = m.item(file, 0);
    CHECK(newCmd.kind == MenuItem::Kind::Action);
    CHECK(newCmd.text == L"新建");
    CHECK(newCmd.shortcut == L"Ctrl+N");
    CHECK(newCmd.command == Command::NewDocument);
    CHECK(newCmd.enabled);
    CHECK_FALSE(newCmd.checked);

    const MenuItem& encEntry = m.item(file, 1);
    CHECK(encEntry.kind == MenuItem::Kind::Submenu);
    CHECK(encEntry.submenu == enc);

    // 级联链可达：文件▸编码▸通过编码重新加载▸GBK
    const MenuItem& reloadEntry = m.item(enc, 0);
    CHECK(reloadEntry.submenu == encReload);
    CHECK(m.item(encReload, 1).command == Command::ReloadWithEncodingGbk);

    // 按命令查找（深度优先跨全部菜单）：三级条目也能定位
    const auto ref = m.findCommand(Command::ReloadWithEncodingGbk);
    REQUIRE(ref.has_value());
    CHECK(ref->menuId == encReload);
    CHECK(ref->index == 1);
    CHECK(m.item(ref->menuId, ref->index).text == L"GBK");
    CHECK_FALSE(m.findCommand(Command::Quit).has_value());  // 未定义命令
}

TEST_CASE("MenuModel 勾选与 enabled 状态更新", "[menu]") {
    MenuModel m;
    const int menu = m.addMenu();
    m.addItem(menu, L"深色主题", {}, Command::ToggleDarkTheme).checked = true;
    m.addItem(menu, L"自动换行", {}, Command::ToggleWordWrap);

    CHECK(m.isChecked(Command::ToggleDarkTheme));
    CHECK_FALSE(m.isChecked(Command::ToggleWordWrap));
    CHECK(m.isEnabled(Command::ToggleDarkTheme));

    // setChecked/setEnabled 经命令定位更新
    CHECK(m.setChecked(Command::ToggleDarkTheme, false));
    CHECK_FALSE(m.isChecked(Command::ToggleDarkTheme));
    CHECK(m.setEnabled(Command::ToggleWordWrap, false));
    CHECK_FALSE(m.isEnabled(Command::ToggleWordWrap));

    // 未定义命令：返回 false 不崩溃（未知命令按不可用处理，fail-safe）
    CHECK_FALSE(m.setChecked(Command::Quit, true));
    CHECK_FALSE(m.setEnabled(Command::Quit, true));
    CHECK_FALSE(m.isChecked(Command::Quit));
    CHECK_FALSE(m.isEnabled(Command::Quit));
}

TEST_CASE("MenuModel 动态构建条目（Tab 右键菜单预留，M1-3）", "[menu]") {
    // 右键菜单特征：运行时按目标 Tab 现场构建、条目文本可动态拼接（含数量）、
    // 无子菜单、程序化指定位置弹出（MenuHost::track 的 anchor 参数，布局外单测）
    MenuModel m;
    const int ctx = m.addMenu();
    m.addItem(ctx, L"关闭当前文件", {}, Command::CloseCurrentTab);
    m.addItem(ctx, L"关闭右边所有 (3)", {}, Command::CloseTabsToRight);
    m.addItem(ctx, L"关闭左边所有 (0)", {}, Command::CloseTabsToLeft);
    m.addItem(ctx, L"关闭其他文件", {}, Command::CloseOtherTabs);

    REQUIRE(m.itemCount(ctx) == 4);
    for (int i = 0; i < 4; ++i) {
        const MenuItem& it = m.item(ctx, i);
        CHECK(it.kind == MenuItem::Kind::Action);
        CHECK(it.enabled);
        CHECK(it.submenu == -1);
    }
    // 四个命令均可定位且互不相同（命令 id 是路由唯一键）
    Command cmds[4] = {Command::CloseCurrentTab, Command::CloseTabsToRight,
                       Command::CloseTabsToLeft, Command::CloseOtherTabs};
    for (const Command c : cmds) {
        CHECK(m.findCommand(c).has_value());
    }
    // 动态菜单同样支持勾选/置灰更新（API 一致性）
    CHECK(m.setEnabled(Command::CloseTabsToLeft, false));  // 无左边标签时置灰
    CHECK_FALSE(m.isEnabled(Command::CloseTabsToLeft));
    CHECK(m.setChecked(Command::CloseCurrentTab, true));
    CHECK(m.item(ctx, 0).checked);
}

// ---------------------------------------------------------------------------
// 主菜单内容（E1）
// ---------------------------------------------------------------------------

TEST_CASE("主菜单结构（E1）默认无文档状态", "[menu][mainmenu]") {
    const MenuModel m = BuildMainMenu(MainMenuState{});
    // 菜单森林：主菜单 + 文件/搜索/视图 + 编码/重载/转存 = 7
    REQUIRE(m.menuCount() == 7);
    REQUIRE(m.itemCount(0) == 6);  // 文件▸/搜索▸/──/视图▸/──/退出

    CHECK(m.item(0, 0).kind == MenuItem::Kind::Submenu);
    CHECK(m.item(0, 0).text == L"文件");
    CHECK(m.item(0, 1).kind == MenuItem::Kind::Submenu);
    CHECK(m.item(0, 1).text == L"搜索");
    CHECK(m.item(0, 2).kind == MenuItem::Kind::Separator);
    CHECK(m.item(0, 3).kind == MenuItem::Kind::Submenu);
    CHECK(m.item(0, 3).text == L"视图");
    CHECK(m.item(0, 4).kind == MenuItem::Kind::Separator);
    CHECK(m.item(0, 5).kind == MenuItem::Kind::Action);
    CHECK(m.item(0, 5).text == L"退出");
    CHECK(m.item(0, 5).command == Command::Quit);
    CHECK(m.item(0, 5).enabled);

    // 文件菜单：五项 + 分隔线 + 编码▸
    const int file = m.item(0, 0).submenu;
    REQUIRE(m.itemCount(file) == 7);
    CHECK(m.item(file, 0).text == L"新建");
    CHECK(m.item(file, 0).shortcut == L"Ctrl+N");
    CHECK(m.item(file, 0).command == Command::NewDocument);
    CHECK(m.item(file, 1).text == L"打开…");
    CHECK(m.item(file, 1).shortcut == L"Ctrl+O");
    CHECK(m.item(file, 2).text == L"保存");
    CHECK(m.item(file, 3).text == L"另存为…");
    CHECK(m.item(file, 3).shortcut == L"Ctrl+Shift+S");
    CHECK(m.item(file, 4).text == L"关闭标签页");
    CHECK(m.item(file, 4).shortcut == L"Ctrl+W");
    CHECK(m.item(file, 5).kind == MenuItem::Kind::Separator);
    CHECK(m.item(file, 6).kind == MenuItem::Kind::Submenu);
    CHECK(m.item(file, 6).text == L"编码");

    // 无文档：文档相关项置灰（M2 接状态源后翻转）
    CHECK(m.item(file, 0).enabled);  // 新建
    CHECK(m.item(file, 1).enabled);  // 打开…
    CHECK_FALSE(m.item(file, 2).enabled);  // 保存
    CHECK_FALSE(m.item(file, 3).enabled);  // 另存为…
    CHECK_FALSE(m.item(file, 4).enabled);  // 关闭标签页

    // 编码三级级联
    const int enc = m.item(file, 6).submenu;
    REQUIRE(m.itemCount(enc) == 2);
    CHECK(m.item(enc, 0).text == L"通过编码重新加载");
    CHECK(m.item(enc, 1).text == L"通过编码保存");
    CHECK_FALSE(m.item(enc, 0).enabled);  // 无 URL（未命名/无文档）整组禁用
    CHECK_FALSE(m.item(enc, 1).enabled);  // 无文档
    const int encReload = m.item(enc, 0).submenu;
    const int encSave = m.item(enc, 1).submenu;
    CHECK(m.item(encReload, 0).command == Command::ReloadWithEncodingUtf8);
    CHECK(m.item(encReload, 1).command == Command::ReloadWithEncodingGbk);
    CHECK(m.item(encSave, 0).command == Command::SaveWithEncodingUtf8);
    CHECK(m.item(encSave, 1).command == Command::SaveWithEncodingGbk);
    // 默认编码 UTF-8：「通过编码保存」UTF-8 打勾、GBK 不勾
    CHECK(m.item(encSave, 0).checked);
    CHECK_FALSE(m.item(encSave, 1).checked);

    // 搜索：四项全置灰（无活动文档，B5 欢迎页语义）
    const int search = m.item(0, 1).submenu;
    REQUIRE(m.itemCount(search) == 5);  // 查找/替换/──/下一个/上一个
    CHECK(m.item(search, 0).text == L"查找…");
    CHECK(m.item(search, 0).shortcut == L"Ctrl+F");
    CHECK(m.item(search, 1).text == L"替换…");
    CHECK(m.item(search, 1).shortcut == L"Ctrl+H");
    CHECK(m.item(search, 2).kind == MenuItem::Kind::Separator);
    CHECK(m.item(search, 3).text == L"下一个");
    CHECK(m.item(search, 3).shortcut == L"F3");
    CHECK(m.item(search, 4).text == L"上一个");
    CHECK(m.item(search, 4).shortcut == L"Shift+F3");
    for (int i = 0; i < 5; ++i) {
        if (m.item(search, i).kind == MenuItem::Kind::Action) {
            CHECK_FALSE(m.item(search, i).enabled);
        }
    }

    // 视图：勾选态 stub（默认全不勾）
    const int view = m.item(0, 3).submenu;
    REQUIRE(m.itemCount(view) == 2);
    CHECK(m.item(view, 0).text == L"深色主题");
    CHECK(m.item(view, 0).command == Command::ToggleDarkTheme);
    CHECK_FALSE(m.item(view, 0).checked);
    CHECK(m.item(view, 1).text == L"自动换行");
    CHECK(m.item(view, 1).command == Command::ToggleWordWrap);
    CHECK_FALSE(m.item(view, 1).checked);
}

TEST_CASE("主菜单状态接线（有文档/GBK/主题开关）", "[menu][mainmenu]") {
    MainMenuState s;
    s.hasActiveDocument = true;
    s.activeDocumentHasUrl = true;
    s.currentEncoding = MainMenuState::Encoding::Gbk;
    s.darkTheme = true;
    s.wordWrap = true;
    const MenuModel m = BuildMainMenu(s);

    CHECK(m.isEnabled(Command::Save));
    CHECK(m.isEnabled(Command::SaveAs));
    CHECK(m.isEnabled(Command::CloseTab));
    CHECK(m.isEnabled(Command::FindDialog));
    CHECK(m.isEnabled(Command::FindNext));
    CHECK(m.isEnabled(Command::FindPrevious));

    // 编码组解禁 + 当前编码 GBK 打勾（互斥）
    const int enc = m.item(m.item(0, 0).submenu, 6).submenu;
    CHECK(m.item(enc, 0).enabled);
    CHECK(m.item(enc, 1).enabled);
    const int encSave = m.item(enc, 1).submenu;
    CHECK_FALSE(m.item(encSave, 0).checked);
    CHECK(m.item(encSave, 1).checked);

    // 视图勾选
    CHECK(m.isChecked(Command::ToggleDarkTheme));
    CHECK(m.isChecked(Command::ToggleWordWrap));
}

TEST_CASE("主菜单按命令更新的集成路径（弹出前重建 + 命令定位）", "[menu][mainmenu]") {
    // M2 状态源接入后的使用方式演练：构建 → 按命令翻转勾选 → 校验
    MenuModel m = BuildMainMenu(MainMenuState{});
    CHECK_FALSE(m.isChecked(Command::ToggleWordWrap));
    CHECK(m.setChecked(Command::ToggleWordWrap, true));  // 模拟切换后重建/更新
    CHECK(m.isChecked(Command::ToggleWordWrap));
    const auto ref = m.findCommand(Command::SaveWithEncodingGbk);
    REQUIRE(ref.has_value());
    CHECK(ref->menuId != 0);  // 深层菜单条目
}

// ---------------------------------------------------------------------------
// ComputeMenuLayout：布局数学
// ---------------------------------------------------------------------------

namespace {
std::vector<MenuItem> MakeLayoutItems() {
    // A（有快捷键）/ B（长标签）/ C（子菜单）
    std::vector<MenuItem> items(3);
    items[0].text = L"标签A";
    items[0].shortcut = L"Ctrl+S";
    items[1].text = L"超长标签条目文本";
    items[2].kind = MenuItem::Kind::Submenu;
    items[2].text = L"子菜单";
    return items;
}
}  // namespace

TEST_CASE("ComputeMenuLayout 列预留与夹取 100%", "[menu]") {
    const auto items = MakeLayoutItems();
    const MenuLayout L = ComputeMenuLayout(items, {100, 200, 60}, {50, 0, 0}, 96);
    // 宽 = 左段(10+20+6) + 标签列 200 + 右段(28+50 + 18 + 10) = 342 → 夹到 MaxWidth 320
    CHECK(L.width == 320);
    // 高 = 1(边框) + 3×32 + 1 = 98
    CHECK(L.height == 98);
    REQUIRE(L.rows.size() == 3);
    CHECK(L.rows[0].bounds.top == 1);
    CHECK(L.rows[0].bounds.bottom == 33);
    CHECK(L.rows[1].bounds.top == 33);
    // 标签区右界 = 快捷键列起点（320 - 106）
    CHECK(L.rows[0].text.right == 214);
    CHECK(L.rows[0].text.left == 36);
    // 快捷键区右对齐列
    CHECK(L.rows[0].shortcut.left == 242);
    CHECK(L.rows[0].shortcut.right == 292);
    // 箭头列仅在子菜单条目行（本菜单存在子菜单 → 整体预留箭头列）
    CHECK(L.rows[2].hasArrow);
    CHECK_FALSE(L.rows[0].hasArrow);
    CHECK_FALSE(L.rows[0].hasCheck);
}

TEST_CASE("ComputeMenuLayout DPI 200% 换算", "[menu]") {
    const auto items = MakeLayoutItems();
    // 200%：文本宽度按同 DPI 字体测量（近似翻倍），布局常量同倍缩放
    const MenuLayout L = ComputeMenuLayout(items, {200, 400, 120}, {100, 0, 0}, 192);
    CHECK(L.width == 640);  // 684 → 夹到 Scale(320,192)=640
    CHECK(L.height == 194);  // 1 + 3×64 + 1
    CHECK(L.rows[0].bounds.bottom == 65);  // 1+64
    CHECK(L.rows[0].text.left == 72);      // (20+40+12)
    CHECK(L.rows[0].text.right == 428);    // 640 - (56+100+36+20)
}

TEST_CASE("ComputeMenuLayout 最小宽/无快捷键列/分隔行", "[menu]") {
    // 单条目无快捷键无箭头：自然宽 66 → 夹到 MinWidth 132
    std::vector<MenuItem> items;
    items.push_back({.kind = MenuItem::Kind::Action, .text = L"确定"});
    MenuLayout L = ComputeMenuLayout(items, {20}, {0}, 96);
    CHECK(L.width == 132);
    CHECK(L.height == 34);  // 1 + 32 + 1
    CHECK(L.rows[0].shortcut.right == 0);  // 无快捷键列
    CHECK_FALSE(L.rows[0].hasArrow);

    // 分隔行高 9 逻辑、位于两行之间；勾选行 hasCheck
    std::vector<MenuItem> items3(3);
    items3[0].text = L"上";
    items3[1].kind = MenuItem::Kind::Separator;
    items3[2].text = L"下";
    items3[2].checked = true;
    L = ComputeMenuLayout(items3, {20, 0, 20}, {0, 0, 0}, 96);
    CHECK(L.rows[1].bounds.bottom - L.rows[1].bounds.top == 9);
    CHECK(L.rows[1].bounds.top == 33);   // 紧随第一行
    CHECK(L.rows[2].bounds.top == 42);
    CHECK(L.rows[2].hasCheck);
    CHECK_FALSE(L.rows[0].hasCheck);
    CHECK(L.height == 75);  // 1 + 32 + 9 + 32 + 1
}

TEST_CASE("MenuLayoutHitTest 行命中", "[menu]") {
    std::vector<MenuItem> items3(3);
    items3[0].text = L"甲";
    items3[1].kind = MenuItem::Kind::Separator;
    items3[2].text = L"乙";
    const MenuLayout L = ComputeMenuLayout(items3, {20, 0, 20}, {0, 0, 0}, 96);
    CHECK(MenuLayoutHitTest(L, {5, 5}) == 0);    // 第一行
    CHECK(MenuLayoutHitTest(L, {130, 40}) == 1); // 分隔行（几何语义：返回下标）
    CHECK(MenuLayoutHitTest(L, {5, 60}) == 2);   // 第三行
    CHECK(MenuLayoutHitTest(L, {5, 200}) == -1); // 菜单下方
    CHECK(MenuLayoutHitTest(L, {200, 5}) == -1); // 菜单右侧
    CHECK(MenuLayoutHitTest(L, {-1, 5}) == -1);  // 菜单左侧
}
