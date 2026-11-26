// 文档列表面板单测（任务 B）：ComputeDocListGeometry 几何（空表/无溢出/恰好
// 装满边界/溢出滚动条与滑块比例/滚动平移/未钳制偏移防御/退化面板/DPI 200%）、
// 命中（条目/空白/滚动条区/面板外/滚动偏移后）、钳制（滚动/宽度/分隔条命中）、
// 无障碍名称、右键菜单模型复用（BuildTabContextMenu hasPath 置灰）、组件级
// 状态机（布局切片与宽度钳制/幂等/收缩自动钳/setItems 保留 active/激活滚入
// 可视区/越界钳制/同值短路/命中三函数/点击激活/滚轮消费语义/滚动条箭头-轨道-
// 滑块拖拽/分隔条拖拽实时-Esc-提交-捕获丢失-失焦收口/拖拽中 leave 不取消）。
// 无窗口夹具（对齐 test_tabs 手法）：数据接口/几何/交互不依赖宿主窗口，
// invalidateArea 对空 hwnd 安全返回；坐标从 geometryForTest 查询推导。
#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <algorithm>
#include <vector>

#include "ui/doclist.h"
#include "ui/tabs.h"

using namespace ui;

namespace {
bool RectEq(const RECT& r, LONG left, LONG top, LONG right, LONG bottom) {
    return r.left == left && r.top == top && r.right == right && r.bottom == bottom;
}
bool RectEq(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
std::vector<DocListItem> Items(int n) {
    return std::vector<DocListItem>(n, DocListItem{L"doc", false});
}
// 标准夹具：96 DPI，面板 {0,40,200,460}（宽 200 = 默认宽，高 420 = 15 行恰满）
constexpr RECT kPanel{0, 40, 200, 460};
}  // namespace

// ---------------------------------------------------------------------------
// ComputeDocListGeometry：几何
// ---------------------------------------------------------------------------

TEST_CASE("DocList 几何：空表（无条目、无滚动条）", "[doclist]") {
    const DocListGeometry g = ComputeDocListGeometry({}, kPanel, 96, 0);
    CHECK(g.items.empty());
    CHECK(g.itemHeight == Scale(doclist_metrics::ItemHeight, 96));
    CHECK(g.contentHeight == 0);
    CHECK(g.viewHeight == 420);
    CHECK(!g.overflow);
    CHECK(g.maxScroll == 0);
    CHECK(RectEq(g.listArea, 0, 40, 200, 460));  // 无溢出：listArea = panel
    CHECK(g.scrollbar.left == g.scrollbar.right);  // 空矩形
    CHECK(g.sbThumb.left == g.sbThumb.right);
}

TEST_CASE("DocList 几何：无溢出（条目自顶排布，右界 = panel）", "[doclist]") {
    const DocListGeometry g = ComputeDocListGeometry(Items(3), kPanel, 96, 0);
    REQUIRE(g.items.size() == 3);
    CHECK(RectEq(g.items[0], 0, 40, 200, 68));
    CHECK(RectEq(g.items[1], 0, 68, 200, 96));
    CHECK(RectEq(g.items[2], 0, 96, 200, 124));
    CHECK(g.contentHeight == 3 * 28);
    CHECK(!g.overflow);
    CHECK(g.maxScroll == 0);
    CHECK(RectEq(g.listArea, kPanel));
    CHECK(g.scrollbar.left == g.scrollbar.right);
}

TEST_CASE("DocList 几何：恰好装满边界（contentHeight == viewHeight 不溢出）", "[doclist]") {
    // 15 行 × 28 = 420 == 面板高：严格大于才溢出（边界不闪滚动条）
    const DocListGeometry g = ComputeDocListGeometry(Items(15), kPanel, 96, 0);
    CHECK(g.contentHeight == 420);
    CHECK(g.viewHeight == 420);
    CHECK(!g.overflow);
    CHECK(g.maxScroll == 0);
    CHECK(RectEq(g.items[14], 0, 432, 200, 460));
    CHECK(g.scrollbar.left == g.scrollbar.right);
}

TEST_CASE("DocList 几何：溢出滚动条与滑块比例", "[doclist]") {
    // 16 行 × 28 = 448 > 420：溢出 28；滚动条宽 12 占右缘
    const DocListGeometry g = ComputeDocListGeometry(Items(16), kPanel, 96, 0);
    CHECK(g.overflow);
    CHECK(g.maxScroll == 28);
    CHECK(RectEq(g.scrollbar, 188, 40, 200, 460));
    CHECK(RectEq(g.listArea, 0, 40, 188, 460));  // 条目右界收缩
    // 箭头按钮 12×12；轨道 = 上下按钮之间（高 396）
    CHECK(RectEq(g.sbUp, 188, 40, 200, 52));
    CHECK(RectEq(g.sbDown, 188, 448, 200, 460));
    CHECK(RectEq(g.sbTrack, 188, 52, 200, 448));
    // 滑块：396×420/448 = 371（就近取整）；内缩 2 → {190,52,198,423}
    CHECK(RectEq(g.sbThumb, 190, 52, 198, 52 + 371));
    // 条目右界 = listArea.right（不进滚动条）
    CHECK(RectEq(g.items[0], 0, 40, 188, 68));
    CHECK(RectEq(g.items[15], 0, 460, 188, 488));  // 末条超出面板底（待滚动）
}

TEST_CASE("DocList 几何：滚动偏移平移条目与滑块随动", "[doclist]") {
    const DocListGeometry g = ComputeDocListGeometry(Items(16), kPanel, 96, 28);
    CHECK(g.maxScroll == 28);
    // 条目整体上移 28：item0 半出顶、item15 完整可见（贴底）
    CHECK(RectEq(g.items[0], 0, 12, 188, 40));
    CHECK(RectEq(g.items[15], 0, 432, 188, 460));
    // 滑块移到轨道底：top = 52 + 25×28/28 = 77（range = 396-371 = 25）
    CHECK(RectEq(g.sbThumb, 190, 77, 198, 77 + 371));
}

TEST_CASE("DocList 几何：未钳制的滚动偏移防御性钳制", "[doclist]") {
    // 传入 999 > maxScroll 28：内部钳到 maxScroll（与滚动偏移 28 同几何）
    const DocListGeometry g = ComputeDocListGeometry(Items(16), kPanel, 96, 999);
    CHECK(g.items[0].top == 12);
    CHECK(RectEq(g.sbThumb, 190, 77, 198, 448));
    // 负偏移同理钳到 0
    const DocListGeometry g0 = ComputeDocListGeometry(Items(16), kPanel, 96, -50);
    CHECK(g0.items[0].top == 40);
}

TEST_CASE("DocList 几何：退化面板（零宽/零高）", "[doclist]") {
    const DocListGeometry g = ComputeDocListGeometry(Items(3), {10, 40, 10, 460}, 96, 0);
    CHECK(g.items.empty());
    CHECK(!g.overflow);
    CHECK(g.maxScroll == 0);
    const DocListGeometry g2 = ComputeDocListGeometry(Items(3), {0, 40, 200, 40}, 96, 0);
    CHECK(g2.items.empty());
    CHECK(g2.viewHeight == 0);
    CHECK(!g2.overflow);
}

TEST_CASE("DocList 几何：DPI 200% 换算（行高/滚动条/箭头翻倍）", "[doclist]") {
    // 面板 {0,0,200,880} @192：行高 56、滚动条宽 24、箭头 24
    const DocListGeometry g = ComputeDocListGeometry(Items(30), {0, 0, 200, 880}, 192, 0);
    CHECK(g.itemHeight == Scale(28, 192));
    CHECK(g.contentHeight == 30 * 56);
    CHECK(g.overflow);
    CHECK(g.maxScroll == 30 * 56 - 880);
    CHECK(RectEq(g.scrollbar, 200 - Scale(12, 192), 0, 200, 880));
    CHECK(RectEq(g.listArea, 0, 0, 200 - Scale(12, 192), 880));
    CHECK(g.sbDown.bottom - g.sbDown.top == Scale(12, 192));
    CHECK(RectEq(g.items[0], 0, 0, 200 - Scale(12, 192), 56));
}

// ---------------------------------------------------------------------------
// DocListHitItem / DocListHitSplitter / 钳制 / 无障碍名称
// ---------------------------------------------------------------------------

TEST_CASE("DocList 命中：条目/空白/滚动条区/面板外", "[doclist]") {
    const DocListGeometry g = ComputeDocListGeometry(Items(3), kPanel, 96, 0);
    CHECK(DocListHitItem(g, {100, 40}) == 0);   // 首条顶边内含
    CHECK(DocListHitItem(g, {100, 67}) == 0);   // 首条底边外（右开区间）
    CHECK(DocListHitItem(g, {100, 68}) == 1);
    CHECK(DocListHitItem(g, {0, 50}) == 0);     // 左缘内含
    CHECK(DocListHitItem(g, {199, 50}) == 0);   // 右缘外不含
    CHECK(DocListHitItem(g, {100, 124}) == -1);  // 条目下方空白
    CHECK(DocListHitItem(g, {300, 50}) == -1);   // 面板右外
    CHECK(DocListHitItem(g, {100, 20}) == -1);   // 面板上外
    CHECK(DocListHitItem(g, {100, 500}) == -1);  // 面板下外
    CHECK(DocListHitItem(DocListGeometry{}, {100, 50}) == -1);  // 空几何防御
    // 空表
    const DocListGeometry ge = ComputeDocListGeometry({}, kPanel, 96, 0);
    CHECK(DocListHitItem(ge, {100, 50}) == -1);
}

TEST_CASE("DocList 命中：滚动偏移后按平移几何命中", "[doclist]") {
    const DocListGeometry g = ComputeDocListGeometry(Items(16), kPanel, 96, 28);
    CHECK(DocListHitItem(g, {100, 20}) == -1);  // item0 半出区在面板上外（不可达区）
    CHECK(DocListHitItem(g, {100, 40}) == 1);   // 滚出后 item1 顶到面板顶
    CHECK(DocListHitItem(g, {100, 455}) == 15); // 末条完整可见
    // 滚动条区不命中条目
    CHECK(DocListHitItem(g, {195, 100}) == -1);
}

TEST_CASE("DocList 滚动条区命中排除（溢出时 x ∈ 滚动条带 → -1）", "[doclist]") {
    const DocListGeometry g = ComputeDocListGeometry(Items(16), kPanel, 96, 0);
    CHECK(DocListHitItem(g, {188, 50}) == -1);  // listArea 右界外
    CHECK(DocListHitItem(g, {187, 50}) == 0);   // listArea 内最后一列
}

TEST_CASE("DocList 分隔条命中：右缘热区矩阵（含 DPI 换算）", "[doclist]") {
    // 96 DPI：热区 = [196,200) × [40,460)
    CHECK(DocListHitSplitter(kPanel, {198, 100}, 96));
    CHECK(DocListHitSplitter(kPanel, {196, 40}, 96));   // 热区左界内含
    CHECK(DocListHitSplitter(kPanel, {195, 100}, 96) == false);  // 热区左外
    CHECK(DocListHitSplitter(kPanel, {200, 100}, 96) == false);  // 面板右缘外
    CHECK(DocListHitSplitter(kPanel, {198, 39}, 96) == false);   // 面板上外
    CHECK(DocListHitSplitter(kPanel, {198, 460}, 96) == false);  // 面板底外
    // 退化矩形恒 false
    CHECK(DocListHitSplitter({}, {0, 0}, 96) == false);
    // 200% DPI：热区宽 = Scale(4,192) = 8 → [192,200)
    CHECK(DocListHitSplitter(kPanel, {195, 100}, 192));
    CHECK(DocListHitSplitter(kPanel, {191, 100}, 192) == false);
}

TEST_CASE("ClampDocListScroll：边界钳制矩阵", "[doclist]") {
    const DocListGeometry g = ComputeDocListGeometry(Items(16), kPanel, 96, 0);
    CHECK(g.maxScroll == 28);
    CHECK(ClampDocListScroll(-5, g) == 0);
    CHECK(ClampDocListScroll(0, g) == 0);
    CHECK(ClampDocListScroll(14, g) == 14);
    CHECK(ClampDocListScroll(28, g) == 28);
    CHECK(ClampDocListScroll(100, g) == 28);
    // 无溢出：恒 0
    const DocListGeometry gn = ComputeDocListGeometry(Items(3), kPanel, 96, 0);
    CHECK(ClampDocListScroll(50, gn) == 0);
    CHECK(ClampDocListScroll(-3, gn) == 0);
    // 空几何防御
    CHECK(ClampDocListScroll(10, DocListGeometry{}) == 0);
}

TEST_CASE("ClampDocListWidthLogical：边界钳制矩阵", "[doclist]") {
    CHECK(ClampDocListWidthLogical(100, 400) == 120);    // 低于下限
    CHECK(ClampDocListWidthLogical(0, 400) == 120);
    CHECK(ClampDocListWidthLogical(-3, 400) == 120);
    CHECK(ClampDocListWidthLogical(120, 400) == 120);    // 恰为下限
    CHECK(ClampDocListWidthLogical(250, 400) == 250);    // 区间内原样
    CHECK(ClampDocListWidthLogical(400, 400) == 400);    // 恰为上界
    CHECK(ClampDocListWidthLogical(500, 400) == 400);    // 超上界钳到上界
    // 极窄窗口：上界 < 下限 → 下限硬约束（最小宽是硬下界）
    CHECK(ClampDocListWidthLogical(300, 100) == 120);
    CHECK(ClampDocListWidthLogical(50, 100) == 120);
}

TEST_CASE("无障碍名称：未保存入名、干净不含、路径不入名", "[doclist]") {
    CHECK(AccessibleDocListName({L"readme.md", true}) == L"readme.md，未保存");
    CHECK(AccessibleDocListName({L"readme.md", false}) == L"readme.md");
    // path 不参与无障碍名称（与 Tab 口径一致：名称 + 未保存）
    CHECK(AccessibleDocListName({L"a.txt", true, true, L"C:\\dir\\a.txt"}) ==
          L"a.txt，未保存");
}

TEST_CASE("右键菜单模型复用：BuildTabContextMenu hasPath 置灰规则", "[doclist]") {
    // 未命名（无路径）：重命名/打开所在文件夹置灰不隐藏（与 Tab 右键一致）
    MenuModel m = BuildTabContextMenu(TabItem{L"未命名-1", true, false}, 0, 3);
    REQUIRE(m.itemCount(0) == 7);
    CHECK(m.item(0, 0).command == Command::CloseCurrentTab);
    CHECK(m.item(0, 0).enabled);
    CHECK(m.item(0, 4).kind == MenuItem::Kind::Separator);
    CHECK_FALSE(m.item(0, 5).enabled);
    CHECK_FALSE(m.item(0, 6).enabled);
    // 有路径：可用
    m = BuildTabContextMenu(TabItem{L"readme.md", false, true}, 1, 3);
    CHECK(m.item(0, 5).enabled);
    CHECK(m.item(0, 6).enabled);
    // 批量关闭动态数量
    m = BuildTabContextMenu(TabItem{L"x", false, true}, 2, 5);
    CHECK(m.item(0, 1).text == L"关闭右边所有 (2)");
    CHECK(m.item(0, 2).text == L"关闭左边所有 (2)");
    CHECK(m.item(0, 3).text == L"关闭其他文件 (4)");
}

// ---------------------------------------------------------------------------
// DocListPanel：布局切片与宽度钳制
// ---------------------------------------------------------------------------

TEST_CASE("DocListPanel：面板矩形切片/宽度钳制/幂等/收缩自动钳", "[doclist]") {
    DocListPanel p;
    // 未布局前 panelRect 为空、宽度仅下限钳制
    CHECK(RectEq(p.panelRect(), 0, 0, 0, 0));
    p.setWidthLogical(50);
    CHECK(p.widthLogical() == 120);
    p.setWidthLogical(260);  // 未布局（无上界）：不钳上界
    CHECK(p.widthLogical() == 260);

    p.layout({0, 40, 1000, 540}, 96);  // 内容区逻辑宽 1000 → 上界 333
    p.setVisible(true);
    CHECK(p.visible());
    CHECK(p.maxWidthLogicalForTest() == 333);
    CHECK(p.widthLogical() == 260);  // ≤ 上界不钳
    CHECK(RectEq(p.panelRect(), 0, 40, 260, 540));  // 左切片
    CHECK(RectEq(p.contentRect(), 0, 40, 1000, 540));

    p.setWidthLogical(50);   // 下限
    CHECK(p.widthLogical() == 120);
    CHECK(RectEq(p.panelRect(), 0, 40, 120, 540));
    p.setWidthLogical(9999);  // 上限
    CHECK(p.widthLogical() == 333);
    // 幂等：同参重复 layout → 同 panelRect
    p.layout({0, 40, 1000, 540}, 96);
    CHECK(RectEq(p.panelRect(), 0, 40, 333, 540));
    // 收缩内容区 → 上界变小自动钳（组装层主动布局驱动，不回调）
    p.layout({0, 40, 400, 540}, 96);  // 上界 = 400/3 = 133
    CHECK(p.widthLogical() == 133);
    CHECK(RectEq(p.panelRect(), 0, 40, 133, 540));

    // 不可见 → 空矩形；contentRect 仍返回最近一次 layout
    p.setVisible(false);
    CHECK(RectEq(p.panelRect(), 0, 0, 0, 0));
    CHECK(RectEq(p.contentRect(), 0, 40, 400, 540));
    // 再次可见恢复
    p.setVisible(true);
    CHECK(RectEq(p.panelRect(), 0, 40, 133, 540));
}

TEST_CASE("DocListPanel：DPI 200% 面板与条目几何换算", "[doclist]") {
    DocListPanel p;
    p.layout({0, 80, 2000, 1080}, 192);  // 物理宽 2000 → 逻辑 1000 → 上界 333
    p.setVisible(true);
    p.setItems(Items(30));  // 30×56 = 1680 > 1000 → 溢出 680
    CHECK(p.widthLogical() == doclist_metrics::DefaultWidth);
    CHECK(RectEq(p.panelRect(), 0, 80, Scale(200, 192), 1080));
    const DocListGeometry g = p.geometryForTest();
    CHECK(g.itemHeight == 56);
    CHECK(g.overflow);
    CHECK(g.maxScroll == 1680 - 1000);
    CHECK(RectEq(g.scrollbar, 400 - 24, 80, 400, 1080));
    CHECK(RectEq(g.items[0], 0, 80, 400 - 24, 80 + 56));
}

// ---------------------------------------------------------------------------
// DocListPanel：数据语义
// ---------------------------------------------------------------------------

TEST_CASE("DocListPanel：setItems 保留 active / setActiveIndex 滚入可视区 / 越界钳制",
          "[doclist]") {
    DocListPanel p;
    p.layout({0, 40, 200, 260}, 96);  // viewH 220 → 8 行恰满 224 微溢出；20 行溢出 340
    p.setVisible(true);
    p.setItems(Items(20));
    CHECK(p.count() == 20);
    CHECK(p.activeIndex() == -1);  // 空表初始 -1，不自动激活（组装层映射）
    // 激活末条：滚入可视区 → scroll = 20×28 - 220 = 340
    p.setActiveIndex(19);
    CHECK(p.activeIndex() == 19);
    CHECK(p.scrollOffsetForTest() == 20 * 28 - 220);
    // 激活首条：滚回 0
    p.setActiveIndex(0);
    CHECK(p.scrollOffsetForTest() == 0);
    // 越界钳制（正/负）
    p.setActiveIndex(999);
    CHECK(p.activeIndex() == 19);
    p.setActiveIndex(-5);
    CHECK(p.activeIndex() == 0);
    // 同值不重滚：滚走后同值激活不拉回
    p.onMouseWheel(-WHEEL_DELTA);
    const int scrolled = p.scrollOffsetForTest();
    REQUIRE(scrolled > 0);
    p.setActiveIndex(0);
    CHECK(p.activeIndex() == 0);
    CHECK(p.scrollOffsetForTest() == scrolled);  // 同值 no-op（防外部高频事件拉回视图）

    // setItems 整表替换：active 尽量保留（原 0，新 5 项 → 保留 0）
    p.setItems(Items(5));
    CHECK(p.activeIndex() == 0);
    // 原 active 超出新表长 → 落到末位
    p.setActiveIndex(3);
    p.setItems(Items(2));
    CHECK(p.activeIndex() == 1);
    // 空表 → -1；空表激活钳制 -1
    p.setItems({});
    CHECK(p.activeIndex() == -1);
    p.setActiveIndex(0);
    CHECK(p.activeIndex() == -1);
    CHECK(p.count() == 0);
}

TEST_CASE("DocListPanel：setModified/setDisplayName/setHasPath 同值短路", "[doclist]") {
    DocListPanel p;
    p.setItems({DocListItem{L"a.txt", true, true, L"C:\\dir\\a.txt"}});
    // 同值 → false（不置脏重绘）
    CHECK_FALSE(p.setModified(0, true));
    CHECK(p.setModified(0, false));
    CHECK(p.item(0).modified == false);
    CHECK(p.setModified(0, true));
    CHECK_FALSE(p.setDisplayName(0, L"a.txt"));
    CHECK(p.setDisplayName(0, L"b.txt"));
    CHECK(p.item(0).displayName == L"b.txt");
    CHECK_FALSE(p.setHasPath(0, true, L"C:\\dir\\a.txt"));
    CHECK(p.setHasPath(0, false, L""));
    CHECK(p.item(0).hasPath == false);
    CHECK(p.item(0).path.empty());
    CHECK_FALSE(p.setHasPath(0, false, L""));
    CHECK(p.setHasPath(0, true, L"D:\\new.txt"));
    CHECK(p.item(0).path == L"D:\\new.txt");
    // 越界防御：false 不崩溃
    CHECK_FALSE(p.setModified(-1, true));
    CHECK_FALSE(p.setModified(1, true));
    CHECK_FALSE(p.setDisplayName(2, L"x"));
    CHECK_FALSE(p.setHasPath(99, true, L""));
    // item() 越界空项防御
    CHECK(p.item(5).displayName.empty());
    CHECK_FALSE(p.item(-1).modified);
}

// ---------------------------------------------------------------------------
// DocListPanel：命中与点击激活
// ---------------------------------------------------------------------------

TEST_CASE("DocListPanel：hitItem/hitSplitter/hitInteractive", "[doclist]") {
    DocListPanel p;
    p.layout({0, 40, 1000, 460}, 96);  // 宽裕内容区：默认宽 200 不被上界钳制
    p.setVisible(true);
    p.setItems({{L"a.txt", false}, {L"b.txt", true}});
    CHECK(p.hitItem({100, 40}) == 0);
    CHECK(p.hitItem({100, 68}) == 1);
    CHECK(p.hitItem({100, 96}) == -1);  // 条目下方空白
    CHECK(p.hitItem({300, 50}) == -1);  // 面板外
    CHECK(p.hitSplitter({198, 100}));
    CHECK_FALSE(p.hitSplitter({100, 100}));
    // 面板空白/滚动条区也属交互区（不穿透编辑器）
    CHECK(p.hitInteractive({100, 300}));
    CHECK_FALSE(p.hitInteractive({250, 300}));
    // 溢出后滚动条区仍属面板
    p.setItems(Items(20));
    CHECK(p.hitInteractive({195, 100}));
    // 不可见 → 全 false
    p.setVisible(false);
    CHECK_FALSE(p.hitSplitter({198, 100}));
    CHECK_FALSE(p.hitInteractive({100, 300}));
}

TEST_CASE("DocListPanel：点击条目激活并回调（同项点击也上抛，对齐 Tab）", "[doclist]") {
    DocListPanel p;
    p.layout({0, 40, 200, 460}, 96);
    p.setVisible(true);
    p.setItems({{L"a", false}, {L"b", false}, {L"c", false}});
    int activated = -1;
    int calls = 0;
    p.onDocActivated = [&](int index) {
        activated = index;
        ++calls;
    };
    p.onLButtonDown({100, 40 + 28 * 2 + 5});  // 第 3 条中部
    CHECK(p.activeIndex() == 2);
    CHECK(activated == 2);
    CHECK(calls == 1);
    // 空白点击：不激活不回调
    p.onLButtonDown({100, 300});
    CHECK(calls == 1);
    // 同项再次点击：activeIndex 同值；回调仍上抛（点击语义，与 Tab 一致）
    p.onLButtonDown({100, 40 + 28 * 2 + 5});
    CHECK(p.activeIndex() == 2);
    CHECK(calls == 2);
    // 点击后 up：清按下态（no-op 安全）
    p.onLButtonUp({100, 40 + 28 * 2 + 5});
    CHECK(p.activeIndex() == 2);
    CHECK(calls == 2);
}

TEST_CASE("DocListPanel：悬停与 onMouseLeave 清 hover", "[doclist]") {
    DocListPanel p;
    p.layout({0, 40, 200, 460}, 96);
    p.setVisible(true);
    p.setItems(Items(5));
    p.onMouseMove({100, 40});
    CHECK(p.hoverIndexForTest() == 0);
    p.onMouseMove({100, 68});
    CHECK(p.hoverIndexForTest() == 1);
    p.onMouseMove({100, 300});
    CHECK(p.hoverIndexForTest() == -1);  // 空白
    p.onMouseMove({100, 40});
    CHECK(p.hoverIndexForTest() == 0);
    p.onMouseLeave();
    CHECK(p.hoverIndexForTest() == -1);
    // 不可见时 move 不悬停
    p.setVisible(false);
    p.onMouseMove({100, 40});
    CHECK(p.hoverIndexForTest() == -1);
}

// ---------------------------------------------------------------------------
// DocListPanel：滚轮
// ---------------------------------------------------------------------------

TEST_CASE("DocListPanel：滚轮消费语义（无条目/无溢出不消费，步长 3 行，边界钳制）",
          "[doclist]") {
    DocListPanel p;
    p.layout({0, 40, 200, 260}, 96);  // viewH 220
    p.setVisible(true);
    CHECK_FALSE(p.onMouseWheel(-WHEEL_DELTA));  // 无条目
    p.setItems(Items(7));  // 196 ≤ 220：无溢出
    CHECK_FALSE(p.onMouseWheel(-WHEEL_DELTA));
    CHECK(p.scrollOffsetForTest() == 0);
    p.setItems(Items(10));  // 280 > 220 → maxScroll 60
    // 向下（负 delta）：+84 → 钳 60
    CHECK(p.onMouseWheel(-WHEEL_DELTA));
    CHECK(p.scrollOffsetForTest() == 60);
    // 边界继续向下：钳制不变但仍消费（防穿透）
    CHECK(p.onMouseWheel(-WHEEL_DELTA));
    CHECK(p.scrollOffsetForTest() == 60);
    // 向上（正 delta）：-84 → 钳 0
    CHECK(p.onMouseWheel(WHEEL_DELTA));
    CHECK(p.scrollOffsetForTest() == 0);
    CHECK(p.onMouseWheel(WHEEL_DELTA));
    CHECK(p.scrollOffsetForTest() == 0);
    // 部分步：半格 delta（细轮）按比例
    p.onMouseWheel(-WHEEL_DELTA / 2);  // +42
    CHECK(p.scrollOffsetForTest() == 42);
    // 不可见 → 不消费
    p.setVisible(false);
    CHECK_FALSE(p.onMouseWheel(-WHEEL_DELTA));
    // 滚动后命中按平移几何
    p.setVisible(true);
    p.onMouseWheel(-WHEEL_DELTA);  // 42+84 → 60
    CHECK(p.hitItem({100, 30}) == -1);  // 面板上外（item0/1 已滚出的半出区不可达）
    CHECK(p.hitItem({100, 40}) == 2);   // item2 平移到顶（top = 2×28−60+40 = 36）
    CHECK(p.hitItem({100, 250}) == 9);  // 末条完整可见（[232,260)）
}

// ---------------------------------------------------------------------------
// DocListPanel：滚动条交互（箭头步进/轨道翻页/滑块拖拽）
// ---------------------------------------------------------------------------

TEST_CASE("DocListPanel：滚动条箭头步进与边界禁用语义", "[doclist]") {
    DocListPanel p;
    p.layout({0, 40, 200, 260}, 96);
    p.setVisible(true);
    p.setItems(Items(10));  // maxScroll 60
    const DocListGeometry g = p.geometryForTest();
    // 下箭头点击：+1 行（28）
    const POINT downBtn{(g.sbDown.left + g.sbDown.right) / 2,
                        (g.sbDown.top + g.sbDown.bottom) / 2};
    p.onLButtonDown(downBtn);
    CHECK(p.scrollOffsetForTest() == 28);
    p.onLButtonUp(downBtn);
    // 上箭头点击：-1 行
    const POINT upBtn{(g.sbUp.left + g.sbUp.right) / 2, (g.sbUp.top + g.sbUp.bottom) / 2};
    p.onLButtonDown(upBtn);
    CHECK(p.scrollOffsetForTest() == 0);
    p.onLButtonUp(upBtn);
    // 边界处再点上箭头：钳制 0（no-op 安全）
    p.onLButtonDown(upBtn);
    CHECK(p.scrollOffsetForTest() == 0);
    p.onLButtonUp(upBtn);
}

TEST_CASE("DocListPanel：滚动条轨道点击翻页与滑块拖拽", "[doclist]") {
    DocListPanel p;
    p.layout({0, 40, 200, 260}, 96);
    p.setVisible(true);
    p.setItems(Items(10));  // maxScroll 60；viewH 220
    // 滑块下方轨道点击：向下翻页 220 → 钳 60
    DocListGeometry g = p.geometryForTest();
    POINT trackBelow{g.sbTrack.left + 2, g.sbTrack.bottom - 3};
    REQUIRE(trackBelow.y >= g.sbThumb.bottom);  // 确在滑块下方
    p.onLButtonDown(trackBelow);
    CHECK(p.scrollOffsetForTest() == 60);
    p.onLButtonUp(trackBelow);
    // 滑块上方轨道点击：向上翻页 → 0
    g = p.geometryForTest();  // scroll 60 → 滑块已下移
    POINT trackAbove{g.sbTrack.left + 2, g.sbTrack.top + 3};
    REQUIRE(trackAbove.y < g.sbThumb.top);
    p.onLButtonDown(trackAbove);
    CHECK(p.scrollOffsetForTest() == 0);
    p.onLButtonUp(trackAbove);

    // 滑块拖拽：从中央（scroll 0）拖满 range → 滚到 maxScroll
    g = p.geometryForTest();
    const POINT thumbC{(g.sbThumb.left + g.sbThumb.right) / 2,
                       (g.sbThumb.top + g.sbThumb.bottom) / 2};
    const int trackH = g.sbTrack.bottom - g.sbTrack.top;
    const int thumbH = g.sbThumb.bottom - g.sbThumb.top;
    const int range = trackH - thumbH;
    REQUIRE(range > 0);
    p.onLButtonDown(thumbC);  // Thumb 命中：起拖（不即刻滚动）
    CHECK(p.scrollOffsetForTest() == 0);
    // 拖动 range 物理像素 → scroll = 0 + range×maxScroll/range = maxScroll
    p.onMouseMove({thumbC.x, thumbC.y + range});
    CHECK(p.scrollOffsetForTest() == g.maxScroll);
    p.onLButtonUp({thumbC.x, thumbC.y + range});
    CHECK(p.scrollOffsetForTest() == g.maxScroll);
    // 非拖拽时捕获变更：no-op 安全（不崩溃、滚动保留）
    p.onCaptureChanged();
    CHECK(p.scrollOffsetForTest() == g.maxScroll);
}

// ---------------------------------------------------------------------------
// DocListPanel：分隔条拖拽状态机（宽度语义）
// ---------------------------------------------------------------------------

TEST_CASE("DocListPanel：分隔条拖拽起拖/实时回调/上下界钳制/提交", "[doclist]") {
    DocListPanel p;
    p.layout({0, 40, 1000, 540}, 96);  // 上界 333
    p.setVisible(true);
    std::vector<int> widths;
    p.onWidthChanged = [&](int w) { widths.push_back(w); };
    // 未拖拽：Esc 不消费
    CHECK_FALSE(p.onKeyDown(VK_ESCAPE));
    // down 于右缘热区 → 起拖
    p.onLButtonDown({198, 100});
    CHECK(p.draggingSplitter());
    // +100 物理 → 逻辑 300：实时生效 + 每变化一次回调
    p.onMouseMove({298, 100});
    CHECK(p.widthLogical() == 300);
    p.onMouseMove({300, 100});
    CHECK(p.widthLogical() == 302);
    p.onMouseMove({298, 100});
    CHECK(p.widthLogical() == 300);
    REQUIRE(widths == std::vector<int>{300, 302, 300});
    // 越上界：dx 500 → 700 → 钳 333
    p.onMouseMove({698, 100});
    CHECK(p.widthLogical() == 333);
    // 越下界：dx -400 → -200 → 钳 120
    p.onMouseMove({-202, 100});
    CHECK(p.widthLogical() == 120);
    // 松手提交：最终 onWidthChanged（120）且收口
    p.onLButtonUp({-202, 100});
    CHECK_FALSE(p.draggingSplitter());
    REQUIRE(widths.size() == 6);  // 4 次实时变化 + 提交
    CHECK(widths.back() == 120);
    CHECK(p.widthLogical() == 120);
    // up 之后已收口：再 move 不进入拖拽
    p.onMouseMove({50, 100});
    CHECK_FALSE(p.draggingSplitter());
    CHECK(widths.size() == 6);
}

TEST_CASE("DocListPanel：拖拽 Esc 回退原宽（经回调）且不消费非拖拽 Esc", "[doclist]") {
    DocListPanel p;
    p.layout({0, 40, 1000, 540}, 96);
    p.setVisible(true);
    std::vector<int> widths;
    p.onWidthChanged = [&](int w) { widths.push_back(w); };
    p.onLButtonDown({198, 100});
    p.onMouseMove({298, 100});  // 200 → 300
    CHECK(p.widthLogical() == 300);
    // 拖拽中 onMouseLeave 不取消（捕获期间光标短暂离窗是正常拖拽）
    p.onMouseLeave();
    CHECK(p.draggingSplitter());
    CHECK(p.widthLogical() == 300);
    // Esc：取消回原宽（200），经 onWidthChanged 通知
    CHECK(p.onKeyDown(VK_ESCAPE));
    CHECK_FALSE(p.draggingSplitter());
    CHECK(p.widthLogical() == 200);
    REQUIRE(widths.size() == 2);
    CHECK(widths.back() == 200);
    // 非拖拽 Esc：不消费
    CHECK_FALSE(p.onKeyDown(VK_ESCAPE));
    // 未移动就 Esc：宽度无变化 → 无回退回调
    widths.clear();
    p.onLButtonDown({198, 100});
    CHECK(p.onKeyDown(VK_ESCAPE));
    CHECK(p.widthLogical() == 200);
    CHECK(widths.empty());
}

TEST_CASE("DocListPanel：拖拽捕获丢失/失焦统一取消收口（回原宽经回调）", "[doclist]") {
    DocListPanel p;
    p.layout({0, 40, 1000, 540}, 96);
    p.setVisible(true);
    p.setItems(Items(3));
    std::vector<int> widths;
    p.onWidthChanged = [&](int w) { widths.push_back(w); };
    // —— 捕获丢失 ——
    p.onLButtonDown({198, 100});
    p.onMouseMove({298, 100});  // 200 → 300
    CHECK(p.widthLogical() == 300);
    p.onCaptureChanged();
    CHECK_FALSE(p.draggingSplitter());
    CHECK(p.widthLogical() == 200);  // 回原宽
    REQUIRE(widths.size() == 2);
    CHECK(widths.back() == 200);
    // 非拖拽时的捕获变更：no-op 安全
    p.onCaptureChanged();
    CHECK(p.widthLogical() == 200);
    CHECK(widths.size() == 2);
    // —— 失焦 ——
    p.onMouseMove({100, 60});  // 先制造悬停
    CHECK(p.hoverIndexForTest() == 0);
    p.onLButtonDown({198, 100});
    p.onMouseMove({250, 100});  // 200 → 252
    p.onWindowKillFocus();
    CHECK_FALSE(p.draggingSplitter());
    CHECK(p.widthLogical() == 200);
    CHECK(widths.back() == 200);
    CHECK(p.hoverIndexForTest() == -1);  // 失焦清 hover/tooltip 态
}

TEST_CASE("DocListPanel：拖拽期间数据变化不影响宽度会话", "[doclist]") {
    // setItems 到达不终止分隔条拖拽（宽度语义与条目数据正交）
    DocListPanel p;
    p.layout({0, 40, 1000, 540}, 96);
    p.setVisible(true);
    p.setItems(Items(3));
    std::vector<int> widths;
    p.onWidthChanged = [&](int w) { widths.push_back(w); };
    p.onLButtonDown({198, 100});
    p.onMouseMove({298, 100});  // 300
    p.setItems(Items(8));
    CHECK(p.draggingSplitter());
    p.onMouseMove({330, 100});  // dx 132 → 332（低于上界 333）
    CHECK(p.widthLogical() == 332);
    p.onLButtonUp({330, 100});
    CHECK_FALSE(p.draggingSplitter());
    CHECK(p.widthLogical() == 332);
    REQUIRE(widths.size() == 3);
    CHECK(widths.back() == 332);
}

TEST_CASE("DocListPanel：右键条目不激活（TC-007 组件语义；无宿主菜单 no-op）",
          "[doclist]") {
    // 右键语义对齐 Tab：命中条目 → 右键菜单（经 MenuHost 弹出）而非激活——
    // active/激活回调均不动；无宿主夹具（未 attach）下 openContextMenu 经
    // hwnd_/menuHost_ 空判定安全返回。命令模型层置灰/动态数量断言见上方
    // BuildTabContextMenu 用例；命令执行→onDocContextCommand 端到端路由
    // 依赖真实弹窗（verify-tabs-selftest 同命令集已覆盖 Tab 侧路径）。
    DocListPanel p;
    p.layout({0, 40, 200, 460}, 96);
    p.setVisible(true);
    p.setItems({{L"a.txt", false}, {L"b.txt", true}, {L"c.txt", false}});
    p.setActiveIndex(0);
    int activated = -1;
    int calls = 0;
    p.onDocActivated = [&](int index) { activated = index; ++calls; };
    // 右键第 3 条：不激活被右键的条目（active 保持 0）、不上抛激活回调
    p.onRButtonDown({100, 40 + 28 * 2 + 5});
    CHECK(p.activeIndex() == 0);
    CHECK(calls == 0);
    CHECK(activated == -1);
    // 右键空白：no-op
    p.onRButtonDown({100, 300});
    CHECK(p.activeIndex() == 0);
    CHECK(calls == 0);
    // 分隔条拖拽期间右键：no-op（不弹菜单、不打断拖拽——守卫统一收口）
    // 注：内容区宽 200 → 上界 200/3 < 下限 120 → 面板实际宽钳到 120，热区
    // 从 panelRect() 实际右缘推导（不假设默认宽 200 生效）
    const int panelRight = p.panelRect().right;
    REQUIRE(panelRight > 0);
    p.onLButtonDown({panelRight - 2, 100});
    CHECK(p.draggingSplitter());
    p.onRButtonDown({100, 60});
    CHECK(p.draggingSplitter());
    CHECK(p.activeIndex() == 0);
    CHECK(calls == 0);
    p.onLButtonUp({panelRight - 2, 100});  // 提交（宽度无变化 → 无回调）
    CHECK_FALSE(p.draggingSplitter());
    // 不可见右键：no-op
    p.setVisible(false);
    p.onRButtonDown({100, 60});
    CHECK(p.activeIndex() == 0);
    CHECK(calls == 0);
}
