// M1-3 Tab 条单测：宽度自适应算法（A2 规格：充足/等比收缩/饱和滚动/单 Tab 上限/
// 向下取整边界/DPI 换算）、Tab 条几何（贴底/间距/内容宽/分隔线规则/● 标记与文本区/
// 滚动偏移命中）、右键菜单模型（A3 动态数量与置灰）、无障碍名称、
// GDI 文本测量（中英文宽度差异——自然宽的数据来源）、拖拽重排（TabDragSwapIndex
// 中点交换矩阵 TC-004 + TabStrip 状态机 TC-005/006/007/008：阈值进入/预览交换/
// 松手一次提交/Esc 与捕获丢失取消回滚/纯点击与陈旧按下防呆——无窗口夹具，
// 坐标一律从 geometry() 查询推导，DPI 无关）。
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <cmath>

#include "ui/commands.h"
#include "ui/tabs.h"

using namespace ui;

namespace {
bool RectEq(RECT const& r, LONG left, LONG top, LONG right, LONG bottom) {
    return r.left == left && r.top == top && r.right == right && r.bottom == bottom;
}
}  // namespace

// ---------------------------------------------------------------------------
// DistributeTabWidths：宽度自适应（96 DPI 基准）
// ---------------------------------------------------------------------------

TEST_CASE("Tab 宽度分配：空间充足各取自然宽", "[tabs]") {
    // cap = max(100, 1000/4=250)：自然宽均低于 cap，Σ+gaps=534 ≤ 1000
    const std::vector<int> w = DistributeTabWidths({200, 150, 180}, 1000, 96);
    REQUIRE(w.size() == 3);
    CHECK(w[0] == 200);
    CHECK(w[1] == 150);
    CHECK(w[2] == 180);
}

TEST_CASE("Tab 宽度分配：自然宽夹取下限与上限", "[tabs]") {
    // 单 Tab 上限 cap = max(100, floor(W/4))：500 → 夹到 250
    CHECK(DistributeTabWidths({500}, 1000, 96) == std::vector<int>{250});
    // 极窄窗口：cap 退化为下限 100
    CHECK(DistributeTabWidths({150}, 350, 96) == std::vector<int>{100});
    // 自然宽不足下限：抬到 100（恒有 min ≤ w_i）
    CHECK(DistributeTabWidths({80}, 1000, 96) == std::vector<int>{100});
}

TEST_CASE("Tab 宽度分配：等比收缩与向下取整", "[tabs]") {
    // 等比收缩需先过 cap 关：cap = W/4，故收缩场景要求 n≥5 才可能 Σ+gaps > W。
    // W=688（cap=172）：{172×5} 全部顶到 cap，D=868-688=180，C=5×72=360，
    // ratio 恰为 0.5（二进制精确）；w=floor(172-36)=136 ×5，Σ+gaps=688 恰好放下
    const std::vector<int> we = DistributeTabWidths({172, 172, 172, 172, 172}, 688, 96);
    REQUIRE(we.size() == 5);
    CHECK(we[0] == 136);
    CHECK(we[4] == 136);
    CHECK(we[0] + we[1] + we[2] + we[3] + we[4] + 4 * 2 == 688);

    // 不对称可收缩量：W=688，{172,160,140,120,100} → Σ=692，D=12，C=192，
    // ratio=12/192=1/16（二进制精确）：
    //   w0=floor(172-4.5)=167（.5 向下取整边界）
    //   w1=floor(160-3.75)=156
    //   w2=floor(140-2.5)=137（.5 向下取整边界）
    //   w3=floor(120-1.25)=118
    //   w4=100（恰为下限，可收缩量 0）
    const std::vector<int> w = DistributeTabWidths({172, 160, 140, 120, 100}, 688, 96);
    REQUIRE(w.size() == 5);
    CHECK(w[0] == 167);
    CHECK(w[1] == 156);
    CHECK(w[2] == 137);
    CHECK(w[3] == 118);
    CHECK(w[4] == 100);
    CHECK(w[0] + w[1] + w[2] + w[3] + w[4] + 4 * 2 <= 688);  // 向下取整保证 Σ+gaps ≤ W

    // 混合取整路径（远离整数边界防浮点抖动）：W=800，{200×6} → D=410，C=600，
    // w=floor(200-68.33)=131 ×6
    const std::vector<int> w3 = DistributeTabWidths({200, 200, 200, 200, 200, 200}, 800, 96);
    REQUIRE(w3.size() == 6);
    CHECK(w3[0] == 131);
    CHECK(w3[5] == 131);
    CHECK(w3[0] * 6 + 5 * 2 <= 800);
}

TEST_CASE("Tab 宽度分配：收缩饱和回退最小宽", "[tabs]") {
    // {150,150} W=150：cap 夹到 100 后 D=52 ≥ C=0 → 全部 100，横向滚动接管
    CHECK(DistributeTabWidths({150, 150}, 150, 96) == (std::vector<int>{100, 100}));
    // 饱和（D ≥ C）：{200×10} W=800 → D=1218 ≥ C=1000 → 全部最小宽，
    // 溢出仍存在（1018 > 800 → 滚动接管）
    const std::vector<int> w = DistributeTabWidths(std::vector<int>(10, 200), 800, 96);
    REQUIRE(w.size() == 10);
    CHECK(w[0] == 100);
    CHECK(w[9] == 100);
}

TEST_CASE("Tab 宽度分配：DPI 换算（下限/间距随 DPI 放大）", "[tabs]") {
    // 200%：min=200，cap=max(200, 2000/4=500)；自然宽 500 恰达上限
    CHECK(DistributeTabWidths({500}, 2000, 192) == std::vector<int>{500});
    // 200% 饱和：{400,400} W=400（物理像素小窗）→ D=404 ≥ C=200 → 全部 200
    CHECK(DistributeTabWidths({400, 400}, 400, 192) == (std::vector<int>{200, 200}));
}

TEST_CASE("Tab 宽度分配：空表与零宽窗口边界", "[tabs]") {
    CHECK(DistributeTabWidths({}, 800, 96).empty());
    // W=0：cap=100（下限），Σ>0 → D≥C（C=0）→ 全部下限
    CHECK(DistributeTabWidths({100}, 0, 96) == std::vector<int>{100});
}

// ---------------------------------------------------------------------------
// ComputeTabStripGeometry：几何
// ---------------------------------------------------------------------------

namespace {
// 3 Tab 标准夹具：区域 {48,0,860,40}（1000 宽窗口的 tabArea），活动/滚动各用例给定
std::vector<TabItem> Fixture3() {
    return {{L"未命名-1", true}, {L"readme.md", false}, {L"很长的中文文件名测试用例.txt", false}};
}
constexpr RECT kArea{48, 0, 860, 40};
}  // namespace

TEST_CASE("Tab 条几何：贴底、间距、内容宽与溢出判定", "[tabs]") {
    const TabStripGeometry g =
        ComputeTabStripGeometry(Fixture3(), 1, {120, 100, 140}, 10, kArea, 96, 0);
    REQUIRE(g.tabs.size() == 3);
    // Tab 高 34，贴底：top = 40-34 = 6；间距 2
    CHECK(RectEq(g.tabs[0].bounds, 48, 6, 168, 40));
    CHECK(RectEq(g.tabs[1].bounds, 170, 6, 270, 40));
    CHECK(RectEq(g.tabs[2].bounds, 272, 6, 412, 40));
    // 内容宽 = Σ + 2×gaps = 360+4 = 364；可视宽 812；未溢出
    CHECK(g.contentWidth == 364);
    CHECK(g.viewWidth == 812);
    CHECK(!g.overflow);
    CHECK(g.maxScroll == 0);
}

TEST_CASE("Tab 条几何：溢出与最大滚动量", "[tabs]") {
    // 8 Tab × 100 + 7×2 = 814 > 812：溢出 2px
    const std::vector<TabItem> items(8, TabItem{L"x", false});
    const TabStripGeometry g =
        ComputeTabStripGeometry(items, 0, std::vector<int>(8, 100), 10, kArea, 96, 0);
    CHECK(g.overflow);
    CHECK(g.contentWidth == 814);
    CHECK(g.maxScroll == 2);
}

TEST_CASE("Tab 条几何：分隔线规则（活动卡片两侧无线）", "[tabs]") {
    const auto items = Fixture3();
    const std::vector<int> widths{120, 100, 140};
    // active=0：tab0 首位无线；tab1 左邻活动无线；tab2 左邻非活动有线
    auto g = ComputeTabStripGeometry(items, 0, widths, 10, kArea, 96, 0);
    CHECK(!g.tabs[0].separatorLeft);
    CHECK(!g.tabs[1].separatorLeft);
    CHECK(g.tabs[2].separatorLeft);
    // active=1：凸起卡片两侧无线
    g = ComputeTabStripGeometry(items, 1, widths, 10, kArea, 96, 0);
    CHECK(!g.tabs[0].separatorLeft);
    CHECK(!g.tabs[1].separatorLeft);
    CHECK(!g.tabs[2].separatorLeft);
    // active=2
    g = ComputeTabStripGeometry(items, 2, widths, 10, kArea, 96, 0);
    CHECK(!g.tabs[0].separatorLeft);
    CHECK(g.tabs[1].separatorLeft);
    CHECK(!g.tabs[2].separatorLeft);
    // 无活动（空态防御）：非首标签均有线
    g = ComputeTabStripGeometry(items, -1, widths, 10, kArea, 96, 0);
    CHECK(!g.tabs[0].separatorLeft);
    CHECK(g.tabs[1].separatorLeft);
    CHECK(g.tabs[2].separatorLeft);
}

TEST_CASE("Tab 条几何：● 标记与文本区（装饰宽单点定义）", "[tabs]") {
    const auto items = Fixture3();
    const TabStripGeometry g =
        ComputeTabStripGeometry(items, 0, {120, 100, 140}, 10, kArea, 96, 0);
    // tab0（modified）：● 区 = 左边距 8 + 实测标记宽 10；文本区左界 = 8+10+间距 5
    CHECK(g.tabs[0].hasMark);
    CHECK(RectEq(g.tabs[0].mark, 56, 6, 66, 40));
    CHECK(g.tabs[0].label.left == 71);
    CHECK(g.tabs[0].label.right == 158);  // 168 - 右内边距 10
    // tab1（无标记）：空标记区；文本区左界 = 纯左边距 10
    CHECK_FALSE(g.tabs[1].hasMark);
    CHECK(g.tabs[1].mark.left == g.tabs[1].mark.right);
    CHECK(g.tabs[1].label.left == 180);
    CHECK(g.tabs[1].label.right == 260);
}

TEST_CASE("Tab 条命中：本体与空白、滚动偏移", "[tabs]") {
    const auto items = Fixture3();
    const std::vector<int> widths{120, 100, 140};
    const TabStripGeometry g0 =
        ComputeTabStripGeometry(items, 0, widths, 10, kArea, 96, 0);
    CHECK(TabStripHitTest(g0, {50, 20}) == 0);    // tab0 内
    CHECK(TabStripHitTest(g0, {200, 20}) == 1);   // tab1 内
    CHECK(TabStripHitTest(g0, {300, 20}) == 2);   // tab2 内
    CHECK(TabStripHitTest(g0, {47, 20}) == -1);   // 区域左缘外
    CHECK(TabStripHitTest(g0, {169, 20}) == -1);  // 间隙（右开区间）
    CHECK(TabStripHitTest(g0, {412, 20}) == -1);  // 末 Tab 右缘外（空白）
    CHECK(TabStripHitTest(g0, {500, 20}) == -1);  // 空白区（→HTCAPTION 路由）
    CHECK(TabStripHitTest(g0, {50, 5}) == -1);    // Tab 带上方（标题栏顶条）
    CHECK(TabStripHitTest(g0, {50, 6}) == 0);     // 带顶内含
    CHECK(TabStripHitTest(g0, {50, 41}) == -1);   // 标题栏以下

    // 滚动偏移 50：整体左移，tab1 移入命中
    const TabStripGeometry g50 =
        ComputeTabStripGeometry(items, 0, widths, 10, kArea, 96, 50);
    CHECK(g50.tabs[0].bounds.left == -2);
    CHECK(TabStripHitTest(g50, {50, 20}) == 0);   // 内容坐标 100 → tab0（0..120）
    CHECK(TabStripHitTest(g50, {150, 20}) == 1);  // tab1 平移到 [120,220)
    CHECK(TabStripHitTest(g50, {400, 20}) == -1);  // 内容尾部 362 已滚出
}

// ---------------------------------------------------------------------------
// 右键菜单模型（A3）与无障碍名称
// ---------------------------------------------------------------------------

TEST_CASE("右键菜单模型：动态数量文本与置灰规则", "[tabs]") {
    // index 1 / 共 5：右 3、左 1、其他 4——关闭项全可用；hasPath=true 新两项可用
    MenuModel m = BuildTabContextMenu({L"readme.md", false, true}, 1, 5);
    REQUIRE(m.itemCount(0) == 7);
    CHECK(m.item(0, 0).text == L"关闭当前文件");
    CHECK(m.item(0, 0).command == Command::CloseCurrentTab);
    CHECK(m.item(0, 0).enabled);
    CHECK(m.item(0, 1).text == L"关闭右边所有 (3)");
    CHECK(m.item(0, 1).command == Command::CloseTabsToRight);
    CHECK(m.item(0, 1).enabled);
    CHECK(m.item(0, 2).text == L"关闭左边所有 (1)");
    CHECK(m.item(0, 2).command == Command::CloseTabsToLeft);
    CHECK(m.item(0, 2).enabled);
    CHECK(m.item(0, 3).text == L"关闭其他文件 (4)");
    CHECK(m.item(0, 3).command == Command::CloseOtherTabs);
    CHECK(m.item(0, 3).enabled);
    // 第 4 项：分隔线（无命令无文本，键盘导航跳过）
    CHECK(m.item(0, 4).kind == MenuItem::Kind::Separator);
    CHECK(m.item(0, 4).command == Command::None);
    CHECK(m.item(0, 4).text.empty());
    // 第 5/6 项：文件定位类新项（hasPath=true → 可用）
    CHECK(m.item(0, 5).text == L"文件重命名…");
    CHECK(m.item(0, 5).command == Command::RenameFile);
    CHECK(m.item(0, 5).enabled);
    CHECK(m.item(0, 6).text == L"打开所在文件夹");
    CHECK(m.item(0, 6).command == Command::OpenContainingFolder);
    CHECK(m.item(0, 6).enabled);

    // index 0 / 共 1：右 0、左 0、其他 0——批量三项置灰（不隐藏）；
    // 未命名文档（hasPath 默认 false）：新两项置灰不隐藏，关闭项行为不变
    m = BuildTabContextMenu({L"未命名-1", true}, 0, 1);
    REQUIRE(m.itemCount(0) == 7);
    CHECK(m.item(0, 0).enabled);
    CHECK_FALSE(m.item(0, 1).enabled);
    CHECK_FALSE(m.item(0, 2).enabled);
    CHECK_FALSE(m.item(0, 3).enabled);
    CHECK(m.item(0, 1).text == L"关闭右边所有 (0)");
    CHECK(m.item(0, 4).kind == MenuItem::Kind::Separator);
    CHECK(m.item(0, 5).text == L"文件重命名…");
    CHECK(m.item(0, 5).command == Command::RenameFile);
    CHECK_FALSE(m.item(0, 5).enabled);
    CHECK(m.item(0, 6).text == L"打开所在文件夹");
    CHECK(m.item(0, 6).command == Command::OpenContainingFolder);
    CHECK_FALSE(m.item(0, 6).enabled);

    // index 2 / 共 3：右 0 置灰、左 2 可用、其他 2 可用（hasPath=true）
    m = BuildTabContextMenu({L"notes.txt", false, true}, 2, 3);
    CHECK_FALSE(m.item(0, 1).enabled);
    CHECK(m.item(0, 2).enabled);
    CHECK(m.item(0, 3).enabled);
}

TEST_CASE("无障碍名称含未保存状态", "[tabs]") {
    CHECK(AccessibleTabName({L"readme.md", true}) == L"readme.md，未保存");
    CHECK(AccessibleTabName({L"readme.md", false}) == L"readme.md");
}

// ---------------------------------------------------------------------------
// TabStrip::setDisplayName 同值短路（Win32界面性能修复 F2）：选区拖拽路径每帧
// 经 OnDocumentMetadataChanged → setDisplayName 调用，同值必须短路返回 false
// 不置脏重绘；实际变化返回 true。未 attach 组件即可测（数据接口不依赖宿主窗口，
// invalidateArea 对空 hwnd 安全返回）。
// ---------------------------------------------------------------------------

TEST_CASE("TabStrip setDisplayName：同值短路 false，变化 true，越界防御", "[tabs]") {
    TabStrip tabs;
    REQUIRE(tabs.addTab({L"a.txt", false}) == 0);
    SECTION("同名：不写入、返回 false") {
        CHECK_FALSE(tabs.setDisplayName(0, L"a.txt"));
        CHECK(tabs.item(0).displayName == L"a.txt");
    }
    SECTION("不同名：写入并返回 true") {
        CHECK(tabs.setDisplayName(0, L"b.txt"));
        CHECK(tabs.item(0).displayName == L"b.txt");
    }
    SECTION("越界：返回 false（防御，不崩溃）") {
        CHECK_FALSE(tabs.setDisplayName(-1, L"x"));
        CHECK_FALSE(tabs.setDisplayName(1, L"x"));
        CHECK(tabs.item(0).displayName == L"a.txt");
    }
}

// ---------------------------------------------------------------------------
// GDI 文本测量（自然宽的数据来源；无窗口依赖，屏幕 DC）
// ---------------------------------------------------------------------------

TEST_CASE("GDI 文本测量：中英文宽度差异", "[tabs]") {
    const UINT dpi = 96;
    HFONT label = CreateFontW(-Scale(tab_metrics::LabelPixelSize, dpi), 0, 0, 0, FW_NORMAL,
                              FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    REQUIRE(label != nullptr);
    HDC dc = GetDC(nullptr);
    REQUIRE(dc != nullptr);
    const HGDIOBJ old = SelectObject(dc, label);
    const auto widthOf = [&](const wchar_t* text) {
        SIZE sz{};
        CHECK(GetTextExtentPoint32W(dc, text, lstrlenW(text), &sz));
        return sz.cx;
    };
    const int cjk = widthOf(L"很长的中文文件名测试用例.txt");
    const int ascii = widthOf(L"readme.md");
    const int singleCjk = widthOf(L"文");
    const int singleAscii = widthOf(L"a");
    SelectObject(dc, old);
    ReleaseDC(nullptr, dc);
    DeleteObject(label);

    CHECK(cjk > 0);
    CHECK(ascii > 0);
    CHECK(cjk > ascii);          // 长中文名自然宽必然大于短英文名（宽度分配输入）
    CHECK(singleCjk > singleAscii);  // 单字：CJK 全宽 > ASCII（自然宽按字符数累积）
}

// ---------------------------------------------------------------------------
// Tab 标签字号常量（Win32界面体验修复 F4 / TC-002）：13→12 = 系统 9pt 标准。
// 自然宽测量（本文件 GDI 用例）与渲染共用该常量；此处钉死常量值防回退。
// ---------------------------------------------------------------------------

TEST_CASE("Tab 标签字号常量：LabelPixelSize == 12", "[tabs]") {
    CHECK(tab_metrics::LabelPixelSize == 12);
}

// ---------------------------------------------------------------------------
// TabDragSwapIndex：拖拽中点交换判定矩阵（TC-004；96 DPI 已知几何）
// ---------------------------------------------------------------------------

TEST_CASE("Tab 拖拽中点交换判定：越过相邻中点矩阵", "[tabs]") {
    const auto items = Fixture3();
    const std::vector<int> widths{120, 100, 140};
    const TabStripGeometry g =
        ComputeTabStripGeometry(items, 0, widths, 10, kArea, 96, 0);
    // bounds：tab0 [48,168) tab1 [170,270) tab2 [272,412)；中点：108 / 220 / 342
    // dragIndex=1（左邻 tab0、右邻 tab2）
    CHECK(TabDragSwapIndex(g, 1, 107) == 0);   // 左邻中点以左（严格越过）→ 交换左
    CHECK(TabDragSwapIndex(g, 1, 108) == -1);  // 恰在左邻中点 → 不交换（严格越过）
    CHECK(TabDragSwapIndex(g, 1, 109) == -1);  // 左邻中点以右 → 不交换
    CHECK(TabDragSwapIndex(g, 1, 219) == -1);  // 同位区间 → 不交换
    CHECK(TabDragSwapIndex(g, 1, 341) == -1);  // 右邻中点以左 → 不交换
    CHECK(TabDragSwapIndex(g, 1, 342) == -1);  // 恰在右邻中点 → 不交换（严格越过）
    CHECK(TabDragSwapIndex(g, 1, 343) == 2);   // 右邻中点以右（严格越过）→ 交换右
    // 首下标向左 / 末下标向右 → -1（无该侧邻居）
    CHECK(TabDragSwapIndex(g, 0, 0) == -1);
    CHECK(TabDragSwapIndex(g, 0, 47) == -1);
    CHECK(TabDragSwapIndex(g, 2, 412) == -1);
    CHECK(TabDragSwapIndex(g, 2, 1000) == -1);
    // 首位向右 / 末位向左严格越过 tab1 中点 → 正常交换
    CHECK(TabDragSwapIndex(g, 0, 221) == 1);
    CHECK(TabDragSwapIndex(g, 2, 219) == 1);
    // 越界输入防御
    CHECK(TabDragSwapIndex(g, -1, 100) == -1);
    CHECK(TabDragSwapIndex(g, 3, 100) == -1);
    CHECK(TabDragSwapIndex(TabStripGeometry{}, 0, 100) == -1);

    // 滚动偏移 50（4 等宽 Tab）：bounds 左移 50，判定随 bounds（已含偏移）：
    // tab0 [-2,98) tab1 [100,200) tab2 [202,302) tab3 [304,404)；中点 150/252/354
    const std::vector<TabItem> items4(4, TabItem{L"x", false});
    const TabStripGeometry gs =
        ComputeTabStripGeometry(items4, 0, std::vector<int>(4, 100), 10, kArea, 96, 50);
    CHECK(TabDragSwapIndex(gs, 2, 149) == 1);  // 越过左邻（tab1）中点 → 交换左
    CHECK(TabDragSwapIndex(gs, 2, 151) == -1);
    CHECK(TabDragSwapIndex(gs, 2, 355) == 3);  // 越过右邻（tab3）中点 → 交换右
    CHECK(TabDragSwapIndex(gs, 2, 353) == -1);
    CHECK(TabDragSwapIndex(gs, 3, 300) == -1);  // 末位向右 → -1
}

// ---------------------------------------------------------------------------
// TabStrip 拖拽状态机（TC-005/006/007/008 + 陈旧按下防呆）。无窗口夹具：未
// attach 的 TabStrip 其 tabArea 经 ComputeTitleBarLayout 规范化为有效窄条
// {tabRight(负), 0, tabLeft(正), capH}（96 DPI 下约 [-140,48)，宽 188），各 Tab
// 仍按统一最小宽顺序排布且可经 geometry() 查询（数据接口/交互不依赖宿主窗口，
// invalidateArea 对空 hwnd 安全——对齐 setDisplayName 用例先例）。该条内容宽
// 大于可视宽（恒溢出），拖拽 move 的 x 钳制与边缘带在此真实生效——坐标一律从
// 查询到的 bounds/区域推导（DPI 无关），并避开边缘带（不触自动滚动）。注意：
// down 即激活会经 scrollIntoView 推滚滚动偏移，按下非活动 Tab 的用例须先滚回
// 并重查几何。合成驱动（无真实鼠标按钮）由 down 时刻采样豁免按钮防呆。
// ---------------------------------------------------------------------------

TEST_CASE("TabStrip 拖拽：越阈值进入、中点预览交换、松手一次提交", "[tabs]") {
    TabStrip tabs;
    tabs.addTab({L"A.txt", false});
    tabs.addTab({L"B.txt", false});
    tabs.addTab({L"C.txt", false});
    TabStripGeometry g = tabs.geometry();
    REQUIRE(g.tabs.size() == 3);
    const int w = g.tabs[0].bounds.right - g.tabs[0].bounds.left;  // 统一最小宽
    REQUIRE(w > 0);
    CHECK(g.tabs[1].bounds.right - g.tabs[1].bounds.left == w);
    const POINT press{g.tabs[0].bounds.left + w / 2,
                      (g.tabs[0].bounds.top + g.tabs[0].bounds.bottom) / 2};
    const int mid1 = (g.tabs[1].bounds.left + g.tabs[1].bounds.right) / 2;

    tabs.onLButtonDown(press);
    CHECK(tabs.activeIndex() == 0);  // down 即激活（语义不变）
    CHECK(!tabs.dragging());
    tabs.onMouseMove({press.x + 1, press.y});  // 低于阈值的微动：不进入拖拽
    CHECK(!tabs.dragging());
    tabs.onMouseMove({press.x + w, press.y});  // 超过阈值 → 进入拖拽
    CHECK(tabs.dragging());
    // 进入拖拽的那次 move（x 尚未越过 tab1 中点，差一个间距）不产生交换；
    // 越过 tab1 中点 → 实时交换预览
    CHECK(tabs.item(0).displayName == L"A.txt");
    tabs.onMouseMove({mid1 + 1, press.y});
    CHECK(tabs.item(0).displayName == L"B.txt");
    CHECK(tabs.item(1).displayName == L"A.txt");
    CHECK(tabs.item(2).displayName == L"C.txt");
    CHECK(tabs.activeIndex() == 1);  // 活动 Tab 随迁（被拖即活动）
    CHECK(tabs.focusIndex() == 1);

    int cbFrom = -99, cbTo = -99, cbCount = 0;
    tabs.onTabReordered = [&](int from, int to) {
        cbFrom = from;
        cbTo = to;
        ++cbCount;
    };
    tabs.onLButtonUp({mid1 + 1, press.y});  // 松手：顺序有变才一次提交
    CHECK(cbCount == 1);
    CHECK(cbFrom == 0);
    CHECK(cbTo == 1);
    CHECK(!tabs.dragging());
    CHECK(tabs.item(0).displayName == L"B.txt");  // 预览序保持为提交序
    CHECK(tabs.item(1).displayName == L"A.txt");
    CHECK(tabs.activeIndex() == 1);
}

TEST_CASE("TabStrip 拖拽：纯点击（未超阈值）不触发重排", "[tabs]") {
    TabStrip tabs;
    tabs.addTab({L"A.txt", false});
    tabs.addTab({L"B.txt", false});
    TabStripGeometry g = tabs.geometry();
    REQUIRE(g.tabs.size() == 2);
    const int w = g.tabs[0].bounds.right - g.tabs[0].bounds.left;
    const POINT press{g.tabs[1].bounds.left + w / 2,
                      (g.tabs[0].bounds.top + g.tabs[0].bounds.bottom) / 2};
    tabs.onLButtonDown(press);
    CHECK(tabs.activeIndex() == 1);  // 激活仍在 down 完成
    tabs.onMouseMove({press.x + 1, press.y});  // 未超阈值
    CHECK(!tabs.dragging());
    int cbCount = 0;
    tabs.onTabReordered = [&](int, int) { ++cbCount; };
    tabs.onLButtonUp(press);  // 未拖拽的 up：仅清按下记录
    CHECK(cbCount == 0);
    CHECK(tabs.item(0).displayName == L"A.txt");  // 顺序不变
    CHECK(tabs.item(1).displayName == L"B.txt");
    CHECK(tabs.activeIndex() == 1);
    CHECK(!tabs.dragging());
    // up 之后 press 已清：再大幅移动不进入拖拽
    tabs.onMouseMove({press.x + w * 3, press.y});
    CHECK(!tabs.dragging());
    CHECK(cbCount == 0);
}

TEST_CASE("TabStrip 拖拽：Esc 取消回滚视觉序且不回调", "[tabs]") {
    TabStrip tabs;
    tabs.addTab({L"A.txt", false});
    tabs.addTab({L"B.txt", false});
    tabs.addTab({L"C.txt", false});
    TabStripGeometry g = tabs.geometry();
    REQUIRE(g.tabs.size() == 3);
    const int w = g.tabs[0].bounds.right - g.tabs[0].bounds.left;
    REQUIRE(w > 0);
    const POINT press{g.tabs[2].bounds.left + w / 2,
                      (g.tabs[0].bounds.top + g.tabs[0].bounds.bottom) / 2};
    const auto midOf = [&g](int i) {
        return (g.tabs[i].bounds.left + g.tabs[i].bounds.right) / 2;
    };
    // 拖末位 C 向左两次交换（等宽夹具交换后 bounds 同形；各步坐标均在钳制区间
    // 内且避开左右边缘带，不触自动滚动）
    tabs.onLButtonDown(press);
    CHECK(tabs.activeIndex() == 2);
    // down 即激活触发 scrollIntoView(2)：滚动被推至 maxScroll——先用滚轮回 0，
    // 再重查几何作为后续判定基准（midOf 按引用捕获自动生效）
    for (int i = 0; i < 10; ++i) {
        tabs.onMouseWheel(true, -WHEEL_DELTA);
    }
    g = tabs.geometry();
    REQUIRE(g.tabs.size() == 3);
    REQUIRE(tabs.scrollOffset() == 0);
    tabs.onMouseMove({midOf(1) + w / 8, press.y});  // 越阈值进入拖拽，未过 pos1 中点
    REQUIRE(tabs.dragging());
    CHECK(tabs.item(0).displayName == L"A.txt");    // 进入态无交换
    tabs.onMouseMove({midOf(1) - w / 8, press.y});  // 越过 pos1 中点 → [A,C,B]
    REQUIRE(tabs.item(0).displayName == L"A.txt");
    REQUIRE(tabs.item(1).displayName == L"C.txt");
    REQUIRE(tabs.item(2).displayName == L"B.txt");
    REQUIRE(tabs.activeIndex() == 1);               // 活动 Tab 随迁
    tabs.onMouseMove({midOf(0) - w / 8, press.y});   // 越过 pos0 中点 → [C,A,B]
    REQUIRE(tabs.item(0).displayName == L"C.txt");
    REQUIRE(tabs.item(1).displayName == L"A.txt");
    REQUIRE(tabs.item(2).displayName == L"B.txt");
    REQUIRE(tabs.activeIndex() == 0);

    int cbCount = 0;
    tabs.onTabReordered = [&](int, int) { ++cbCount; };
    CHECK(tabs.onKeyDown(VK_ESCAPE, false));  // 拖拽中 Esc：取消并消费
    CHECK(!tabs.dragging());
    CHECK(cbCount == 0);                       // 取消不回调（DEC-002）
    CHECK(tabs.item(0).displayName == L"A.txt");  // 回滚到按下时顺序
    CHECK(tabs.item(1).displayName == L"B.txt");
    CHECK(tabs.item(2).displayName == L"C.txt");
    CHECK(tabs.activeIndex() == 2);           // 活动关系同步恢复（C 回到 pos2）
    CHECK(tabs.focusIndex() == 2);
    CHECK_FALSE(tabs.onKeyDown(VK_ESCAPE, false));  // 非拖拽 Esc：不消费（原语义）
}

TEST_CASE("TabStrip 拖拽：捕获丢失取消回滚（同 Esc 语义）", "[tabs]") {
    TabStrip tabs;
    tabs.addTab({L"A.txt", false});
    tabs.addTab({L"B.txt", false});
    tabs.addTab({L"C.txt", false});
    TabStripGeometry g = tabs.geometry();
    REQUIRE(g.tabs.size() == 3);
    const int w = g.tabs[0].bounds.right - g.tabs[0].bounds.left;
    const POINT press{g.tabs[0].bounds.left + w / 2,
                      (g.tabs[0].bounds.top + g.tabs[0].bounds.bottom) / 2};
    const int mid1 = (g.tabs[1].bounds.left + g.tabs[1].bounds.right) / 2;
    tabs.onLButtonDown(press);
    tabs.onMouseMove({press.x + w, press.y});  // 进入拖拽
    REQUIRE(tabs.dragging());
    tabs.onMouseMove({mid1 + 1, press.y});     // 预览交换 → [B,A,C]
    REQUIRE(tabs.item(1).displayName == L"A.txt");
    REQUIRE(tabs.activeIndex() == 1);

    int cbCount = 0;
    tabs.onTabReordered = [&](int, int) { ++cbCount; };
    tabs.onCaptureChanged();  // 捕获丢失：取消回滚，不回调
    CHECK(!tabs.dragging());
    CHECK(cbCount == 0);
    CHECK(tabs.item(0).displayName == L"A.txt");
    CHECK(tabs.item(1).displayName == L"B.txt");
    CHECK(tabs.item(2).displayName == L"C.txt");
    CHECK(tabs.activeIndex() == 0);
    // 非拖拽时的捕获变更：no-op（不崩溃、不影响数据）
    tabs.onCaptureChanged();
    CHECK(tabs.item(0).displayName == L"A.txt");
    CHECK(tabs.activeIndex() == 0);
}

TEST_CASE("TabStrip 拖拽：陈旧按下不影响键盘导航与再次按下", "[tabs]") {
    TabStrip tabs;
    tabs.addTab({L"A.txt", false});
    tabs.addTab({L"B.txt", false});
    tabs.addTab({L"C.txt", false});
    TabStripGeometry g = tabs.geometry();
    REQUIRE(g.tabs.size() == 3);
    const int w = g.tabs[1].bounds.right - g.tabs[1].bounds.left;
    const POINT press{g.tabs[1].bounds.left + w / 2,
                      (g.tabs[0].bounds.top + g.tabs[0].bounds.bottom) / 2};
    // 只 down 无 up（既有 selftest 场景 2 的序列）：激活 B，留下陈旧按下记录
    tabs.onLButtonDown(press);
    REQUIRE(tabs.activeIndex() == 1);
    // 键盘导航不受陈旧按下影响（Esc 不消费、方向键/回车照常）
    CHECK_FALSE(tabs.onKeyDown(VK_ESCAPE, false));
    CHECK(tabs.onKeyDown(VK_LEFT, false));
    CHECK(tabs.focusIndex() == 0);
    CHECK(tabs.onKeyDown(VK_RETURN, false));
    CHECK(tabs.activeIndex() == 0);
    // 再次 down + 超阈值移动：从新按下点进入拖拽（陈旧记录被覆盖）
    g = tabs.geometry();
    const int w2 = g.tabs[2].bounds.right - g.tabs[2].bounds.left;
    const POINT press2{g.tabs[2].bounds.left + w2 / 2, press.y};
    tabs.onLButtonDown(press2);
    CHECK(tabs.activeIndex() == 2);
    tabs.onMouseMove({press2.x + w2, press2.y});
    CHECK(tabs.dragging());
    // 末位向右无交换：from == to，松手不回调
    int cbCount = 0;
    tabs.onTabReordered = [&](int, int) { ++cbCount; };
    tabs.onLButtonUp({press2.x + w2, press2.y});
    CHECK(!tabs.dragging());
    CHECK(cbCount == 0);
    CHECK(tabs.item(2).displayName == L"C.txt");
}
