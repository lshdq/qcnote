// qcnote M1：无边框窗口基建 + 自绘标题栏（窗口控制、菜单按钮、Tab 区）。
// M1 第二阶段：菜单系统（ui/menu + ui/mainmenu）；M1 第三阶段：Tab 条 + 欢迎页。
// M2-B：Scintilla 编辑区（editor/scintillaedit，每文档一实例）+ DocumentManager
//       接线（事件驱动 Tab/标题/状态栏/编辑器实例增删）+ 命令路由实装（A1 全
//       链路：新建/打开/保存/另存为/关闭/编码重载与转存/换行）+ 工具栏/状态栏
//       （E3）+ 基础自绘对话框（ui/dialog，E4 基座）+ 命令行位置参数打开（D2）。
// M3-B：查找替换对话框（ui/searchdialog + editor/searchcontroller，B5/DEC-009）
//       + 多光标交互映射（editor/multicursorcontroller，B4/DEC-008）+ 匹配高亮
//       indicator + 状态栏多光标字段。
// M4-B：会话/草稿接线（core/sessionmanager + core/draftcachemanager）——启动
//       恢复时序（会话文档/几何→显示→草稿→冲突决策）、运行中防抖（200ms
//       WM_TIMER 驱动会话 500ms / 草稿 1000ms）、失焦即写、热退出免提示
//       （C4：flushNow+persistNow 后直接退出）、关闭三选确认（A4，含 Tab 右键
//       批量关闭锚点 A3）、编码重载确认（B1）、草稿冲突汇总对话框（DEC-006）；
//       测试钩子：--userdir=<path>（用户目录重定向）与 --selftest-dump=<path>
//       （WM_APP+6 状态采样 JSON）。
// M5：单实例/Shell（platform/singleinstance + platform/shellintegration）——
//       启动早期命名互斥量判主（二次实例 WM_COPYDATA 转发命令行后退出码 0，
//       失败降级独立运行，D1）、主实例打开队列消化（WM_APP+8，去重激活+前置
//       窗口）、命令行位置参数绝对化（D2）、首启「添加右键菜单」提示（D3，
//       旧版询问语义非自动注册）；测试钩子：--no-shell-prompt 与
//       --selftest-shell-register/unregister/query=<suffix>（临时键名注入）。
//
// 模块职责划分：
//   ui/theme    —— 语义主题 token + DPI 换算
//   ui/titlebar —— 标题栏布局/自绘/按钮交互
//   ui/tabs     —— Tab 条（数据来自 DocumentManager 快照）
//   ui/welcome  —— 欢迎页（无文档时显示；内容区随工具栏/状态栏 chrome 内缩）
//   ui/menu     —— 自绘弹出菜单框架；ui/mainmenu 主菜单内容
//   ui/toolbar  —— 工具栏（新建/打开/保存，E3）
//   ui/statusbar—— 状态栏（行列/字符数/编码/换行/路径，E3）
//   ui/dialog   —— 基础模态对话框基座（E4：居中/拖动把手/边界约束）
//   editor/…    —— Scintilla 编辑器组件（每文档一实例；内容增量同步）
//   core/…      —— 纯逻辑文档系统（M2-A，只读使用）
//   main.cpp    —— 窗口过程与组装层：事件接线、命令路由、快捷键（消息循环拦截）、
//                  文件对话框、布局（标题栏→工具栏→编辑区/欢迎页→状态栏）
//
// 无边框方案要点（需求 E1/E2）：保留 WS_CAPTION|WS_THICKFRAME|WS_SYSMENU…，
// WM_NCCALCSIZE 客户区全窗化，WM_NCHITTEST 自定义命中（详见 M1 注释）。
//
// 快捷键：窗口级，消息循环内拦截（对齐旧 QML 窗口级 Shortcut）——编辑器持有
// 焦点时 Ctrl+N/O/S/W、Ctrl+Shift+S、Ctrl+Tab/Shift+Tab 仍生效；Ctrl+F/H 打开
// 查找替换对话框、F3/Shift+F3 关闭态跳转（B5）。多光标触发键（Ctrl+D、
// Ctrl+Alt+方向等）在 Scintilla 子类化层拦截（editor/multicursorcontroller）。
// 菜单/对话框/文件对话框打开期间各自运行模态消息循环，主循环不泵消息——
// 快捷键天然屏蔽。
//
// M3 接入：查找替换对话框（Ctrl+F/H 实装）；M4 接入：关闭三选确认/草稿/会话；
// M5：单实例/shell；M6：深色主题。

#include <windows.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM
#include <dwmapi.h>    // DwmSetWindowAttribute（Win11 隐藏系统边框线）
#include <commdlg.h>   // GetOpenFileNameW / GetSaveFileNameW
#include <shellapi.h>  // CommandLineToArgvW
#include <imm.h>       // IMN_* 常量（IME 消息注入自查；不调用 IMM 函数）

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Scintilla.h"  // Scintilla_RegisterClasses：静态链接时替代 DLL 自注册

#include "core/document.h"
#include "core/documentmanager.h"
#include "core/draftcachemanager.h"
#include "core/encoding.h"
#include "core/localstorage.h"
#include "core/sessionmanager.h"
#include "core/textutil.h"
#include "editor/multicursorcontroller.h"
#include "editor/scintillaedit.h"
#include "editor/searchcontroller.h"
#include "platform/shellintegration.h"
#include "platform/singleinstance.h"
#include "ui/commands.h"
#include "ui/dialog.h"
#include "ui/mainmenu.h"
#include "ui/menu.h"
#include "ui/searchdialog.h"
#include "ui/statusbar.h"
#include "ui/tabs.h"
#include "ui/theme.h"
#include "ui/titlebar.h"
#include "ui/toolbar.h"
#include "ui/welcome.h"

namespace {

constexpr wchar_t kMainWindowClass[] = L"QcnoteMainWindow";

// 与 qcnote.rc 中 IDI_ICON1 的数值保持一致（exe 首个图标资源即任务栏/资源管理器默认图标）
constexpr int kIconResourceId = 101;

// 初始窗口尺寸（逻辑单位，需求 C1 默认几何 1000×640；PMv2 下按主屏 DPI 换算成像素）
constexpr int kInitialWidthLogical = 1000;
constexpr int kInitialHeightLogical = 640;

// 自动化验证阶段消息（WM_APP..WM_APP+9 中 MenuHost 仅透传 +0..+9；主窗口侧
// 用 +2/+3/+4 触发各模式自测推进，与弹出层透传通道无冲突、模式互斥）
constexpr UINT kMsgSelftestStage = WM_APP + 2;       // M1-2 菜单
constexpr UINT kMsgTabsSelftestStage = WM_APP + 3;   // M1-3 Tab 条
constexpr UINT kMsgDocSelftestStage = WM_APP + 4;    // M2-B 文档全链路
constexpr UINT kMsgSearchSelftestStage = WM_APP + 5; // M3-B 查找替换 + 多光标
constexpr UINT kMsgSelftestDump = WM_APP + 6;        // M4-B 状态采样（--selftest-dump）
// e2e 命令触发钩子（--headless 组合）：wParam 1=NewDocument（=Ctrl+N）
// 2=CycleTab(+1)（=Ctrl+Tab）3=CloseTab（=Ctrl+W）4=ToggleDarkTheme（=菜单
// 「深色主题」，E5 持久化 e2e 用）。真实键盘路径的 GetKeyState 判定不认
// post/sent 消息（M4-B 实测），不可见窗口也不能走
// SetForegroundWindow+keybd_event 真实输入流（会抢用户前台焦点）——测试等价
// 路径：直接触发与快捷键处理完全相同的命令函数；真实键盘路径属人工验收
constexpr UINT kMsgSelftestCommand = WM_APP + 7;
// M5 单实例打开队列消化（WM_COPYDATA 处理内只入队+投递，实际打开在此异步
// 执行——解除转发方 SendMessage 阻塞后由消息循环消化）
constexpr UINT kMsgDrainForwardedPaths = WM_APP + 8;
// M6 主题自测推进（--selftest-theme；与既有 +2..+8 模式互斥不并发）
constexpr UINT kMsgThemeSelftestStage = WM_APP + 9;

// 主窗口宿主 timer id 分配（tabs.cpp / toolbar.h / statusbar.h 同源约定，
// 单窗口内必须互斥，否则 WM_TIMER 互相误路由）：
//   1 = 会话防抖轮询 / 2 = Tab tooltip / 3 = 工具栏 tooltip / 4 = 状态栏
//   tooltip / 5 = 转发队列消化重试
constexpr UINT_PTR kTimerSessionFlush = 1;
// M5 转发队列消化重试定时器（模态对话框打开期间推迟消化，防重入叠加）
constexpr UINT_PTR kTimerForwardDrainRetry = 5;
static_assert(kTimerForwardDrainRetry >= 5,
              "宿主 timer id 须避开 1-4（会话轮询/Tab/工具栏/状态栏已占用）");

ui::TitleBar g_titleBar;
ui::MenuHost g_menuHost;            // 菜单弹出层（模态 track；Tab 右键菜单复用）
ui::MainMenuState g_menuState;      // 弹出前按文档状态刷新
ui::TabStrip g_tabs;                // Tab 条（数据 = DocumentManager 快照）
ui::WelcomePage g_welcome;          // 欢迎页（无文档时显示，A5）
ui::Toolbar g_toolbar;              // 工具栏（E3）
ui::StatusBar g_statusBar;          // 状态栏（E3）
ui::DialogHost g_dialogHost;        // 基础对话框（E4 基座）
ui::SearchDialog g_searchDialog;    // 查找替换对话框（B5，M3-B）
editor::EditorHost g_editors;       // Scintilla 编辑器（每文档一实例）
editor::MultiCursorHost g_multiCursor;      // 多光标交互映射（B4，M3-B）
editor::SearchController g_searchCtl;       // 查找控制器（B5，M3-B）
core::DocumentManager g_docManager; // 全局唯一文档管理器（M2-A）
HWND g_mainWindow = nullptr;
bool g_windowActive = true;    // E2 边界线活动/非活动换色（标题栏配色不随焦点变化，E1）
bool g_trackingClientMouse = false;  // 客户区 TrackMouseEvent（TME_LEAVE）挂载标志
bool g_fileDialogOpen = false;       // 文件对话框打开中（uiLocked 语义 + WM_CLOSE 拒绝）
bool g_wordWrap = false;             // 自动换行全局开关（旧版全局语义，B2）

// 启动位置参数（D2：待打开文件路径，可多个）
std::vector<std::filesystem::path> g_initialPaths;

// ---------------------------------------------------------------------------
// M4-B 会话/草稿状态
// ---------------------------------------------------------------------------

core::SessionPaths g_paths;                        // resolvePaths 结果（ini + drafts 落点）
std::unique_ptr<core::DraftCacheManager> g_drafts; // 草稿防抖管线（wWinMain 构造）
core::Debouncer g_sessionDebounce{500};            // 会话 500ms 防抖（时钟注入）
bool g_restoring = false;            // 启动恢复期屏蔽标志：事件触发的防抖不落盘
core::WindowGeometry g_lastNormal{}; // 最近 Windowed 态 normal 几何（最大化期间不更新）
bool g_lastMaximized = false;        // 最近非最小化态是否最大化（最小化不更新）
std::filesystem::path g_userDirOverride; // --userdir=<path> 测试钩子（用户根目录重定向）
std::wstring g_selftestDumpPath;         // --selftest-dump=<path> 测试钩子（状态采样输出）
bool g_headless = false;    // --headless 测试钩子：窗口不显示（消息循环照常；不扰前台）
bool g_noActivate = false;  // --noactivate 测试钩子：SW_SHOWNOACTIVATE（可见不抢焦点；metrics 用）

// —— M5 单实例/Shell（D1/D3）状态 ——
HANDLE g_singleInstanceMutex = nullptr;  // 主实例持有的命名互斥量（进程退出自动释放）
ULONG_PTR g_instanceDomainTag = 0;       // 本进程单实例域（互斥量名/窗口属性同源派生）
std::vector<std::filesystem::path> g_forwardedOpenQueue;  // WM_COPYDATA 到达的待打开队列
bool g_noShellPrompt = false;            // --no-shell-prompt 测试钩子（跳过首启右键菜单提示）
enum class ShellHookOp { None, Register, Unregister, Query };
ShellHookOp g_shellHookOp = ShellHookOp::None;  // --selftest-shell-* 测试钩子
std::wstring g_shellHookSuffix;                 // 注入的临时键名后缀（TC-I11）

// —— M1-2 菜单自动化验证（--selftest-menu=<结果文件>）状态 ——
bool g_selftestMenu = false;
std::wstring g_selftestOut;
int g_selftestStage = 0;
bool g_stCascade = false, g_stEscapeLevels = false, g_stNoCommand = false;
bool g_stFocusOpen = false, g_stFocusRestore = false;
bool g_stMouseExpand = false, g_stMouseQuit = false;
bool g_stF4BareIgnored = false, g_stF4SysQuit = false;  // P3 回归：F4 形态区分
bool g_stMenuCloseRejected = false;  // P10 回归：菜单 tracking 中 WM_CLOSE 被拒
int g_stProbe[2] = {-1, -1};
ui::Command g_lastRouted = ui::Command::None;

// —— M1-3 Tab 条自动化验证（--selftest-tabs=<结果文件>）状态 ——
bool g_selftestTabs = false;
std::wstring g_selftestTabsOut;
int g_tabsStage = 0;
int g_stActivatedCb = -1;      // 最近一次 onTabActivated 回调参数（自测采样）
int g_stCtxIndex = -1;         // 最近一次 onTabContextCommand 的 index
ui::Command g_stCtxCmd = ui::Command::None;
bool g_stTabsInit = false, g_stTabsClick = false, g_stTabsKeyboard = false;
bool g_stTabsContext = false, g_stTabsOverflow = false, g_stTabsWelcome = false;

// —— M2-B 文档全链路自动化验证（--selftest-doc=<结果文件>）状态 ——
bool g_selftestDoc = false;
std::wstring g_selftestDocOut;
int g_docStage = 0;
std::filesystem::path g_selftestBaseDir;  // 结果文件所在目录（临时文件落点）

// —— M3-B 查找替换 + 多光标自动化验证（--selftest-search=<结果文件>）状态 ——
bool g_selftestSearch = false;
std::wstring g_selftestSearchOut;
int g_searchStage = 0;
std::wstring g_searchDetail;  // 各阶段采样明细（失败定位；写 <结果文件>.detail.txt）

// —— M6 主题自动化验证（--selftest-theme=<结果文件>）状态 ——
bool g_selftestTheme = false;
std::wstring g_selftestThemeOut;
int g_themeStage = 0;
std::wstring g_themeDetail;  // 采样明细（像素/DWM/Scintilla 色；失败定位）

// ---------------------------------------------------------------------------
// 状态查询/刷新（事件驱动）
// ---------------------------------------------------------------------------

// 持久化总开关：自测模式（menu/tabs/doc/search）完全跳过会话/草稿读写——
// 保持既有自测语义不被真实用户目录数据破坏，也不向真实用户目录写入
bool SelftestModeActive() {
    return g_selftestMenu || g_selftestTabs || g_selftestDoc || g_selftestSearch ||
           g_selftestTheme;
}
bool PersistenceActive() {
    return !SelftestModeActive();
}

/// 注入时钟（epoch ms；savedAt 与防抖共用，旧版 QDateTime 语义）
long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// 当前 exe 完整路径（运行时解析；shell 注册/D5 便携判定共用。缓冲给足
/// 长路径余量，截断时回退空路径——调用方写注册表失败会弹错误框）
std::filesystem::path ExePathOf() {
    std::wstring buffer(0x8000, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buffer.data(),
                                       static_cast<DWORD>(buffer.size()));
    if (n == 0 || n >= buffer.size()) return {};
    buffer.resize(n);
    return std::filesystem::path(buffer);
}

// ---------------------------------------------------------------------------
// 会话持久化（构建 = 文档快照 + 本层维护的窗口几何/全局设置）
// ---------------------------------------------------------------------------

/// 立即落盘当前会话（文档 + normal 几何 + 最大化标志 + wordWrap/darkTheme）
void PersistSessionNow() {
    if (!PersistenceActive()) return;
    core::SessionData data = core::SessionManager::buildSessionData(g_docManager);
    data.window = g_lastNormal;
    data.window->maximized = g_lastMaximized;
    data.wordWrap = g_wordWrap;
    data.darkTheme = ui::IsDarkTheme();
    core::SessionManager::persistNow(g_paths.iniPath, data);
}

/// 会话防抖触发（documentsChanged/activeDocumentChanged/documentMetadataChanged/
/// 关闭/几何变化；恢复期 g_restoring 屏蔽）
void TriggerSessionSave() {
    if (!PersistenceActive() || g_restoring) return;
    g_sessionDebounce.trigger(NowMs());
}

int IndexOfDocument(core::Document* doc) {
    if (!doc) return -1;
    const auto docs = g_docManager.documents();
    for (size_t i = 0; i < docs.size(); ++i) {
        if (docs[i] == doc) return static_cast<int>(i);
    }
    return -1;
}

bool UiLocked() {
    // 查找对话框打开同属模态锁定（C4：模态期间拒绝 WM_CLOSE）；
    // 菜单 tracking（模态弹出中）同锁：WM_CLOSE 拒绝 + 转发消化走 200ms
    // 重试路径推迟（kMsgDrainForwardedPaths 的 UiLocked 分支为重试非丢弃）
    return g_dialogHost.active() || g_fileDialogOpen || g_searchDialog.active() ||
           g_menuHost.tracking();
}

// 窗口标题：「文档名 *" - qcnote」（旧 QML title 绑定同构；无文档 "qcnote"）
void RefreshWindowTitle() {
    if (!g_mainWindow) return;
    std::wstring title = L"qcnote";
    if (core::Document* doc = g_docManager.activeDocument()) {
        title = doc->displayName();
        if (doc->modified()) title += L" *";
        title += L" - qcnote";
    }
    SetWindowTextW(g_mainWindow, title.c_str());
}

// 主菜单状态源刷新（弹出前取值；BuildMainMenu 为纯函数）
void RefreshMenuState() {
    core::Document* doc = g_docManager.activeDocument();
    g_menuState.hasActiveDocument = doc != nullptr;
    g_menuState.activeDocumentHasUrl = doc && !doc->url().empty();
    g_menuState.currentEncoding =
        doc && doc->encoding() == core::TextEncoding::Gbk
            ? ui::MainMenuState::Encoding::Gbk
            : ui::MainMenuState::Encoding::Utf8;
    g_menuState.darkTheme = ui::IsDarkTheme();
    g_menuState.wordWrap = g_wordWrap;
}

// ---------------------------------------------------------------------------
// 主题切换编排（E5）：token → 全部控件重绘 → DWM 属性 → Scintilla 重配置，
// 一次完成无中间态。UI 绘制层全部经 CurrentPalette() 动态取色，故除 tooltip
// 换肤与编辑器/系统表面重配置外无须逐控件通知——统一 InvalidateRect 全客户区
// + RedrawWindow 非客户区即可。
// ---------------------------------------------------------------------------

void ApplyThemeToWindowTree() {
    if (!g_mainWindow) return;
    // 1) DWM 沉浸式暗色（标题栏阴影/边框等系统表面）+ DarkMode_Explorer
    //    视觉样式类（Win11 深色滚动条路径的主窗口侧）
    ui::SetImmersiveDark(g_mainWindow, ui::IsDarkTheme());
    ui::ApplyDarkExplorerThemeClass(g_mainWindow, ui::IsDarkTheme());
    // 2) Scintilla 全部实例重配色 + 滚动条视觉样式类（retheme 内含重绘）
    g_editors.retheme();
    // 3) 系统 tooltip 控件换肤（Tab 条/工具栏/状态栏各自持有实例）
    g_tabs.onThemeChanged();
    g_toolbar.onThemeChanged();
    g_statusBar.onThemeChanged();
    // 4) 整窗重绘：客户区全量（自绘控件全部经 CurrentPalette 取新值）+ 非客户
    //    区（FRAMECHANGED 触发 NC 重算）+ 同步更新（缩小中间态窗口）
    SetWindowPos(g_mainWindow, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
    RedrawWindow(g_mainWindow, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN |
                     RDW_UPDATENOW);
}

// 主题切换入口（菜单命令/启动恢复共用）：翻转 + 全树应用 + 菜单勾选态 + 会话
// 记忆（500ms 防抖；持久化路径 e2e 由退出前 flush 兜底）
void SetDarkTheme(bool dark) {
    if (ui::IsDarkTheme() == dark) return;
    ui::SetTheme(dark);
    ApplyThemeToWindowTree();
    RefreshMenuState();
    TriggerSessionSave();
}

void RefreshStatusBar() {
    ui::StatusData d;
    if (core::Document* doc = g_docManager.activeDocument()) {
        const editor::EditorStatus st = g_editors.statusOf(doc);
        d.hasDoc = true;
        d.cursorLine = st.line;
        d.cursorColumn = st.column;
        d.selectedChars = st.selectedChars;
        d.totalChars = st.totalChars;
        d.cursorCount = st.cursorCount;
        d.encoding = core::encodingDisplayName(doc->encoding());
        if (doc->encoding() == core::TextEncoding::Utf8 && doc->hasBom()) {
            d.encoding += L" BOM";
        }
        d.lineEnding = core::lineEndingDisplayName(doc->lineEnding());
        if (!doc->url().empty()) d.path = doc->url().wstring();
    }
    g_statusBar.update(d);
}

// Tab 数据 = 管理器文档列表快照（displayName + modified）
void RefreshTabsFromManager() {
    std::vector<ui::TabItem> items;
    for (core::Document* doc : g_docManager.documents()) {
        items.push_back({doc->displayName(), doc->modified()});
    }
    g_tabs.setItems(std::move(items));
}

// 编辑器实例集合与管理器对齐（新增建实例、消失销毁；幂等）
void SyncEditorSet() {
    g_editors.syncToDocuments(g_docManager.documents());
}

void RefreshAllAfterDocumentsChanged() {
    SyncEditorSet();
    RefreshTabsFromManager();
    g_toolbar.setSaveEnabled(g_docManager.activeDocument() != nullptr);
    RefreshWindowTitle();
    RefreshStatusBar();
    if (g_mainWindow) InvalidateRect(g_mainWindow, nullptr, FALSE);
}

void RefreshAllAfterActiveChanged() {
    g_editors.setActiveDocument(g_docManager.activeDocument());
    // B5：活动文档切换 → 查找状态重算（索引归 0；对话框关闭态 F3 沿新文档）
    g_searchCtl.bind(g_docManager.activeDocument(), g_editors.activeWindow());
    const int idx = IndexOfDocument(g_docManager.activeDocument());
    if (idx >= 0) g_tabs.setActiveIndex(idx);  // 程序化激活（不触发回调）
    g_toolbar.setSaveEnabled(g_docManager.activeDocument() != nullptr);
    RefreshWindowTitle();
    RefreshStatusBar();
    RefreshMenuState();
    if (g_mainWindow) InvalidateRect(g_mainWindow, nullptr, FALSE);
}

void OnDocumentsChanged() {
    RefreshAllAfterDocumentsChanged();
    // M4 会话：文档集合（数量/顺序）变化 → 防抖记忆
    TriggerSessionSave();
}
void OnActiveDocumentChanged() {
    RefreshAllAfterActiveChanged();
    // M4 会话：活动标签变化 → 防抖记忆
    TriggerSessionSave();
}
void OnDocumentModifiedChanged(core::Document* doc) {
    const int idx = IndexOfDocument(doc);
    if (idx >= 0) g_tabs.setModified(idx, doc->modified());
    if (doc == g_docManager.activeDocument()) RefreshWindowTitle();
    // M4 草稿：脏→干净翻转（保存/另存为/编码重载路径）→ 删条目（DEC-003）
    if (PersistenceActive() && !g_restoring && !doc->modified()) {
        g_drafts->onModifiedCleared(doc);
    }
}
void OnDocumentContentChanged(core::Document* doc) {
    // 编辑器自身回写不回灌（isApplying 判定）；外部重置（saveAs 遮蔽/编码重载）
    // 重装编辑器文本
    if (!g_editors.isApplying(doc)) {
        g_editors.reloadFromDocument(doc);
    }
    // B5：内容变化 → 查找匹配重算（打开态重画高亮 + 计数；关闭态缓存重算
    // 供 F3 继续——照旧版 contentsChange 恒重算语义）
    if (doc == g_docManager.activeDocument()) {
        g_searchCtl.onContentChanged();
    }
    // M4 草稿：内容变化且脏 → 1000ms 防抖排队写；内容回到基线 → 立即删条目
    //（恢复期 g_restoring 屏蔽；恢复完成后由种子写入补挂，见 RestoreAtStartup）
    if (PersistenceActive() && !g_restoring) {
        g_drafts->onContentChanged(doc, NowMs());
    }
    if (doc == g_docManager.activeDocument()) RefreshStatusBar();
}
void OnDocumentMetadataChanged(core::Document* doc) {
    const int idx = IndexOfDocument(doc);
    if (idx >= 0) g_tabs.setDisplayName(idx, doc->displayName());
    if (doc == g_docManager.activeDocument()) {
        RefreshWindowTitle();
        RefreshStatusBar();
    }
    RefreshMenuState();
    // 光标/显示名等元数据变化 → 会话记忆更新（500ms 防抖）
    TriggerSessionSave();
}

void FocusActiveEditor() {
    if (HWND sci = g_editors.activeWindow()) {
        SetFocus(sci);
    } else if (g_mainWindow) {
        SetFocus(g_mainWindow);
    }
}

// ---------------------------------------------------------------------------
// 查找替换（B5）：SearchDialog 客户端适配（controller + 编辑器 → 对话框）
// ---------------------------------------------------------------------------

class SearchClientAdapter final : public ui::SearchDialogClient {
public:
    std::wstring GetPattern() override {
        return core::utf8ToWide(g_searchCtl.params().pattern);
    }
    void SetPattern(const std::wstring& text) override {
        g_searchCtl.setPattern(core::wideToUtf8(text));
    }
    std::wstring GetReplacement() override {
        return core::utf8ToWide(g_searchCtl.params().replacement);
    }
    void SetReplacement(const std::wstring& text) override {
        g_searchCtl.setReplacement(core::wideToUtf8(text));
    }
    bool GetOption(int idx) override {
        const auto& o = g_searchCtl.params().options;
        return idx == 0 ? o.matchCase : idx == 1 ? o.wholeWord : o.regex;
    }
    void SetOption(int idx, bool on) override {
        auto o = g_searchCtl.params().options;
        if (idx == 0) o.matchCase = on;
        else if (idx == 1) o.wholeWord = on;
        else o.regex = on;
        g_searchCtl.setOptions(o);
    }
    ui::SearchDialogState GetState() override {
        ui::SearchDialogState st;
        st.errorMessage = g_searchCtl.errorMessage();
        st.matchCount = g_searchCtl.matchCount();
        st.currentIndex = g_searchCtl.currentIndex();
        st.overflowed = g_searchCtl.overflowed();
        st.generation = g_searchCtl.generation();
        return st;
    }
    void FindNext() override { g_searchCtl.findNext(); }
    void FindPrev() override { g_searchCtl.findPrev(); }
    void SelectCurrent() override { g_searchCtl.selectCurrent(); }
    bool SelectionIsCurrentMatch() override {
        HWND sci = g_searchCtl.window();
        int start = 0, len = 0;
        if (!sci || !g_searchCtl.currentMatchRange(&start, &len)) return false;
        return static_cast<int>(SendMessageW(sci, SCI_GETSELECTIONSTART, 0, 0)) == start &&
               static_cast<int>(SendMessageW(sci, SCI_GETSELECTIONEND, 0, 0)) ==
                   start + len;
    }
    bool ReplaceCurrent() override { return g_searchCtl.replaceCurrent(); }
    int ReplaceAll() override { return g_searchCtl.replaceAll(); }
    void OnClosed() override {
        g_searchCtl.setHighlightsEnabled(false);  // DEC-009：清高亮（状态保留）
        FocusActiveEditor();
    }
};

SearchClientAdapter g_searchClient;

// Ctrl+F / Ctrl+H / 菜单「查找…/替换…」（无文档无操作，B5）
void DoSearchDialog(bool replaceFocus) {
    core::Document* doc = g_docManager.activeDocument();
    if (!doc) return;
    if (HWND sci = g_editors.activeWindow()) {
        g_multiCursor.collapseIfActive(sci);  // DEC-008：与多光标互斥
        const std::string sel = editor::SelectionTextForSearch(sci);
        if (!sel.empty()) {
            // 预填：编辑器有不含换行的选区 → 填查找框；否则沿用上次查找串
            g_searchCtl.setPattern(sel);
        }
    }
    g_searchCtl.setHighlightsEnabled(true);  // 打开期绘制全部匹配高亮
    g_searchDialog.open(g_mainWindow, replaceFocus);
    FocusActiveEditor();  // 焦点定回编辑器（模态归还后双保险）
}

// F3 / Shift+F3（关闭态；DEC-009：沿用缓存状态跳转，不恢复全部高亮）
void SearchJumpGlobal(bool forward) {
    if (!g_docManager.activeDocument()) return;
    if (!g_searchCtl.hasPattern() || g_searchCtl.matchCount() == 0) return;
    if (forward) g_searchDialog.doFindNext();
    else g_searchDialog.doFindPrev();
}

// ---------------------------------------------------------------------------
// 对话框与文件对话框
// ---------------------------------------------------------------------------

void ShowErrorDialog(const std::wstring& title, const std::wstring& message) {
    ui::DialogSpec spec;
    spec.title = title;
    spec.message = message;
    spec.buttons.push_back({L"确定", 1, true});
    spec.escId = 1;
    g_dialogHost.showModal(g_mainWindow, spec);
    FocusActiveEditor();
}

// 过滤器：DocumentManager::nameFilters（"文本文件 (*.txt)" / "所有文件 (*)"）
// → Win32 双 \0 分隔形态；通配 "*" 映射 "*.*"
std::wstring BuildFileDialogFilter() {
    std::wstring filter;
    for (const std::wstring& f : g_docManager.nameFilters()) {
        filter += f;
        filter.push_back(L'\0');
        std::wstring patterns;
        const size_t open = f.find_last_of(L'(');
        const size_t close = f.find_last_of(L')');
        std::wstring inner = (open != std::wstring::npos && close != std::wstring::npos &&
                              close > open)
                                 ? f.substr(open + 1, close - open - 1)
                                 : L"*";
        std::wstring pat;
        const auto flush = [&]() {
            if (pat.empty()) return;
            if (pat == L"*") pat = L"*.*";
            if (!patterns.empty()) patterns += L";";
            patterns += pat;
            pat.clear();
        };
        for (const wchar_t ch : inner) {
            if (ch == L' ') flush();
            else pat.push_back(ch);
        }
        flush();
        if (patterns.empty()) patterns = L"*.*";
        filter += patterns;
        filter.push_back(L'\0');
    }
    return filter;  // c_str() 终止符补最后一层 \0（双 null 结尾）
}

std::vector<std::filesystem::path> ShowOpenFileDialog() {
    std::vector<std::filesystem::path> paths;
    std::wstring filter = BuildFileDialogFilter();
    auto buffer = std::make_unique_for_overwrite<wchar_t[]>(32768);  // 多选缓冲
    buffer[0] = L'\0';
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_mainWindow;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buffer.get();
    ofn.nMaxFile = 32768;
    ofn.lpstrTitle = L"打开文件";
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST |
                OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    g_fileDialogOpen = true;
    const BOOL ok = GetOpenFileNameW(&ofn);
    g_fileDialogOpen = false;
    FocusActiveEditor();
    if (!ok) return paths;
    // 多选解析：dir\0file1\0file2\0\0；单选：完整路径\0
    const wchar_t* p = buffer.get();
    const std::wstring first(p);
    p += first.size() + 1;
    if (*p == L'\0') {
        paths.push_back(first);  // 单选即完整路径
    } else {
        const std::filesystem::path dir(first);
        while (*p) {
            const std::wstring name(p);
            p += name.size() + 1;
            paths.push_back(dir / name);
        }
    }
    return paths;
}

std::optional<std::filesystem::path> ShowSaveFileDialog(const core::Document* doc) {
    std::wstring filter = BuildFileDialogFilter();
    wchar_t buffer[4096] = {};
    if (doc && !doc->url().empty()) {
        wcsncpy_s(buffer, doc->url().wstring().c_str(), _TRUNCATE);
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_mainWindow;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = 4096;
    ofn.lpstrTitle = L"另存为";
    ofn.lpstrDefExt = L"txt";  // 无扩展名时补默认扩展（旧系统对话框行为）
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY |
                OFN_NOCHANGEDIR | OFN_NOREADONLYRETURN;
    g_fileDialogOpen = true;
    const BOOL ok = GetSaveFileNameW(&ofn);
    g_fileDialogOpen = false;
    FocusActiveEditor();
    if (!ok || !buffer[0]) return std::nullopt;
    return std::filesystem::path(buffer);
}

// ---------------------------------------------------------------------------
// 命令实现（A1 全链路）
// ---------------------------------------------------------------------------

std::wstring SaveTargetLabel(const core::Document* doc) {
    if (!doc) return L"";
    return doc->url().empty() ? doc->displayName() : doc->url().wstring();
}

void DoSaveAsDocument(core::Document* doc) {
    if (!doc) return;
    const auto picked = ShowSaveFileDialog(doc);
    if (!picked) return;  // 取消另存为（M4：关闭流程接续点）
    const core::SaveAsResult r = g_docManager.saveAs(doc, *picked);
    if (!r.written) {
        ShowErrorDialog(L"另存为失败", picked->wstring() + L"\n" + r.error->message);
        return;
    }
    g_editors.markSaved(doc);
    if (r.error) {
        // 写盘成功但被遮蔽文档从磁盘重载失败（极少见；文档本身已保存成功）
        ShowErrorDialog(L"另存为",
                        L"文件已保存，但同路径的另一个已打开文档重新加载失败：\n" +
                            r.error->message);
    }
}

void DoSave() {
    core::Document* doc = g_docManager.activeDocument();
    if (!doc) return;
    const auto r = g_docManager.save(doc);
    if (r) {
        g_editors.markSaved(doc);
        return;
    }
    if (r.error().kind == core::StorageErrorKind::InvalidUrl) {
        DoSaveAsDocument(doc);  // 无路径：转另存为（旧 QML doSave 语义）
        return;
    }
    ShowErrorDialog(L"保存失败", SaveTargetLabel(doc) + L"\n" + r.error().message);
}

void DoOpenPaths(const std::vector<std::filesystem::path>& paths) {
    if (paths.empty()) return;
    const auto errors = g_docManager.openPaths(paths);
    for (const core::OpenError& e : errors) {
        // 打开失败逐个弹错（不中断已成功打开的文档；内容不丢失）
        ShowErrorDialog(L"打开失败", e.path.wstring() + L"\n" + e.message);
    }
}

void DoReloadWithEncoding(core::TextEncoding encoding) {
    core::Document* doc = g_docManager.activeDocument();
    if (!doc || doc->url().empty()) return;  // 未命名禁用（菜单已置灰，防御）
    // B1：有未保存修改先确认（重新加载将丢弃；取消则不动）——干净文档直接重载
    if (doc->modified()) {
        ui::DialogSpec spec;
        spec.title = L"通过编码重新加载";
        spec.message = L"通过 ";
        spec.message += encoding == core::TextEncoding::Gbk ? L"GBK" : L"UTF-8";
        spec.message += L" 重新加载将丢弃未保存的修改";
        spec.buttons.push_back({L"重新加载", 1, true});
        spec.buttons.push_back({L"取消", 2, false});
        spec.escId = 2;
        if (g_dialogHost.showModal(g_mainWindow, spec) != 1) return;
    }
    // 重载后 modified 翻转 false → 草稿条目经 onModifiedCleared 清理（DEC-003）
    const auto r = g_docManager.reloadWithEncoding(doc, encoding);
    if (!r) {
        ShowErrorDialog(L"通过编码重新加载",
                        doc->url().wstring() + L"\n" + r.error().message);
    }
}

void DoSaveWithEncoding(core::TextEncoding encoding) {
    core::Document* doc = g_docManager.activeDocument();
    if (!doc) return;
    const auto r = g_docManager.saveWithEncoding(doc, encoding);
    if (r) {
        g_editors.markSaved(doc);
        return;
    }
    if (r.error().kind == core::StorageErrorKind::InvalidUrl) {
        // 未命名：编码已保留在文档上，转另存为按该编码写出（旧语义）
        DoSaveAsDocument(doc);
        return;
    }
    ShowErrorDialog(L"保存失败", SaveTargetLabel(doc) + L"\n" + r.error().message);
}

// ---------------------------------------------------------------------------
// 关闭流程（A4 三选确认 + A3 批量关闭队列；旧 Main.qml
// closeDocumentWithConfirm/processCloseQueue/finishCloseFlow/resetCloseFlow 移植）
// ---------------------------------------------------------------------------

core::Document* g_closeFlowDoc = nullptr;   // 三选框挂起的文档
std::vector<core::Document*> g_closeQueue;  // 批量关闭待处理队列（左→右序）
core::Document* g_closeAnchor = nullptr;    // 批量关闭完成后的锚点标签（A3）

/// 三选确认框（保存=1 默认/不保存=2/取消=3=Esc）。「保存」与「不保存」经
/// ProcessCloseQueue 续推；「取消」（含 Esc）经 ResetCloseFlow 中止剩余
int ShowUnsavedChoiceDialog(const std::wstring& displayName) {
    ui::DialogSpec spec;
    spec.title = L"未保存的修改";
    spec.message = L"是否要将更改保存到 " + displayName + L"？";
    spec.buttons.push_back({L"保存", 1, true});
    spec.buttons.push_back({L"不保存", 2, false});
    spec.buttons.push_back({L"取消", 3, false});
    spec.escId = 3;
    return g_dialogHost.showModal(g_mainWindow, spec);
}

/// 三选框「保存」路径：保存当前文档。无路径转另存为（取消另存为 = 中止
/// 整个关闭流程，内容保留）；保存失败弹错误并中止（文档不关闭停留）。
/// @return true = 已保存成功（可继续关闭）；false = 中止关闭流程
bool SaveForCloseFlow(core::Document* doc) {
    const auto r = g_docManager.save(doc);
    if (r) {
        g_editors.markSaved(doc);
        return true;
    }
    if (r.error().kind == core::StorageErrorKind::InvalidUrl) {
        DoSaveAsDocument(doc);  // 无路径：转另存为（取消则中止）
        return !doc->modified();  // 另存为成功即干净；取消/失败仍脏 → 中止
    }
    ShowErrorDialog(L"保存失败", SaveTargetLabel(doc) + L"\n" + r.error().message);
    return false;
}

void ProcessCloseQueue();
void FinishCloseBatch();
void ResetCloseFlow() {
    g_closeFlowDoc = nullptr;
    g_closeQueue.clear();
    g_closeAnchor = nullptr;
}

/// 批量收尾：锚点文档仍打开则激活（A3 确定性回到被右键标签；被外部移除时由
/// close 的邻居激活规则兜底）
void FinishCloseBatch() {
    if (g_closeAnchor && IndexOfDocument(g_closeAnchor) >= 0) {
        g_docManager.setActiveDocument(g_closeAnchor);
    }
    g_closeAnchor = nullptr;
}

/// 批量关闭队列状态机：逐个取队首——
/// - 文档已不在列表（处理期间被外部移除，防御）→ 跳过；
/// - 未保存 → 挂起等三选框（「保存/不保存」续推下一个；「取消」中止剩余，
///   已完成部分生效）；
/// - 无修改 → 直接关闭继续下一个；
/// 队列空 → FinishCloseBatch 收尾（锚点仍打开则激活）。
/// 单文档路径（Ctrl+W/菜单/右键「关闭当前文件」）队列为 [doc]、锚点为空，
/// 经此无副作用
void ProcessCloseQueue() {
    while (!g_closeQueue.empty()) {
        core::Document* doc = g_closeQueue.front();
        g_closeQueue.erase(g_closeQueue.begin());
        if (IndexOfDocument(doc) < 0) continue;  // 已被外部移除：跳过
        if (!doc->modified()) {
            g_docManager.closeDocument(doc);  // 干净文档直接关闭
            continue;
        }
        g_closeFlowDoc = doc;
        const int choice = ShowUnsavedChoiceDialog(doc->displayName());
        if (choice == 1) {  // 保存
            if (!SaveForCloseFlow(doc)) {
                ResetCloseFlow();  // 保存失败/取消另存为：中止剩余全部
                return;
            }
            g_closeFlowDoc = nullptr;
            g_docManager.closeDocument(doc);
            continue;
        }
        if (choice == 2) {  // 不保存：直接关闭（documentClosed 事件自动清草稿）
            g_closeFlowDoc = nullptr;
            g_docManager.closeDocument(doc);
            continue;
        }
        // 取消（含 Esc）：中止剩余批量关闭（已关闭的不恢复）
        ResetCloseFlow();
        return;
    }
    FinishCloseBatch();
}

/// 单文档关闭入口（Ctrl+W / 菜单「关闭标签页」/ 右键「关闭当前文件」）：
/// 未保存弹三选框；干净直接关闭。多光标激活先收缩（模态交互前归位单光标）
void CloseDocumentWithConfirm(core::Document* doc) {
    if (!doc) return;
    if (HWND sci = g_editors.activeWindow()) g_multiCursor.collapseIfActive(sci);
    g_closeQueue = {doc};
    g_closeAnchor = nullptr;
    ProcessCloseQueue();
}

/// 批量关闭入口（右键「关闭右边/左边/其他文件」）：docs 从左到右逐个走单
/// 文档关闭语义，全部处理完后激活锚点（被右键的标签）
void CloseRangeWithConfirm(std::vector<core::Document*> docs, core::Document* anchor) {
    if (HWND sci = g_editors.activeWindow()) g_multiCursor.collapseIfActive(sci);
    g_closeQueue = std::move(docs);
    g_closeAnchor = anchor;
    ProcessCloseQueue();
}

void CycleTab(int delta) {
    const int n = static_cast<int>(g_docManager.documentCount());
    if (n == 0) return;
    int i = IndexOfDocument(g_docManager.activeDocument());
    if (i < 0) i = 0;
    g_docManager.activate(static_cast<size_t>(((i + delta) % n + n) % n));
}

void LogStubCommand(ui::Command cmd) {
    std::wstring log = L"[qcnote] command stub: ";
    log += ui::CommandName(cmd);
    log += L"\n";
    OutputDebugStringW(log.c_str());
}

// 命令路由（菜单/工具栏/快捷键统一入口）
void RouteCommand(ui::Command cmd) {
    g_lastRouted = cmd;  // 供自动化验证断言

    // M1 自测模式：保持 stub 语义（除退出），mock 数据不被真实文档系统破坏
    if (g_selftestMenu || g_selftestTabs) {
        if (cmd == ui::Command::Quit && g_mainWindow) {
            PostMessageW(g_mainWindow, WM_CLOSE, 0, 0);
        } else {
            LogStubCommand(cmd);
        }
        return;
    }

    // 模态期间防御性拒绝（正常路径：模态循环已使本函数不可达）
    if (UiLocked() && cmd != ui::Command::Quit) return;

    switch (cmd) {
    case ui::Command::NewDocument:
        g_docManager.newDocument();  // 新建即激活（activeDocumentChanged → 焦点编辑器）
        break;
    case ui::Command::OpenDialog:
        DoOpenPaths(ShowOpenFileDialog());
        break;
    case ui::Command::Save:
        DoSave();
        break;
    case ui::Command::SaveAs:
        DoSaveAsDocument(g_docManager.activeDocument());
        break;
    case ui::Command::CloseTab:
        CloseDocumentWithConfirm(g_docManager.activeDocument());
        break;
    case ui::Command::ReloadWithEncodingUtf8:
        DoReloadWithEncoding(core::TextEncoding::Utf8);
        break;
    case ui::Command::ReloadWithEncodingGbk:
        DoReloadWithEncoding(core::TextEncoding::Gbk);
        break;
    case ui::Command::SaveWithEncodingUtf8:
        DoSaveWithEncoding(core::TextEncoding::Utf8);
        break;
    case ui::Command::SaveWithEncodingGbk:
        DoSaveWithEncoding(core::TextEncoding::Gbk);
        break;
    case ui::Command::FindDialog:
        DoSearchDialog(false);
        break;
    case ui::Command::ReplaceDialog:
        DoSearchDialog(true);
        break;
    case ui::Command::FindNext:
        SearchJumpGlobal(true);
        break;
    case ui::Command::FindPrevious:
        SearchJumpGlobal(false);
        break;
    case ui::Command::ToggleDarkTheme:
        // E5 双主题：token/控件/DWM/Scintilla 一次切换（SetDarkTheme 编排）
        SetDarkTheme(!ui::IsDarkTheme());
        break;
    case ui::Command::ToggleWordWrap:
        g_wordWrap = !g_wordWrap;
        g_editors.setWordWrap(g_wordWrap);  // 全局（旧版所有文档共享）
        RefreshMenuState();
        break;
    case ui::Command::Quit:
        // C4 热退出：无模态对话框直接退出免提示（WM_CLOSE 内 flush 草稿+会话）
        if (g_mainWindow) PostMessageW(g_mainWindow, WM_CLOSE, 0, 0);
        break;
    default:
        LogStubCommand(cmd);
        break;
    }
}

// ---------------------------------------------------------------------------
// Tab 条路由（激活/右键菜单批量关闭）
// ---------------------------------------------------------------------------

void OnTabContextCommand(int index, ui::Command cmd) {
    g_stCtxIndex = index;  // 自测采样
    g_stCtxCmd = cmd;

    // M1 Tab 自测模式：mock 数据上的 M1 行为（直接移除 + 锚点激活）
    if (g_selftestTabs) {
        const int count = g_tabs.count();
        switch (cmd) {
        case ui::Command::CloseCurrentTab:
            if (index >= 0 && index < count) g_tabs.removeTab(index);
            break;
        case ui::Command::CloseTabsToRight:
            if (index >= 0 && index < count) g_tabs.removeRange(index + 1, count);
            break;
        case ui::Command::CloseTabsToLeft:
            if (index > 0 && index < count) g_tabs.removeRange(0, index);
            break;
        case ui::Command::CloseOtherTabs:
            for (int i = count - 1; i >= 0; --i) {
                if (i != index) g_tabs.removeTab(i);
            }
            break;
        default:
            break;
        }
        if (cmd != ui::Command::CloseCurrentTab && index >= 0 && index < g_tabs.count()) {
            g_tabs.setActiveIndex(index);  // A3 锚点
        }
        if (g_tabs.count() > 0) g_welcome.reset();
        return;
    }

    const auto docs = g_docManager.documents();
    const int count = static_cast<int>(docs.size());
    if (index < 0 || index >= count) return;
    core::Document* anchor = docs[static_cast<size_t>(index)];

    // A3 批量关闭：左→右逐个三选确认（「取消」中止剩余，已完成部分生效），
    // 全部走完后激活锚点标签（FinishCloseBatch）
    std::vector<core::Document*> range;
    switch (cmd) {
    case ui::Command::CloseCurrentTab:
        CloseDocumentWithConfirm(anchor);
        return;  // 关闭当前无锚点（邻居激活规则兜底）
    case ui::Command::CloseTabsToRight:
        for (int i = index + 1; i < count; ++i) range.push_back(docs[static_cast<size_t>(i)]);
        break;
    case ui::Command::CloseTabsToLeft:
        for (int i = 0; i < index; ++i) range.push_back(docs[static_cast<size_t>(i)]);
        break;
    case ui::Command::CloseOtherTabs:
        for (int i = 0; i < count; ++i) {
            if (i != index) range.push_back(docs[static_cast<size_t>(i)]);
        }
        break;
    default:
        return;
    }
    CloseRangeWithConfirm(std::move(range), anchor);
}

void OnTabActivated(int index) {
    g_stActivatedCb = index;  // 自测采样
    if (index < 0 || index >= static_cast<int>(g_docManager.documentCount())) return;
    g_docManager.activate(static_cast<size_t>(index));
}

// ---------------------------------------------------------------------------
// 主菜单弹出（标题栏菜单按钮 → 主菜单，锚点=按钮左下角）
// ---------------------------------------------------------------------------

ui::Command OpenMainMenu(const ui::MainMenuState& state = g_menuState) {
    if (!g_mainWindow) return ui::Command::None;
    RefreshMenuState();  // 每次打开前按当前文档状态刷新（勾选/置灰实时）
    const ui::TitleBarLayout layout = g_titleBar.layout();
    POINT anchor{layout.menuButton.left, layout.menuButton.bottom};
    ClientToScreen(g_mainWindow, &anchor);
    return g_menuHost.track(g_mainWindow, ui::BuildMainMenu(state), anchor);
}

// ---------------------------------------------------------------------------
// 布局（标题栏 → 工具栏 → 编辑区/欢迎页 → 状态栏）
// ---------------------------------------------------------------------------

// 编辑区矩形（客户区坐标）：工具栏底 ~ 状态栏顶
RECT EditorContentRect(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const UINT dpi = ui::WindowDpi(hwnd);
    const int top = client.top + ui::Scale(ui::metrics::CaptionHeight, dpi) +
                    ui::Scale(ui::toolbar_metrics::Height, dpi);
    const int bottom = client.bottom - ui::Scale(ui::statusbar_metrics::Height, dpi);
    return {client.left, top, client.right, bottom};
}

// ---------------------------------------------------------------------------
// 窗口级快捷键（消息循环拦截；详见文件头注释）
// ---------------------------------------------------------------------------

bool HandleGlobalShortcut(UINT vk) {
    if (UiLocked()) return false;
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (ctrl && !shift) {
        switch (vk) {
        case 'N': RouteCommand(ui::Command::NewDocument); return true;
        case 'O': RouteCommand(ui::Command::OpenDialog); return true;
        case 'S': RouteCommand(ui::Command::Save); return true;
        case 'W': RouteCommand(ui::Command::CloseTab); return true;
        case 'F': RouteCommand(ui::Command::FindDialog); return true;  // M3 占位
        case 'H': RouteCommand(ui::Command::ReplaceDialog); return true;  // M3 占位
        case VK_TAB: CycleTab(+1); return true;
        default: return false;
        }
    }
    if (ctrl && shift) {
        switch (vk) {
        case 'S': RouteCommand(ui::Command::SaveAs); return true;
        case VK_TAB: CycleTab(-1); return true;
        default: return false;
        }
    }
    if (!ctrl && vk == VK_F3) {
        RouteCommand(shift ? ui::Command::FindPrevious : ui::Command::FindNext);  // M3 占位
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// M1-2 菜单自动化验证（--selftest-menu[=<结果文件>]）
// ---------------------------------------------------------------------------

void RunMenuSelftestStage() {
    if (g_selftestStage == 0) {
        g_selftestStage = 1;
        // 场景 A：三级级联 文件▸编码▸通过编码重新加载▸GBK → 触发命令（键盘）
        ui::MainMenuState st;
        st.hasActiveDocument = true;
        st.activeDocumentHasUrl = true;
        st.currentEncoding = ui::MainMenuState::Encoding::Gbk;
        g_menuHost.onOpened = [] {
            g_stFocusOpen = (GetFocus() == g_mainWindow);
            HWND root = g_menuHost.rootPopupForTest();
            if (!root) return;
            for (int i = 0; i < 10; ++i) PostMessageW(root, WM_KEYDOWN, VK_DOWN, 0);
            PostMessageW(root, WM_KEYDOWN, VK_RETURN, 0);
        };
        g_stCascade = (OpenMainMenu(st) == ui::Command::ReloadWithEncodingGbk);
        g_stFocusRestore = (GetFocus() == g_mainWindow);
        PostMessageW(g_mainWindow, kMsgSelftestStage, 0, 0);
    } else if (g_selftestStage == 1) {
        g_selftestStage = 2;
        // 场景 B：默认状态主菜单，Escape 逐级关闭 + 无命令触发
        g_menuHost.onAppMessage = [](int code, WPARAM, LPARAM) -> LRESULT {
            if (code >= 0 && code < 2) g_stProbe[code] = g_menuHost.openLevelCountForTest();
            return 0;
        };
        g_menuHost.onOpened = [] {
            HWND root = g_menuHost.rootPopupForTest();
            if (!root) return;
            PostMessageW(root, WM_KEYDOWN, VK_DOWN, 0);
            PostMessageW(root, WM_APP + 0, 0, 0);
            PostMessageW(root, WM_KEYDOWN, VK_ESCAPE, 0);
            PostMessageW(root, WM_APP + 1, 0, 0);
            PostMessageW(root, WM_KEYDOWN, VK_ESCAPE, 0);
        };
        g_lastRouted = ui::Command::None;
        const ui::Command cmd = OpenMainMenu();
        g_stEscapeLevels = (g_stProbe[0] == 2 && g_stProbe[1] == 1);
        g_stNoCommand = (cmd == ui::Command::None && g_lastRouted == ui::Command::None);
        g_stFocusRestore = g_stFocusRestore && (GetFocus() == g_mainWindow);
        g_menuHost.onAppMessage = nullptr;
        PostMessageW(g_mainWindow, kMsgSelftestStage, 0, 0);
    } else if (g_selftestStage == 2) {
        g_selftestStage = 3;
        // 场景 C：鼠标点击路径（布局数学计算行 Y）
        const UINT dpi = ui::WindowDpi(g_mainWindow);
        const int ih = ui::Scale(ui::menu_metrics::ItemHeight, dpi);
        const int sh = ui::Scale(ui::menu_metrics::SeparatorHeight, dpi);
        const int ySearch = 1 + ih + ih / 2;
        const int yQuit = 1 + 3 * ih + 2 * sh + ih / 2;
        const int xMid = ui::Scale(60, dpi);
        g_menuHost.onAppMessage = [](int code, WPARAM, LPARAM) -> LRESULT {
            if (code == 0) g_stProbe[0] = g_menuHost.openLevelCountForTest();
            return 0;
        };
        g_menuHost.onOpened = [ySearch, yQuit, xMid] {
            HWND root = g_menuHost.rootPopupForTest();
            if (!root) return;
            PostMessageW(root, WM_MOUSEMOVE, 0, MAKELPARAM(xMid, ySearch));
            PostMessageW(root, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(xMid, ySearch));
            PostMessageW(root, WM_APP + 0, 0, 0);
            PostMessageW(root, WM_LBUTTONUP, 0, MAKELPARAM(xMid, ySearch));
            PostMessageW(root, WM_MOUSEMOVE, 0, MAKELPARAM(xMid, yQuit));
            PostMessageW(root, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(xMid, yQuit));
            PostMessageW(root, WM_LBUTTONUP, 0, MAKELPARAM(xMid, yQuit));
        };
        g_lastRouted = ui::Command::None;
        const ui::Command cmd = OpenMainMenu();
        g_stMouseExpand = (g_stProbe[0] == 2);
        g_stMouseQuit = (cmd == ui::Command::Quit && g_lastRouted == ui::Command::Quit);
        g_menuHost.onAppMessage = nullptr;

        // 场景 D（P3 回归）：单按 F4（WM_KEYDOWN 形态）不退出也不关菜单；
        // Alt+F4（WM_SYSKEYDOWN 形态）关菜单并按退出路由。
        // 同场（P10 回归）：菜单 tracking（模态）期间主窗口 WM_CLOSE 被拒
        g_menuHost.onAppMessage = [](int code, WPARAM, LPARAM) -> LRESULT {
            if (code == 0) g_stProbe[0] = g_menuHost.openLevelCountForTest();
            return 0;
        };
        g_menuHost.onOpened = [] {
            HWND root = g_menuHost.rootPopupForTest();
            if (!root) return;
            g_stProbe[0] = -1;
            PostMessageW(root, WM_KEYDOWN, VK_F4, 0);  // 单按 F4：忽略
            // 菜单 tracking 中 WM_CLOSE：UiLocked 拒绝（posted 队列 FIFO，
            // 先于下面的采样消息处理）
            PostMessageW(g_mainWindow, WM_CLOSE, 0, 0);
            PostMessageW(root, WM_APP + 0, 0, 0);      // 采样：菜单应仍打开
            PostMessageW(root, WM_KEYDOWN, VK_ESCAPE, 0);  // 正常关闭（无命令）
        };
        g_lastRouted = ui::Command::None;
        g_stF4BareIgnored = (OpenMainMenu() == ui::Command::None &&
                             g_lastRouted == ui::Command::None && g_stProbe[0] == 1);
        g_stMenuCloseRejected = IsWindow(g_mainWindow) != FALSE;  // tracking 中关闭被拒
        g_menuHost.onOpened = [] {
            HWND root = g_menuHost.rootPopupForTest();
            if (!root) return;
            PostMessageW(root, WM_SYSKEYDOWN, VK_F4, 0);  // Alt+F4 形态：退出
        };
        g_stF4SysQuit = (OpenMainMenu() == ui::Command::Quit);
        g_menuHost.onAppMessage = nullptr;
        g_menuHost.onOpened = [] {};

        const bool all = g_stCascade && g_stEscapeLevels && g_stNoCommand && g_stFocusOpen &&
                         g_stFocusRestore && g_stMouseExpand && g_stMouseQuit &&
                         g_stF4BareIgnored && g_stF4SysQuit && g_stMenuCloseRejected;
        std::error_code ec;
        std::filesystem::path out(g_selftestOut);
        if (g_selftestOut.empty()) out = std::filesystem::current_path(ec) / "qcnote-menu-selftest.txt";
        std::ofstream f(out, std::ios::trunc);
        f << "scenario=A three-level cascade select (file>encoding>reload>gbk): "
          << (g_stCascade ? "PASS" : "FAIL") << "\n";
        f << "scenario=A focus stays on owner while menu open: "
          << (g_stFocusOpen ? "PASS" : "FAIL") << "\n";
        f << "scenario=B escape collapses level-wise (2->1->0): "
          << (g_stEscapeLevels ? "PASS" : "FAIL") << "\n";
        f << "scenario=B no command on escape: " << (g_stNoCommand ? "PASS" : "FAIL") << "\n";
        f << "scenario=B focus restored to main window: "
          << (g_stFocusRestore ? "PASS" : "FAIL") << "\n";
        f << "scenario=C mouse click expands submenu: "
          << (g_stMouseExpand ? "PASS" : "FAIL") << "\n";
        f << "scenario=C mouse click triggers command (quit): "
          << (g_stMouseQuit ? "PASS" : "FAIL") << "\n";
        f << "scenario=D bare F4 (WM_KEYDOWN) ignored, menu stays open: "
          << (g_stF4BareIgnored ? "PASS" : "FAIL") << "\n";
        f << "scenario=D Alt+F4 (WM_SYSKEYDOWN) routes quit: "
          << (g_stF4SysQuit ? "PASS" : "FAIL") << "\n";
        f << "scenario=D menu tracking rejects WM_CLOSE on main window: "
          << (g_stMenuCloseRejected ? "PASS" : "FAIL") << "\n";
        f << "overall: " << (all ? "PASS" : "FAIL") << "\n";
        f.close();
        PostMessageW(g_mainWindow, WM_CLOSE, 0, 0);
    }
}

// 解析 --selftest-menu / --selftest-tabs / --selftest-doc（[=<结果文件>]；互斥，menu 优先）
void ParseSelftestArg(PWSTR cmdLine) {
    struct Mode {
        const wchar_t* name;
        bool* flag;
        std::wstring* out;
    };
    const Mode modes[] = {
        {L"--selftest-menu", &g_selftestMenu, &g_selftestOut},
        {L"--selftest-tabs", &g_selftestTabs, &g_selftestTabsOut},
        {L"--selftest-doc", &g_selftestDoc, &g_selftestDocOut},
        {L"--selftest-search", &g_selftestSearch, &g_selftestSearchOut},
        {L"--selftest-theme", &g_selftestTheme, &g_selftestThemeOut},
    };
    for (const Mode& m : modes) {
        const wchar_t* p = wcsstr(cmdLine, m.name);
        if (!p) continue;
        *m.flag = true;
        const wchar_t* end = wcschr(p, L' ');
        if (!end) end = p + wcslen(p);
        const wchar_t* eq = wcschr(p, L'=');
        if (eq && eq < end) {
            std::wstring path(eq + 1, end);
            if (!path.empty() && path.front() == L'"') path.erase(path.begin());
            if (!path.empty() && path.back() == L'"') path.pop_back();
            *m.out = path;
        }
        break;  // 互斥：首个命中生效
    }
    if (g_selftestDoc && !g_selftestDocOut.empty()) {
        std::error_code ec;
        g_selftestBaseDir = std::filesystem::path(g_selftestDocOut).parent_path();
        if (g_selftestBaseDir.empty()) {
            g_selftestBaseDir = std::filesystem::current_path(ec);
        }
    }}

// ---------------------------------------------------------------------------
// M1-3 Tab 条自动化验证（--selftest-tabs[=<结果文件>]）
// ---------------------------------------------------------------------------

void RunTabsSelftestStage() {
    if (!g_mainWindow) {
        return;
    }
    const UINT dpi = ui::WindowDpi(g_mainWindow);
    const int capH = ui::Scale(ui::metrics::CaptionHeight, dpi);

    if (g_tabsStage == 0) {
        g_tabsStage = 1;
        // —— 场景 1：初始 mock 状态与几何 ——
        ui::TabStripGeometry geo = g_tabs.geometry();
        const int w0 = geo.tabs[0].bounds.right - geo.tabs[0].bounds.left;
        const int w1 = geo.tabs[1].bounds.right - geo.tabs[1].bounds.left;
        const int w2 = geo.tabs[2].bounds.right - geo.tabs[2].bounds.left;
        const int gaps = ui::Scale(ui::tab_metrics::Spacing, dpi) * 2;
        const int minW = ui::Scale(ui::tab_metrics::MinWidth, dpi);
        g_stTabsInit =
            g_tabs.count() == 3 && g_tabs.activeIndex() == 0 && g_tabs.focusIndex() == 0 &&
            g_tabs.item(0).modified && !g_tabs.item(1).modified &&
            g_tabs.item(0).displayName == L"未命名-1" &&
            g_tabs.item(1).displayName == L"readme.md" &&
            g_tabs.item(2).displayName == L"很长的中文文件名测试用例.txt" &&
            static_cast<int>(geo.tabs.size()) == 3 && !geo.overflow && geo.maxScroll == 0 &&
            w0 >= minW && w1 >= minW && w2 >= minW && (w0 + w1 + w2 + gaps) <= geo.viewWidth &&
            w2 > w1 &&
            !geo.tabs[0].separatorLeft && !geo.tabs[1].separatorLeft &&
            geo.tabs[2].separatorLeft;
        PostMessageW(g_mainWindow, kMsgTabsSelftestStage, 0, 0);
    } else if (g_tabsStage == 1) {
        g_tabsStage = 2;
        // —— 场景 2：左键点击激活 ——
        ui::TabStripGeometry geo = g_tabs.geometry();
        const RECT& b = geo.tabs[2].bounds;
        const POINT pt{(b.left + b.right) / 2, (b.top + b.bottom) / 2};
        g_stActivatedCb = -1;
        g_tabs.onLButtonDown(pt);
        geo = g_tabs.geometry();
        g_stTabsClick = g_tabs.activeIndex() == 2 && g_stActivatedCb == 2 &&
                        g_tabs.focusIndex() == 2 &&
                        geo.tabs[1].separatorLeft &&
                        !geo.tabs[2].separatorLeft;
        PostMessageW(g_mainWindow, kMsgTabsSelftestStage, 0, 0);
    } else if (g_tabsStage == 2) {
        g_tabsStage = 3;
        // —— 场景 3：键盘导航循环 ——
        g_tabs.onKeyDown(VK_LEFT, false);
        bool ok = g_tabs.focusIndex() == 1 && g_tabs.activeIndex() == 2;
        g_tabs.onKeyDown(VK_LEFT, false);
        ok = ok && g_tabs.focusIndex() == 0;
        g_tabs.onKeyDown(VK_LEFT, false);
        ok = ok && g_tabs.focusIndex() == 2;
        g_stActivatedCb = -1;
        g_tabs.onKeyDown(VK_RETURN, false);
        ok = ok && g_tabs.activeIndex() == 2 && g_stActivatedCb == 2;
        g_tabs.onKeyDown(VK_RIGHT, false);
        ok = ok && g_tabs.focusIndex() == 0;
        g_stTabsKeyboard = ok;
        PostMessageW(g_mainWindow, kMsgTabsSelftestStage, 0, 0);
    } else if (g_tabsStage == 3) {
        g_tabsStage = 4;
        // —— 场景 4：右键菜单弹出与命令回调 ——
        ui::TabStripGeometry geo = g_tabs.geometry();
        const RECT& b = geo.tabs[0].bounds;
        const POINT pt{(b.left + b.right) / 2, (b.top + b.bottom) / 2};
        g_menuHost.onOpened = [] {
            HWND root = g_menuHost.rootPopupForTest();
            if (!root) return;
            PostMessageW(root, WM_KEYDOWN, VK_DOWN, 0);
            PostMessageW(root, WM_KEYDOWN, VK_DOWN, 0);
            PostMessageW(root, WM_KEYDOWN, VK_RETURN, 0);
        };
        g_stCtxIndex = -1;
        g_stCtxCmd = ui::Command::None;
        g_tabs.onRButtonDown(pt);
        g_menuHost.onOpened = [] {};
        g_stTabsContext =
            g_stCtxIndex == 0 && g_stCtxCmd == ui::Command::CloseTabsToRight &&
            g_tabs.count() == 1 && g_tabs.activeIndex() == 0 &&
            g_tabs.item(0).displayName == L"未命名-1";
        PostMessageW(g_mainWindow, kMsgTabsSelftestStage, 0, 0);
    } else if (g_tabsStage == 4) {
        g_tabsStage = 5;
        // —— 场景 5：超量溢出 + 横向滚动 + 命中路由 ——
        for (int i = 0; i < 20; ++i) {
            g_tabs.addTab({L"溢出测试文档-" + std::to_wstring(i) + L".txt", i % 3 == 0});
        }
        ui::TabStripGeometry geo = g_tabs.geometry();
        bool ok = g_tabs.count() == 21 && geo.overflow && geo.maxScroll > 0 &&
                  g_tabs.scrollOffset() == 0;
        POINT blank{geo.area.left + ui::Scale(10, dpi), ui::Scale(ui::metrics::ResizeEdge, dpi)};
        POINT s = blank;
        ClientToScreen(g_mainWindow, &s);
        ok = ok && SendMessageW(g_mainWindow, WM_NCHITTEST, 0,
                                MAKELPARAM(s.x, s.y)) == HTCLIENT;
        g_tabs.onMouseWheel(true, WHEEL_DELTA);
        ok = ok && g_tabs.scrollOffset() > 0;
        for (int i = 0; i < 200; ++i) {
            g_tabs.onMouseWheel(true, WHEEL_DELTA);
        }
        ok = ok && g_tabs.scrollOffset() == geo.maxScroll;
        for (int i = 0; i < 500; ++i) {
            g_tabs.onMouseWheel(true, -WHEEL_DELTA);
        }
        ok = ok && g_tabs.scrollOffset() == 0;
        g_stTabsOverflow = ok;
        PostMessageW(g_mainWindow, kMsgTabsSelftestStage, 0, 0);
    } else if (g_tabsStage == 5) {
        g_tabsStage = 6;
        // —— 场景 6：清空 → 欢迎页 → 重新出现 Tab ——
        g_tabs.removeRange(0, g_tabs.count());
        bool ok = g_tabs.count() == 0 && g_tabs.activeIndex() == -1 && g_tabs.focusIndex() == -1;
        RECT br = g_welcome.buttonRect();
        const POINT bc{(br.left + br.right) / 2, (br.top + br.bottom) / 2};
        ok = ok && br.right > br.left && g_welcome.hitButton(bc);
        g_lastRouted = ui::Command::None;
        g_welcome.onLButtonDown(bc);
        ok = ok && g_lastRouted == ui::Command::OpenDialog;
        ok = ok && g_welcome.onKeyDown(VK_TAB, false) && g_welcome.buttonFocusedForTest();
        g_lastRouted = ui::Command::None;
        ok = ok && g_welcome.onKeyDown(VK_RETURN, false) &&
             g_lastRouted == ui::Command::OpenDialog;
        POINT cap{ui::Scale(500, dpi), capH / 2};
        POINT cs = cap;
        ClientToScreen(g_mainWindow, &cs);
        ok = ok && SendMessageW(g_mainWindow, WM_NCHITTEST, 0,
                                MAKELPARAM(cs.x, cs.y)) == HTCAPTION;
        g_tabs.addTab({L"新文档.md", false});
        g_tabs.setActiveIndex(0);
        ok = ok && g_tabs.count() == 1 && g_tabs.activeIndex() == 0;
        ui::TabStripGeometry geo = g_tabs.geometry();
        const RECT& tb = geo.tabs[0].bounds;
        const POINT tc{(tb.left + tb.right) / 2, (tb.top + tb.bottom) / 2};
        POINT ts = tc;
        ClientToScreen(g_mainWindow, &ts);
        ok = ok && SendMessageW(g_mainWindow, WM_NCHITTEST, 0,
                                MAKELPARAM(ts.x, ts.y)) == HTCLIENT;
        g_stTabsWelcome = ok;
        PostMessageW(g_mainWindow, kMsgTabsSelftestStage, 0, 0);
    } else if (g_tabsStage == 6) {
        g_tabsStage = 7;
        const bool all = g_stTabsInit && g_stTabsClick && g_stTabsKeyboard && g_stTabsContext &&
                         g_stTabsOverflow && g_stTabsWelcome;
        std::error_code ec;
        std::filesystem::path out(g_selftestTabsOut);
        if (g_selftestTabsOut.empty()) {
            out = std::filesystem::current_path(ec) / "qcnote-tabs-selftest.txt";
        }
        std::ofstream f(out, std::ios::trunc);
        f << "scenario=initial mock state and geometry (3 tabs, separators, widths): "
          << (g_stTabsInit ? "PASS" : "FAIL") << "\n";
        f << "scenario=left click activates tab (active/focus follow, separators flip): "
          << (g_stTabsClick ? "PASS" : "FAIL") << "\n";
        f << "scenario=keyboard navigation cycles without activation, enter activates: "
          << (g_stTabsKeyboard ? "PASS" : "FAIL") << "\n";
        f << "scenario=tab context menu popup and close-right command callback: "
          << (g_stTabsContext ? "PASS" : "FAIL") << "\n";
        f << "scenario=overflow horizontal scroll with clamping and hit routing: "
          << (g_stTabsOverflow ? "PASS" : "FAIL") << "\n";
        f << "scenario=clear all shows welcome page, open button routes OpenDialog: "
          << (g_stTabsWelcome ? "PASS" : "FAIL") << "\n";
        f << "overall: " << (all ? "PASS" : "FAIL") << "\n";
        f.close();
        PostMessageW(g_mainWindow, WM_CLOSE, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// M2-B 文档全链路自动化验证（--selftest-doc[=<结果文件>]，可携带位置参数文件）
// ---------------------------------------------------------------------------

// 断言采样集合
bool g_stDocArgOpen = false, g_stDocNew = false, g_stDocType = false, g_stDocSwitch = false;
bool g_stDocSaveAs = false, g_stDocGbk = false, g_stDocReopen = false, g_stDocStatus = false;
bool g_stDocDialog = false, g_stDocIme = false, g_stDocWrap = false, g_stDocCloseAll = false;
bool g_stDocToolbar = false, g_stDocErrorDlg = false;
std::wstring g_docDetail;  // 各阶段采样明细（失败定位；写 <结果文件>.detail.txt）

// 辅助：读编辑器全文（UTF-8）
std::string EditorTextOf(core::Document* doc) {
    HWND sci = g_editors.windowForTest(doc);
    if (!sci) return {};
    const int len = static_cast<int>(SendMessageW(sci, SCI_GETLENGTH, 0, 0));
    std::string out(static_cast<size_t>(len) + 1, '\0');
    SendMessageW(sci, SCI_GETTEXT, out.size(), reinterpret_cast<LPARAM>(out.data()));
    out.resize(static_cast<size_t>(len));
    return out;
}

// 辅助：向编辑器同步投递一串宽字符（WM_CHAR 直投；等效 IME 上屏/键盘输入路径）
void TypeIntoEditor(core::Document* doc, const std::wstring& text) {
    HWND sci = g_editors.windowForTest(doc);
    if (!sci) return;
    for (const wchar_t ch : text) {
        SendMessageW(sci, WM_CHAR, ch, 0);
    }
}

// 辅助：读文件字节
std::string ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void WriteDocSelftestResult() {
    std::error_code ec;
    std::filesystem::path out(g_selftestDocOut);
    if (g_selftestDocOut.empty()) {
        out = std::filesystem::current_path(ec) / "qcnote-doc-selftest.txt";
    }
    const std::pair<const char*, bool*> named[] = {
        {"startup: positional arg opens document (url/content/tab/title)", &g_stDocArgOpen},
        {"new document x3: untitled naming, activation, editor focus", &g_stDocNew},
        {"typing via WM_CHAR syncs content incrementally (modified/tab dot/title)", &g_stDocType},
        {"tab switching preserves per-document content, cursor and undo", &g_stDocSwitch},
        {"saveAs writes utf-8 lf to disk, clears dirty, updates tab/title", &g_stDocSaveAs},
        {"saveWithEncoding gbk: encoding retained on untitled, gbk bytes on disk", &g_stDocGbk},
        {"close then reopen: content/encoding/newline restored from disk", &g_stDocReopen},
        {"status bar sampling: line/col, selection chars, total, encoding, ending", &g_stDocStatus},
        {"base dialog: enter triggers default button, esc returns escId", &g_stDocDialog},
        {"ime message injection on editor does not crash (code-level check)", &g_stDocIme},
        {"word wrap toggle applies sc_wrap_word to editors", &g_stDocWrap},
        {"toolbar new button routes command via geometry hit", &g_stDocToolbar},
        {"open failure shows error dialog, no document created", &g_stDocErrorDlg},
        {"close all documents shows welcome page and returns focus", &g_stDocCloseAll},
    };
    bool all = true;
    std::ofstream f(out, std::ios::trunc);
    for (const auto& [name, flag] : named) {
        f << "scenario=" << name << ": " << (*flag ? "PASS" : "FAIL") << "\n";
        all = all && *flag;
    }
    f << "overall: " << (all ? "PASS" : "FAIL") << "\n";
    f.close();
    // 诊断明细（自动化失败定位；验证脚本不依赖此文件）
    std::ofstream detail(std::filesystem::path(out).wstring() + L".detail.txt",
                         std::ios::trunc);
    detail << core::wideToUtf8(g_docDetail);
    detail.close();
}

void RunDocSelftestStage() {
    if (!g_mainWindow) return;
    const std::string kStartupContent = "你好 qcnote\nsecond line";

    if (g_docStage == 0) {
        g_docStage = 1;
        // —— 启动参数打开（D2）——
        bool ok = !g_initialPaths.empty() && g_docManager.documentCount() == 1;
        core::Document* argDoc = g_docManager.activeDocument();
        ok = ok && argDoc != nullptr;
        if (ok) {
            std::error_code ec;
            ok = argDoc->url().lexically_normal() ==
                     g_initialPaths[0].lexically_normal() &&
                 argDoc->content() == kStartupContent &&
                 g_tabs.count() == 1 && g_tabs.item(0).displayName == argDoc->displayName() &&
                 GetFocus() == g_editors.windowForTest(argDoc);
            wchar_t title[256] = {};
            GetWindowTextW(g_mainWindow, title, 256);
            ok = ok && wcsstr(title, argDoc->displayName().c_str()) != nullptr;
            // 状态栏字段
            ok = ok && g_statusBar.data().hasDoc &&
                 g_statusBar.data().encoding == L"UTF-8" &&
                 g_statusBar.data().lineEnding == L"LF" &&
                 g_statusBar.data().cursorLine == 1;
        }
        g_stDocArgOpen = ok;
        if (argDoc) g_docManager.closeDocument(argDoc);  // 清场进入后续场景
        PostMessageW(g_mainWindow, kMsgDocSelftestStage, 0, 0);
    } else if (g_docStage == 1) {
        g_docStage = 2;
        // —— 新建 3 文档 ——
        RouteCommand(ui::Command::NewDocument);
        RouteCommand(ui::Command::NewDocument);
        RouteCommand(ui::Command::NewDocument);
        const auto docs = g_docManager.documents();
        g_stDocNew =
            docs.size() == 3 && g_docManager.activeDocument() == docs[2] &&
            docs[0]->displayName() == L"未命名-1" && docs[1]->displayName() == L"未命名-2" &&
            docs[2]->displayName() == L"未命名-3" && g_tabs.count() == 3 &&
            g_tabs.activeIndex() == 2 &&
            GetFocus() == g_editors.windowForTest(docs[2]);
        PostMessageW(g_mainWindow, kMsgDocSelftestStage, 0, 0);
    } else if (g_docStage == 2) {
        g_docStage = 3;
        // —— 输入同步（字符级增量回写 + 脏标记 + 状态栏字符数）——
        auto docs = g_docManager.documents();
        core::Document* d3 = docs[2];  // 活动（未命名-3）
        TypeIntoEditor(d3, L"中文a");
        wchar_t title[256] = {};
        GetWindowTextW(g_mainWindow, title, 256);
        // 诊断明细（写 <结果文件>.detail.txt，便于失败定位）
        {
            const ui::StatusData& sd = g_statusBar.data();
            g_docDetail += L"[stage2] content=" +
                           core::utf8ToWide(d3->content()) + L" editorText=" +
                           core::utf8ToWide(EditorTextOf(d3)) +
                           L" modified=" + (d3->modified() ? L"1" : L"0") +
                           L" tabMod=" + (g_tabs.item(2).modified ? L"1" : L"0") +
                           L" title=" + std::wstring(title) +
                           L" total=" + std::to_wstring(sd.totalChars) +
                           L" line=" + std::to_wstring(sd.cursorLine) +
                           L" col=" + std::to_wstring(sd.cursorColumn) + L"\n";
        }
        bool ok = d3->content() == core::wideToUtf8(L"中文a") &&
                  EditorTextOf(d3) == core::wideToUtf8(L"中文a") && d3->modified() &&
                  g_tabs.item(2).modified;
        ok = ok && wcsstr(title, L"*") != nullptr;
        // 「中文a」= 3 字符；光标在行 1 列 4（字节偏移 7，前 3 字符）
        g_stDocType = ok && g_statusBar.data().totalChars == 3 &&
                      g_statusBar.data().cursorLine == 1 && g_statusBar.data().cursorColumn == 4;
        PostMessageW(g_mainWindow, kMsgDocSelftestStage, 0, 0);
    } else if (g_docStage == 3) {
        g_docStage = 4;
        // —— 切换保留（内容/光标/撤销按文档独立）——
        auto docs = g_docManager.documents();
        core::Document *d1 = docs[0], *d2 = docs[1], *d3 = docs[2];
        g_docManager.setActiveDocument(d1);
        TypeIntoEditor(d1, L"AAA");
        HWND sci1 = g_editors.windowForTest(d1);
        const int caret1 = static_cast<int>(SendMessageW(sci1, SCI_GETCURRENTPOS, 0, 0));
        g_docManager.setActiveDocument(d2);
        TypeIntoEditor(d2, L"BBB");
        g_docManager.setActiveDocument(d1);
        bool ok = EditorTextOf(d1) == "AAA" && d2->content() == "BBB" &&
                  d3->content() == core::wideToUtf8(L"中文a") &&
                  static_cast<int>(SendMessageW(sci1, SCI_GETCURRENTPOS, 0, 0)) == caret1;
        // 撤销栈跨切换保留（方案 A 核心收益）
        SendMessageW(sci1, SCI_UNDO, 0, 0);
        ok = ok && EditorTextOf(d1).empty() && d1->content().empty() && !d1->modified();
        g_stDocSwitch = ok;
        PostMessageW(g_mainWindow, kMsgDocSelftestStage, 0, 0);
    } else if (g_docStage == 4) {
        g_docStage = 5;
        // —— 另存为（直调 manager 等价路径；系统对话框不可自动化，UI 层另测）——
        auto docs = g_docManager.documents();
        core::Document* d2 = docs[1];  // 内容 "BBB"
        g_docManager.setActiveDocument(d2);  // 上一阶段激活停在 d1，切换到被测文档
        const auto target = g_selftestBaseDir / L"qcnote-selftest-saveas.txt";
        std::error_code ec;
        std::filesystem::remove(target, ec);
        const core::SaveAsResult r = g_docManager.saveAs(d2, target);
        bool ok = r.written && !r.error.has_value() && !d2->modified() &&
                  d2->displayName() == L"qcnote-selftest-saveas.txt" &&
                  ReadFileBytes(target) == std::string("BBB");
        g_editors.markSaved(d2);
        ok = ok && !g_tabs.item(1).modified;
        wchar_t title[256] = {};
        GetWindowTextW(g_mainWindow, title, 256);
        ok = ok && g_docManager.activeDocument() == d2 && wcsstr(title, L"*") == nullptr;
        g_stDocSaveAs = ok;
        PostMessageW(g_mainWindow, kMsgDocSelftestStage, 0, 0);
    } else if (g_docStage == 5) {
        g_docStage = 6;
        // —— GBK 编码转存链路 ——
        auto docs = g_docManager.documents();
        core::Document* d3 = docs[2];  // 未命名-3，内容「中文a」
        const auto gbkPath = g_selftestBaseDir / L"qcnote-selftest-gbk.txt";
        std::error_code ec;
        std::filesystem::remove(gbkPath, ec);
        // saveWithEncoding：未命名 → InvalidUrl 且编码保留（旧语义）
        auto r = g_docManager.saveWithEncoding(d3, core::TextEncoding::Gbk);
        bool ok = !r.has_value() &&
                  r.error().kind == core::StorageErrorKind::InvalidUrl &&
                  d3->encoding() == core::TextEncoding::Gbk;
        const core::SaveAsResult sr = g_docManager.saveAs(d3, gbkPath);
        ok = ok && sr.written;
        // GBK 字节：「中」= D6D0 「文」= CEC4 'a' = 61（Win32 CP936）
        const std::string gbkExpected = "\xD6\xD0\xCE\xC4\x61";
        ok = ok && ReadFileBytes(gbkPath) == gbkExpected;
        ok = ok && d3->encoding() == core::TextEncoding::Gbk && !d3->modified();
        g_stDocGbk = ok;
        PostMessageW(g_mainWindow, kMsgDocSelftestStage, 0, 0);
    } else if (g_docStage == 6) {
        g_docStage = 7;
        // —— 关闭 + 重开（openPaths）——
        auto docs = g_docManager.documents();
        core::Document* d3 = docs[2];
        const auto gbkPath = g_selftestBaseDir / L"qcnote-selftest-gbk.txt";
        const std::string expectedContent = core::wideToUtf8(L"中文a");
        g_docManager.closeDocument(d3);
        bool ok = g_docManager.documentCount() == 2;
        const auto errors = g_docManager.openPaths({gbkPath});
        core::Document* reopened = g_docManager.activeDocument();
        ok = ok && errors.empty() && reopened != nullptr &&
             reopened->content() == expectedContent &&
             reopened->encoding() == core::TextEncoding::Gbk &&
             reopened->lineEnding() == core::LineEnding::Lf && !reopened->modified();
        ok = ok && EditorTextOf(reopened) == expectedContent;
        // 状态栏编码显示 GBK；选中 2 字节（1 字符）→「已选 1 字符」
        HWND sci = g_editors.windowForTest(reopened);
        SendMessageW(sci, SCI_SETSEL, 0, 2);
        RefreshStatusBar();
        ok = ok && g_statusBar.data().encoding == L"GBK";
        g_stDocReopen = ok;
        PostMessageW(g_mainWindow, kMsgDocSelftestStage, 0, 0);
    } else if (g_docStage == 7) {
        g_docStage = 8;
        // —— 状态栏采样（行/列/选区/总字符）——
        core::Document* doc = g_docManager.activeDocument();  // GBK 重开文档「中文a」（7 字节）
        HWND sci = g_editors.windowForTest(doc);
        SendMessageW(sci, SCI_SETSEL, 0, 0);          // 清选区，光标 0
        SendMessageW(sci, SCI_GOTOPOS, 6, 0);         // 'a'（字节 6）行 1 列 3（前 2 字符）
        RefreshStatusBar();
        {
            const ui::StatusData& sd = g_statusBar.data();
            g_docDetail += L"[stage7] line=" + std::to_wstring(sd.cursorLine) +
                           L" col=" + std::to_wstring(sd.cursorColumn) +
                           L" sel=" + std::to_wstring(sd.selectedChars) +
                           L" total=" + std::to_wstring(sd.totalChars) +
                           L" enc=" + sd.encoding + L" eol=" + sd.lineEnding + L"\n";
        }
        bool ok = g_statusBar.data().cursorLine == 1 && g_statusBar.data().cursorColumn == 3 &&
                  g_statusBar.data().selectedChars == 0 && g_statusBar.data().totalChars == 3;
        SendMessageW(sci, SCI_SETSEL, 0, 7);          // 全选「中文a」（7 字节 3 字符）
        RefreshStatusBar();
        g_docDetail += L"[stage7] fullsel sel=" +
                       std::to_wstring(g_statusBar.data().selectedChars) + L"\n";
        ok = ok && g_statusBar.data().selectedChars == 3;
        // 多行内容行/列：临时改内容（经编辑器输入触发增量同步）
        SendMessageW(sci, SCI_SETSEL, 0, 0);
        TypeIntoEditor(doc, L"\nx");                  // 内容 "\nx中文a"（行 2 起点 = 字节 1）
        SendMessageW(sci, SCI_GOTOPOS, 1, 0);         // 行 2 列 1（'x' 之前）
        RefreshStatusBar();
        {
            const LRESULT rawPos = SendMessageW(sci, SCI_GETCURRENTPOS, 0, 0);
            const LRESULT rawLen = SendMessageW(sci, SCI_GETLENGTH, 0, 0);
            const editor::EditorStatus st2 = g_editors.statusOf(doc);
            g_docDetail += L"[stage7] multiline pos=" + std::to_wstring(static_cast<long long>(rawPos)) +
                           L" len=" + std::to_wstring(static_cast<long long>(rawLen)) +
                           L" liveLine=" + std::to_wstring(st2.line) +
                           L" liveCol=" + std::to_wstring(st2.column) +
                           L" barLine=" + std::to_wstring(g_statusBar.data().cursorLine) +
                           L" barCol=" + std::to_wstring(g_statusBar.data().cursorColumn) +
                           L"\n";
        }
        ok = ok && g_statusBar.data().cursorLine == 2 && g_statusBar.data().cursorColumn == 1;
        g_stDocStatus = ok;
        PostMessageW(g_mainWindow, kMsgDocSelftestStage, 0, 0);
    } else if (g_docStage == 8) {
        g_docStage = 9;
        // —— 基础对话框（Enter 默认按钮 / Esc）——
        ui::DialogSpec spec;
        spec.title = L"测试错误";
        spec.message = L"这是一条测试消息。";
        spec.buttons.push_back({L"确定", 1, true});
        spec.buttons.push_back({L"取消", 2, false});
        spec.escId = 2;
        g_dialogHost.onOpened = [] {
            if (HWND h = g_dialogHost.dialogWindowForTest()) {
                PostMessageW(h, WM_KEYDOWN, VK_RETURN, 0);
            }
        };
        int id = g_dialogHost.showModal(g_mainWindow, spec);
        g_dialogHost.onOpened = nullptr;
        bool ok = id == 1;
        g_dialogHost.onOpened = [] {
            if (HWND h = g_dialogHost.dialogWindowForTest()) {
                PostMessageW(h, WM_KEYDOWN, VK_ESCAPE, 0);
            }
        };
        id = g_dialogHost.showModal(g_mainWindow, spec);
        g_dialogHost.onOpened = nullptr;
        g_stDocDialog = ok && id == 2;
        FocusActiveEditor();
        PostMessageW(g_mainWindow, kMsgDocSelftestStage, 0, 0);
    } else if (g_docStage == 9) {
        g_docStage = 10;
        // —— IME 消息注入（代码级自查：不崩溃、消息循环仍响应）——
        HWND sci = g_editors.activeWindow();
        bool ok = sci != nullptr;
        if (ok) {
            SendMessageW(sci, WM_IME_SETCONTEXT, FALSE, 0);
            SendMessageW(sci, WM_IME_ENDCOMPOSITION, 0, 0);
            SendMessageW(sci, WM_IME_NOTIFY, IMN_CLOSECANDIDATE, 0);
            ok = SendMessageW(sci, WM_NULL, 0, 0) == 0;  // 窗口仍响应
        }
        g_stDocIme = ok;
        // —— 自动换行切换（全局）——
        RouteCommand(ui::Command::ToggleWordWrap);
        ok = g_editors.wordWrap();
        for (core::Document* d : g_docManager.documents()) {
            if (HWND h = g_editors.windowForTest(d)) {
                ok = ok && SendMessageW(h, SCI_GETWRAPMODE, 0, 0) == SC_WRAP_WORD;
            }
        }
        RouteCommand(ui::Command::ToggleWordWrap);
        g_stDocWrap = ok && !g_editors.wordWrap();
        // —— 工具栏按钮路由（命中几何 + onCommand → RouteCommand → 新建）——
        {
            const size_t before = g_docManager.documentCount();
            RECT client{};
            GetClientRect(g_mainWindow, &client);
            RECT btns[3];
            g_toolbar.buttonRects(client, btns);
            const POINT newBtn{(btns[0].left + btns[0].right) / 2,
                               (btns[0].top + btns[0].bottom) / 2};
            g_toolbar.onLButtonDown(newBtn);
            g_stDocToolbar = g_docManager.documentCount() == before + 1;
            CloseDocumentWithConfirm(g_docManager.activeDocument());  // 还原（干净→直接关）
        }
        // —— 打开失败错误对话框（UI 错误路径 + 模态退出注入）——
        {
            const size_t before = g_docManager.documentCount();
            g_dialogHost.onOpened = [] {
                if (HWND h = g_dialogHost.dialogWindowForTest()) {
                    PostMessageW(h, WM_KEYDOWN, VK_RETURN, 0);
                }
            };
            DoOpenPaths({g_selftestBaseDir / L"不存在的文件-自测.txt"});
            g_dialogHost.onOpened = nullptr;
            g_stDocErrorDlg = g_docManager.documentCount() == before;  // 失败不产生文档
        }
        PostMessageW(g_mainWindow, kMsgDocSelftestStage, 0, 0);
    } else if (g_docStage == 10) {
        g_docStage = 11;
        // —— 全部关闭 → 欢迎页 + 焦点回主窗口 ——
        const auto docs = g_docManager.documents();
        for (core::Document* d : docs) {
            // M4-B：UI 关闭路径经三选确认（CloseDocumentWithConfirm），模态框
            // 不可自动化注入——自测直调 manager 关闭（干净/脏同径，草稿清理
            // 经 documentClosed 事件驱动，自测模式持久化关闭无副作用）
            g_docManager.closeDocument(d);
        }
        bool ok = g_docManager.documentCount() == 0 && g_tabs.count() == 0 &&
                  GetFocus() == g_mainWindow;
        RECT client{};
        GetClientRect(g_mainWindow, &client);
        RECT br = g_welcome.buttonRect();
        ok = ok && br.right > br.left;  // 欢迎页按钮可见区域有效
        g_stDocCloseAll = ok;
        WriteDocSelftestResult();
        PostMessageW(g_mainWindow, WM_CLOSE, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// M3-B 查找替换 + 多光标自动化验证（--selftest-search[=<结果文件>]）
//
// 驱动方式：对话框模态循环内经投递 kMsgSearchSelftestStage 逐阶段推进（主
// 窗口 EnableWindow 禁用不影响消息投递）；对话框内控件用同步 SendMessage
// 驱动（EDIT WM_CHAR/WM_KEYDOWN、按钮/复选 WM_LBUTTONDOWN/UP 命中几何）。
// ---------------------------------------------------------------------------

bool g_stSchPrefill = false, g_stSchCount = false, g_stSchEnterWrap = false;
bool g_stSchPrevBtn = false, g_stSchToggles = false, g_stSchInvalidRe = false;
bool g_stSchReplaceCur = false, g_stSchReplaceAll = false, g_stSchUndoOnce = false;
bool g_stSchCloseKeep = false, g_stSchF3NoHi = false, g_stSchInvalidate = false;
bool g_stSchCtrlD = false, g_stSchAllOcc = false, g_stSchAddLine = false;
bool g_stSchImeDist = false, g_stSchUndoOnceMc = false, g_stSchPasteDist = false;
bool g_stSchPasteFull = false, g_stSchCopyJoin = false, g_stSchLineEnds = false;
bool g_stSchKbColumn = false;

// 测试内容（字节偏移在注释中给出）
const std::string kSearchContent = "foo abc foo\nbar foo\nabc\nfoofoo";
// foo: 0,8,16,24,27（foofoo 内两处不与全字边界重合）；abc: 4,20
const std::string kMcContent = "foo bar foo\nbaz foo\nab foo";
// L0 0-10(\n11) L1 12-18(\n19) L2 20-25(end26)；foo: 0,8,16,23；baz: 12
const std::string kColumnContent = "aaa\nbbbb\ncc\ndddd";
// L0 0-2(\n3) L1 4-7(\n8) L2 9-10(\n11) L3 12-15(end16)

void SearchSelftestClick(HWND dialog, const RECT& r) {
    const POINT c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    SendMessageW(dialog, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(c.x, c.y));
    SendMessageW(dialog, WM_LBUTTONUP, 0, MAKELPARAM(c.x, c.y));
}

// 测试内存剪贴板（经 MultiCursorHost 注入；系统剪贴板在本机被环境级
// ERROR_ACCESS_DENIED 拒绝——见 M3-B 报告「问题与决策」；注入只影响
// 测试进程内多光标控制器的剪贴板存取，分发/拼接判定逻辑与真实路径相同）
std::string g_testClipboard;

// 查找框输入模式（EM_SETSEL 全选 + 逐字符 WM_CHAR——EN_CHANGE 可靠触发）
void SearchSelftestTypePattern(HWND edit, const wchar_t* text) {
    SendMessageW(edit, EM_SETSEL, 0, -1);
    for (const wchar_t* p = text; *p; ++p) {
        SendMessageW(edit, WM_CHAR, *p, 0);
    }
}

bool SearchSelftestClipboardText(HWND, std::wstring* out) {
    *out = core::utf8ToWide(g_testClipboard);
    return true;
}

bool SearchSelftestSetClipboard(HWND, const std::string& utf8) {
    g_testClipboard = utf8;
    return true;
}

void WriteSearchSelftestResult() {
    std::error_code ec;
    std::filesystem::path out(g_selftestSearchOut);
    if (g_selftestSearchOut.empty()) {
        out = std::filesystem::current_path(ec) / "qcnote-search-selftest.txt";
    }
    const std::pair<const char*, bool*> named[] = {
        {"dialog open prefills selection text and selects all", &g_stSchPrefill},
        {"live recompute: count + indicator sampling (fill/clear)", &g_stSchCount},
        {"enter advances with wrap-around (5 matches cycle)", &g_stSchEnterWrap},
        {"previous button steps backward", &g_stSchPrevBtn},
        {"three toggles change counts (case/word/regex semantics)", &g_stSchToggles},
        {"invalid regex shows error, no crash", &g_stSchInvalidRe},
        {"replace current: single edit, index advances to next", &g_stSchReplaceCur},
        {"replace all: N edits in one undo group", &g_stSchReplaceAll},
        {"undo once reverts whole replace-all batch", &g_stSchUndoOnce},
        {"close keeps match state (DEC-009) and clears highlights", &g_stSchCloseKeep},
        {"closed-state F3 jumps without restoring highlights", &g_stSchF3NoHi},
        {"content change after close recomputes cached matches", &g_stSchInvalidate},
        {"ctrl+D selects word then appends next occurrence", &g_stSchCtrlD},
        {"ctrl+shift+L selects all occurrences, esc collapses to last", &g_stSchAllOcc},
        {"ctrl+alt+up adds cursor at same char column", &g_stSchAddLine},
        {"IME commit path (WM_CHAR) distributes to all carets", &g_stSchImeDist},
        {"multi-caret edit is single undo step", &g_stSchUndoOnceMc},
        {"paste distributes when lines match carets", &g_stSchPasteDist},
        {"paste inserts full text when lines mismatch", &g_stSchPasteFull},
        {"copy joins selections by line with empty for cursors", &g_stSchCopyJoin},
        {"shift+alt+I puts cursors at line ends", &g_stSchLineEnds},
        {"keyboard column select: rect extend + no-cross-line step", &g_stSchKbColumn},
    };
    bool all = true;
    std::ofstream f(out, std::ios::trunc);
    for (const auto& [name, flag] : named) {
        f << "scenario=" << name << ": " << (*flag ? "PASS" : "FAIL") << "\n";
        all = all && *flag;
    }
    f << "overall: " << (all ? "PASS" : "FAIL") << "\n";
    f.close();
    std::ofstream detail(std::filesystem::path(out).wstring() + L".detail.txt",
                         std::ios::trunc);
    detail << core::wideToUtf8(g_searchDetail);
    detail.close();
}

void RunSearchSelftestStage() {
    if (!g_mainWindow) return;

    if (g_searchStage == 0) {
        g_searchStage = 1;
        // —— 建文档 + 装内容 ——
        g_docManager.newDocument();
        core::Document* doc = g_docManager.activeDocument();
        doc->setContent(kSearchContent);
        HWND sci = g_editors.windowForTest(doc);
        bool ok = sci != nullptr && EditorTextOf(doc) == kSearchContent;
        if (ok) {
            SendMessageW(sci, SCI_SETSEL, 0, 3);  // 选 "foo"（预填源）
            g_searchDialog.onOpened = [] {
                HWND edit = g_searchDialog.findEditForTest();
                wchar_t buf[64] = {};
                GetWindowTextW(edit, buf, 64);
                DWORD selStart = 0, selEnd = 0;
                SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&selStart),
                             reinterpret_cast<LPARAM>(&selEnd));
                g_stSchPrefill = wcscmp(buf, L"foo") == 0 && selStart == 0 && selEnd == 3 &&
                                 GetFocus() == edit;
                // 实时计算 + indicator 采样（匹配处 1 / 非匹配处 0）
                const HWND s = g_editors.activeWindow();
                g_stSchCount =
                    g_searchCtl.matchCount() == 5 &&
                    g_searchCtl.currentIndex() == 0 &&
                    SendMessageW(s, SCI_INDICATORVALUEAT, editor::kIndicatorSearchMatch, 0) == 1 &&
                    SendMessageW(s, SCI_INDICATORVALUEAT, editor::kIndicatorSearchMatch, 3) == 0 &&
                    g_searchDialog.statusTextForTest() == L"第 1 个 / 共 5 个";
                // 对话框顶部对齐（y≈96 逻辑）+ 水平居中
                RECT wr{}, or_ = {};
                GetWindowRect(g_searchDialog.dialogWindowForTest(), &wr);
                GetWindowRect(g_mainWindow, &or_);
                const UINT dpi = ui::WindowDpi(g_mainWindow);
                auto diff = [](LONG a, LONG b) { return a > b ? a - b : b - a; };
                const bool posOk =
                    diff(wr.top, or_.top + ui::Scale(96, dpi)) <= ui::Scale(4, dpi) &&
                    diff((wr.left + wr.right) / 2,
                         (or_.left + or_.right) / 2) <= ui::Scale(4, dpi);
                g_searchDetail += L"[open] buf=" + std::wstring(buf) +
                                  L" sel=" + std::to_wstring(selStart) + L"," +
                                  std::to_wstring(selEnd) +
                                  L" focusIsEdit=" +
                                  (GetFocus() == edit ? L"1" : L"0") +
                                  L" top=" + std::to_wstring(wr.top) +
                                  L" expTop=" + std::to_wstring(or_.top + ui::Scale(96, dpi)) +
                                  L" cx=" + std::to_wstring((wr.left + wr.right) / 2) +
                                  L" expCx=" + std::to_wstring((or_.left + or_.right) / 2) +
                                  L" posOk=" + (posOk ? L"1" : L"0") + L"\n";
                g_stSchPrefill = g_stSchPrefill && posOk;
                PostMessageW(g_mainWindow, kMsgSearchSelftestStage, 0, 0);
            };
            RouteCommand(ui::Command::FindDialog);  // 模态（阻塞至阶段 7 关闭）
            g_searchDialog.onOpened = nullptr;
            // —— 关闭后断言（DEC-009）——
            HWND s = g_editors.activeWindow();
            g_stSchCloseKeep =
                g_searchCtl.matchCount() == 4 &&  // 匹配状态保留（替换后的 4 处）
                SendMessageW(s, SCI_INDICATORVALUEAT, editor::kIndicatorSearchMatch, 8) == 0 &&
                GetFocus() == s;  // 焦点回编辑器
        }
        PostMessageW(g_mainWindow, kMsgSearchSelftestStage, 0, 0);
    } else if (g_searchStage == 1) {
        g_searchStage = 2;
        // （模态循环内）Enter 环绕推进（预填选区已在首个匹配上 → 首按即推进）
        HWND edit = g_searchDialog.findEditForTest();
        HWND s = g_editors.activeWindow();
        const int expect[] = {8, 16, 24, 27, 0};  // 5 次按下（含环绕回 0）
        bool ok = true;
        for (int pos : expect) {
            SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
            const int got = static_cast<int>(SendMessageW(s, SCI_GETSELECTIONSTART, 0, 0));
            g_searchDetail += L"[enter] expect=" + std::to_wstring(pos) + L" got=" +
                              std::to_wstring(got) + L" idx=" +
                              std::to_wstring(g_searchCtl.currentIndex()) + L"\n";
            ok = ok && got == pos;
        }
        g_stSchEnterWrap = ok && g_searchCtl.currentIndex() == 0;
        PostMessageW(g_mainWindow, kMsgSearchSelftestStage, 0, 0);
    } else if (g_searchStage == 2) {
        g_searchStage = 3;
        // 「上一个」按钮（鼠标命中几何驱动）
        HWND dlg = g_searchDialog.dialogWindowForTest();
        SearchSelftestClick(dlg, g_searchDialog.buttonRectForTest(
                                     ui::searchdialog_metrics::ButtonFindPrev));
        HWND s = g_editors.activeWindow();
        g_stSchPrevBtn = g_searchCtl.currentIndex() == 4 &&
                         static_cast<int>(SendMessageW(s, SCI_GETSELECTIONSTART, 0, 0)) == 27;
        g_searchDetail += L"[prev] idx=" + std::to_wstring(g_searchCtl.currentIndex()) +
                          L" start=" + std::to_wstring(static_cast<int>(
                                           SendMessageW(s, SCI_GETSELECTIONSTART, 0, 0))) +
                          L"\n";
        PostMessageW(g_mainWindow, kMsgSearchSelftestStage, 0, 0);
    } else if (g_searchStage == 3) {
        g_searchStage = 4;
        // —— 三开关计数变化 ——
        HWND dlg = g_searchDialog.dialogWindowForTest();
        HWND edit = g_searchDialog.findEditForTest();
        SearchSelftestTypePattern(edit, L"Foo");  // 大小写不敏感默认：5
        bool ok = g_searchCtl.matchCount() == 5;
        SearchSelftestClick(dlg, g_searchDialog.checkBoxRectForTest(
                                     ui::searchdialog_metrics::CheckCase));
        ok = ok && g_searchCtl.matchCount() == 0 &&
             g_searchDialog.statusTextForTest() == L"无匹配";  // "Foo" 无精确匹配
        SearchSelftestClick(dlg, g_searchDialog.checkBoxRectForTest(
                                     ui::searchdialog_metrics::CheckCase));
        ok = ok && g_searchCtl.matchCount() == 5;
        SearchSelftestClick(dlg, g_searchDialog.checkBoxRectForTest(
                                     ui::searchdialog_metrics::CheckWholeWord));
        ok = ok && g_searchCtl.matchCount() == 3;  // foofoo 内两处非全字
        SearchSelftestClick(dlg, g_searchDialog.checkBoxRectForTest(
                                     ui::searchdialog_metrics::CheckRegex));
        ok = ok && g_searchCtl.matchCount() == 5;  // 正则模式忽略全字
        g_stSchToggles = ok;
        // —— 无效正则 ——
        SearchSelftestTypePattern(edit, L"[");
        g_stSchInvalidRe = g_searchCtl.matchCount() == 0 &&
                           !g_searchCtl.errorMessage().empty() &&
                           g_searchDialog.statusTextForTest() == g_searchCtl.errorMessage();
        // 恢复：模式 "foo"、全字关、正则关
        SearchSelftestTypePattern(edit, L"foo");
        SearchSelftestClick(dlg, g_searchDialog.checkBoxRectForTest(
                                     ui::searchdialog_metrics::CheckRegex));
        ok = g_searchCtl.matchCount() == 3;  // 正则关后全字仍开
        SearchSelftestClick(dlg, g_searchDialog.checkBoxRectForTest(
                                     ui::searchdialog_metrics::CheckWholeWord));
        g_stSchToggles = g_stSchToggles && ok && g_searchCtl.matchCount() == 5;
        PostMessageW(g_mainWindow, kMsgSearchSelftestStage, 0, 0);
    } else if (g_searchStage == 4) {
        g_searchStage = 5;
        // —— 替换当前（替换后索引指向下一个）——
        HWND dlg = g_searchDialog.dialogWindowForTest();
        // 替换框经 EM_SETSEL + WM_CHAR 录入（EN_CHANGE 可靠触发路径）
        SearchSelftestTypePattern(g_searchDialog.replaceEditForTest(), L"XY");
        SearchSelftestClick(dlg, g_searchDialog.buttonRectForTest(
                                     ui::searchdialog_metrics::ButtonReplace));
        core::Document* doc = g_docManager.activeDocument();
        HWND s = g_editors.activeWindow();
        // [0,3)→"XY"（-1 字节）：余 foo 在 7,15,23,26；caret=2 → 下一个=7（index 0）
        g_stSchReplaceCur =
            doc->content() == "XY abc foo\nbar foo\nabc\nfoofoo" &&
            g_searchCtl.matchCount() == 4 && g_searchCtl.currentIndex() == 0 &&
            static_cast<int>(SendMessageW(s, SCI_GETCURRENTPOS, 0, 0)) == 2 &&
            g_searchDialog.statusTextForTest() == L"第 1 个 / 共 4 个";
        g_searchDetail += L"[replaceCur] content=" + core::utf8ToWide(doc->content()) +
                          L" idx=" + std::to_wstring(g_searchCtl.currentIndex()) +
                          L" status=" + g_searchDialog.statusTextForTest() + L"\n";
        PostMessageW(g_mainWindow, kMsgSearchSelftestStage, 0, 0);
    } else if (g_searchStage == 5) {
        g_searchStage = 6;
        // —— 全部替换单批撤销 ——
        HWND dlg = g_searchDialog.dialogWindowForTest();
        SearchSelftestClick(dlg, g_searchDialog.buttonRectForTest(
                                     ui::searchdialog_metrics::ButtonReplaceAll));
        core::Document* doc = g_docManager.activeDocument();
        HWND s = g_editors.activeWindow();
        bool ok = doc->content() == "XY abc XY\nbar XY\nabc\nXYXY" &&
                  g_searchDialog.statusTextForTest() == L"已替换 4 处";
        // SCI_UNDO 一步回滚全部替换（单批撤销）
        SendMessageW(s, SCI_UNDO, 0, 0);
        g_stSchReplaceAll = ok && g_searchDialog.statusTextForTest() == L"已替换 4 处";
        g_stSchUndoOnce =
            doc->content() == "XY abc foo\nbar foo\nabc\nfoofoo" &&
            g_searchCtl.matchCount() == 4;  // 内容变化自动重算
        PostMessageW(g_mainWindow, kMsgSearchSelftestStage, 0, 0);
    } else if (g_searchStage == 6) {
        g_searchStage = 7;
        // —— Esc 关闭（DEC-009：高亮清、状态留）——
        SendMessageW(g_searchDialog.dialogWindowForTest(), WM_KEYDOWN, VK_ESCAPE, 0);
        // 模态循环在本阶段返回后退出；后续断言在阶段 0 的 RouteCommand 返回处
    } else if (g_searchStage == 7) {
        g_searchStage = 8;
        // —— 关闭态 F3（不恢复高亮）+ 缓存失效 ——
        HWND s = g_editors.activeWindow();
        RouteCommand(ui::Command::FindNext);
        // 撤销后重算保持 currentIndex=-1（replaceAll 后 0 匹配 → -1；undo 重算
        // 不凭空制造当前匹配）→ F3 首跳定位首个匹配（7）
        g_stSchF3NoHi =
            g_searchCtl.currentIndex() == 0 &&
            static_cast<int>(SendMessageW(s, SCI_GETSELECTIONSTART, 0, 0)) == 7 &&
            static_cast<int>(SendMessageW(s, SCI_GETSELECTIONEND, 0, 0)) == 10 &&
            SendMessageW(s, SCI_INDICATORVALUEAT, editor::kIndicatorSearchMatch, 7) == 0;
        g_searchDetail += L"[f3] idx=" + std::to_wstring(g_searchCtl.currentIndex()) +
                          L" start=" + std::to_wstring(static_cast<int>(
                                           SendMessageW(s, SCI_GETSELECTIONSTART, 0, 0))) +
                          L"\n";
        // 内容破坏一处匹配 → 重算（关闭态缓存失效）
        SendMessageW(s, SCI_SETSEL, 7, 10);
        SendMessageW(s, SCI_CLEAR, 0, 0);  // 删一个 "foo"（余 13,20,23）
        bool ok = g_searchCtl.matchCount() == 3;
        RouteCommand(ui::Command::FindNext);  // F3 仍可用（index 1 → 2 → 20）
        g_stSchInvalidate = ok && g_searchCtl.matchCount() == 3 &&
                            static_cast<int>(SendMessageW(s, SCI_GETSELECTIONSTART, 0, 0)) == 20;
        g_searchDetail += L"[invalidate] count=" + std::to_wstring(g_searchCtl.matchCount()) +
                          L" start=" + std::to_wstring(static_cast<int>(
                                           SendMessageW(s, SCI_GETSELECTIONSTART, 0, 0))) +
                          L"\n";
        PostMessageW(g_mainWindow, kMsgSearchSelftestStage, 0, 0);
    } else if (g_searchStage == 8) {
        g_searchStage = 9;
        // —— 多光标（换内容）——
        core::Document* doc = g_docManager.activeDocument();
        doc->setContent(kMcContent);
        HWND s = g_editors.activeWindow();
        auto selStart = [&] {
            return static_cast<int>(SendMessageW(s, SCI_GETSELECTIONSTART, 0, 0));
        };
        auto selEnd = [&] {
            return static_cast<int>(SendMessageW(s, SCI_GETSELECTIONEND, 0, 0));
        };
        auto selCount = [&] {
            return static_cast<int>(SendMessageW(s, SCI_GETSELECTIONS, 0, 0));
        };
        // Ctrl+D：选词 → 追加下一个相同词
        SendMessageW(s, SCI_GOTOPOS, 2, 0);  // 光标置于首个 "foo" 中
        g_multiCursor.testKeyDown(s, 'D', true, false, false);
        bool ok = selCount() == 1 && selStart() == 0 && selEnd() == 3;
        g_multiCursor.testKeyDown(s, 'D', true, false, false);
        ok = ok && selCount() == 2 &&
             static_cast<int>(SendMessageW(s, SCI_GETSELECTIONNSTART, 1, 0)) == 8;
        g_stSchCtrlD = ok;
        // Esc 收缩末光标（保留其 anchor/caret）
        g_multiCursor.testKeyDown(s, VK_ESCAPE, false, false, false);
        ok = selCount() == 1 && selStart() == 8 && selEnd() == 11;
        // Ctrl+Shift+L：全部相同 → Esc 收缩
        g_multiCursor.testKeyDown(s, 'L', true, true, false);
        ok = ok && selCount() == 4;
        g_stSchAllOcc = selCount() == 4;
        g_multiCursor.testKeyDown(s, VK_ESCAPE, false, false, false);
        ok = ok && selCount() == 1 && selStart() == 23 && selEnd() == 26;
        // Ctrl+Alt+↑：同字符列加光标（L2 col3 → L1 col3 = pos15）
        SendMessageW(s, SCI_SETSEL, 23, 23);
        g_multiCursor.testKeyDown(s, VK_UP, true, false, true);
        g_stSchAddLine = selCount() == 2 &&
                         static_cast<int>(SendMessageW(s, SCI_GETSELECTIONNCARET, 0, 0)) == 15;
        // 状态栏多光标字段（B4：光标 ×N；选区字符数 = 各选区合计）
        SendMessageW(s, SCI_SETSEL, 0, 3);  // 单选区 "foo"（3 字符）
        SendMessageW(s, SCI_ADDSELECTION, 8, 11);  // 第二选区 "foo"（+3 字符）
        RefreshStatusBar();
        g_stSchAddLine = g_stSchAddLine &&
                         g_statusBar.data().cursorCount == 2 &&
                         g_statusBar.data().selectedChars == 6;
        SendMessageW(s, SCI_SETSEL, 23, 23);  // 还原为单光标进入 IME 场景
        g_multiCursor.testKeyDown(s, VK_UP, true, false, true);
        // IME 提交分发路径（WM_CHAR → InsertCharacter 与 GCS_RESULTSTR 同路径）：
        // 两个空光标 {15} {23} 各插入「中」（字符列 3：L1 在 'z' 后、L2 在空格后）
        TypeIntoEditor(doc, L"\x4E2D");
        g_stSchImeDist = doc->content() == "foo bar foo\nbaz中 foo\nab 中foo";
        g_searchDetail += L"[ime-pre] typing=" +
                          std::to_wstring(static_cast<int>(
                              SendMessageW(s, SCI_GETADDITIONALSELECTIONTYPING, 0, 0))) +
                          L" multiSel=" +
                          std::to_wstring(static_cast<int>(
                              SendMessageW(s, SCI_GETMULTIPLESELECTION, 0, 0))) +
                          L" selCount=" + std::to_wstring(selCount()) +
                          L" content=" + core::utf8ToWide(doc->content()) + L"\n";
        // 单批撤销：一步回滚两处编辑（UndoSelectionHistory 还原多选区）
        SendMessageW(s, SCI_UNDO, 0, 0);
        g_stSchUndoOnceMc = doc->content() == kMcContent && selCount() == 2;
        g_searchDetail += L"[ime] content=" + core::utf8ToWide(doc->content()) +
                          L" undid=" + (doc->content() == kMcContent ? L"1" : L"0") +
                          L" selCount=" + std::to_wstring(selCount()) + L"\n";
        (void)ok;
        PostMessageW(g_mainWindow, kMsgSearchSelftestStage, 0, 0);
    } else if (g_searchStage == 9) {
        g_searchStage = 10;
        // 注入内存剪贴板（后续 Ctrl+C/V 断言确定性；见 g_testClipboard 注释）
        g_multiCursor.setClipboardAccessForTest(
            [](HWND) { return g_testClipboard; },
            [](HWND, const std::string& s) { g_testClipboard = s; return true; });
        core::Document* doc = g_docManager.activeDocument();
        HWND s = g_editors.activeWindow();
        auto selCount = [&] {
            return static_cast<int>(SendMessageW(s, SCI_GETSELECTIONS, 0, 0));
        };
        // —— 粘贴分发（行数 == 光标数）——
        SendMessageW(s, SCI_SETSEL, 1, 1);
        const int x = static_cast<int>(
            SendMessageW(s, SCI_POINTXFROMPOSITION, 0, 12));  // 'b' of baz
        const int y = static_cast<int>(SendMessageW(s, SCI_POINTYFROMPOSITION, 0, 12));
        g_multiCursor.testAltClick(s, {x, y});  // 加光标 → 2 个
        bool twoCarets = selCount() == 2;
        const bool setOk = SearchSelftestSetClipboard(s, "X1\nY2");
        std::wstring clipProbe;
        const bool readOk = SearchSelftestClipboardText(s, &clipProbe);
        g_searchDetail += L"[clipprobe] setOk=" + std::to_wstring(setOk ? 1 : 0) +
                          L" readOk=" + std::to_wstring(readOk ? 1 : 0) +
                          L" readback=" + clipProbe + L"\n";
        g_multiCursor.testPaste(s);
        g_stSchPasteDist = twoCarets && selCount() == 2 &&
                           doc->content() == "fX1oo bar foo\nY2baz foo\nab foo";
        g_searchDetail += L"[pasteDist] two=" + std::to_wstring(twoCarets ? 1 : 0) +
                          L" content=" + core::utf8ToWide(doc->content()) + L"\n";
        // —— 粘贴整份（行数 != 光标数）——
        SendMessageW(s, SCI_SETSEL, 0, 0);
        SearchSelftestSetClipboard(s, "A\nB");
        g_multiCursor.testPaste(s);
        g_stSchPasteFull = doc->content() == "A\nBfX1oo bar foo\nY2baz foo\nab foo";
        // —— 多选区复制（排序拼接；空光标贡献空行）——
        SendMessageW(s, SCI_SETSEL, 0, 3);  // 选中前 3 字符 "A\nB"（跨行选区）
        const int px = static_cast<int>(SendMessageW(s, SCI_POINTXFROMPOSITION, 0, 5));
        const int py = static_cast<int>(SendMessageW(s, SCI_POINTYFROMPOSITION, 0, 5));
        g_multiCursor.testAltClick(s, {px, py});  // 加空光标（pos 5）
        std::wstring clip;
        g_multiCursor.testKeyDown(s, 'C', true, false, false);
        SearchSelftestClipboardText(s, &clip);
        g_stSchCopyJoin = clip == L"A\nB\n";  // 选区文本 + 空光标空行
        g_searchDetail += L"[copyJoin] clip=" + core::utf8ToWide(core::wideToUtf8(clip)) +
                          L" (len " + std::to_wstring(clip.size()) + L")\n";
        // —— Shift+Alt+I 行尾光标 ——
        doc->setContent(kMcContent);
        s = g_editors.activeWindow();
        SendMessageW(s, SCI_SETSEL, 0, 14);  // 跨 L0..L1
        g_multiCursor.testKeyDown(s, 'I', false, true, true);
        g_stSchLineEnds = selCount() == 2 &&
                          static_cast<int>(SendMessageW(s, SCI_GETSELECTIONNCARET, 0, 0)) == 11 &&
                          static_cast<int>(SendMessageW(s, SCI_GETSELECTIONNCARET, 1, 0)) == 19;
        PostMessageW(g_mainWindow, kMsgSearchSelftestStage, 0, 0);
    } else if (g_searchStage == 10) {
        g_searchStage = 11;
        // —— 键盘列选（Ctrl+Alt+Shift+方向）——
        core::Document* doc = g_docManager.activeDocument();
        doc->setContent(kColumnContent);
        HWND s = g_editors.activeWindow();
        auto selCount = [&] {
            return static_cast<int>(SendMessageW(s, SCI_GETSELECTIONS, 0, 0));
        };
        SendMessageW(s, SCI_SETSEL, 2, 2);  // L0 col2
        g_multiCursor.testKeyDown(s, VK_DOWN, true, true, true);  // 矩形扩到 L1
        bool ok = selCount() == 2 &&
                  SendMessageW(s, SCI_SELECTIONISRECTANGLE, 0, 0) != 0;
        g_multiCursor.testKeyDown(s, VK_DOWN, true, true, true);  // 扩到 L2
        ok = ok && selCount() == 3;
        g_multiCursor.testKeyDown(s, VK_LEFT, true, true, true);  // 活动角 2→1（不跨行）
        ok = ok && selCount() == 3 &&
             static_cast<int>(SendMessageW(s, SCI_GETSELECTIONNSTART, 0, 0)) == 1 &&
             static_cast<int>(SendMessageW(s, SCI_GETSELECTIONNEND, 0, 0)) == 2;
        g_stSchKbColumn = ok;
        g_searchDetail += L"[kbcol] sels2=" +
                          std::to_wstring(static_cast<int>(SendMessageW(s, SCI_GETSELECTIONS, 0, 0))) +
                          L" rect=" +
                          std::to_wstring(static_cast<int>(SendMessageW(s, SCI_SELECTIONISRECTANGLE, 0, 0))) +
                          L" n0=[" + std::to_wstring(static_cast<int>(SendMessageW(s, SCI_GETSELECTIONNSTART, 0, 0))) +
                          L"," + std::to_wstring(static_cast<int>(SendMessageW(s, SCI_GETSELECTIONNEND, 0, 0))) +
                          L"]\n";
        // Enter：矩形选区先转普通多选区再逐光标换行（不塌缩）。经真实
        // WM_KEYDOWN 路径（子类化 → streamify 放行 → 原生 SCI_NEWLINE）；
        // WM_CHAR '\r' 只会插入字面 CR，不是换行命令
        SendMessageW(s, WM_KEYDOWN, VK_RETURN, 0);
        g_stSchKbColumn = g_stSchKbColumn && selCount() == 3 &&
                          doc->content() == "a\na\nb\nbb\nc\n\ndddd";
        g_searchDetail += L"[kbcol-enter] sels=" + std::to_wstring(selCount()) +
                          L" content=" + core::utf8ToWide(doc->content()) + L"\n";
        WriteSearchSelftestResult();
        PostMessageW(g_mainWindow, WM_CLOSE, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// M6 主题自动化验证（--selftest-theme[=<结果文件>]）
//
// 断言层次（浅 → 深 → 浅一轮回合）：
//   1) 状态标志与菜单命令路径（RouteCommand(ToggleDarkTheme) = 视图菜单等价）；
//   2) DWMWA_USE_IMMERSIVE_DARK_MODE：主窗口 + 基础对话框 + 查找对话框
//     （DwmGetWindowAttribute 读回）；
//   3) 像素采样：PrintWindow(PW_RENDERFULLCONTENT) 抓离屏窗口（M5 已验证
//     「离屏可见可截图」路径），标题栏/工具栏/内容区（欢迎页）/状态栏四点
//     命中两套 token（浅 #dde2ec/#f2f4f8/#ffffff/#e8ebf1 ↔ 深 #171c24/
//     #232833/#262b34/#1d222b）；
//   4) Scintilla 配色：STYLEGETFORE/BACK、GETCARETFORE、INDICGETFORE/ALPHA
//     按主题重设（编辑区像素经 API 断言——PrintWindow 对子窗口内容的
//     稳定性不作假设）。
// 驱动方式与既有自测一致：kMsgThemeSelftestStage 逐阶段推进，防扰（离屏
// TOOLWINDOW + SW_SHOWNOACTIVATE，全程零可见元素零前台抢占）。
// ---------------------------------------------------------------------------

bool g_stThLightBaseline = false, g_stThLightEditor = false;
bool g_stThDarkApply = false, g_stThDarkPixels = false, g_stThDarkEditor = false;
bool g_stThDialogDwm = false, g_stThSearchDwm = false;
bool g_stThBackLight = false, g_stThBackEditor = false;

// PrintWindow + PW_RENDERFULLCONTENT：经 DWM 重定向表面抓取（对「可见但
// 离屏」的自测窗口可用）；无边框自绘窗口 client == 整窗，客户区坐标即位图
// 坐标。outArgb 为 0xAARRGGBB
bool CaptureClientPixel(HWND hwnd, int x, int y, uint32_t& outArgb) {
    RECT rc{};
    if (!hwnd || !GetClientRect(hwnd, &rc)) return false;
    if (x < 0 || y < 0 || x >= rc.right || y >= rc.bottom) return false;
    HDC wndDc = GetDC(hwnd);
    if (!wndDc) return false;
    HDC mem = CreateCompatibleDC(wndDc);
    HBITMAP bmp = CreateCompatibleBitmap(wndDc, rc.right, rc.bottom);
    bool ok = false;
    if (mem && bmp) {
        const HGDIOBJ old = SelectObject(mem, bmp);
        ok = PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT) != FALSE;
        const COLORREF px = ok ? GetPixel(mem, x, y) : CLR_INVALID;
        if (px == CLR_INVALID) {
            ok = false;
        } else {
            outArgb = 0xFF000000u | ((px & 0xFFu) << 16) | (px & 0xFF00u) |
                      ((px >> 16) & 0xFFu);
        }
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(hwnd, wndDc);
    return ok;
}

// 四点采样（标题栏/工具栏/内容区/状态栏）与期望色比对；不匹配记明细。
// 首帧竞态防御：窗口刚显示时 DWM 重定向表面可能尚未合成（抓到纯黑），
// 逐样泵消息 + 300ms 内重试；纯黑不是任何 token 色，重试不吞真回归
bool SampleThemePixels(uint32_t titleExp, uint32_t toolbarExp, uint32_t contentExp,
                       uint32_t statusExp) {
    RECT client{};
    GetClientRect(g_mainWindow, &client);
    const UINT dpi = ui::WindowDpi(g_mainWindow);
    const int capH = ui::Scale(ui::metrics::CaptionHeight, dpi);
    const int toolH = ui::Scale(ui::toolbar_metrics::Height, dpi);
    const int statH = ui::Scale(ui::statusbar_metrics::Height, dpi);
    struct Sample {
        const char* name;
        POINT pt;
        uint32_t expected;
    };
    const Sample samples[] = {
        {"titleBar", {client.right / 2, capH / 2}, titleExp},
        {"toolbar", {client.right / 2, capH + toolH / 2}, toolbarExp},
        {"content", {client.right / 2, capH + toolH + ui::Scale(30, dpi)}, contentExp},
        {"statusBar", {client.right / 2, client.bottom - statH / 2}, statusExp},
    };
    bool ok = true;
    for (const Sample& s : samples) {
        uint32_t got = 0;
        bool captured = false;
        bool match = false;
        for (int attempt = 0; attempt < 30 && !match; ++attempt) {
            UpdateWindow(g_mainWindow);  // 同步刷新后采样
            captured = CaptureClientPixel(g_mainWindow, s.pt.x, s.pt.y, got);
            match = captured && (got & 0xFFFFFFu) == (s.expected & 0xFFFFFFu);
            if (!match) {
                MSG m;
                while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                }
                Sleep(10);
            }
        }
        ok = ok && match;
        wchar_t buf[128];
        swprintf_s(buf, L"[%hs] exp=%06X got=%06X captured=%d\n", s.name,
                   static_cast<unsigned>(s.expected & 0xFFFFFFu),
                   static_cast<unsigned>(got & 0xFFFFFFu), captured ? 1 : 0);
        g_themeDetail += buf;
    }
    return ok;
}

// Scintilla 当前配色按期望 token 断言（编辑器 API 读回；比像素更稳定）
bool CheckEditorThemeColors(uint32_t foreExp, uint32_t backExp, uint32_t caretExp,
                            uint32_t indicMatchExp, int indicAlphaExp,
                            uint32_t indicBorderExp) {
    core::Document* doc = g_docManager.activeDocument();
    HWND sci = g_editors.windowForTest(doc);
    if (!sci) return false;
    const auto colorOf = [](LRESULT c) {
        return static_cast<uint32_t>((c & 0xFFu) << 16) | (c & 0xFF00u) |
               ((c >> 16) & 0xFFu);
    };
    const LRESULT fore = SendMessageW(sci, SCI_STYLEGETFORE, STYLE_DEFAULT, 0);
    const LRESULT back = SendMessageW(sci, SCI_STYLEGETBACK, STYLE_DEFAULT, 0);
    const LRESULT caret = SendMessageW(sci, SCI_GETCARETFORE, 0, 0);
    const LRESULT ind8 = SendMessageW(sci, SCI_INDICGETFORE,
                                      editor::kIndicatorSearchMatch, 0);
    const LRESULT ind8a = SendMessageW(sci, SCI_INDICGETALPHA,
                                       editor::kIndicatorSearchMatch, 0);
    const LRESULT ind9 = SendMessageW(sci, SCI_INDICGETFORE,
                                      editor::kIndicatorSearchCurrent, 0);
    const auto u = [](LRESULT c) -> unsigned {
        return static_cast<unsigned>(c & 0xFFFFFFu);
    };
    const bool ok = colorOf(fore) == (foreExp & 0xFFFFFFu) &&
                    colorOf(back) == (backExp & 0xFFFFFFu) &&
                    colorOf(caret) == (caretExp & 0xFFFFFFu) &&
                    colorOf(ind8) == (indicMatchExp & 0xFFFFFFu) &&
                    ind8a == indicAlphaExp &&
                    colorOf(ind9) == (indicBorderExp & 0xFFFFFFu);
    wchar_t buf[160];
    swprintf_s(buf, L"[editor] fore=%06X back=%06X caret=%06X i8=%06X a8=%d i9=%06X\n",
               u(fore), u(back), u(caret), u(ind8), static_cast<int>(ind8a), u(ind9));
    g_themeDetail += buf;
    return ok;
}

void WriteThemeSelftestResult() {
    std::error_code ec;
    std::filesystem::path out(g_selftestThemeOut);
    if (g_selftestThemeOut.empty()) {
        out = std::filesystem::current_path(ec) / "qcnote-theme-selftest.txt";
    }
    const std::pair<const char*, bool*> named[] = {
        {"light baseline: flag off + main-window DWM dark attr false + pixel hits "
         "(#dde2ec/#f2f4f8/#ffffff/#e8ebf1)",
         &g_stThLightBaseline},
        {"light baseline: editor style/indicator colors from light palette",
         &g_stThLightEditor},
        {"toggle to dark via menu command: flag flips + DWM dark attr true",
         &g_stThDarkApply},
        {"dark pixels hit tokens (#171c24/#232833/#262b34/#1d222b)",
         &g_stThDarkPixels},
        {"dark editor: back/fore/caret/indicator 8+9 reconfigured", &g_stThDarkEditor},
        {"base dialog popup inherits DWM dark attribute in dark theme",
         &g_stThDialogDwm},
        {"search dialog popup + its DWM dark attribute in dark theme",
         &g_stThSearchDwm},
        {"toggle back to light: pixels return to light tokens", &g_stThBackLight},
        {"light editor colors restored after toggling back", &g_stThBackEditor},
    };
    bool all = true;
    std::ofstream f(out, std::ios::trunc);
    for (const auto& [name, flag] : named) {
        f << "scenario=" << name << ": " << (*flag ? "PASS" : "FAIL") << "\n";
        all = all && *flag;
    }
    f << "overall: " << (all ? "PASS" : "FAIL") << "\n";
    f.close();
    std::ofstream detail(std::filesystem::path(out).wstring() + L".detail.txt",
                         std::ios::trunc);
    detail << core::wideToUtf8(g_themeDetail);
    detail.close();
}

void RunThemeSelftestStage() {
    if (!g_mainWindow) return;
    const auto& light = ui::LightPalette();
    const auto& dark = ui::DarkPalette();

    if (g_themeStage == 0) {
        g_themeStage = 1;
        // —— 浅色基线（无文档 → 欢迎页内容区可采样）——
        g_stThLightBaseline = !ui::IsDarkTheme() &&
                              !ui::QueryImmersiveDark(g_mainWindow) &&
                              SampleThemePixels(light.titleBarBackground,
                                                light.toolbarBackground,
                                                light.contentBackground,
                                                light.statusBarBackground);
        // 建文档：编辑器实例配色断言（浅色 token）
        g_docManager.newDocument();
        g_stThLightEditor = CheckEditorThemeColors(
            light.textPrimary, light.contentBackground, light.accent,
            light.searchMatch, static_cast<int>(light.searchMatch >> 24),
            light.searchMatchBorder);
        PostMessageW(g_mainWindow, kMsgThemeSelftestStage, 0, 0);
    } else if (g_themeStage == 1) {
        g_themeStage = 2;
        // —— 切深色（真实命令路径：与视图菜单同一 RouteCommand）——
        RouteCommand(ui::Command::ToggleDarkTheme);
        g_stThDarkApply = ui::IsDarkTheme() && ui::QueryImmersiveDark(g_mainWindow);
        // 关文档回欢迎页：内容区为父窗口绘制（欢迎页 contentBackground）
        g_docManager.closeDocument(g_docManager.activeDocument());
        g_stThDarkPixels = g_stThDarkApply &&
                           SampleThemePixels(dark.titleBarBackground,
                                             dark.toolbarBackground,
                                             dark.contentBackground,
                                             dark.statusBarBackground);
        // 再建文档：编辑器配色按深色 token 重设
        g_docManager.newDocument();
        g_stThDarkEditor = CheckEditorThemeColors(
            dark.textPrimary, dark.contentBackground, dark.accent, dark.searchMatch,
            static_cast<int>(dark.searchMatch >> 24), dark.searchMatchBorder);
        // 基础对话框弹出层：深色下创建即挂 DWM 暗色（读回断言）
        {
            ui::DialogSpec spec;
            spec.title = L"主题自测";
            spec.message = L"深色主题弹出层 DWM 属性采样。";
            spec.buttons.push_back({L"确定", 1, true});
            spec.escId = 1;
            g_dialogHost.onOpened = [] {
                g_stThDialogDwm = ui::QueryImmersiveDark(
                    g_dialogHost.dialogWindowForTest());
                if (HWND h = g_dialogHost.dialogWindowForTest()) {
                    PostMessageW(h, WM_KEYDOWN, VK_RETURN, 0);
                }
            };
            g_dialogHost.showModal(g_mainWindow, spec);
            g_dialogHost.onOpened = nullptr;
        }
        PostMessageW(g_mainWindow, kMsgThemeSelftestStage, 0, 0);
    } else if (g_themeStage == 2) {
        g_themeStage = 3;
        // —— 查找对话框（深色下打开：DWM 属性 + EDIT 换肤创建路径走通即关闭）——
        g_searchDialog.onOpened = [] {
            HWND d = g_searchDialog.dialogWindowForTest();
            g_stThSearchDwm = d != nullptr && ui::QueryImmersiveDark(d);
            if (d) SendMessageW(d, WM_KEYDOWN, VK_ESCAPE, 0);  // 采样即关
        };
        RouteCommand(ui::Command::FindDialog);
        g_searchDialog.onOpened = nullptr;
        PostMessageW(g_mainWindow, kMsgThemeSelftestStage, 0, 0);
    } else if (g_themeStage == 3) {
        g_themeStage = 4;
        // —— 切回浅色：像素/编辑器全部回到浅 token ——
        RouteCommand(ui::Command::ToggleDarkTheme);
        g_docManager.closeDocument(g_docManager.activeDocument());
        g_stThBackLight =
            !ui::IsDarkTheme() && !ui::QueryImmersiveDark(g_mainWindow) &&
            SampleThemePixels(light.titleBarBackground, light.toolbarBackground,
                              light.contentBackground, light.statusBarBackground);
        g_docManager.newDocument();
        g_stThBackEditor = CheckEditorThemeColors(
            light.textPrimary, light.contentBackground, light.accent,
            light.searchMatch, static_cast<int>(light.searchMatch >> 24),
            light.searchMatchBorder);
        WriteThemeSelftestResult();
        PostMessageW(g_mainWindow, WM_CLOSE, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// M4-B 启动恢复（会话 → 几何 → 显示 → 草稿 → 冲突决策）+ 状态采样钩子
// ---------------------------------------------------------------------------

/// 文件存在性预检注入（parseSession）：缺失/目录按坏条目静默跳过（C1）
bool SessionFileProbe(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && std::filesystem::is_regular_file(path, ec);
}

/// 磁盘状态查询注入（恢复分类）：url → 磁盘加载归一化内容（LF、无 BOM）的
/// SHA-256；缺失/不可读 nullopt（LocalStorage 读 + sha256Hex，与写入侧同源）
std::optional<std::string> DiskHashOf(const std::filesystem::path& path) {
    const core::LocalStorage storage;
    auto loaded = storage.read(path, std::nullopt);
    if (!loaded) return std::nullopt;
    return core::sha256Hex(loaded->content);
}

/// 应用单条草稿恢复计划；返回落位文档（代开失败返回 nullptr）
core::Document* ApplyDraftRestorePlan(const core::DraftRestorePlan& plan) {
    if (plan.target == core::DraftRestorePlan::Target::ApplyToUrlDoc) {
        core::Document* doc = g_docManager.documentForPath(plan.url);
        if (!doc) {
            // 会话未打开该文件：代开（自动识别编码；打开失败按计划落空处理）
            auto opened = g_docManager.openPath(plan.url);
            if (opened) doc = *opened;
        }
        if (!doc) return nullptr;
        // 光标先设（Document 只钳负值，上界在此钳到草稿内容长度），setContent
        // 触发的 documentContentChanged → 编辑器重装时按文档光标落位（单次同步）
        doc->setCursorPosition(std::min(plan.entry.cursor,
                                        static_cast<int>(plan.entry.content.size())));
        doc->setContent(plan.entry.content);  // 应用为未保存修改（置脏 → ●）
        return doc;
    }
    // 未命名重建：沿用记录显示名（编号器跳占用），插入位置钳制后按位插入
    const int index = core::DraftCacheManager::clampInsertIndex(
        plan.insertIndex, static_cast<int>(g_docManager.documentCount()));
    core::Document* doc = g_docManager.restoreUntitledDraft(plan.entry.untitledName, index);
    // 光标先设、内容后写：setContent 触发的 documentContentChanged → 编辑器
    // 重装时按文档光标落位（与 ApplyToUrlDoc 分支同一次序，单次同步）
    doc->setCursorPosition(std::min(plan.entry.cursor,
                                    static_cast<int>(plan.entry.content.size())));
    doc->setContent(plan.entry.content);  // 内容即缓存 → 置脏（● 标记随事件亮起）
    return doc;
}

/// 草稿冲突汇总对话框（DEC-006 单框汇总；标题/文案按 M4-B 需求）。
/// @return 1=恢复 2=丢弃 0=未决（Esc——条目保留在缓存目录，下次启动再问）
int ShowDraftConflictDialog(const std::vector<core::DraftConflict>& conflicts) {
    ui::DialogSpec spec;
    spec.title = L"检测到未恢复的草稿";
    spec.message = L"下列文件在上次退出时有未保存的修改，可以恢复为未保存的修改或丢弃：\n";
    for (const core::DraftConflict& c : conflicts) {
        spec.message += L"\n" + c.displayName;
        spec.message += c.missing ? L"（文件已不存在）" : L"（文件已被其他程序修改）";
    }
    spec.widthLogical = 440;  // 旧 QML 草稿冲突框同宽
    spec.buttons.push_back({L"恢复", 1, true});
    spec.buttons.push_back({L"丢弃", 2, false});
    spec.escId = 0;  // Esc = 稍后处理：不动作（旧版同款，下次启动再次提示）
    return g_dialogHost.showModal(g_mainWindow, spec);
}

/// 枚举全部显示器工作区（validateGeometry 的 screens 注入）
std::vector<RECT> CollectWorkAreas() {
    std::vector<RECT> areas;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR, HDC, LPRECT rect, LPARAM lp) -> BOOL {
            reinterpret_cast<std::vector<RECT>*>(lp)->push_back(*rect);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&areas));
    return areas;
}

/// 默认几何：1000×640（主屏 DPI 换算）居中于主屏工作区（首启/几何离屏回退）
core::WindowGeometry DefaultGeometry() {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const UINT dpi = ui::WindowDpi(nullptr);
    core::WindowGeometry geom;
    geom.width = ui::Scale(kInitialWidthLogical, dpi);
    geom.height = ui::Scale(kInitialHeightLogical, dpi);
    geom.x = work.left + ((work.right - work.left) - geom.width) / 2;
    geom.y = work.top + ((work.bottom - work.top) - geom.height) / 2;
    geom.maximized = false;
    return geom;
}

/// 启动恢复（窗口显示前执行文档/几何恢复，显示后执行草稿恢复与冲突决策）。
/// 时序（对齐旧 Main.qml Component.onCompleted + M4-A 执行计划）：
///  1) parseSession（存在性预检注入）→ 按序 openPath（forced=记录编码）→
///     逐文档光标钳制恢复 → active 命中激活 / 未命中激活最后成功文档；
///  2) validateGeometry（真实屏幕工作区）→ 应用或默认 1000×640 居中；
///     先几何后最大化标志；
///  3) wordWrap 恢复（全局开关）；
///  4) ShowWindow（此前不可见——避免几何跳变闪烁）；
///  5) restore(drafts) 执行恢复计划；conflicts 非空 → 冲突汇总对话框 →
///     resolveConflicts 执行；激活按 activatedRestoredIndex / activateFirst；
///  6) 解除恢复屏蔽 → 恢复的脏文档按新 uid 种子入草稿防抖（旧版「续写」
///     语义：崩溃安全窗回到 1s）→ 统一 persistNow 一次（首启也写 [window]）。
void RestoreAtStartup(HWND hwnd) {
    g_restoring = true;

    const core::SessionData session = core::SessionManager::parseSession(
        g_paths.iniPath, &SessionFileProbe);

    // —— [editor] darkTheme（E5）：先于文档打开与窗口显示——编辑器实例 configure
    //    与 DWM/滚动条属性都按目标主题一次就位，无浅→深跳变闪烁
    if (session.darkTheme && *session.darkTheme) {
        SetDarkTheme(true);
    }

    // —— [editor] wordWrap（先于文档打开：新编辑器实例 configure 时继承）——
    if (session.wordWrap) {
        g_wordWrap = *session.wordWrap;
        g_editors.setWordWrap(g_wordWrap);
        RefreshMenuState();
    }

    // —— 1) 文档按序打开 + 光标恢复 ——
    core::Document* lastSuccess = nullptr;
    for (const auto& entry : session.documents) {
        if (entry.url.empty()) continue;  // 未命名占位（由草稿恢复按位重建）
        auto opened = g_docManager.openPath(entry.url, entry.encoding);
        if (!opened) continue;  // 打开失败静默跳过（C1：零弹错）
        core::Document* doc = *opened;
        // 光标：钳上界（Document 只钳负值）；编辑器经 reloadFromDocument 同步
        doc->setCursorPosition(std::min(entry.cursor,
                                        static_cast<int>(doc->content().size())));
        g_editors.reloadFromDocument(doc);
        lastSuccess = doc;
    }
    // active 命中激活 / 未命中（含 activeUid 场景兜底）激活最后成功文档；
    // activeUid 的精确匹配由草稿恢复的 activatedRestoredIndex 接力（C2 三级优先）
    core::Document* activeTarget = nullptr;
    if (session.activeUrl) {
        activeTarget = g_docManager.documentForPath(*session.activeUrl);
    }
    if (!activeTarget) activeTarget = lastSuccess;
    if (activeTarget) g_docManager.setActiveDocument(activeTarget);

    // —— 2) 窗口几何：校验后应用（先几何后最大化标志）——
    core::WindowGeometry geom = DefaultGeometry();
    if (session.window) {
        if (auto valid = core::SessionManager::validateGeometry(*session.window,
                                                                 CollectWorkAreas())) {
            geom = *valid;
        }
    }
    SetWindowPos(hwnd, nullptr, geom.x, geom.y, geom.width, geom.height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    g_lastNormal = geom;  // WM_MOVE/WM_SIZE 亦会记录（此处显式兜底）
    g_lastMaximized = false;

    // —— 4) 显示（几何已就位，无跳变；恢复最大化优先于 nCmdShow）——
    // --noactivate：可见但不抢前台焦点（metrics「启动到可见」计时口径保留）；
    // --headless（e2e/持久化路径）：完全不显示——e2e 驱动全为消息直投，
    // 无焦点语义依赖；几何断言用 GetWindowRect，不可见不受影响
    if (g_noActivate) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
    } else if (!g_headless) {
        ShowWindow(hwnd, session.window && session.window->maximized
                             ? SW_SHOWMAXIMIZED
                             : SW_SHOWNORMAL);
        UpdateWindow(hwnd);
    }

    // —— 5) 草稿恢复（纯计划 → 执行）——
    core::RestoreOutcome outcome =
        core::DraftCacheManager::restore(g_paths.draftsDir, session, &DiskHashOf);
    std::vector<core::Document*> applied;  // 与 restored 计划一一对应（失败为空）
    for (const auto& plan : outcome.restored) {
        applied.push_back(ApplyDraftRestorePlan(plan));
    }
    if (!outcome.conflicts.empty()) {
        const int choice = ShowDraftConflictDialog(outcome.conflicts);
        if (choice != 0) {  // 0 = Esc 未决：条目保留，下次启动再问
            core::ConflictResolution resolution =
                core::DraftCacheManager::resolveConflicts(
                    outcome.conflicts, choice == 1,
                    g_docManager.activeDocument() != nullptr, &DiskHashOf);
            core::Document* firstConflictApplied = nullptr;
            for (const auto& plan : resolution.plans) {
                core::Document* doc = ApplyDraftRestorePlan(plan);
                if (doc && !firstConflictApplied) firstConflictApplied = doc;
            }
            // 冲突恢复是用户显式交互决策：空场景激活首个恢复文档（DEC-006）
            if (resolution.activateFirst && firstConflictApplied) {
                g_docManager.setActiveDocument(firstConflictApplied);
            }
        }
    }
    // 激活三级优先的本轮落地：activatedRestoredIndex ≥ 0 → 激活对应恢复文档
    //（activeUid 匹配 / 空场景首个恢复；-1 = 维持会话恢复设定的活动标签）
    if (outcome.activatedRestoredIndex >= 0) {
        const auto idx = static_cast<size_t>(outcome.activatedRestoredIndex);
        if (idx < applied.size() && applied[idx]) {
            g_docManager.setActiveDocument(applied[idx]);
        }
    }

    // —— 6) 收尾：解除屏蔽 → 种子续写 → 统一 persistNow ——
    g_restoring = false;
    for (core::Document* doc : applied) {
        if (doc && doc->modified()) {
            g_drafts->onContentChanged(doc, NowMs());  // 按新 uid 续写（预期行为）
        }
    }
    PersistSessionNow();
    FocusActiveEditor();
}

// ---------------------------------------------------------------------------
// --selftest-dump 状态采样（e2e 断言辅助，仅测试钩子用途）
// ---------------------------------------------------------------------------

/// JSON 字符串转义（UTF-8 内容原样，控制字符/引号/反斜杠转义）
std::string JsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char raw : s) {
        const unsigned char c = static_cast<unsigned char>(raw);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    return out;
}

/// kMsgSelftestDump 处理：当前文档列表（名/路径/uid/光标/脏/内容）、激活
/// 下标、窗口矩形与 normal 几何 → JSON 写 g_selftestDumpPath（临时文件 +
/// 原子替换，避免采样方读到半截）。e2e 经 SendMessageTimeout 同步调用后读文件
void WriteSelftestDump() {
    if (g_selftestDumpPath.empty() || !g_mainWindow) return;
    std::string json = "{\n  \"documents\": [\n";
    const auto docs = g_docManager.documents();
    core::Document* active = g_docManager.activeDocument();
    int activeIndex = -1;
    for (size_t i = 0; i < docs.size(); ++i) {
        core::Document* doc = docs[i];
        if (doc == active) activeIndex = static_cast<int>(i);
        char editorHwnd[32] = "null";
        if (HWND sci = g_editors.windowForTest(doc)) {
            snprintf(editorHwnd, sizeof(editorHwnd), "\"0x%llX\"",
                     static_cast<unsigned long long>(
                         reinterpret_cast<ULONG_PTR>(sci)));
        }
        json += "    {\"name\": \"" + JsonEscape(core::wideToUtf8(doc->displayName())) +
                "\", \"url\": \"" + JsonEscape(core::wideToUtf8(doc->url().wstring())) +
                "\", \"uid\": \"" + JsonEscape(doc->uid()) +
                "\", \"cursor\": " + std::to_string(doc->cursorPosition()) +
                ", \"modified\": " + (doc->modified() ? "true" : "false") +
                ", \"editor\": " + editorHwnd +
                ", \"content\": \"" + JsonEscape(doc->content()) + "\"}";
        if (i + 1 < docs.size()) json += ",";
        json += "\n";
    }
    json += "  ],\n  \"activeIndex\": " + std::to_string(activeIndex) + ",\n";
    json += "  \"darkTheme\": " + std::string(ui::IsDarkTheme() ? "true" : "false") +
            ",\n";
    RECT wr{};
    GetWindowRect(g_mainWindow, &wr);
    json += "  \"window\": {\"x\": " + std::to_string(wr.left) +
            ", \"y\": " + std::to_string(wr.top) +
            ", \"width\": " + std::to_string(wr.right - wr.left) +
            ", \"height\": " + std::to_string(wr.bottom - wr.top) +
            ", \"maximized\": " + (IsZoomed(g_mainWindow) ? "true" : "false") + "},\n";
    json += "  \"normal\": {\"x\": " + std::to_string(g_lastNormal.x) +
            ", \"y\": " + std::to_string(g_lastNormal.y) +
            ", \"width\": " + std::to_string(g_lastNormal.width) +
            ", \"height\": " + std::to_string(g_lastNormal.height) +
            ", \"maximized\": " + (g_lastMaximized ? "true" : "false") + "}\n}\n";

    const std::filesystem::path out(g_selftestDumpPath);
    const std::filesystem::path tmp = out;
    auto tmpStr = tmp.wstring() + L".tmp";
    {
        std::ofstream f(tmpStr, std::ios::binary | std::ios::trunc);
        f << json;
    }
    MoveFileExW(tmpStr.c_str(), out.wstring().c_str(), MOVEFILE_REPLACE_EXISTING);
}

// ---------------------------------------------------------------------------
// DPI
// ---------------------------------------------------------------------------

void EnablePerMonitorV2Awareness() {
    // manifest 已声明 PerMonitorV2（src/qcnote.manifest 经 .rc 嵌入），此处运行时
    // 再设一次双保险。动态解析而非静态导入：exe 不出现该符号依赖。
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    const auto fn = reinterpret_cast<SetProcessDpiAwarenessContextFn>(::GetProcAddress(
        ::GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (fn) {
        fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
}

// ---------------------------------------------------------------------------
// 无边框窗口机制
// ---------------------------------------------------------------------------

// WM_NCCALCSIZE：客户区扩展到整窗（详见 M1 注释：创建期 FALSE 形态、最大化夹回工作区）
LRESULT HandleNcCalcSize(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    if (!wParam) {
        RECT windowRect{};
        GetWindowRect(hwnd, &windowRect);
        *reinterpret_cast<RECT*>(lParam) = windowRect;  // 客户区 = 整窗
        return 0;
    }
    auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
    if (IsZoomed(hwnd)) {
        HMONITOR mon = MonitorFromRect(&params->rgrc[0], MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
            RECT& r = params->rgrc[0];
            if (r.left < mi.rcWork.left) r.left = mi.rcWork.left;
            if (r.top < mi.rcWork.top) r.top = mi.rcWork.top;
            if (r.right > mi.rcWork.right) r.right = mi.rcWork.right;
            if (r.bottom > mi.rcWork.bottom) r.bottom = mi.rcWork.bottom;
        }
    }
    return 0;
}

// WM_NCHITTEST：屏幕坐标 → 客户坐标后复用 TitleBar 纯函数，叠加 Tab 条接管
LRESULT HandleNcHitTest(HWND hwnd, LPARAM lParam) {
    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    POINT c = pt;
    ScreenToClient(hwnd, &c);
    RECT client{};
    GetClientRect(hwnd, &client);
    SIZE size{client.right, client.bottom};
    const LRESULT hit = ui::ComputeCaptionHitTest(c, size, ui::WindowDpi(hwnd),
                                                  IsZoomed(hwnd) != FALSE);
    if (hit == HTCAPTION && g_tabs.hitInteractive(c)) {
        return HTCLIENT;
    }
    return hit;
}

void RestoreFromMaximizedAndDrag(HWND hwnd, POINT press);  // 定义见下

// 最大化时按住标题栏（需求 E1）：DragDetect 区分点击/拖动，拖动才还原+锚定
void HandleMaximizedCaptionPress(HWND hwnd, LPARAM pressLParam) {
    const POINT press{GET_X_LPARAM(pressLParam), GET_Y_LPARAM(pressLParam)};
    POINT client = press;
    ScreenToClient(hwnd, &client);
    if (!DragDetect(hwnd, client)) {
        g_titleBar.onNcMouseLeave();
        return;
    }
    RestoreFromMaximizedAndDrag(hwnd, press);
}

// 还原并锚定后继续系统拖拽（参考 Notepad++，详见 M1 注释）
void RestoreFromMaximizedAndDrag(HWND hwnd, POINT press) {
    RECT wr{};
    GetWindowRect(hwnd, &wr);
    const int zoomedWidth = wr.right - wr.left;
    const double ratio =
        zoomedWidth > 0 ? static_cast<double>(press.x - wr.left) / zoomedWidth : 0.5;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    GetWindowPlacement(hwnd, &wp);
    const int normalWidth = wp.rcNormalPosition.right - wp.rcNormalPosition.left;

    POINT now{};
    GetCursorPos(&now);
    int x = now.x - static_cast<int>(ratio * normalWidth);
    int y = now.y;  // 顶边对齐光标（垂直锚定）

    HMONITOR mon = MonitorFromPoint(now, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        if (x + normalWidth <= mi.rcWork.left) x = mi.rcWork.left;
        if (x >= mi.rcWork.right) x = mi.rcWork.right - normalWidth;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
    }

    ShowWindow(hwnd, SW_RESTORE);
    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(now.x, now.y));
}

// 右键标题栏 / Shift+F10：弹出系统菜单（E1）
void ShowSystemMenu(HWND hwnd, POINT pt) {
    HMENU menu = GetSystemMenu(hwnd, FALSE);
    if (!menu) return;
    const bool zoomed = IsZoomed(hwnd) != FALSE;
    EnableMenuItem(menu, SC_RESTORE, MF_BYCOMMAND | (zoomed ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(menu, SC_MOVE, MF_BYCOMMAND | (zoomed ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(menu, SC_SIZE, MF_BYCOMMAND | (zoomed ? MF_GRAYED : MF_ENABLED));
    EnableMenuItem(menu, SC_MINIMIZE, MF_BYCOMMAND | MF_ENABLED);
    EnableMenuItem(menu, SC_MAXIMIZE, MF_BYCOMMAND | (zoomed ? MF_GRAYED : MF_ENABLED));
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
}

// 窗口控制按钮统一触发：先记按下视觉，再执行动作（down 即执行，Win11 惯例）
void TriggerCaptionButton(ui::CaptionButton button) {
    g_titleBar.setPressed(button);
    g_titleBar.performAction(button);
}

// Win11：隐藏 DWM 自绘的 1px 系统窗口边线（与 E2 自绘边界线避免双线）
void SuppressDwmBorderLine(HWND hwnd) {
    const DWORD color = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &color, sizeof(color));
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------

LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_GETMINMAXINFO: {
        // 最小尺寸 800×500 逻辑按窗口 DPI 换算
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = ui::WindowDpi(hwnd);
        mmi->ptMinTrackSize.x = ui::Scale(ui::metrics::MinWindowWidth, dpi);
        mmi->ptMinTrackSize.y = ui::Scale(ui::metrics::MinWindowHeight, dpi);
        return 0;
    }
    case WM_CREATE: {
        g_mainWindow = hwnd;
        g_titleBar.attach(hwnd);
        SuppressDwmBorderLine(hwnd);
        // E5：创建即应用当前主题的系统表面属性（默认浅色；启动恢复深色时由
        // RestoreAtStartup 在显示前重设——自测/恢复路径统一经 ApplyThemeToWindowTree）
        ui::SetImmersiveDark(hwnd, ui::IsDarkTheme());
        ui::ApplyDarkExplorerThemeClass(hwnd, ui::IsDarkTheme());
        // M5 单实例：主实例在主窗口挂域标签（转发方按域匹配窗口；窗口销毁时
        // 系统自动回收属性）
        if (g_singleInstanceMutex) {
            platform::singleinstance::AttachDomainTag(hwnd, g_instanceDomainTag);
        }
        // —— 菜单系统 ——
        g_menuHost.onCommand = &RouteCommand;
        g_menuHost.onOpened = [] {};
        g_menuHost.onClosed = [] {};
        g_titleBar.onMenuButtonClicked = [] { OpenMainMenu(); };
        // —— Tab 条 + 欢迎页 ——
        g_tabs.attach(hwnd, &g_menuHost);
        g_tabs.onTabActivated = &OnTabActivated;
        g_tabs.onTabContextCommand = &OnTabContextCommand;
        g_welcome.attach(hwnd);
        g_welcome.setChromeInsets(ui::toolbar_metrics::Height, ui::statusbar_metrics::Height);
        g_welcome.onOpenClicked = [] { RouteCommand(ui::Command::OpenDialog); };
        // —— 工具栏 / 状态栏（E3）——
        g_toolbar.attach(hwnd);
        g_toolbar.onCommand = &RouteCommand;
        g_toolbar.setSaveEnabled(false);  // 初始无文档
        g_statusBar.attach(hwnd);
        // —— Scintilla 编辑器 ——
        g_editors.attach(hwnd);
        // g_editors.onUserEdited：内容事件订阅位（M4-B 草稿防抖经
        // DocumentManager.documentContentChanged 统一接线，此回调未使用；
        // SCN_MODIFIED 时光标尚未推进，此回调不做 UI 采样）
        // 状态栏刷新：光标路径（SCN_CHARADDED 即时 / SCN_UPDATEUI 空闲排队）
        g_editors.onCursorChanged = [](core::Document*) { RefreshStatusBar(); };
        // M3-B：实例窗口创建/销毁 → 多光标子类化挂接（Alt+Click/触发键拦截）
        g_editors.onWindowCreated = [](HWND sci) { g_multiCursor.attachWindow(sci); };
        g_editors.onWindowDestroyed = [](HWND sci) { g_multiCursor.detachWindow(sci); };
        // 多光标集合重建后的状态栏刷新（键入/移动路径经 SCN_UPDATEUI 已覆盖）
        g_multiCursor.onSelectionsRebuilt = [](HWND) { RefreshStatusBar(); };
        // —— 查找替换（B5）——
        g_searchDialog.setClient(&g_searchClient);
        g_searchCtl.onChanged = [] {
            if (g_searchDialog.active()) g_searchDialog.refreshState();
        };
        // —— DocumentManager 事件接线（M2-B 核心；M4-B 会话/草稿挂点并入）——
        g_docManager.documentsChanged.subscribe([] { OnDocumentsChanged(); });
        g_docManager.activeDocumentChanged.subscribe([] { OnActiveDocumentChanged(); });
        g_docManager.documentModifiedChanged.subscribe(
            [](core::Document* doc) { OnDocumentModifiedChanged(doc); });
        g_docManager.documentContentChanged.subscribe(
            [](core::Document* doc) { OnDocumentContentChanged(doc); });
        g_docManager.documentMetadataChanged.subscribe(
            [](core::Document* doc) { OnDocumentMetadataChanged(doc); });
        // 文档关闭（移除后、销毁前）：草稿「不保存」放弃清理 + 会话记忆
        g_docManager.documentClosed.subscribe([](core::Document* doc) {
            if (PersistenceActive() && !g_restoring) g_drafts->onDocumentClosed(doc);
            TriggerSessionSave();
        });
        // —— M4 防抖驱动定时器（200ms tick；会话 500ms / 草稿 1000ms 到期轮询）——
        SetTimer(hwnd, kTimerSessionFlush, 200, nullptr);
        // —— 初始布局 ——
        g_editors.layout(EditorContentRect(hwnd));
        RefreshStatusBar();
        // M1 Tab 自测：mock 假 Tab（仅该模式；正常模式数据来自文档系统）
        if (g_selftestTabs) {
            g_tabs.addTab({L"未命名-1", true});
            g_tabs.addTab({L"readme.md", false});
            g_tabs.addTab({L"很长的中文文件名测试用例.txt", false});
            g_tabs.setActiveIndex(0);
        }
        return 0;
    }
    case WM_NCCALCSIZE:
        return HandleNcCalcSize(hwnd, wParam, lParam);
    case WM_NCHITTEST:
        return HandleNcHitTest(hwnd, lParam);
    case WM_NCACTIVATE:
        return DefWindowProcW(hwnd, msg, wParam, static_cast<LPARAM>(-1));
    case WM_ACTIVATE:
        g_windowActive = LOWORD(wParam) != WA_INACTIVE;
        // C3 失焦即写：草稿立即落盘（把强杀丢失窗口从「最后 1 秒防抖」压缩到
        // 「失焦前 1 秒」）；挂起中的会话一并冲刷
        if (LOWORD(wParam) == WA_INACTIVE && PersistenceActive() && !g_restoring) {
            g_drafts->flushNow();
            if (g_sessionDebounce.pending()) {
                g_sessionDebounce.cancel();
                PersistSessionNow();
            }
        }
        InvalidateRect(hwnd, nullptr, FALSE);  // E2 边界线换色
        break;
    case WM_NCLBUTTONDOWN:
        switch (static_cast<int>(wParam)) {
        case HTCAPTION:
            if (IsZoomed(hwnd)) {
                HandleMaximizedCaptionPress(hwnd, lParam);
                return 0;
            }
            break;
        case HTCLOSE:
            TriggerCaptionButton(ui::CaptionButton::Close);
            return 0;
        case HTMINBUTTON:
            TriggerCaptionButton(ui::CaptionButton::Minimize);
            return 0;
        case HTMAXBUTTON:
            TriggerCaptionButton(ui::CaptionButton::MaximizeRestore);
            return 0;
        case HTBORDER:
            TriggerCaptionButton(ui::CaptionButton::Menu);
            return 0;
        default:
            break;
        }
        break;
    case WM_NCLBUTTONUP:
        g_titleBar.onLButtonUp();
        break;
    case WM_NCMOUSEMOVE:
        g_titleBar.onNcMouseMove({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        g_tabs.onMouseLeave();
        break;
    case WM_NCMOUSELEAVE:
        g_titleBar.onNcMouseLeave();
        return 0;
    case WM_NOTIFY:
        // Scintilla 通知（SCN_MODIFIED/SCN_UPDATEUI）→ 编辑器组件
        if (g_editors.handleNotify(reinterpret_cast<const NMHDR*>(lParam))) {
            return 0;
        }
        break;
    case kMsgSelftestStage:
        RunMenuSelftestStage();
        return 0;
    case kMsgTabsSelftestStage:
        RunTabsSelftestStage();
        return 0;
    case kMsgDocSelftestStage:
        RunDocSelftestStage();
        return 0;
    case kMsgSearchSelftestStage:
        RunSearchSelftestStage();
        return 0;
    case kMsgThemeSelftestStage:
        RunThemeSelftestStage();
        return 0;
    case kMsgSelftestDump:
        // --selftest-dump 采样钩子（e2e 跨进程断言辅助；同步处理后文件即就绪）
        WriteSelftestDump();
        return 0;
    case kMsgSelftestCommand:
        // e2e 命令触发钩子：仅 --headless 测试形态响应（真实运行窗口不被
        // 外部 WM_APP+7 驱动；e2e 全部以 --headless 启动——见常量处注释）
        if (!g_headless) return 0;
        switch (static_cast<int>(wParam)) {
        case 1: RouteCommand(ui::Command::NewDocument); break;      // = Ctrl+N
        case 2: CycleTab(+1); break;                                 // = Ctrl+Tab
        case 3: RouteCommand(ui::Command::CloseTab); break;         // = Ctrl+W
        case 4: RouteCommand(ui::Command::ToggleDarkTheme); break;  // = 菜单「深色主题」
        default: break;
        }
        return 0;
    case WM_COPYDATA: {
        // M5 单实例转发到达（D1）：只校验入队 + 投递消化消息，立即返回 TRUE——
        // 转发方 SendMessageTimeout 快速解除阻塞，实际打开在消息循环消化
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (platform::singleinstance::IsForwardPayload(cds)) {
            if (auto paths = platform::singleinstance::DecodeForwardPayload(cds->lpData,
                                                                           cds->cbData)) {
                g_forwardedOpenQueue.insert(g_forwardedOpenQueue.end(),
                                            paths->begin(), paths->end());
            }
            PostMessageW(hwnd, kMsgDrainForwardedPaths, 0, 0);
            return TRUE;  // 空列表也投递（仅激活）；坏帧吞掉不落默认处理
        }
        break;  // 非本协议消息：默认处理
    }
    case kMsgDrainForwardedPaths:
        // 打开队列消化：模态对话框打开期间推迟（定时器重试，防对话框重入叠加）
        if (UiLocked() && !g_forwardedOpenQueue.empty()) {
            SetTimer(hwnd, kTimerForwardDrainRetry, 200, nullptr);
            return 0;
        }
        KillTimer(hwnd, kTimerForwardDrainRetry);
        if (!g_forwardedOpenQueue.empty()) {
            // 「打开文件」语义：去重激活已有标签 + 失败逐个弹错误框（非会话
            // 恢复的静默跳过；空队列 = 仅激活转发）
            const auto paths = std::exchange(g_forwardedOpenQueue, {});
            DoOpenPaths(paths);
        }
        // 前置主窗口（旧版 activationRequested 语义：最小化先还原 + 前置；
        // 不可见测试窗口跳过——无激活语义且防自动化扰动）
        if (!g_headless && !g_noActivate) {
            platform::singleinstance::ActivateMainWindow(hwnd);
        }
        return 0;
    case WM_MOUSEMOVE: {
        const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (!g_trackingClientMouse) {
            TRACKMOUSEEVENT t{};
            t.cbSize = sizeof(t);
            t.dwFlags = TME_LEAVE | TME_NONCLIENT;
            t.hwndTrack = hwnd;
            g_trackingClientMouse = TrackMouseEvent(&t) != FALSE;
        }
        RECT client{};
        GetClientRect(hwnd, &client);
        const UINT dpi = ui::WindowDpi(hwnd);
        const int capH = ui::Scale(ui::metrics::CaptionHeight, dpi);
        if (pt.y < capH) {
            g_tabs.onMouseMove(pt);  // Tab 悬停/tooltip
        } else {
            g_tabs.onMouseLeave();
        }
        g_toolbar.onMouseMove(pt);   // 工具栏按钮悬停/tooltip（命中自判）
        g_statusBar.onMouseMove(pt); // 状态栏路径 tooltip（命中自判）
        if (g_tabs.count() == 0 && pt.y >= capH) {
            g_welcome.onMouseMove(pt);  // 欢迎页按钮悬停（仅无文档时）
        }
        break;
    }
    case WM_MOUSELEAVE:
        g_trackingClientMouse = false;
        g_tabs.onMouseLeave();
        g_toolbar.onMouseLeave();
        g_statusBar.onMouseLeave();
        g_welcome.onMouseLeave();
        return 0;
    case WM_LBUTTONDOWN: {
        const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT client{};
        GetClientRect(hwnd, &client);
        const UINT dpi = ui::WindowDpi(hwnd);
        const int capH = ui::Scale(ui::metrics::CaptionHeight, dpi);
        if (pt.y < capH) {
            g_tabs.onLButtonDown(pt);  // 命中 Tab → 激活
            return 0;
        }
        const int toolbarBottom =
            capH + ui::Scale(ui::toolbar_metrics::Height, dpi);
        if (pt.y < toolbarBottom) {
            g_toolbar.onLButtonDown(pt);  // 工具栏按钮（命中自判）
            return 0;
        }
        if (g_tabs.count() == 0) {
            g_welcome.onLButtonDown(pt);  // 欢迎页「打开文件…」（A5）
            return 0;
        }
        break;  // 编辑区：Scintilla 子窗口自行处理
    }
    case WM_RBUTTONDOWN: {
        const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (pt.y < ui::Scale(ui::metrics::CaptionHeight, ui::WindowDpi(hwnd))) {
            g_tabs.onRButtonDown(pt);  // 命中 Tab → 右键菜单（A3，不激活）
            return 0;
        }
        break;  // 编辑区右键：Scintilla（popup 已禁用，对齐旧版无右键菜单）
    }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &pt);
        if (g_tabs.overflow() && pt.y >= 0 &&
            pt.y < ui::Scale(ui::metrics::CaptionHeight, ui::WindowDpi(hwnd))) {
            g_tabs.onMouseWheel(msg == WM_MOUSEHWHEEL, delta);
            return 0;
        }
        break;  // 编辑区滚轮：Scintilla 原生处理
    }
    case WM_KEYDOWN: {
        // 主窗口持有焦点时（无文档/欢迎页）的键盘处理；编辑器焦点时按键直达
        // Scintilla（窗口级快捷键经消息循环拦截）
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (g_tabs.count() > 0) {
            if (g_tabs.onKeyDown(static_cast<UINT>(wParam), shift)) {
                return 0;
            }
        } else if (g_welcome.onKeyDown(static_cast<UINT>(wParam), shift)) {
            return 0;
        }
        break;
    }
    case WM_TIMER:
        g_tabs.onTimer(static_cast<UINT_PTR>(wParam));
        g_toolbar.onTimer(static_cast<UINT_PTR>(wParam));
        g_statusBar.onTimer(static_cast<UINT_PTR>(wParam));
        // M5 转发队列消化重试：模态打开期间排队，空闲后重投消化消息
        if (wParam == kTimerForwardDrainRetry) {
            KillTimer(hwnd, kTimerForwardDrainRetry);
            PostMessageW(hwnd, kMsgDrainForwardedPaths, 0, 0);
            return 0;
        }
        // M4 防抖到期轮询（200ms tick）：草稿 1000ms / 会话 500ms（恢复期屏蔽）
        if (wParam == kTimerSessionFlush && PersistenceActive() && !g_restoring) {
            const long long now = NowMs();
            g_drafts->flushDue(now);
            if (g_sessionDebounce.flushDue(now)) PersistSessionNow();
        }
        return 0;
    case WM_KILLFOCUS:
        g_tabs.onWindowKillFocus();
        break;
    case WM_CONTEXTMENU: {
        const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const bool keyboard = (pt.x == -1 && pt.y == -1);
        POINT c = pt;
        ScreenToClient(hwnd, &c);
        const ui::TitleBarLayout layout = g_titleBar.layout();
        const bool onButton = ui::CaptionButtonAt(layout, c) != ui::CaptionButton::None;
        const bool onTab =
            !keyboard && c.y >= 0 && c.y < layout.captionHeight && g_tabs.hitTab(c) >= 0;
        const bool inCaption =
            (keyboard || (c.y >= 0 && c.y < layout.captionHeight)) && !onButton && !onTab;
        if (inCaption) {
            POINT menuPt = pt;
            if (keyboard) {
                RECT wr{};
                GetWindowRect(hwnd, &wr);
                menuPt = {wr.left + 10, wr.top + 10};
            }
            ShowSystemMenu(hwnd, menuPt);
            return 0;
        }
        break;  // 编辑区：Scintilla（无右键菜单，SCI_USEPOPUP NONE）
    }
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        g_titleBar.onDpiChanged();
        g_tabs.onDpiChanged();
        // Scintilla 经 WM_DPICHANGED_AFTERPARENT 自行重测字体；布局经 WM_SIZE 重排
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_SIZE:
        g_tabs.onLayoutChanged();
        g_editors.layout(EditorContentRect(hwnd));
        // M4 会话：normal 几何仅在非最大化态记录（最大化期间保持上次 Windowed
        // 几何，「还原」回到最大化前尺寸）；最大化标志随状态迁移更新
        if (wParam == SIZE_MAXIMIZED) {
            if (!g_lastMaximized) {
                g_lastMaximized = true;
                TriggerSessionSave();
            }
        } else if (wParam != SIZE_MINIMIZED && !IsZoomed(hwnd)) {
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            bool changed = g_lastNormal.width != wr.right - wr.left ||
                           g_lastNormal.height != wr.bottom - wr.top;
            g_lastNormal.width = wr.right - wr.left;
            g_lastNormal.height = wr.bottom - wr.top;
            if (g_lastMaximized) {
                g_lastMaximized = false;
                changed = true;
            }
            if (changed) TriggerSessionSave();
        }
        return 0;
    case WM_MOVE: {
        // 位置记录与 WM_SIZE 同门：仅 Windowed 态（最大化/最小化期间的移动
        // 不是 normal 几何）
        if (!IsZoomed(hwnd) && !IsIconic(hwnd)) {
            const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
            if (g_lastNormal.x != x || g_lastNormal.y != y) {
                g_lastNormal.x = x;
                g_lastNormal.y = y;
                TriggerSessionSave();
            }
        }
        return 0;
    }
    case WM_CLOSE:
        // C4 热退出：无模态对话框打开时直接退出免提示（未保存修改经草稿保留，
        // 下次启动恢复）；模态期间拒绝关闭
        if (UiLocked()) return 0;
        if (PersistenceActive()) {
            g_drafts->flushNow();  // 挂起草稿立即落盘（含防抖未到期的键入）
            g_sessionDebounce.cancel();
            PersistSessionNow();
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;  // 背景由 WM_PAINT 全覆盖绘制，禁系统擦除防闪白
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client{};
        GetClientRect(hwnd, &client);
        // 组装顺序：标题栏底色/按钮 → Tab 条 → 工具栏 → 欢迎页（无文档）→
        // 状态栏 → E2 边界线最后画。编辑区由 Scintilla 子窗口自绘
        // （WS_CLIPCHILDREN 保证父窗口绘制不覆盖子窗口）
        g_titleBar.paint(dc, client, g_windowActive, IsZoomed(hwnd) != FALSE);
        g_tabs.paint(dc);
        g_toolbar.paint(dc, client);
        if (g_tabs.count() == 0) {
            g_welcome.paint(dc, client);
        }
        g_statusBar.paint(dc, client);
        g_titleBar.paintWindowBorder(dc, client, g_windowActive, IsZoomed(hwnd) != FALSE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        g_editors.detach();
        g_toolbar.detach();
        g_statusBar.detach();
        g_tabs.detach();
        g_welcome.detach();
        g_titleBar.detach();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR cmdLine, int nCmdShow) {
    // DPI 感知必须最先设置（先于任何窗口创建与 DC 查询）
    EnablePerMonitorV2Awareness();
    // 自动化验证参数解析 + 启动位置参数（D2）
    ParseSelftestArg(cmdLine);
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 1; i < argc; ++i) {
                const std::wstring arg = argv[i];
                if (arg.rfind(L"--userdir=", 0) == 0) {
                    // 测试钩子（仅 e2e 隔离用）：重定向用户根目录，防止自动化
                    // 验证污染真实 %LOCALAPPDATA%\qcnote（便携判定不受影响）。
                    // 同时派生单实例互斥量域（M5）：不同 userdir = 不同域
                    g_userDirOverride = arg.substr(10);
                } else if (arg.rfind(L"--selftest-dump=", 0) == 0) {
                    // 测试钩子：启用 kMsgSelftestDump 状态采样（输出 JSON 到该路径）
                    g_selftestDumpPath = arg.substr(16);
                } else if (arg == L"--headless") {
                    // 测试钩子：不显示窗口（消息直投/几何断言不受影响），
                    // 并把窗口停泊到离屏坐标——弹出菜单/对话框以主窗口为锚，
                    // 停泊保证它们也渲染在屏幕外，自动化全程零可见元素
                    g_headless = true;
                } else if (arg == L"--noactivate") {
                    // 测试钩子：SW_SHOWNOACTIVATE 显示（不抢前台焦点；metrics 用）
                    g_noActivate = true;
                } else if (arg == L"--no-shell-prompt") {
                    // 测试钩子（M5）：跳过首启右键菜单提示（不读不写注册表）
                    g_noShellPrompt = true;
                } else if (arg.rfind(L"--selftest-shell-register=", 0) == 0) {
                    constexpr wchar_t kPrefix[] = L"--selftest-shell-register=";
                    g_shellHookOp = ShellHookOp::Register;
                    g_shellHookSuffix = arg.substr(wcslen(kPrefix));
                } else if (arg.rfind(L"--selftest-shell-unregister=", 0) == 0) {
                    constexpr wchar_t kPrefix[] = L"--selftest-shell-unregister=";
                    g_shellHookOp = ShellHookOp::Unregister;
                    g_shellHookSuffix = arg.substr(wcslen(kPrefix));
                } else if (arg.rfind(L"--selftest-shell-query=", 0) == 0) {
                    constexpr wchar_t kPrefix[] = L"--selftest-shell-query=";
                    g_shellHookOp = ShellHookOp::Query;
                    g_shellHookSuffix = arg.substr(wcslen(kPrefix));
                } else if (!arg.empty() && arg[0] != L'-') {
                    // 位置参数 = 待打开文件（D2）。引号包裹由 CommandLineToArgvW
                    // 剥离；相对路径按当前目录绝对化（旧版 QUrl::fromUserInput
                    // 语义）——转发与会话去重都需要绝对形态
                    g_initialPaths.push_back(
                        platform::singleinstance::AbsolutizeForOpen(
                            std::filesystem::path(arg)));
                }
            }
            LocalFree(argv);
        }
    }

    // —— M5 shell 集成测试钩子（TC-I11）：临时键名注入执行后立即退出——
    // 不创建窗口、不参与单实例、不触碰会话（生产键全程不读写）
    if (g_shellHookOp != ShellHookOp::None) {
        platform::shell::Config cfg;
        cfg.menuKeyName = L"qcnote-selftest-" + g_shellHookSuffix;
        cfg.markerRoot = L"Software\\qcnote-selftest-" + g_shellHookSuffix;
        platform::shell::ShellIntegration shell(cfg);
        std::wstring error;
        switch (g_shellHookOp) {
        case ShellHookOp::Register:
            // 与生产首启「添加」等价：写菜单 + 首启标记
            return shell.applyContextMenuChoice(true, ExePathOf(), &error) ? 0 : 1;
        case ShellHookOp::Unregister:
            return shell.unregisterContextMenu(&error) ? 0 : 1;
        case ShellHookOp::Query:
            return shell.isRegistered() ? 0 : 1;
        default:
            return 0;
        }
    }

    // —— M5 单实例（D1）：自测模式不参与；测试钩子启动（--headless/--noactivate
    // 且无 --userdir）不参与（tools/ 冒烟/指标脚本形态——避免与同机真实实例
    // 基础域互相干扰）；--headless+--userdir（e2e 形态）按 userdir 域参与 ——
    if (!SelftestModeActive() &&
        (!g_userDirOverride.empty() || (!g_headless && !g_noActivate))) {
        g_instanceDomainTag =
            platform::singleinstance::DomainTag(g_userDirOverride.wstring());
        SetLastError(ERROR_SUCCESS);
        g_singleInstanceMutex = CreateMutexW(
            nullptr, FALSE,
            platform::singleinstance::MutexName(g_userDirOverride.wstring()).c_str());
        if (g_singleInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            // 已有主实例：转发命令行（空列表 = 仅激活，旧版语义）→ 成功即以
            // 退出码 0 结束；失败（窗口枚举不到/发送超时）降级独立启动，不阻断
            CloseHandle(g_singleInstanceMutex);
            g_singleInstanceMutex = nullptr;
            if (platform::singleinstance::TryForwardToPrimary(
                    g_initialPaths, g_instanceDomainTag, kMainWindowClass)) {
                return 0;
            }
            // 降级实例不持锁不挂域标签：后续二次启动仍只会命中真实主实例
        }
        // CreateMutex 失败（极罕见）：无锁独立运行（D1 降级），启动不中断
    }

    // —— M4-B 落点解析（DEC-014：便携 = exe 旁标记；否则 %LOCALAPPDATA%\qcnote；
    // --userdir 覆写用户根目录仅测试钩子）+ 草稿管理器构造 ——
    {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        const std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        g_paths = g_userDirOverride.empty()
                      ? core::SessionManager::resolvePaths(exeDir)
                      : core::SessionManager::resolvePaths(exeDir, g_userDirOverride);
        g_drafts = std::make_unique<core::DraftCacheManager>(
            g_paths.draftsDir, [] { return NowMs(); });
    }

    // Scintilla 以静态库形式链入：必须先注册其窗口类，进程内才能创建 Scintilla 控件
    if (!Scintilla_RegisterClasses(hInstance)) {
        MessageBoxW(nullptr, L"Scintilla 窗口类注册失败", L"qcnote", MB_ICONERROR);
        return 1;
    }

    // 图标从 exe 资源加载（qcnote.rc 嵌入的多尺寸 ico）
    const HICON bigIcon = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(kIconResourceId), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    const HICON smallIcon = static_cast<HICON>(LoadImageW(
        hInstance, MAKEINTRESOURCEW(kIconResourceId), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;  // 尺寸变化整窗重绘（自绘窗口必需）
    wc.lpfnWndProc = MainWindowProc;
    wc.hInstance = hInstance;
    wc.hIcon = bigIcon;
    wc.hIconSm = smallIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // 背景由 WM_PAINT 绘制（WM_ERASEBKGND 返回 1）
    wc.lpszClassName = kMainWindowClass;
    if (!RegisterClassExW(&wc)) {
        return 1;
    }

    // 保留完整 WS_OVERLAPPEDWINDOW（系统缩放边框/DWM 阴影/Snap/Alt+Space）；
    // WS_CLIPCHILDREN：Scintilla 子窗口区域从父窗口绘制中裁除（防闪烁/防覆盖）。
    // M4-B：窗口初始不可见——恢复几何/文档后再 ShowWindow（避免几何跳变）
    const UINT primaryDpi = ui::WindowDpi(nullptr);
    HWND hwnd = CreateWindowExW(
        0, kMainWindowClass, L"qcnote", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        ui::Scale(kInitialWidthLogical, primaryDpi), ui::Scale(kInitialHeightLogical, primaryDpi),
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        return 1;
    }

    // --headless 离屏停泊（恢复/自测逻辑之前）：弹出层/对话框以主窗口几何为
    // 锚，停泊到 -32000 保证它们也出现在屏幕外；TOOLWINDOW 扩展样式去掉任务
    // 栏按钮；后续恢复流程的 SetWindowPos 会覆盖为真实几何（不可见，无影响）
    if (g_headless) {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                          GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_TOOLWINDOW);
        SetWindowPos(hwnd, nullptr, -32000, -32000, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                         SWP_FRAMECHANGED);
    }

    if (PersistenceActive()) {
        // M4-B 启动恢复（会话文档/几何 → 显示 → 草稿 → 冲突决策；含 ShowWindow）
        RestoreAtStartup(hwnd);
    } else {
        // 自测模式：不恢复会话/草稿，保持 M1~M3 自测语义（mock 数据不被污染）。
        // --headless（自测路径）：窗口「可见但离屏」——部分自测断言依赖焦点
        // 语义（GetFocus/SetFocus 要求窗口可见），故用 TOOLWINDOW（无任务栏
        // 按钮）+ 离屏坐标 + SW_SHOWNOACTIVATE（不激活不抢前台）：对 Windows
        // 可见、对用户不可见、全程零前台抢占；弹出层锚点随之离屏
        if (g_headless) {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            // 未激活窗口线程内无焦点（自测的焦点断言需要）——同线程显式
            // SetFocus：不激活窗口、不触发前台抢占
            SetFocus(hwnd);
        } else if (g_noActivate) {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        } else {
            ShowWindow(hwnd, nCmdShow);
        }
        UpdateWindow(hwnd);
    }

    // 启动位置参数打开（D2；恢复完成后叠加打开——旧版 openPendingUrls 时序；
    // 事件接线已在 WM_CREATE 完成）
    if (!g_initialPaths.empty() && !g_selftestMenu && !g_selftestTabs &&
        !g_selftestSearch && !g_selftestTheme) {
        DoOpenPaths(g_initialPaths);
    }

    // —— M5 shell 集成首启提示（D3；旧版语义：首启**询问**而非自动注册）——
    // 时序对齐旧版：会话恢复 + 启动参数打开完成后再弹；自动化形态
    //（自测模式/--headless/--noactivate/--no-shell-prompt）一律跳过——不读不写
    // 注册表，杜绝测试机首启弹框（模态会锁 WM_CLOSE）与真实键污染
    if (!SelftestModeActive() && !g_headless && !g_noActivate && !g_noShellPrompt) {
        platform::shell::ShellIntegration shell;  // 生产默认键
        if (shell.contextMenuPromptNeeded()) {
            ui::DialogSpec spec;
            spec.title = L"添加右键菜单";
            spec.message = L"是否将「用 qcnote 打开」加入资源管理器右键菜单？";
            spec.buttons.push_back({L"添加", 1, true});
            spec.buttons.push_back({L"不添加", 2, false});
            spec.escId = 2;  // Esc 视为「不添加」（旧版 closePolicy Escape → onRejected）
            const int choice = g_dialogHost.showModal(hwnd, spec);
            std::wstring error;
            if (!shell.applyContextMenuChoice(choice == 1, ExePathOf(), &error)) {
                ShowErrorDialog(L"右键菜单设置失败", error);
            }
        }
    }

    if (g_selftestMenu) {
        PostMessageW(hwnd, kMsgSelftestStage, 0, 0);
    } else if (g_selftestTabs) {
        PostMessageW(hwnd, kMsgTabsSelftestStage, 0, 0);
    } else if (g_selftestDoc) {
        PostMessageW(hwnd, kMsgDocSelftestStage, 0, 0);
    } else if (g_selftestSearch) {
        PostMessageW(hwnd, kMsgSearchSelftestStage, 0, 0);
    } else if (g_selftestTheme) {
        PostMessageW(hwnd, kMsgThemeSelftestStage, 0, 0);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // 窗口级快捷键拦截（Ctrl+N/O/S/W、Ctrl+Shift+S、Ctrl+Tab/Shift+Tab、
        // Ctrl+F/H、F3/Shift+F3）：焦点在编辑器时同样生效；菜单/对话框模态
        // 循环期间主循环不泵消息，天然屏蔽
        if (msg.message == WM_KEYDOWN && !g_selftestMenu && !g_selftestTabs &&
            HandleGlobalShortcut(static_cast<UINT>(msg.wParam))) {
            continue;  // 已消费：不产生 WM_CHAR、不分发
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
