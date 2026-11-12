// M1-3 Tab 条单测：宽度自适应算法（A2 规格：充足/等比收缩/饱和滚动/单 Tab 上限/
// 向下取整边界/DPI 换算）、Tab 条几何（贴底/间距/内容宽/分隔线规则/● 标记与文本区/
// 滚动偏移命中）、右键菜单模型（A3 动态数量与置灰）、无障碍名称、
// GDI 文本测量（中英文宽度差异——自然宽的数据来源）。
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
    // index 1 / 共 5：右 3、左 1、其他 4——全可用
    MenuModel m = BuildTabContextMenu(1, 5);
    REQUIRE(m.itemCount(0) == 4);
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

    // index 0 / 共 1：右 0、左 0、其他 0——批量三项置灰（不隐藏）
    m = BuildTabContextMenu(0, 1);
    REQUIRE(m.itemCount(0) == 4);
    CHECK(m.item(0, 0).enabled);
    CHECK_FALSE(m.item(0, 1).enabled);
    CHECK_FALSE(m.item(0, 2).enabled);
    CHECK_FALSE(m.item(0, 3).enabled);
    CHECK(m.item(0, 1).text == L"关闭右边所有 (0)");

    // index 2 / 共 3：右 0 置灰、左 2 可用、其他 2 可用
    m = BuildTabContextMenu(2, 3);
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
