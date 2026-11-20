// M1-3 Tab 条单测：宽度自适应算法（A2 规格：充足/等比收缩/饱和滚动/单 Tab 上限/
// 向下取整边界/DPI 换算）、Tab 条几何（贴底/间距/内容宽/分隔线规则/● 标记与文本区/
// 滚动偏移命中）、右键菜单模型（A3 动态数量与置灰）、无障碍名称、
// GDI 文本测量（中英文宽度差异——自然宽的数据来源）、拖拽重排（TabDragSwapIndex
// 历史口径矩阵 + 新视觉偏移式三纯函数 TC-001/002/003：压缩流中点插入槽位/让位
// 偏移四区间/ApproachPx 收敛终止性 + TabStrip 状态机：阈值进入/数据零改动/
// 松手一次提交/Esc 与捕获丢失取消（数据恒不变）/onTimer 合成节拍收敛/纯点击与
// 陈旧按下防呆——无窗口夹具，坐标一律从 geometry() 查询推导，DPI 无关）。
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
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
// TabDragInsertIndex / ComputeTabDragOffsets / ApproachPx（视觉偏移式拖拽的三
// 个纯函数；TC-001 插入槽位矩阵 / TC-002 让位偏移矩阵 / TC-003 缓动终止性）
// ---------------------------------------------------------------------------

TEST_CASE("Tab 拖拽插入槽位判定：压缩流中点矩阵", "[tabs]") {
    const auto items = Fixture3();
    const std::vector<int> widths{120, 100, 140};
    const TabStripGeometry g =
        ComputeTabStripGeometry(items, 0, widths, 10, kArea, 96, 0);
    // bounds：tab0 [48,168) tab1 [170,270) tab2 [272,412)
    // 拖 tab0（被拖宽 120）：压缩流 tab1 [48,148) 中点 98、tab2 [150,290) 中点 220
    CHECK(TabDragInsertIndex(g, 0, 0) == 0);    // 首下标向左 → 首槽位
    CHECK(TabDragInsertIndex(g, 0, 47) == 0);
    CHECK(TabDragInsertIndex(g, 0, 97) == 0);   // 未越过 tab1 压缩中点
    CHECK(TabDragInsertIndex(g, 0, 98) == 0);   // 恰在压缩中点 → 不计入（严格越过）
    CHECK(TabDragInsertIndex(g, 0, 99) == 1);   // 严格越过 tab1 压缩中点
    CHECK(TabDragInsertIndex(g, 0, 219) == 1);
    CHECK(TabDragInsertIndex(g, 0, 220) == 1);  // 恰在 tab2 压缩中点
    CHECK(TabDragInsertIndex(g, 0, 221) == 2);  // 严格越过 tab2 压缩中点
    CHECK(TabDragInsertIndex(g, 0, 1000) == 2); // 末向右 → 钳制末槽位
    // 拖 tab1（被拖宽 100）：tab0 静息 [48,168) 中点 108；tab2 压缩 [170,310) 中点 240
    CHECK(TabDragInsertIndex(g, 1, 107) == 0);
    CHECK(TabDragInsertIndex(g, 1, 108) == 0);  // 恰在左邻静息中点
    CHECK(TabDragInsertIndex(g, 1, 109) == 1);  // 同位
    CHECK(TabDragInsertIndex(g, 1, 240) == 1);  // 恰在右邻压缩中点
    CHECK(TabDragInsertIndex(g, 1, 241) == 2);
    // 拖 tab2（被拖宽 140）：左邻静息不动（中点 108 / 220）
    CHECK(TabDragInsertIndex(g, 2, 107) == 0);
    CHECK(TabDragInsertIndex(g, 2, 109) == 1);
    CHECK(TabDragInsertIndex(g, 2, 220) == 1);
    CHECK(TabDragInsertIndex(g, 2, 221) == 2);
    CHECK(TabDragInsertIndex(g, 2, 1000) == 2);
    // 越界/空防御：钳制到 [0, n-1]（空几何无有效槽位返回 0）
    CHECK(TabDragInsertIndex(g, -1, 100) == 0);
    CHECK(TabDragInsertIndex(g, 3, 100) == 2);
    CHECK(TabDragInsertIndex(TabStripGeometry{}, 0, 100) == 0);

    // 滚动偏移 50（4 等宽 Tab）：bounds 整体左移 50，压缩判定随 bounds：
    // tab0 [-2,98) tab1 [100,200) tab2 [202,302) tab3 [304,404)
    // 拖 tab2：左邻静息中点 48/150；tab3 压缩 [202,302) 中点 252
    const std::vector<TabItem> items4(4, TabItem{L"x", false});
    const TabStripGeometry gs =
        ComputeTabStripGeometry(items4, 0, std::vector<int>(4, 100), 10, kArea, 96, 50);
    CHECK(TabDragInsertIndex(gs, 2, 47) == 0);
    CHECK(TabDragInsertIndex(gs, 2, 49) == 1);
    CHECK(TabDragInsertIndex(gs, 2, 151) == 2);
    CHECK(TabDragInsertIndex(gs, 2, 252) == 2);
    CHECK(TabDragInsertIndex(gs, 2, 253) == 3);
}

TEST_CASE("Tab 拖拽让位偏移：四区间/单邻居/同位矩阵", "[tabs]") {
    const auto items = Fixture3();
    const std::vector<int> widths{120, 100, 140};
    const TabStripGeometry g =
        ComputeTabStripGeometry(items, 0, widths, 10, kArea, 96, 0);
    // 让位量：拖 tab0/tab1/tab2 分别为 122 / 102 / 142（被拖宽 + 间距 2）
    CHECK(ComputeTabDragOffsets(g, 0, 0, 42) == std::vector<int>{42, 0, 0});   // insert==from
    CHECK(ComputeTabDragOffsets(g, 1, 1, 7) == std::vector<int>{0, 7, 0});
    CHECK(ComputeTabDragOffsets(g, 2, 2, -9) == std::vector<int>{0, 0, -9});
    CHECK(ComputeTabDragOffsets(g, 0, 1, 42) == std::vector<int>{42, -122, 0});   // 右移一格
    CHECK(ComputeTabDragOffsets(g, 0, 2, -30) == std::vector<int>{-30, -122, -122});  // 右移两格
    CHECK(ComputeTabDragOffsets(g, 2, 1, 7) == std::vector<int>{0, 142, 7});     // 左移一格
    CHECK(ComputeTabDragOffsets(g, 2, 0, 55) == std::vector<int>{142, 142, 55}); // 左移两格
    // 单邻居（2 Tab）
    const TabStripGeometry g2 = ComputeTabStripGeometry(
        {TabItem{L"a", false}, TabItem{L"b", false}}, 0, {120, 100}, 10, kArea, 96, 0);
    CHECK(ComputeTabDragOffsets(g2, 0, 1, 10) == (std::vector<int>{10, -122}));
    CHECK(ComputeTabDragOffsets(g2, 1, 0, -20) == (std::vector<int>{102, -20}));
    // 参数钳制防御：from/insert 越界钳到 [0, n-1]；空几何返回空
    CHECK(ComputeTabDragOffsets(g, -1, 5, 33) == (std::vector<int>{33, -122, -122}));
    CHECK(ComputeTabDragOffsets(g, 0, 99, 8) == (std::vector<int>{8, -122, -122}));
    CHECK(ComputeTabDragOffsets(g, 99, -5, 1) == (std::vector<int>{142, 142, 1}));
    CHECK(ComputeTabDragOffsets(TabStripGeometry{}, 0, 0, 5).empty());
}

TEST_CASE("Tab 拖拽缓动步进：单调收敛与精确落位终止性", "[tabs]") {
    // |Δ|≤1px：精确取目标（含 cur==target 恒不变）
    CHECK(ApproachPx(8, 8, 0.45) == 8);
    CHECK(ApproachPx(7, 8, 0.45) == 8);
    CHECK(ApproachPx(8, 7, 0.45) == 7);
    CHECK(ApproachPx(-3, -4, 0.45) == -4);
    // 常规步进：current + round(Δ×ratio)
    CHECK(ApproachPx(0, 3, 0.45) == 1);     // round(1.35) = 1
    CHECK(ApproachPx(0, 100, 0.45) == 45);
    CHECK(ApproachPx(100, 0, 0.45) == 55);  // 负向对称
    // 反复步进：单调不越目标、有限帧内精确落位、落位后恒不变
    //（正/负向 × 小/大量程；102px = 让位量的典型量级）
    const std::array<std::pair<int, int>, 5> cases{{
        {0, 102},
        {0, 1000},
        {1000, 0},
        {-500, 500},
        {7, -360},
    }};
    for (const auto& [start, target] : cases) {
        int cur = start;
        bool monotonic = true;
        int frames = 0;
        while (cur != target && frames < 10000) {
            const int next = ApproachPx(cur, target, 0.45);
            const bool toward = target > start ? (next > cur && next <= target)
                                               : (next < cur && next >= target);
            monotonic = monotonic && toward;
            cur = next;
            ++frames;
        }
        CHECK(monotonic);
        REQUIRE(cur == target);                          // 有限帧内精确落位
        CHECK(ApproachPx(cur, target, 0.45) == target);  // 落位后恒不变
    }
    // 极小比例：步进取整退化时至少前进 1px，仍保证终止性
    int cur = 0;
    int frames = 0;
    while (cur != 5 && frames < 1000) {
        cur = ApproachPx(cur, 5, 0.001);
        ++frames;
    }
    CHECK(cur == 5);
    // 比例接近 1：一步或数步到位且不越目标
    CHECK(ApproachPx(0, 10, 0.99) == 10);
    CHECK(ApproachPx(0, 3, 0.99) == 3);
}

// ---------------------------------------------------------------------------
// TabStrip 拖拽状态机（视觉偏移式，DEC-001：拖拽期间 items_/active/focus 零改动；
// TC-004..009）。无窗口夹具：未 attach 的 TabStrip 其 tabArea 经
// ComputeTitleBarLayout 规范化为有效窄条 {tabRight(负), 0, tabLeft(正), capH}
// （96 DPI 下约 [-140,48)，宽 188），各 Tab 仍按统一最小宽顺序排布且可经
// geometry() 查询（数据接口/交互不依赖宿主窗口，invalidateArea 对空 hwnd
// 安全——对齐 setDisplayName 用例先例）。该条内容宽大于可视宽（恒溢出），
// 拖拽 move 的 x 钳制与边缘带在此真实生效——坐标一律从查询到的 bounds/区域
// 推导（DPI 无关），并避开左右边缘带（不触自动滚动）。等宽最小 Tab 夹具下
// 「被拖 Tab 中心」与其右邻压缩中点恰好重合（压缩中点 = 静息左缘 + w/2 =
// tab0 槽位中心）——跨线坐标由此推导。注意：down 即激活会经 scrollIntoView
// 推滚滚动偏移，按下非活动 Tab 的用例须先滚回并重查几何。合成驱动（无真实
// 鼠标按钮）由 down 时刻采样豁免按钮防呆。
// ---------------------------------------------------------------------------

TEST_CASE("TabStrip 拖拽：越阈值进入、数据零改动、松手一次提交", "[tabs]") {
    TabStrip tabs;
    tabs.addTab({L"A.txt", false});
    tabs.addTab({L"B.txt", false});
    tabs.addTab({L"C.txt", false});
    TabStripGeometry g = tabs.geometry();
    REQUIRE(g.tabs.size() == 3);
    const int w = g.tabs[0].bounds.right - g.tabs[0].bounds.left;  // 统一最小宽
    REQUIRE(w > 0);
    CHECK(g.tabs[1].bounds.right - g.tabs[1].bounds.left == w);
    const int gap = g.tabs[1].bounds.left - g.tabs[0].bounds.right;
    const POINT press{g.tabs[0].bounds.left + w / 2,
                      (g.tabs[0].bounds.top + g.tabs[0].bounds.bottom) / 2};
    // tab1 压缩中点（抽掉 tab0 槽位后的静息中点）：等宽下与 tab0 中心重合
    const int compMid1 =
        (g.tabs[1].bounds.left - (w + gap)) + w / 2;
    REQUIRE(compMid1 == press.x);

    // 非拖拽态测试缝（DEC-004）：插入槽位 -1，视觉偏移全 0
    CHECK(tabs.dragInsertIndex() == -1);
    CHECK(tabs.dragVisualOffsets() == std::vector<int>{0, 0, 0});

    int cbFrom = -99, cbTo = -99, cbCount = 0;
    tabs.onTabReordered = [&](int from, int to) {
        cbFrom = from;
        cbTo = to;
        ++cbCount;
    };

    tabs.onLButtonDown(press);
    CHECK(tabs.activeIndex() == 0);  // down 即激活（语义不变）
    CHECK(!tabs.dragging());
    tabs.onMouseMove({press.x + 1, press.y});  // 低于阈值的微动：不进入拖拽
    CHECK(!tabs.dragging());
    int dragThresh = std::max(GetSystemMetrics(SM_CXDRAG), GetSystemMetrics(SM_CYDRAG));
    if (dragThresh <= 0) dragThresh = 4;
    tabs.onMouseMove({press.x + dragThresh + 2, press.y});  // 超过阈值 → 进入拖拽
    CHECK(tabs.dragging());
    // 数据零改动（视觉偏移式，DEC-001）：items/active/focus 全程原样
    CHECK(tabs.item(0).displayName == L"A.txt");
    CHECK(tabs.item(1).displayName == L"B.txt");
    CHECK(tabs.item(2).displayName == L"C.txt");
    CHECK(tabs.activeIndex() == 0);
    CHECK(tabs.focusIndex() == 0);
    // 被拖 Tab 即时跟随（不缓动）：未触钳制时 followDx = x − press.x
    CHECK(tabs.dragVisualOffsets()[0] == dragThresh + 2);

    // 回撤到压缩中点以左：插入槽位回 0（单阈值无振荡）
    tabs.onMouseMove({compMid1 - 1, press.y});
    CHECK(tabs.dragInsertIndex() == 0);
    // 严格越过压缩中点：插入槽位 = 1，数据仍零改动
    tabs.onMouseMove({compMid1 + 1, press.y});
    CHECK(tabs.dragInsertIndex() == 1);
    CHECK(tabs.item(0).displayName == L"A.txt");
    CHECK(tabs.activeIndex() == 0);
    CHECK(tabs.dragVisualOffsets()[0] == 1);  // followDx = x − press.x

    // 跟随钳制（TC-011）：光标 x 在条内但被拖 Tab 右缘将越界 → 左界钳到
    // area.right − w（避开右边缘带取 x，不触步进滚动）
    const int zone = Scale(tab_metrics::DragEdgeZone, WindowDpi(nullptr));
    const int clampX = g.area.right - zone - 1;
    tabs.onMouseMove({clampX, press.y});
    const int maxDx = (g.area.right - w) - g.tabs[0].bounds.left;
    CHECK(tabs.dragVisualOffsets()[0] == maxDx);
    // 竖直位移只用于进入拖拽的阈值判定：跟随偏移仅随 x 变化（视觉 y 不变）
    tabs.onMouseMove({clampX, press.y - 80});
    CHECK(tabs.dragVisualOffsets()[0] == maxDx);

    // 回到插入槽位 1 后松手：恰一次提交 (0,1)；数据零改动是 mock 直驱下的
    // 正确语义（真实换序由生产 moveDocument 完成）
    tabs.onMouseMove({compMid1 + 1, press.y});
    tabs.onLButtonUp({compMid1 + 1, press.y});
    CHECK(cbCount == 1);
    CHECK(cbFrom == 0);
    CHECK(cbTo == 1);
    CHECK(!tabs.dragging());
    CHECK(tabs.dragInsertIndex() == -1);           // 收口复位
    CHECK(tabs.dragVisualOffsets() == std::vector<int>{0, 0, 0});
    CHECK(tabs.item(0).displayName == L"A.txt");
    CHECK(tabs.item(1).displayName == L"B.txt");
    CHECK(tabs.item(2).displayName == L"C.txt");
    CHECK(tabs.activeIndex() == 0);
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

TEST_CASE("TabStrip 拖拽：单 Tab（n=1）条内跟随、越界钳制、松手不回调", "[tabs]") {
    // 需求明示边界：无邻居——插入槽位恒 0，被拖 Tab 在条内跟随光标、拉出
    // 条外时钳制在条区右缘；from==to 松手不回调（DEC-002），数据恒不变
    TabStrip tabs;
    tabs.addTab({L"solo.txt", false});
    TabStripGeometry g = tabs.geometry();
    REQUIRE(g.tabs.size() == 1);
    const int w = g.tabs[0].bounds.right - g.tabs[0].bounds.left;
    REQUIRE(w > 0);
    const POINT press{g.tabs[0].bounds.left + w / 2,
                      (g.tabs[0].bounds.top + g.tabs[0].bounds.bottom) / 2};

    int cbCount = 0;
    tabs.onTabReordered = [&](int, int) { ++cbCount; };

    tabs.onLButtonDown(press);
    CHECK(tabs.activeIndex() == 0);
    CHECK(!tabs.dragging());
    int dragThresh = std::max(GetSystemMetrics(SM_CXDRAG), GetSystemMetrics(SM_CYDRAG));
    if (dragThresh <= 0) dragThresh = 4;
    // 条内小幅越阈值：进入拖拽，即时跟随（未触钳制时 offset = x − press.x）
    tabs.onMouseMove({press.x + dragThresh + 2, press.y});
    CHECK(tabs.dragging());
    REQUIRE(tabs.dragVisualOffsets().size() == 1);
    const int maxDx = (static_cast<int>(g.area.right) - w)
                    - static_cast<int>(g.tabs[0].bounds.left);
    CHECK(tabs.dragVisualOffsets()[0] == std::min(dragThresh + 2, maxDx));
    CHECK(tabs.dragInsertIndex() == 0);  // 无邻居：同槽位恒 0
    // 数据零改动（视觉偏移式，DEC-001）
    CHECK(tabs.item(0).displayName == L"solo.txt");
    CHECK(!tabs.item(0).modified);
    CHECK(tabs.activeIndex() == 0);
    CHECK(tabs.focusIndex() == 0);

    // 光标大幅拉到条区右界之外：跟随偏移被钳制——被拖 Tab 视觉右缘不超过
    // 条区右缘（视觉右缘 = bounds.right + offset ≤ area.right）
    tabs.onMouseMove({g.area.right + 500, press.y});
    CHECK(tabs.dragging());
    CHECK(tabs.dragInsertIndex() == 0);
    CHECK(tabs.dragVisualOffsets()[0] == maxDx);
    CHECK(g.tabs[0].bounds.right + tabs.dragVisualOffsets()[0] <= g.area.right);
    CHECK(tabs.item(0).displayName == L"solo.txt");
    CHECK(tabs.activeIndex() == 0);
    CHECK(tabs.focusIndex() == 0);

    // 松手：from==to 不回调，状态收口复位
    tabs.onLButtonUp({g.area.right + 500, press.y});
    CHECK(cbCount == 0);
    CHECK(!tabs.dragging());
    CHECK(tabs.dragInsertIndex() == -1);
    CHECK(tabs.dragVisualOffsets() == std::vector<int>{0});
    CHECK(tabs.item(0).displayName == L"solo.txt");
    CHECK(tabs.activeIndex() == 0);
    CHECK(tabs.focusIndex() == 0);
}

TEST_CASE("TabStrip 拖拽：Esc 取消（数据恒不变）且不回调", "[tabs]") {
    TabStrip tabs;
    tabs.addTab({L"A.txt", false});
    tabs.addTab({L"B.txt", false});
    tabs.addTab({L"C.txt", false});
    TabStripGeometry g = tabs.geometry();
    REQUIRE(g.tabs.size() == 3);
    const int w = g.tabs[1].bounds.right - g.tabs[1].bounds.left;
    REQUIRE(w > 0);
    const POINT press{g.tabs[2].bounds.left + w / 2,
                      (g.tabs[0].bounds.top + g.tabs[0].bounds.bottom) / 2};
    const auto midOf = [&g](int i) {
        return (g.tabs[i].bounds.left + g.tabs[i].bounds.right) / 2;
    };
    // 拖末位 C 向左（等宽夹具；各步坐标均在钳制区间内且避开左右边缘带，
    // 不触自动滚动）
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
    CHECK(tabs.dragInsertIndex() == 2);             // 进入态：同槽位
    CHECK(tabs.item(0).displayName == L"A.txt");
    // 越过 pos1 中点 → 插入槽位 1（视觉序 [A,·C,B]），数据不动
    tabs.onMouseMove({midOf(1) - w / 8, press.y});
    CHECK(tabs.dragInsertIndex() == 1);
    CHECK(tabs.item(0).displayName == L"A.txt");
    CHECK(tabs.item(1).displayName == L"B.txt");
    CHECK(tabs.item(2).displayName == L"C.txt");
    CHECK(tabs.activeIndex() == 2);  // active 不漂移（视觉偏移式的顺带收益）
    // 越过 pos0 中点 → 插入槽位 0（视觉序 [·C,A,B]）
    tabs.onMouseMove({midOf(0) - w / 8, press.y});
    CHECK(tabs.dragInsertIndex() == 0);
    CHECK(tabs.activeIndex() == 2);

    int cbCount = 0;
    tabs.onTabReordered = [&](int, int) { ++cbCount; };
    CHECK(tabs.onKeyDown(VK_ESCAPE, false));  // 拖拽中 Esc：取消并消费
    CHECK(!tabs.dragging());
    CHECK(tabs.dragInsertIndex() == -1);
    CHECK(cbCount == 0);  // 取消不回调（DEC-002）
    CHECK(tabs.dragVisualOffsets() == std::vector<int>{0, 0, 0});  // 偏移复位（回原位）
    CHECK(tabs.item(0).displayName == L"A.txt");  // 数据从未变动（无需回滚）
    CHECK(tabs.item(1).displayName == L"B.txt");
    CHECK(tabs.item(2).displayName == L"C.txt");
    CHECK(tabs.activeIndex() == 2);
    CHECK(tabs.focusIndex() == 2);
    CHECK_FALSE(tabs.onKeyDown(VK_ESCAPE, false));  // 非拖拽 Esc：不消费（原语义）
}

TEST_CASE("TabStrip 拖拽：捕获丢失取消（同 Esc 语义，数据恒不变）", "[tabs]") {
    TabStrip tabs;
    tabs.addTab({L"A.txt", false});
    tabs.addTab({L"B.txt", false});
    tabs.addTab({L"C.txt", false});
    TabStripGeometry g = tabs.geometry();
    REQUIRE(g.tabs.size() == 3);
    const int w = g.tabs[0].bounds.right - g.tabs[0].bounds.left;
    const POINT press{g.tabs[0].bounds.left + w / 2,
                      (g.tabs[0].bounds.top + g.tabs[0].bounds.bottom) / 2};
    const int gap = g.tabs[1].bounds.left - g.tabs[0].bounds.right;
    const int compMid1 = (g.tabs[1].bounds.left - (w + gap)) + w / 2;
    tabs.onLButtonDown(press);
    tabs.onMouseMove({press.x + w, press.y});  // 进入拖拽
    REQUIRE(tabs.dragging());
    tabs.onMouseMove({compMid1 + 1, press.y});  // 跨线 → 插入槽位 1（items 恒原序）
    REQUIRE(tabs.dragInsertIndex() == 1);
    REQUIRE(tabs.item(0).displayName == L"A.txt");
    REQUIRE(tabs.activeIndex() == 0);

    int cbCount = 0;
    tabs.onTabReordered = [&](int, int) { ++cbCount; };
    tabs.onCaptureChanged();  // 捕获丢失：清状态停表回原位，不回调
    CHECK(!tabs.dragging());
    CHECK(tabs.dragInsertIndex() == -1);
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

TEST_CASE("TabStrip 拖拽：onTimer 合成节拍驱动邻居偏移收敛", "[tabs]") {
    TabStrip tabs;
    tabs.addTab({L"A.txt", false});
    tabs.addTab({L"B.txt", false});
    tabs.addTab({L"C.txt", false});
    TabStripGeometry g = tabs.geometry();
    REQUIRE(g.tabs.size() == 3);
    const int w = g.tabs[0].bounds.right - g.tabs[0].bounds.left;
    REQUIRE(w > 0);
    const int gap = g.tabs[1].bounds.left - g.tabs[0].bounds.right;
    const POINT press{g.tabs[0].bounds.left + w / 2,
                      (g.tabs[0].bounds.top + g.tabs[0].bounds.bottom) / 2};
    const int compMid1 = (g.tabs[1].bounds.left - (w + gap)) + w / 2;
    int dragThresh = std::max(GetSystemMetrics(SM_CXDRAG), GetSystemMetrics(SM_CYDRAG));
    if (dragThresh <= 0) dragThresh = 4;
    tabs.onLButtonDown(press);
    tabs.onMouseMove({press.x + dragThresh + 2, press.y});  // 进入拖拽
    REQUIRE(tabs.dragging());
    tabs.onMouseMove({compMid1 + 1, press.y});  // 跨线 → 插入槽位 1
    REQUIRE(tabs.dragInsertIndex() == 1);
    const int shift = w + gap;  // tab1 让位目标：左移 (被拖宽 + 间距)
    // 跨线后未到节拍：邻居视觉仍在原位，被拖项即时跟随
    std::vector<int> offs = tabs.dragVisualOffsets();
    REQUIRE(offs.size() == 3);
    CHECK(offs[0] == 1);  // followDx = x − press.x
    CHECK(offs[1] == 0);
    CHECK(offs[2] == 0);
    // 合成节拍：邻居向 −shift 单调收敛、不越目标，有限帧内精确落位
    const std::vector<int> settled{1, -shift, 0};
    int frames = 0;
    while (tabs.dragVisualOffsets() != settled && frames < 1000) {
        const std::vector<int> prev = tabs.dragVisualOffsets();
        tabs.onTimer(kDragAnimTimerId);
        const std::vector<int> cur = tabs.dragVisualOffsets();
        CHECK(cur[0] == 1);           // 被拖项不参与缓动（恒跟随值）
        CHECK(cur[1] <= prev[1]);     // 单调向目标（负向）
        CHECK(cur[1] >= -shift);      // 不越目标
        CHECK(cur[2] == 0);           // 区间外邻居不动
        ++frames;
    }
    REQUIRE(tabs.dragVisualOffsets() == settled);
    CHECK(frames <= 100);  // 0.45 指数收敛约 7 帧（102px 量级）
    // 收敛后续帧不变（停表语义：数值稳定）
    tabs.onTimer(kDragAnimTimerId);
    CHECK(tabs.dragVisualOffsets() == settled);
    // 其他 id 无副作用（tooltip id=2 与未占用 id 均不触碰拖拽偏移）
    tabs.onTimer(2);
    tabs.onTimer(99);
    CHECK(tabs.dragVisualOffsets() == settled);
    // 松手收口：偏移归零 + 一次提交 (0,1)
    int cbFrom = -99, cbTo = -99, cbCount = 0;
    tabs.onTabReordered = [&](int from, int to) {
        cbFrom = from;
        cbTo = to;
        ++cbCount;
    };
    tabs.onLButtonUp({compMid1 + 1, press.y});
    CHECK(cbCount == 1);
    CHECK(cbFrom == 0);
    CHECK(cbTo == 1);
    CHECK(!tabs.dragging());
    CHECK(tabs.dragVisualOffsets() == std::vector<int>{0, 0, 0});
}

TEST_CASE("TabStrip 拖拽：跟随钳制矩阵（条区极左/条外捕获坐标/竖直不变）", "[tabs]") {
    // TC-011：被拖 Tab 视觉矩形钳制在 [area.left, area.right−w]；竖直位移只参与
    // 进入拖拽的阈值判定、不参与跟随（仅 x）。无窗口夹具 tabArea 恒溢出——条外
    // 右侧捕获坐标的 x 钳到 area.right−1 后落入右边缘带，恰好同时验证边缘步进
    // 滚动恰一次（TC-010 的单测级子集）与滚动后几何平移仍钳制：断言一律从重查
    // 的 geometry() 推导（DPI 无关）。
    TabStrip tabs;
    tabs.addTab({L"A.txt", false});
    tabs.addTab({L"B.txt", false});
    tabs.addTab({L"C.txt", false});
    TabStripGeometry g = tabs.geometry();
    REQUIRE(g.tabs.size() == 3);
    const int w = g.tabs[2].bounds.right - g.tabs[2].bounds.left;
    REQUIRE(w > 0);
    const POINT press{g.tabs[2].bounds.left + w / 2,
                      (g.tabs[0].bounds.top + g.tabs[0].bounds.bottom) / 2};
    tabs.onLButtonDown(press);  // down 即激活（scrollIntoView 推滚至 maxScroll）
    CHECK(tabs.activeIndex() == 2);
    for (int i = 0; i < 10; ++i) {
        tabs.onMouseWheel(true, -WHEEL_DELTA);  // 滚回 0 再重查几何（对齐 Esc 用例）
    }
    REQUIRE(tabs.scrollOffset() == 0);
    g = tabs.geometry();
    // 进入拖拽：move 至条中心（条区中部恒避开左右边缘带；位移远超阈值）
    tabs.onMouseMove({(g.area.left + g.area.right) / 2, press.y});
    REQUIRE(tabs.dragging());

    // 条区极左（含条外捕获坐标 x≪area.left）：x 钳到 area.left，视觉左缘贴条
    // 左缘；左带步进被 clamp(0) 抵消（scrollOffset 已 0）——无滚动副作用
    tabs.onMouseMove({g.area.left - 500, press.y});
    const int leftDx = static_cast<int>(g.area.left) - static_cast<int>(g.tabs[2].bounds.left);
    REQUIRE(leftDx < 0);  // 末位槽位不在条左缘：钳制产生非零负偏移（确已钳制）
    CHECK(tabs.dragVisualOffsets()[2] == leftDx);
    CHECK(tabs.scrollOffset() == 0);
    CHECK(tabs.dragInsertIndex() == 0);  // 光标钳在条最左 → 插入槽位 0
    // 竖直位移只参与进入拖拽的阈值判定：大幅竖直移动后跟随偏移不变
    tabs.onMouseMove({g.area.left - 500, press.y - 200});
    CHECK(tabs.dragVisualOffsets()[2] == leftDx);
    CHECK(tabs.item(0).displayName == L"A.txt");  // 数据恒不变（视觉偏移式）
    CHECK(tabs.item(2).displayName == L"C.txt");
    CHECK(tabs.activeIndex() == 2);
    CHECK(tabs.focusIndex() == 2);

    // 条外右侧捕获坐标：x 钳到 area.right−1 落入右带 → 恰一次步进滚动；滚动后
    // bounds 平移，视觉右缘仍钳在 area.right−w（maxLeft）
    const int step = Scale(tab_metrics::DragEdgeScrollStep, WindowDpi(nullptr));
    tabs.onMouseMove({g.area.right + 500, press.y});
    CHECK(tabs.scrollOffset() == step);
    const TabStripGeometry g2 = tabs.geometry();
    const int w2 = g2.tabs[2].bounds.right - g2.tabs[2].bounds.left;
    CHECK(tabs.dragVisualOffsets()[2] ==
          (static_cast<int>(g2.area.right) - w2) - static_cast<int>(g2.tabs[2].bounds.left));
    CHECK(tabs.item(0).displayName == L"A.txt");  // 滚动全程 items 恒不变
    CHECK(tabs.activeIndex() == 2);

    // Esc 收口：清状态回原位、不回调（取消完整语义见 Esc 专测，此处确认可收口）
    int cbCount = 0;
    tabs.onTabReordered = [&](int, int) { ++cbCount; };
    CHECK(tabs.onKeyDown(VK_ESCAPE, false));
    CHECK(!tabs.dragging());
    CHECK(cbCount == 0);
    CHECK(tabs.dragVisualOffsets() == std::vector<int>{0, 0, 0});
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
    // 末位向右：插入槽位 == from（x 已被钳制到条内，无更右槽位）——
    // from == to，松手不回调
    CHECK(tabs.dragInsertIndex() == 2);
    int cbCount = 0;
    tabs.onTabReordered = [&](int, int) { ++cbCount; };
    tabs.onLButtonUp({press2.x + w2, press2.y});
    CHECK(!tabs.dragging());
    CHECK(cbCount == 0);
    CHECK(tabs.dragInsertIndex() == -1);
    CHECK(tabs.item(2).displayName == L"C.txt");
}
