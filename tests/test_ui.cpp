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

TEST_CASE("工具栏几何：4 按钮等宽等距互不重叠且落在条带内", "[ui][toolbar]") {
    Toolbar tb;  // 未 attach：DPI 兜底 96
    const RECT client{0, 0, 600, 400};
    RECT btns[4] = {};
    tb.buttonRects(client, btns);
    const RECT bar = tb.rect(client);
    CHECK(RectEq(bar, 0, 40, 600, 80));  // CaptionHeight 40 + Height 40
    const int bw = toolbar_metrics::ButtonWidth;   // 44
    const int bh = toolbar_metrics::ButtonHeight;  // 32
    const int gap = toolbar_metrics::Spacing;      // 2
    const int y = 40 + (toolbar_metrics::Height - bh) / 2;  // 垂直居中 = 44
    for (int i = 0; i < 4; ++i) {
        CHECK(btns[i].left == toolbar_metrics::LeftMargin + i * (bw + gap));
        CHECK(btns[i].right == btns[i].left + bw);
        CHECK(btns[i].top == y);
        CHECK(btns[i].bottom == y + bh);  // 同高
        // 均落在工具栏条带内
        CHECK(btns[i].left >= bar.left);
        CHECK(btns[i].right <= bar.right);
        CHECK(btns[i].top >= bar.top);
        CHECK(btns[i].bottom <= bar.bottom);
    }
    // 互不重叠且间距一致
    for (int i = 1; i < 4; ++i) {
        CHECK(btns[i].left >= btns[i - 1].right);
        CHECK(btns[i].left - btns[i - 1].right == gap);
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
    tb.onMouseLeave();
    CHECK_FALSE(tb.tooltipVisibleForTest());
}

TEST_CASE("工具栏 wrap 激活态切换与绘制不崩溃（内存 DC 全流程释放）", "[ui][toolbar]") {
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
