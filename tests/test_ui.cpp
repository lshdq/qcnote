// M1 UI 基建单测：主题 token 数学（Scale/BlendOver）、标题栏三段式布局、
// WM_NCHITTEST 核心（按钮/8 向缩放边缘命中，含 DPI 200% 换算正确性——
// 把「静态审查级自查」升级为可回归的代码级验证）。
#include <catch2/catch_test_macros.hpp>

#include "ui/dialog.h"
#include "ui/searchdialog.h"
#include "ui/statusbar.h"
#include "ui/theme.h"
#include "ui/titlebar.h"
#include "ui/toolbar.h"

using namespace ui;

namespace {
// RECT 相等辅助（Win32 RECT 无 operator==）
bool RectEq(RECT const& r, LONG left, LONG top, LONG right, LONG bottom) {
    return r.left == left && r.top == top && r.right == right && r.bottom == bottom;
}
}  // namespace

// ---------------------------------------------------------------------------
// Scale：逻辑像素 → 物理像素
// ---------------------------------------------------------------------------

TEST_CASE("Scale 按窗口 DPI 换算逻辑像素", "[ui]") {
    CHECK(Scale(44, 96) == 44);      // 100%：1:1
    CHECK(Scale(44, 192) == 88);     // 200%：按钮宽 44→88
    CHECK(Scale(40, 192) == 80);     // 200%：按钮高/标题栏高 40→80
    CHECK(Scale(800, 192) == 1600);  // 200%：最小宽度
    CHECK(Scale(500, 192) == 1000);  // 200%：最小高度
    CHECK(Scale(5, 192) == 10);      // 200%：边缘命中宽 5→10
    CHECK(Scale(12, 192) == 24);     // 200%：四角命中宽 12→24
    CHECK(Scale(500, 150) == 781);   // MulDiv 就近舍入：781.25 → 781
}

// ---------------------------------------------------------------------------
// BlendOver：半透明叠加 token 预合成
// ---------------------------------------------------------------------------

TEST_CASE("BlendOver 半透明叠加换算", "[ui]") {
    // hover #16000000（alpha=22）叠浅色标题栏底 #dde2ec：
    //   r = 221*233/255 = 201.93 → 202 (0xCA)
    //   g = 226*233/255 = 206.50 → 207 (0xCF)
    //   b = 236*233/255 = 215.64 → 216 (0xD8)
    CHECK(BlendOver(0xFFDDE2ECu, 0x16000000u) == 0xFFCACFD8u);
    // pressed #26000000（alpha=38）：
    //   r = 221*217/255 = 188.07 → 188 (0xBC)
    //   g = 226*217/255 = 192.32 → 192 (0xC0)
    //   b = 236*217/255 = 200.83 → 201 (0xC9)
    CHECK(BlendOver(0xFFDDE2ECu, 0x26000000u) == 0xFFBCC0C9u);
    // 不透明叠加：直接返回（关闭按钮红底路径）
    CHECK(BlendOver(0xFFDDE2ECu, 0xFFC42B1Cu) == 0xFFC42B1Cu);
    // 全透明：返回底色（补 alpha）
    CHECK(BlendOver(0xFFDDE2ECu, 0x00C42B1Cu) == 0xFFDDE2ECu);
}

// ---------------------------------------------------------------------------
// 深色色板（E5/M6）：值逐条照抄 AppTheme.qml 深色分支 + 主题切换分发
// ---------------------------------------------------------------------------

TEST_CASE("DarkPalette 值照抄 AppTheme.qml 深色分支", "[ui][theme]") {
    const ThemePalette& d = DarkPalette();
    // 表面（DEC-102：状态栏不随标题栏联动，两套独立 token）
    CHECK(d.titleBarBackground == 0xFF171C24u);
    CHECK(d.toolbarBackground == 0xFF232833u);
    CHECK(d.contentBackground == 0xFF262B34u);
    CHECK(d.statusBarBackground == 0xFF1D222Bu);
    CHECK(d.activeTab == d.toolbarBackground);       // 连通式约束
    // 文本
    CHECK(d.textPrimary == 0xFFE8EAF0u);
    CHECK(d.textSecondary == 0xFF9AA3B5u);
    CHECK(d.textDisabled == 0xFF5D6472u);
    // 语义
    CHECK(d.accent == 0xFF6F9BFFu);
    CHECK(d.focus == d.accent);
    CHECK(d.error == 0xFFFF7A68u);
    CHECK(d.hover == 0x22FFFFFFu);
    CHECK(d.pressed == 0x33FFFFFFu);
    CHECK(d.closeHover == 0xFFC42B1Cu);              // 红，两主题同值
    CHECK(d.closePressed == 0xFFA12316u);
    CHECK(d.closeGlyphForeground == 0xFFFFFFFFu);
    CHECK(d.onAccentForeground == 0xFFFFFFFFu);
    // 边框
    CHECK(d.border == 0xFF3A4150u);
    CHECK(d.windowBorder == 0xFF4A5364u);
    CHECK(d.windowBorderInactive == 0xFF333947u);
    CHECK(d.unsavedMark == 0xFFFFB454u);
    // 选区/搜索
    CHECK(d.multiCursor == d.accent);
    CHECK(d.selection == 0x336F9BFFu);
    CHECK(d.multiSelection == 0x336F9BFFu);
    CHECK(d.searchMatch == 0x4DFFB454u);
    CHECK(d.searchMatchBorder == 0xFFFFB454u);
    // 两套色板确实不同（防「深色=浅色占位」回归——M1 曾以浅色占位）
    CHECK(DarkPalette().titleBarBackground != LightPalette().titleBarBackground);
    CHECK(DarkPalette().contentBackground != LightPalette().contentBackground);
    CHECK(DarkPalette().textPrimary != LightPalette().textPrimary);
}

TEST_CASE("SetTheme 切换 CurrentPalette 分发与深色叠加换算", "[ui][theme]") {
    REQUIRE_FALSE(IsDarkTheme());  // 默认浅色（进程初始）
    CHECK(&CurrentPalette() == &LightPalette());

    SetTheme(true);
    CHECK(IsDarkTheme());
    CHECK(&CurrentPalette() == &DarkPalette());
    // 深色 hover #22ffffff（alpha=34，a≈0.1333）叠标题栏底 #171c24：
    //   r = 23*0.8667 + 255*0.1333 = 53.93 → 54 (0x36)
    //   g = 28*0.8667 + 255*0.1333 = 58.27 → 58 (0x3A)
    //   b = 36*0.8667 + 255*0.1333 = 65.20 → 65 (0x41)
    CHECK(BlendOver(0xFF171C24u, 0x22FFFFFFu) == 0xFF363A41u);
    // 深色选区 #336f9bff（alpha=51，a=0.2）叠内容底 #262b34：
    //   r = 38*0.8 + 111*0.2 = 52.6 → 53 (0x35)
    //   g = 43*0.8 + 155*0.2 = 65.4 → 65 (0x41)
    //   b = 52*0.8 + 255*0.2 = 92.6 → 93 (0x5D)
    CHECK(BlendOver(0xFF262B34u, 0x336F9BFFu) == 0xFF35415Du);

    SetTheme(false);  // 恢复浅色（避免影响同进程其他用例）
    CHECK_FALSE(IsDarkTheme());
    CHECK(&CurrentPalette() == &LightPalette());
}

// ---------------------------------------------------------------------------
// 选区实底 token（Win32界面体验修复 F3 / TC-003）
// ---------------------------------------------------------------------------

TEST_CASE("selectionBackground 选区实底：两主题取值与失焦淡一档预合成", "[ui][theme]") {
    const ThemePalette& l = LightPalette();
    const ThemePalette& d = DarkPalette();
    // 用户确认值（VS Code 同款）：浅 #ADD6FF、深 #264F78，实底不透明
    CHECK(l.selectionBackground == 0xFFADD6FFu);
    CHECK(d.selectionBackground == 0xFF264F78u);
    CHECK((l.selectionBackground >> 24) == 0xFFu);
    CHECK((d.selectionBackground >> 24) == 0xFFu);
    // 与旧 16% 透明 token（选中后看不见文字的根因）明显区分，防回退
    CHECK(l.selectionBackground != l.selection);
    CHECK(d.selectionBackground != d.selection);

    // 失焦非活动选区：内容底上按 overlay alpha=0x55 预合成「淡一档」——
    // 与 EditorHost::applyThemeColors 喂 SCI_SETELEMENTCOLOUR 的色值同公式。
    // 浅：#ffffff 底叠 #ADD6FF@1/3 → R=228 G=241 B=255；深：#262b34 底叠
    // #264F78@1/3 → R=38 G=55 B=75（BlendOver 为 lround 就近取整）
    const uint32_t lInactive = BlendOver(l.contentBackground,
        0x55000000u | (l.selectionBackground & 0xFFFFFFu));
    const uint32_t dInactive = BlendOver(d.contentBackground,
        0x55000000u | (d.selectionBackground & 0xFFFFFFu));
    CHECK(lInactive == 0xFFE4F1FFu);
    CHECK(dInactive == 0xFF26374Bu);
}

// ---------------------------------------------------------------------------
// 标题栏三段式布局
// ---------------------------------------------------------------------------

TEST_CASE("标题栏布局 100%", "[ui]") {
    const TitleBarLayout L = ComputeTitleBarLayout(1000, 96);
    CHECK(L.captionHeight == 40);
    CHECK(RectEq(L.caption, 0, 0, 1000, 40));
    CHECK(RectEq(L.menuButton, 0, 0, 44, 40));            // 左：菜单按钮
    CHECK(RectEq(L.closeButton, 956, 0, 1000, 40));       // 右起第 1：关闭
    CHECK(RectEq(L.maxRestoreButton, 912, 0, 956, 40));   // 右起第 2：最大化/还原
    CHECK(RectEq(L.minimizeButton, 868, 0, 912, 40));     // 右起第 3：最小化
    CHECK(RectEq(L.tabArea, 48, 0, 860, 40));  // Tab 占位：44+4 左、1000-132-8 右
}

TEST_CASE("标题栏布局 200%（DPI 换算正确性）", "[ui]") {
    const TitleBarLayout L = ComputeTitleBarLayout(2000, 192);
    CHECK(L.captionHeight == 80);
    CHECK(RectEq(L.menuButton, 0, 0, 88, 80));
    CHECK(RectEq(L.closeButton, 1912, 0, 2000, 80));
    CHECK(RectEq(L.maxRestoreButton, 1824, 0, 1912, 80));
    CHECK(RectEq(L.minimizeButton, 1736, 0, 1824, 80));
    CHECK(RectEq(L.tabArea, 96, 0, 1720, 80));  // 88+8 左、2000-264-16 右
}

TEST_CASE("标题栏布局退化窗口宽不产生负尺寸", "[ui]") {
    const TitleBarLayout L = ComputeTitleBarLayout(10, 96);
    CHECK(L.tabArea.left <= L.tabArea.right);  // 收缩为空矩形（左右相等），无负宽
    CHECK(L.closeButton.right == 10);
}

// ---------------------------------------------------------------------------
// WM_NCHITTEST 核心：按钮与 8 向缩放命中
// ---------------------------------------------------------------------------

TEST_CASE("NCHITTEST 命中 100%", "[ui]") {
    const SIZE size{1000, 640};
    // 8 向边缘（edge=5, corner=12）
    CHECK(ComputeCaptionHitTest({2, 30}, size, 96, false) == HTLEFT);
    CHECK(ComputeCaptionHitTest({997, 30}, size, 96, false) == HTRIGHT);
    CHECK(ComputeCaptionHitTest({500, 2}, size, 96, false) == HTTOP);
    CHECK(ComputeCaptionHitTest({500, 637}, size, 96, false) == HTBOTTOM);
    CHECK(ComputeCaptionHitTest({2, 2}, size, 96, false) == HTTOPLEFT);
    CHECK(ComputeCaptionHitTest({997, 2}, size, 96, false) == HTTOPRIGHT);
    CHECK(ComputeCaptionHitTest({2, 637}, size, 96, false) == HTBOTTOMLEFT);
    CHECK(ComputeCaptionHitTest({997, 637}, size, 96, false) == HTBOTTOMRIGHT);
    // 标题栏空白区 → HTCAPTION（拖动/双击最大化由系统接管）
    CHECK(ComputeCaptionHitTest({500, 20}, size, 96, false) == HTCAPTION);
    // Tab 占位区也属 caption（需求：空白区含 Tab 条无 Tab 覆盖处可拖动）
    CHECK(ComputeCaptionHitTest({100, 30}, size, 96, false) == HTCAPTION);
    // 按钮精确命中（不落入 HTCAPTION）
    CHECK(ComputeCaptionHitTest({978, 20}, size, 96, false) == HTCLOSE);
    CHECK(ComputeCaptionHitTest({930, 20}, size, 96, false) == HTMAXBUTTON);
    CHECK(ComputeCaptionHitTest({885, 20}, size, 96, false) == HTMINBUTTON);
    CHECK(ComputeCaptionHitTest({20, 20}, size, 96, false) == HTBORDER);  // 菜单按钮
    // 四角命中优先于按钮（左上角盖住菜单按钮一角，对齐系统惯例）
    CHECK(ComputeCaptionHitTest({10, 10}, size, 96, false) == HTTOPLEFT);
    CHECK(ComputeCaptionHitTest({995, 10}, size, 96, false) == HTTOPRIGHT);  // 盖关闭按钮右上角
    // 标题栏以下 → 客户区
    CHECK(ComputeCaptionHitTest({500, 60}, size, 96, false) == HTCLIENT);
    // 最大化：无 resize 命中，顶部归 HTCAPTION（支持最大化态拖拽还原）；
    // 按钮命中不受影响（菜单/控制按钮在最大化态仍可点击）
    CHECK(ComputeCaptionHitTest({500, 30}, size, 96, true) == HTCAPTION);
    CHECK(ComputeCaptionHitTest({997, 637}, size, 96, true) == HTCLIENT);
    CHECK(ComputeCaptionHitTest({978, 20}, size, 96, true) == HTCLOSE);  // 按钮命中不受影响
}

TEST_CASE("NCHITTEST 命中 200%（DPI 换算正确性）", "[ui]") {
    const SIZE size{2000, 1280};
    // edge=10 / corner=24：命中宽随 DPI 翻倍
    CHECK(ComputeCaptionHitTest({5, 60}, size, 192, false) == HTLEFT);     // 5<10
    CHECK(ComputeCaptionHitTest({1995, 60}, size, 192, false) == HTRIGHT);
    CHECK(ComputeCaptionHitTest({5, 5}, size, 192, false) == HTTOPLEFT);
    CHECK(ComputeCaptionHitTest({1980, 1275}, size, 192, false) == HTBOTTOMRIGHT);
    // 标题栏高 80：y=79 属 caption、y=80 属客户区
    CHECK(ComputeCaptionHitTest({500, 79}, size, 192, false) == HTCAPTION);
    CHECK(ComputeCaptionHitTest({500, 80}, size, 192, false) == HTCLIENT);
    // 按钮按 88px 宽换算
    CHECK(ComputeCaptionHitTest({1960, 40}, size, 192, false) == HTCLOSE);
    CHECK(ComputeCaptionHitTest({1860, 40}, size, 192, false) == HTMAXBUTTON);
    CHECK(ComputeCaptionHitTest({1780, 40}, size, 192, false) == HTMINBUTTON);
    CHECK(ComputeCaptionHitTest({40, 40}, size, 192, false) == HTBORDER);  // menu 0..88
    // 最大化：顶部归 caption（拖拽还原入口）；菜单按钮命中不受影响
    CHECK(ComputeCaptionHitTest({500, 40}, size, 192, true) == HTCAPTION);
    CHECK(ComputeCaptionHitTest({5, 60}, size, 192, true) == HTBORDER);
}

// ---------------------------------------------------------------------------
// M2-B：状态栏字段文本（纯函数）
// ---------------------------------------------------------------------------

TEST_CASE("状态栏字段文本（有文档）", "[ui][statusbar]") {
    StatusData d;
    d.hasDoc = true;
    d.cursorLine = 3;
    d.cursorColumn = 7;
    d.selectedChars = 0;
    d.totalChars = 123;
    d.encoding = L"UTF-8";
    d.lineEnding = L"LF";
    CHECK(StatusLineColText(d) == L"行 3，列 7");
    CHECK(StatusSelectionText(d).empty());   // 无选区隐藏
    CHECK(StatusCursorCountText(d).empty()); // 单光标隐藏
    CHECK(StatusTotalText(d) == L"共 123 字符");
    CHECK(StatusEncodingText(d) == L"UTF-8");
    CHECK(StatusLineEndingText(d) == L"LF");
}

TEST_CASE("状态栏字段文本（无文档/边界）", "[ui][statusbar]") {
    StatusData d;  // hasDoc=false
    CHECK(StatusLineColText(d) == L"行 —，列 —");
    CHECK(StatusSelectionText(d).empty());
    CHECK(StatusTotalText(d).empty());
    CHECK(StatusEncodingText(d).empty());
    CHECK(StatusLineEndingText(d).empty());
    // 有选区/多光标/带 BOM/CRLF
    StatusData e;
    e.hasDoc = true;
    e.cursorLine = 1;
    e.cursorColumn = 1;
    e.selectedChars = 42;
    e.cursorCount = 3;
    e.totalChars = 100;
    e.encoding = L"UTF-8 BOM";
    e.lineEnding = L"CRLF";
    CHECK(StatusSelectionText(e) == L"已选 42 字符");
    CHECK(StatusCursorCountText(e) == L"光标 ×3");
    CHECK(StatusEncodingText(e) == L"UTF-8 BOM");
    CHECK(StatusLineEndingText(e) == L"CRLF");
    // 多光标但无文档：隐藏
    StatusData f;
    f.cursorCount = 2;
    CHECK(StatusCursorCountText(f).empty());
}

// ---------------------------------------------------------------------------
// TC-006（列选修复）：StatusData 全值等值比较——StatusBar::update 等值跳过
// 的判定谓词。defaulted operator== 必须覆盖全部字段：拖拽中每次 mouse move
// 触发一次 update，任一字段漏比较会导致「数据已变但不重绘」（显示陈旧）
// 或「数据未变仍失效」（闪烁修复失效）。
// 说明：update() 跳过行为本身（不置脏/不失效重绘）依赖私有 layoutDirty_ 与
// 宿主窗口 InvalidateRect，无公开观测钩子，纯单测不可直接断言；此处锁定
// 谓词语义，行为层由 search selftest 状态栏数据断言与 verify-theme 像素
// 采样覆盖。
// ---------------------------------------------------------------------------

TEST_CASE("StatusData 等值比较覆盖全部字段（update 等值跳过谓词）", "[ui][statusbar]") {
    StatusData base;
    base.hasDoc = true;
    base.cursorLine = 3;
    base.cursorColumn = 7;
    base.selectedChars = 42;
    base.totalChars = 123;
    base.cursorCount = 2;
    base.encoding = L"UTF-8";
    base.lineEnding = L"LF";
    base.path = L"C:\\a.txt";
    CHECK(base == base);      // 自反
    StatusData same = base;
    CHECK(base == same);      // 全字段相同 → 相等
    // 逐字段扰动必不等（defaulted == 不得遗漏任何字段）
    StatusData d = base;
    d.hasDoc = false;             CHECK(base != d);
    d = base; d.cursorLine = 4;      CHECK(base != d);
    d = base; d.cursorColumn = 1;    CHECK(base != d);
    d = base; d.selectedChars = 0;   CHECK(base != d);
    d = base; d.totalChars = 999;    CHECK(base != d);
    d = base; d.cursorCount = 3;     CHECK(base != d);
    d = base; d.encoding = L"GBK";   CHECK(base != d);
    d = base; d.lineEnding = L"CRLF";CHECK(base != d);
    d = base; d.path = L"D:\\b.txt"; CHECK(base != d);
    // 默认构造的两实例相等（等值跳过在无文档/初始态同样成立）
    CHECK(StatusData{} == StatusData{});
}

// ---------------------------------------------------------------------------
// R2-A：段级差分失效纯函数（StatusDiffInvalidRects）——用户真机复验闪烁仍在
// 的根因是 update() 任何数据变化都整条失效+整条重绘；改为只失效变化段的
// 旧∪新并集。此处直接构造布局段向量锁定差分语义（无 HWND 依赖）。
// ---------------------------------------------------------------------------

namespace {
// 段向量便捷构造：7 段对齐 SegId，全部相同可见段（差分基准形态）
std::vector<StatusSegment> MakeSegments() {
    std::vector<StatusSegment> segs(static_cast<size_t>(7));
    for (size_t i = 0; i < segs.size(); ++i) {
        segs[i] = {L"seg" + std::to_wstring(i),
                   {static_cast<LONG>(10 + i * 100), 500,
                    static_cast<LONG>(90 + i * 100), 528},
                   true, false};
    }
    return segs;
}
bool ContainsRect(const std::vector<RECT>& rects, LONG l, LONG t, LONG r, LONG b) {
    for (const RECT& x : rects) {
        if (RectEq(x, l, t, r, b)) return true;
    }
    return false;
}
}  // namespace

TEST_CASE("StatusDiffInvalidRects：仅列数文本变化 → 恰该段一个矩形", "[ui][statusbar]") {
    std::vector<StatusSegment> oldSegs = MakeSegments();
    std::vector<StatusSegment> newSegs = oldSegs;
    // 纯文本变化（数字 1→12 恰好等宽）：bounds 不变，仅 LineCol(下标 0) 文本变
    oldSegs[0].text = L"行 1，列 1";
    newSegs[0].text = L"行 1，列 12";
    const std::vector<RECT> rects = StatusDiffInvalidRects(oldSegs, newSegs);
    REQUIRE(rects.size() == 1);
    CHECK(RectEq(rects[0], 10, 500, 90, 528));  // 旧∪新（同 bounds）= 该段本身
}

TEST_CASE("StatusDiffInvalidRects：宽度变化位移后续段 → 位移段矩形也在集合",
          "[ui][statusbar]") {
    std::vector<StatusSegment> oldSegs = MakeSegments();
    std::vector<StatusSegment> newSegs = oldSegs;
    // 列数变长 20px：LineCol 段变宽，其后 Total 段（下标 3）整体右移 20px
    oldSegs[0].text = L"行 1，列 1";
    oldSegs[0].bounds = {10, 500, 90, 528};
    newSegs[0].text = L"行 1，列 12";
    newSegs[0].bounds = {10, 500, 110, 528};
    oldSegs[3].bounds = {106, 500, 186, 528};  // Total 旧位
    newSegs[3].bounds = {126, 500, 206, 528};  // Total 新位（右移 20）
    const std::vector<RECT> rects = StatusDiffInvalidRects(oldSegs, newSegs);
    REQUIRE(rects.size() == 2);
    CHECK(ContainsRect(rects, 10, 500, 110, 528));   // LineCol 旧[10,90]∪新[10,110]
    CHECK(ContainsRect(rects, 106, 500, 206, 528));  // Total 旧[106,186]∪新[126,206]
}

TEST_CASE("StatusDiffInvalidRects：数据完全相同 → 空集合（零失效）", "[ui][statusbar]") {
    const std::vector<StatusSegment> segs = MakeSegments();
    CHECK(StatusDiffInvalidRects(segs, segs).empty());
    // 与 update() 等值跳过组合：同数据二次 update 先被 !layoutDirty_&&data_==data
    // 拦截；即便到达差分（布局脏等场景）也是空集 → 不产生任何失效
    CHECK(StatusDiffInvalidRects({}, {}).empty());  // 空向量（未附着/首次前）
    // 双侧均不可见、仅 text 变化 → 不产生任何矩形（不可见段无需重绘）
    std::vector<StatusSegment> oHidden = MakeSegments(), nHidden = oHidden;
    oHidden[2] = {L"old", {}, false, false};
    nHidden[2] = {L"new", {}, false, false};
    CHECK(StatusDiffInvalidRects(oHidden, nHidden).empty());
}

TEST_CASE("StatusDiffInvalidRects：旧不可见→新可见（选区从无到有）→ 新 bounds 在集合",
          "[ui][statusbar]") {
    std::vector<StatusSegment> oldSegs = MakeSegments();
    std::vector<StatusSegment> newSegs = oldSegs;
    oldSegs[1] = {};  // Selection 段旧不可见（默认构造）
    newSegs[1] = {L"已选 3 字符", {106, 500, 190, 528}, true, false};
    const std::vector<RECT> rects = StatusDiffInvalidRects(oldSegs, newSegs);
    REQUIRE(rects.size() == 1);
    CHECK(RectEq(rects[0], 106, 500, 190, 528));  // 旧无矩形 → 仅新 bounds
    // 反向（选区清除）：新不可见 → 仅旧 bounds（重画清掉旧字）
    std::vector<RECT> back = StatusDiffInvalidRects(newSegs, oldSegs);
    REQUIRE(back.size() == 1);
    CHECK(RectEq(back[0], 106, 500, 190, 528));
}

TEST_CASE("StatusDiffInvalidRects：旧段全空（首次 update）→ 全部新可见段各自失效",
          "[ui][statusbar]") {
    const std::vector<StatusSegment> newSegs = MakeSegments();
    const std::vector<RECT> rects = StatusDiffInvalidRects({}, newSegs);
    REQUIRE(rects.size() == newSegs.size());
    for (size_t i = 0; i < newSegs.size(); ++i) {
        CHECK(ContainsRect(rects, newSegs[i].bounds.left, newSegs[i].bounds.top,
                           newSegs[i].bounds.right, newSegs[i].bounds.bottom));
    }
}

TEST_CASE("StatusDiffInvalidRects：长度不齐（新旧段数不同）→ 越界下标按不可见空段",
          "[ui][statusbar]") {
    // 扩张：旧 3 段 vs 新 5 段——下标 3/4 旧侧越界按不可见 → 仅新 bounds
    std::vector<StatusSegment> grown = MakeSegments();
    grown.resize(5);
    std::vector<StatusSegment> shrunk = grown;
    shrunk.resize(3);
    const std::vector<RECT> rects = StatusDiffInvalidRects(shrunk, grown);
    REQUIRE(rects.size() == 2);                       // 前 3 段相同不失效
    CHECK(RectEq(rects[0], 310, 500, 390, 528));      // 段 3 仅新 bounds
    CHECK(RectEq(rects[1], 410, 500, 490, 528));      // 段 4 仅新 bounds
    // 收缩：旧 5 段 vs 新 3 段——下标 3/4 新侧越界按不可见 → 仅旧 bounds
    const std::vector<RECT> back = StatusDiffInvalidRects(grown, shrunk);
    REQUIRE(back.size() == 2);
    CHECK(RectEq(back[0], 310, 500, 390, 528));       // 段 3 仅旧 bounds（清残影）
    CHECK(RectEq(back[1], 410, 500, 490, 528));       // 段 4 仅旧 bounds
}

TEST_CASE("StatusDiffInvalidRects：负坐标与交错新旧 → 逐边 min/max 并集",
          "[ui][statusbar]") {
    std::vector<StatusSegment> oldSegs = MakeSegments();
    std::vector<StatusSegment> newSegs = oldSegs;
    // 段 0：旧含负坐标且新旧交错（旧{-20,-5,30,10} 新{10,0,50,20}）——
    // 并集须逐边 min/max = {-20,-5,50,20}（负值防御，如 DPI 变换中间态）
    oldSegs[0].text = L"a";
    oldSegs[0].bounds = {-20, -5, 30, 10};
    newSegs[0].text = L"b";
    newSegs[0].bounds = {10, 0, 50, 20};
    // 段 1：完全分离两块（旧左新右，中间隔 gap）——并集为横跨 gap 的单一
    // 矩形（并集语义，非两个独立矩形；gap 区域一并重画无损正确性）
    oldSegs[1].text = L"c";
    oldSegs[1].bounds = {0, 500, 40, 528};
    newSegs[1].text = L"d";
    newSegs[1].bounds = {100, 500, 160, 528};
    const std::vector<RECT> rects = StatusDiffInvalidRects(oldSegs, newSegs);
    REQUIRE(rects.size() == 2);
    CHECK(RectEq(rects[0], -20, -5, 50, 20));         // 交错并集
    CHECK(RectEq(rects[1], 0, 500, 160, 528));        // 分离并集横跨 gap
}

// ---------------------------------------------------------------------------
// M2-B：对话框高度计算（纯函数；随 DPI 换算、行数下限 1）
// ---------------------------------------------------------------------------

TEST_CASE("ComputeDialogHeight 随行数与 DPI 单调增长", "[ui][dialog]") {
    const UINT dpi = 96;
    const int h1 = ComputeDialogHeight(1, dpi);
    const int h3 = ComputeDialogHeight(3, dpi);
    CHECK(h1 > 0);
    CHECK(h3 > h1);
    CHECK(h3 - h1 == 2 * Scale(dialog_metrics::MessagePixelSize + 8, dpi));  // 每行增量
    // 200% DPI：缩放部分 ×2（边框 2 物理像素不随 DPI——同菜单 1px 边框惯例）
    CHECK(ComputeDialogHeight(1, 192) == 2 * h1 - 2);
    // 行数下限 1（0/负数防御）
    CHECK(ComputeDialogHeight(0, dpi) == h1);
    CHECK(ComputeDialogHeight(-5, dpi) == h1);
}

// ---------------------------------------------------------------------------
// M3-B：查找对话框状态行文本（BuildSearchStatusText 纯函数；优先级与旧
// SearchDialog.qml 状态 Label 一致：错误 → 已替换 N 处 → 无匹配 → >10000 →
// 第 N 个 / 共 M 个 → 共 M 个）
// ---------------------------------------------------------------------------

TEST_CASE("BuildSearchStatusText 计数与无匹配", "[ui][search]") {
    SearchDialogState st;
    st.matchCount = 5;
    st.currentIndex = 0;
    CHECK(BuildSearchStatusText(st, -1) == L"第 1 个 / 共 5 个");
    st.currentIndex = 4;
    CHECK(BuildSearchStatusText(st, -1) == L"第 5 个 / 共 5 个");
    st.currentIndex = -1;
    CHECK(BuildSearchStatusText(st, -1) == L"共 5 个");  // 有匹配无当前
    st.matchCount = 0;
    CHECK(BuildSearchStatusText(st, -1) == L"无匹配");
}

TEST_CASE("BuildSearchStatusText 溢出与已替换与错误优先级", "[ui][search]") {
    SearchDialogState st;
    st.matchCount = 10000;
    st.currentIndex = 0;
    st.overflowed = true;
    CHECK(BuildSearchStatusText(st, -1) == L">10000");   // 溢出优先于计数
    CHECK(BuildSearchStatusText(st, 7) == L"已替换 7 处");  // 已替换优先于溢出
    st.errorMessage = L"无效的正则表达式";
    CHECK(BuildSearchStatusText(st, 7) == L"无效的正则表达式");  // 错误最优先
    CHECK(BuildSearchStatusText(st, -1) == L"无效的正则表达式");
}

// ---------------------------------------------------------------------------
// F2/F5：工具栏 4 按钮（新建/打开/保存/自动换行）
// 几何/命中用未 attach 的 Toolbar（hwnd_ 空 → DPI 兜底 96，断言值确定）；
// 交互（点击/tooltip/绘制）经真实弹出窗口宿主驱动（test_ui 链接 user32）。
// 防扰：宿主离屏、不可见（无 WS_VISIBLE）、WS_EX_NOACTIVATE 不抢焦点。
// ---------------------------------------------------------------------------

namespace {

struct ToolbarHost {
    HWND hwnd = nullptr;
    ToolbarHost() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"qcnote_test_toolbar_host";
        RegisterClassW(&wc);  // 重复注册（ERROR_CLASS_ALREADY_EXISTS）可忽略
        hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName,
                               L"", WS_POPUP, -2000, -2000, 600, 400, nullptr,
                               nullptr, wc.hInstance, nullptr);
    }
    ~ToolbarHost() {
        if (hwnd) DestroyWindow(hwnd);
    }
    ToolbarHost(const ToolbarHost&) = delete;
    ToolbarHost& operator=(const ToolbarHost&) = delete;
};

POINT centerOf(const RECT& r) {
    return {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
}
}  // namespace

// ---------------------------------------------------------------------------
// R3-A：StatusBar::update 行为级——差分矩形同步直绘的像素级验证。
//
// 宿主选型依据（探针实证，Win11/DWM）：完全离屏的可见窗口中，WM_PAINT
//（BeginPaint）路径绘制可落地且可读回（theme selftest 的 PrintWindow 采样
// 即此路径），但 GetDC 直写（BitBlt）被 GDI 按「可见区域为空」整批裁剪
// 丢弃、永不落地——本用例要观测的恰是 update() 的 GetDC 直绘，离屏窗口
// 物理不可行。故宿主为「在屏 + 近零感知」分层窗口（PixelHost）：
// WS_EX_LAYERED alpha=1（0.4% 不透明度叠加，视觉阈下：8bit 通道叠加量
// 1/255，低于大面积均匀场 ~1-2% 的人类对比阈）+ WS_EX_TRANSPARENT 点击
// 穿透（零输入干扰）+ WS_EX_TOOLWINDOW（无任务栏项）+ SW_SHOWNOACTIVATE
//（不抢焦点）+ TOPMOST（不被遮挡——遮挡同样导致写入被裁剪）→ 满足
// 「零可见元素、零输入干扰、零前台抢占」防扰铁律。
//
// 断言体系（标记色存活法，比朴素双快照更强）：
//   1) update(B)（列号 1→12：LineCol 段变宽、Total 段右移）后：
//      - 左组区标记色被新内容覆写（直绘落地的证明）且与 A 态快照存在
//        像素差异（新内容确实不同）；
//      - 右组区（编码/换行/路径）标记色逐像素存活 → 直绘写且只写差分
//        矩形（整条重绘回归即红：右组标记色会被同内容重写抹掉）；
//      - 状态栏最底行（窗口底缘 1px 边界线位置）标记色全宽存活 →
//        R3-A 底缘钳制（bar.bottom-1）的直证（去钳制回归即红）；
//      - GetUpdateRect==0：无失效排队（直绘特征）。
//   2) 等值二次 update(B) 后整条快照逐字节一致（标记色与左组内容均原样
//      → 等值跳过零绘制）。
//
// 可失败性论证（回退为失效路径即红）：失效路径下无直绘，本宿主为纯
// DefWindowProc 窗口、测试无消息循环，WM_PAINT 永不派发 → 左组区标记色
// 存活不被覆写 → 断言「左组标记色被覆写」红；同时失效区非空 →
// GetUpdateRect 断言红。整条失效（invalidateBar）与整条直绘重刷同理
// 分别经 ③ / 右组存活断言变红。
// ---------------------------------------------------------------------------

namespace {

// R3-B 近零感知像素宿主（选型依据见上断言体系注释块）
struct PixelHost {
    HWND hwnd = nullptr;
    PixelHost() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"qcnote_test_statusbar_pixel_host";
        RegisterClassW(&wc);  // 重复注册（ERROR_CLASS_ALREADY_EXISTS）可忽略
        RECT wa{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);  // 主屏工作区左上角
        hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                                   WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                               wc.lpszClassName, L"", WS_POPUP, wa.left + 8, wa.top + 8,
                               600, 400, nullptr, nullptr, wc.hInstance, nullptr);
        SetLayeredWindowAttributes(hwnd, 0, 1, LWA_ALPHA);  // 1/255 ≈ 0.4% 不透明
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }
    ~PixelHost() {
        if (hwnd) {
            ShowWindow(hwnd, SW_HIDE);
            DestroyWindow(hwnd);
        }
    }
    PixelHost(const PixelHost&) = delete;
    PixelHost& operator=(const PixelHost&) = delete;
};

}  // namespace

TEST_CASE("StatusBar::update 行为级：差分直绘像素断言与等值零绘制", "[ui][statusbar]") {
    PixelHost host;
    StatusBar sb;
    sb.attach(host.hwnd);
    StatusData a;
    a.hasDoc = true;
    a.cursorLine = 1;
    a.cursorColumn = 1;  // 一位数列号
    a.totalChars = 123;
    a.encoding = L"UTF-8";
    a.lineEnding = L"CRLF";
    a.path = L"C:\\very\\long\\dir\\structure\\some\\user\\documents\\note.txt";
    StatusData a2 = a;
    a2.cursorColumn = 2;  // 首帧竞态重试时强制差分重绘用

    Sleep(150);  // DWM 分层面建立留裕量（对齐 theme selftest 首帧竞态防御）
    sb.update(a);  // 首次：全段不可见→可见，直绘各段 bounds

    RECT client{};
    GetClientRect(host.hwnd, &client);
    const RECT bar = sb.rect(client);
    const int barW = static_cast<int>(bar.right - bar.left);
    const int barH = static_cast<int>(bar.bottom - bar.top);
    REQUIRE(barW > 0);
    REQUIRE(barH > 1);

    // 条带像素快照：GetDC + 兼容位图 BitBlt SRCCOPY + GetDIBits 读回
    //（32bpp top-down，DWORD 序 0x00RRGGBB；分层 alpha 不参与——读到的是
    // 合成前表面全色值，探针已证）。防御性 REQUIRE：GDI 基础设施缺失即终止
    struct Snap {
        std::vector<DWORD> px;  // 行优先，px[row * w + col]
        int w = 0, h = 0;
    };
    const auto snap = [&]() {
        Snap s;
        s.w = barW;
        s.h = barH;
        HDC dc = GetDC(host.hwnd);
        REQUIRE(dc != nullptr);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, s.w, s.h);
        REQUIRE(mem != nullptr);
        REQUIRE(bmp != nullptr);
        const HGDIOBJ old = SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, s.w, s.h, dc, bar.left, bar.top, SRCCOPY);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = s.w;
        bi.bmiHeader.biHeight = -s.h;  // 负高 = top-down，首行即条带顶行
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        s.px.resize(static_cast<size_t>(s.w) * static_cast<size_t>(s.h));
        REQUIRE(GetDIBits(mem, bmp, 0, s.h, s.px.data(), &bi, DIB_RGB_COLORS) == s.h);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(host.hwnd, dc);
        return s;
    };
    // 条带内相对坐标 [l,t,r,b) 的像素差计数
    const auto diffCount = [](const Snap& x, const Snap& y, int l, int t, int r, int b) {
        int n = 0;
        for (int row = t; row < b; ++row) {
            for (int col = l; col < r; ++col) {
                if (x.px[static_cast<size_t>(row) * x.w + col] !=
                    y.px[static_cast<size_t>(row) * y.w + col]) {
                    ++n;
                }
            }
        }
        return n;
    };

    // 首帧就绪金丝雀（带界重试，重试以 a↔a2 差分强制重绘——等值 update
    // 会被跳过不重画）：路径段 bounds 顶行应为状态栏顶部分隔线颜色
    //（pal.border）。表面未就绪/直绘未落地时此处先红，避免后续误判。
    // 字节序注意：快照 DWORD 为 GetDIBits 序 0x00RRGGBB，COLORREF 为
    // 0x00BBGGRR，须换序比较（对齐 main.cpp CaptureClientPixel 的转换）
    const ThemePalette& pal = CurrentPalette();
    const COLORREF borderCref = ToColorRef(pal.border);
    const DWORD borderColor = ((borderCref & 0xFFu) << 16) | (borderCref & 0xFF00u) |
                              ((borderCref >> 16) & 0xFFu);
    {
        bool ready = false;
        for (int attempt = 0; attempt < 20 && !ready; ++attempt) {
            GdiFlush();
            const Snap s = snap();
            const RECT pathB = sb.pathRect(client);
            REQUIRE(!IsRectEmpty(&pathB));
            const int sepRowX = static_cast<int>(pathB.left) - static_cast<int>(bar.left) + 2;
            ready = s.px[static_cast<size_t>(sepRowX)] == borderColor;
            if (!ready) {
                sb.update(attempt % 2 == 0 ? a2 : a);
                MSG m;
                while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                    DispatchMessageW(&m);
                }
                Sleep(10);
            }
        }
        REQUIRE(ready);
    }

    // ShowWindow 产生的初始失效区与直绘无关，清空后进入干净观测态
    REQUIRE(ValidateRect(host.hwnd, nullptr));
    sb.update(a);  // 归一化到 A 态（就绪重试可能停在 a2）
    GdiFlush();
    const Snap s1 = snap();  // A 态（列 1）

    // 标记色涂抹整条（洋红：两主题任何 token/文字抗锯齿色都不可能合成；
    // 且 RR/BB 对称，COLORREF 与快照序数值相同，免换序）
    const DWORD kMarker = 0x00FF00FF;  // RGB(255,0,255)
    {
        HDC dc = GetDC(host.hwnd);
        HBRUSH brush = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(dc, &bar, brush);
        DeleteObject(brush);
        ReleaseDC(host.hwnd, dc);
        GdiFlush();
    }
    const Snap sMark = snap();
    // 涂抹落地金丝雀：整条全为标记色（写入被裁剪/表面丢失时此处先红）
    {
        int nonMarker = 0;
        for (DWORD p : sMark.px) {
            if (p != kMarker) ++nonMarker;
        }
        REQUIRE(nonMarker == 0);
    }

    // —— 1) update(B)：列号 1→12 → LineCol 变宽、Total 右移 ——
    StatusData b = a;
    b.cursorColumn = 12;
    sb.update(b);
    GdiFlush();
    // 直绘特征：无失效排队（回退失效路径时此处非 0 → 红）
    CHECK(GetUpdateRect(host.hwnd, nullptr, FALSE) == 0);
    const Snap s2 = snap();

    // 右组左界 = 路径段左缘（右组自右向左布置：编码→换行→路径，路径是
    // 组内最左段；经组件公开测试钩子取得，不依赖布局常量推算）
    const int rightGroupLeft = static_cast<int>(sb.pathRect(client).left) -
                               static_cast<int>(bar.left);
    // ① 直绘落地：左组区（LineCol/Total 差分矩形所在，最底行除外）标记色
    //    被新内容覆写（回退失效路径 → 无人覆写 → 红）
    CHECK(diffCount(sMark, s2, 0, 0, rightGroupLeft, barH - 1) > 0);
    // ② 新内容确实不同：左组区与 A 态快照存在像素差异（新列号多一位数字
    //    且 Total 段整体右移，旧文字像素 ↔ 底色必然互变）
    CHECK(diffCount(s1, s2, 0, 0, rightGroupLeft, barH - 1) > 0);
    // ③ 写且只写差分矩形：右组区（编码/换行/路径 + 组间空档 + 右边距）
    //    标记色逐像素存活（整条直绘重刷回归 → 同内容重写抹掉标记 → 红）
    CHECK(diffCount(sMark, s2, rightGroupLeft, 0, barW, barH) == 0);
    // ④ 底缘钳制直证：最底行（窗口底缘 1px 边界线所在）全宽标记色存活
    //    （去钳制回归 → 差分矩形含最底行 → 被重写 → 红）
    CHECK(diffCount(sMark, s2, 0, barH - 1, barW, barH) == 0);

    // —— 2) 等值二次 update(B) → 零绘制，整条逐字节一致 ——
    sb.update(b);
    GdiFlush();
    const Snap s3 = snap();
    CHECK(s3.px.size() == s2.px.size());
    CHECK(diffCount(s2, s3, 0, 0, barW, barH) == 0);
    CHECK(GetUpdateRect(host.hwnd, nullptr, FALSE) == 0);

    sb.detach();
}

TEST_CASE("工具栏几何：3+1 分组布局等宽互不重叠且落在条带内", "[ui][toolbar]") {
    Toolbar tb;  // 未 attach：DPI 兜底 96
    const RECT client{0, 0, 600, 400};
    RECT btns[4] = {};
    tb.buttonRects(client, btns);
    const RECT bar = tb.rect(client);
    CHECK(RectEq(bar, 0, 40, 600, 80));  // CaptionHeight 40 + Height 40
    const int bw = toolbar_metrics::ButtonWidth;   // 44
    const int bh = toolbar_metrics::ButtonHeight;  // 32
    const int gap = toolbar_metrics::Spacing;      // 组内间距 2
    // 组间间隔区（DEC-005）：分隔竖线两侧各 SeparatorMargin + 竖线宽 1
    const int sepGap = toolbar_metrics::SeparatorMargin * 2 + toolbar_metrics::SeparatorWidth;
    const int y = 40 + (toolbar_metrics::Height - bh) / 2;  // 垂直居中 = 44
    for (int i = 0; i < 4; ++i) {
        CHECK(btns[i].right == btns[i].left + bw);
        CHECK(btns[i].top == y);
        CHECK(btns[i].bottom == y + bh);  // 同高
        // 均落在工具栏条带内
        CHECK(btns[i].left >= bar.left);
        CHECK(btns[i].right <= bar.right);
        CHECK(btns[i].top >= bar.top);
        CHECK(btns[i].bottom <= bar.bottom);
    }
    // 组内（按钮 0-2：新建/打开/保存）等距递进
    for (int i = 0; i < 3; ++i) {
        CHECK(btns[i].left == toolbar_metrics::LeftMargin + i * (bw + gap));
    }
    // 组间（按钮 2→3：自动换行）跨越分隔间隔区（含组间间距语义）
    CHECK(btns[3].left - btns[2].right == sepGap);
    // 互不重叠
    for (int i = 1; i < 4; ++i) {
        CHECK(btns[i].left >= btns[i - 1].right);
    }
}

TEST_CASE("工具栏命中：4 按钮中心返回下标 0-3，空白返回 -1", "[ui][toolbar]") {
    Toolbar tb;
    const RECT client{0, 0, 600, 400};
    RECT btns[4] = {};
    tb.buttonRects(client, btns);
    for (int i = 0; i < 4; ++i) {
        CHECK(tb.hitButton(centerOf(btns[i]), client) == i);
    }
    CHECK(tb.hitButton(POINT{300, 200}, client) == -1);  // 内容区空白
    CHECK(tb.hitButton(POINT{400, 60}, client) == -1);   // 条带内按钮右侧空白
}

TEST_CASE("工具栏换行按钮触发 ToggleWordWrap 命令", "[ui][toolbar]") {
    ToolbarHost host;
    REQUIRE(host.hwnd != nullptr);
    Toolbar tb;
    tb.attach(host.hwnd);
    RECT client{};
    GetClientRect(host.hwnd, &client);
    RECT btns[4] = {};
    tb.buttonRects(client, btns);
    int count = 0;
    Command got = Command::None;
    tb.onCommand = [&](Command c) { ++count; got = c; };
    tb.onLButtonDown(centerOf(btns[3]));  // 第 4 按钮 = 自动换行（toggle）
    CHECK(count == 1);
    CHECK(got == Command::ToggleWordWrap);
    tb.onLButtonDown(centerOf(btns[0]));  // 回归：第 1 按钮 = 新建
    CHECK(count == 2);
    CHECK(got == Command::NewDocument);
}

TEST_CASE("工具栏保存禁用时吞点击，wrap 按钮不受影响", "[ui][toolbar]") {
    ToolbarHost host;
    REQUIRE(host.hwnd != nullptr);
    Toolbar tb;
    tb.attach(host.hwnd);
    RECT client{};
    GetClientRect(host.hwnd, &client);
    RECT btns[4] = {};
    tb.buttonRects(client, btns);
    int count = 0;
    tb.onCommand = [&](Command) { ++count; };
    tb.setSaveEnabled(false);
    tb.onLButtonDown(centerOf(btns[2]));  // 保存禁用：点击被吞、无命令
    CHECK(count == 0);
    tb.onLButtonDown(centerOf(btns[3]));  // wrap 不受保存禁用影响
    CHECK(count == 1);
    tb.setSaveEnabled(true);
    tb.onLButtonDown(centerOf(btns[2]));  // 恢复后恢复触发
    CHECK(count == 2);
}

// TTM_GETTOOLCOUNT（WM_USER+13）：现代 commctrl.h 已定义；旧 SDK 缺失时兜底
#ifndef TTM_GETTOOLCOUNT
#define TTM_GETTOOLCOUNT (WM_USER + 13)
#endif

TEST_CASE("工具栏 tooltip：工具注册成功（TTM_GETTOOLCOUNT==1 回归钉）", "[ui][toolbar]") {
    ToolbarHost host;
    REQUIRE(host.hwnd != nullptr);
    Toolbar tb;
    tb.attach(host.hwnd);
    HWND tip = tb.tooltipHwndForTest();
    REQUIRE(tip != nullptr);
    // 回归钉（DEC-001/006）：进程须绑定 comctl32 v6（manifest Common-Controls
    // 6.0）。v5 拒绝现代 72 字节 TOOLINFO、TTM_ADDTOOL 静默失败 → 计数 0；
    // v6 接受 → create() 恰好注册 1 个工具
    LRESULT n = SendMessageW(tip, TTM_GETTOOLCOUNT, 0, 0);
    CHECK(n == 1);
}

TEST_CASE("工具栏 tooltip：悬停 600ms 计时到显示，离开隐藏", "[ui][toolbar]") {
    ToolbarHost host;
    REQUIRE(host.hwnd != nullptr);
    Toolbar tb;
    tb.attach(host.hwnd);
    RECT client{};
    GetClientRect(host.hwnd, &client);
    RECT btns[4] = {};
    tb.buttonRects(client, btns);
    tb.onMouseMove(centerOf(btns[1]));        // 进入「打开」按钮
    CHECK_FALSE(tb.tooltipVisibleForTest());  // 延迟期内未显示
    tb.onTimer(3);  // tipTimerId_ = 3（宿主 timer id 分配约定见 toolbar.h）
    CHECK(tb.tooltipVisibleForTest());
    // 系统级真实可见（TRACK 激活后 tooltip 窗口显示；v5 拒绝注册类静默失败
    // 时内部标志看似正常而窗口不显示，只有 IsWindowVisible 能发现）
    CHECK(IsWindowVisible(tb.tooltipHwndForTest()));
    tb.onMouseLeave();
    CHECK_FALSE(tb.tooltipVisibleForTest());
    CHECK_FALSE(IsWindowVisible(tb.tooltipHwndForTest()));
}

// TC-006 禁用态文案链路：保存禁用时悬停 tooltip 显示禁用文案「保存文档
// （当前没有文档）」，启用后恢复「保存文档（Ctrl+S）」。既有用例只断言
// 可见性标志与系统窗口可见，覆盖不到「显示了哪段文案」——此处经真实
// tooltip 控件 TTM_GETTEXT 读回控件当前文本，钉住 showTooltipFor 的
// 三元文案分支（toolbar.cpp，旧 QML enabled 三元文案对等行为）。
TEST_CASE("工具栏 tooltip：保存禁用/启用态文案切换（TTM_GETTEXT 链路）", "[ui][toolbar]") {
    ToolbarHost host;
    REQUIRE(host.hwnd != nullptr);
    Toolbar tb;
    tb.attach(host.hwnd);
    RECT client{};
    GetClientRect(host.hwnd, &client);
    RECT btns[4] = {};
    tb.buttonRects(client, btns);
    HWND tip = tb.tooltipHwndForTest();
    REQUIRE(tip != nullptr);
    // ManualTooltip 单工具 TRACK 模式：工具 uId 固定 1（见 src/ui/tooltip.cpp
    // makeToolInfo）。TTM_GETTEXT 读回控件当前文本
    auto currentTipText = [&]() -> std::wstring {
        wchar_t buf[128] = {};
        TOOLINFO ti{};
        ti.cbSize = sizeof(ti);
        ti.hwnd = host.hwnd;
        ti.uId = 1;
        ti.lpszText = buf;  // 缓冲区大小经 TTM_GETTEXT 的 wParam 传入
                            // （新 SDK TOOLINFO 已无 cchTextMax 成员）
        SendMessageW(tip, TTM_GETTEXT, 128, reinterpret_cast<LPARAM>(&ti));
        return std::wstring(buf);
    };
    tb.setSaveEnabled(false);
    tb.onMouseMove(centerOf(btns[2]));  // 进入禁用的保存按钮
    tb.onTimer(3);
    REQUIRE(tb.tooltipVisibleForTest());
    CHECK(currentTipText() == L"保存文档（当前没有文档）");
    tb.setSaveEnabled(true);
    tb.onMouseMove(centerOf(btns[0]));  // 移出 → 真实 hideTooltip 链路
    CHECK_FALSE(tb.tooltipVisibleForTest());
    tb.onMouseMove(centerOf(btns[2]));  // 重新进入保存按钮 → 重新计时
    tb.onTimer(3);
    REQUIRE(tb.tooltipVisibleForTest());
    CHECK(currentTipText() == L"保存文档（Ctrl+S）");
    tb.onMouseLeave();
    CHECK_FALSE(tb.tooltipVisibleForTest());
}

TEST_CASE("工具栏 wrap 激活态切换与绘制不崩溃（内存 DC 全流程释放）", "[ui][toolbar]") {
    // DEC-002：图标画布 20×20 逻辑像素（形状坐标仍 16 网格映射）
    CHECK(toolbar_metrics::IconSize == 20);
    ToolbarHost host;
    REQUIRE(host.hwnd != nullptr);
    Toolbar tb;
    tb.attach(host.hwnd);
    RECT client{};
    GetClientRect(host.hwnd, &client);
    HDC screenDc = GetDC(host.hwnd);
    REQUIRE(screenDc != nullptr);
    HDC mem = CreateCompatibleDC(screenDc);
    REQUIRE(mem != nullptr);
    HBITMAP bmp = CreateCompatibleBitmap(screenDc, client.right, client.bottom);
    REQUIRE(bmp != nullptr);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    // 未激活 → 激活 → 未激活三轮绘制：覆盖 4 图标全部状态组合的实际绘制
    // 路径（GDI+ 或回退，按运行环境分发），仅断言不崩溃
    tb.paint(mem, client);
    tb.setWrapEnabled(true);
    tb.paint(mem, client);
    tb.setWrapEnabled(false);
    tb.paint(mem, client);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(host.hwnd, screenDc);
}

// ---------------------------------------------------------------------------
// GDI+ 图标渲染稳健性（Win32界面体验修复 F2 / TC-008）：深浅主题 × 悬停/
// 禁用/wrap 激活态全组合内存 DC 完整绘制，只断言不崩溃（GDI/GDI+ 对象
// RAII/释放完整由用例自身全流程释放路径验证）。DPI 96/192 维度依赖真实
// attach 窗口的每监视器 DPI，单测环境无法注入，属受限项（e2e/人工补充）。
// ---------------------------------------------------------------------------

TEST_CASE("工具栏绘制稳健性：主题/悬停/禁用态内存 DC 全流程", "[ui][toolbar]") {
    ToolbarHost host;
    REQUIRE(host.hwnd != nullptr);
    Toolbar tb;
    tb.attach(host.hwnd);
    RECT client{};
    GetClientRect(host.hwnd, &client);
    HDC screenDc = GetDC(host.hwnd);
    REQUIRE(screenDc != nullptr);
    HDC mem = CreateCompatibleDC(screenDc);
    REQUIRE(mem != nullptr);
    HBITMAP bmp = CreateCompatibleBitmap(screenDc, client.right, client.bottom);
    REQUIRE(bmp != nullptr);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    RECT btns[4] = {};
    tb.buttonRects(client, btns);
    tb.onMouseMove(centerOf(btns[1]));  // 悬停态（「打开」按钮）
    tb.setSaveEnabled(false);           // 保存禁用态（textDisabled 图标路径）
    const bool darkBefore = IsDarkTheme();
    for (int round = 0; round < 2; ++round) {
        SetTheme(round == 0);           // 深 → 浅两主题全状态组合绘制
        tb.paint(mem, client);
        tb.setWrapEnabled(true);
        tb.paint(mem, client);
        tb.setWrapEnabled(false);
        tb.paint(mem, client);
    }
    SetTheme(darkBefore);               // 恢复进程初始主题（用例隔离）
    tb.onMouseLeave();
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(host.hwnd, screenDc);
}

// ---------------------------------------------------------------------------
// DEC-003：工具栏图标彩色语义 token（6 个，两主题各一档）
// ---------------------------------------------------------------------------

TEST_CASE("工具栏图标彩色 token：两主题取值不透明且深浅适配", "[ui][theme][toolbar]") {
    const ThemePalette& l = LightPalette();
    const ThemePalette& d = DarkPalette();
    // 全部不透明（alpha=0xFF）且非零——彩色填充面直接实色落笔
    CHECK((l.iconNewBadge >> 24) == 0xFFu);   CHECK(l.iconNewBadge != 0u);
    CHECK((l.iconOpenBack >> 24) == 0xFFu);   CHECK(l.iconOpenBack != 0u);
    CHECK((l.iconOpenFront >> 24) == 0xFFu);  CHECK(l.iconOpenFront != 0u);
    CHECK((l.iconSaveBody >> 24) == 0xFFu);   CHECK(l.iconSaveBody != 0u);
    CHECK((l.iconSaveLabel >> 24) == 0xFFu);  CHECK(l.iconSaveLabel != 0u);
    CHECK((l.iconWrapArrow >> 24) == 0xFFu);  CHECK(l.iconWrapArrow != 0u);
    CHECK((d.iconNewBadge >> 24) == 0xFFu);   CHECK(d.iconNewBadge != 0u);
    CHECK((d.iconOpenBack >> 24) == 0xFFu);   CHECK(d.iconOpenBack != 0u);
    CHECK((d.iconOpenFront >> 24) == 0xFFu);  CHECK(d.iconOpenFront != 0u);
    CHECK((d.iconSaveBody >> 24) == 0xFFu);   CHECK(d.iconSaveBody != 0u);
    CHECK((d.iconSaveLabel >> 24) == 0xFFu);  CHECK(d.iconSaveLabel != 0u);
    CHECK((d.iconWrapArrow >> 24) == 0xFFu);  CHECK(d.iconWrapArrow != 0u);
    // 同 token 浅 != 深（深浅适配，防「深色=浅色占位」回归）
    CHECK(l.iconNewBadge != d.iconNewBadge);
    CHECK(l.iconOpenBack != d.iconOpenBack);
    CHECK(l.iconOpenFront != d.iconOpenFront);
    CHECK(l.iconSaveBody != d.iconSaveBody);
    CHECK(l.iconSaveLabel != d.iconSaveLabel);
    CHECK(l.iconWrapArrow != d.iconWrapArrow);
    // 浅色 6 值互不相同（彩色语义，防退化成单色占位）
    CHECK(l.iconNewBadge != l.iconOpenBack);
    CHECK(l.iconNewBadge != l.iconOpenFront);
    CHECK(l.iconNewBadge != l.iconSaveBody);
    CHECK(l.iconNewBadge != l.iconSaveLabel);
    CHECK(l.iconNewBadge != l.iconWrapArrow);
    CHECK(l.iconOpenBack != l.iconOpenFront);
    CHECK(l.iconOpenBack != l.iconSaveBody);
    CHECK(l.iconOpenBack != l.iconSaveLabel);
    CHECK(l.iconOpenBack != l.iconWrapArrow);
    CHECK(l.iconOpenFront != l.iconSaveBody);
    CHECK(l.iconOpenFront != l.iconSaveLabel);
    CHECK(l.iconOpenFront != l.iconWrapArrow);
    CHECK(l.iconSaveBody != l.iconSaveLabel);
    CHECK(l.iconSaveBody != l.iconWrapArrow);
    CHECK(l.iconSaveLabel != l.iconWrapArrow);
}

// ---------------------------------------------------------------------------
// DEC-005：组间分隔间隔区——不入任何按钮矩形（命中 -1）、点击无命令、
// 悬停不显示 tooltip（现有 hideTooltip 逻辑天然成立，此处防回归钉死）
// ---------------------------------------------------------------------------

TEST_CASE("工具栏分隔间隔区：命中 -1、点击无命令、悬停不显示 tooltip", "[ui][toolbar]") {
    ToolbarHost host;
    REQUIRE(host.hwnd != nullptr);
    Toolbar tb;
    tb.attach(host.hwnd);
    RECT client{};
    GetClientRect(host.hwnd, &client);
    RECT btns[4] = {};
    tb.buttonRects(client, btns);
    // 间隔区中点：按钮 2 右缘与按钮 3 左缘之间、按钮行垂直中心
    const POINT gapPt{(btns[2].right + btns[3].left) / 2, (btns[2].top + btns[2].bottom) / 2};
    CHECK(gapPt.x > btns[2].right);  // 前置校验：取点确在间隔区内
    CHECK(gapPt.x < btns[3].left);
    CHECK(tb.hitButton(gapPt, client) == -1);
    int count = 0;
    tb.onCommand = [&](Command) { ++count; };
    tb.onLButtonDown(gapPt);  // 间隔区点击：无按钮命中 → 无命令
    CHECK(count == 0);
    // 先悬停按钮 2 至 tooltip 显示，再移入间隔区：tooltip 隐藏且不显示
    tb.onMouseMove(centerOf(btns[2]));
    tb.onTimer(3);
    CHECK(tb.tooltipVisibleForTest());
    tb.onMouseMove(gapPt);
    CHECK_FALSE(tb.tooltipVisibleForTest());
}
