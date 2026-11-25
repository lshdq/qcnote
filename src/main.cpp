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
// 外部文件改动检测（core/externalchangemonitor + main.cpp 接线，2026-11）：
//       仅活动文档受检——timer 7 轮询（默认 1000ms）+ 切换标签/窗口激活立即
//       触发；stat 初筛 + 内容哈希两级校验。干净文档静默自动重载（编码自动
//       识别），脏文档三选决策框（重新加载/保留修改/另存为…），删除一次性
//       提示；基线快照刷新 = 写路径事件订阅 + 显式刷新双保险（DEC-005）。
//       测试钩子：--watch-interval-ms=<n>（轮询间隔注入）与
//       --selftest-filewatch[=<结果文件>]（WM_APP+12 阶段推进，生产 poll tick
//       驱动等待）。
//
// 模块职责划分：
//   ui/theme    —— 语义主题 token + DPI 换算
//   ui/titlebar —— 标题栏布局/自绘/按钮交互
//   ui/tabs     —— Tab 条（数据来自 DocumentManager 快照）
//   ui/welcome  —— 欢迎页（无文档时显示；内容区随工具栏/状态栏 chrome 内缩）
//   ui/menu     —— 自绘弹出菜单框架；ui/mainmenu 主菜单内容
//   ui/toolbar  —— 工具栏（新建/打开/保存/自动换行，E3 + F2）
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
// M5：单实例/shell；M6：深色主题；外部文件改动检测（timer 7 轮询 + 决策编排）。

#include <windows.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM
#include <dwmapi.h>    // DwmSetWindowAttribute（Win11 隐藏系统边框线）
#include <commdlg.h>   // GetOpenFileNameW / GetSaveFileNameW
#include <shellapi.h>  // CommandLineToArgvW
#include <imm.h>       // IMN_* 常量（IME 消息注入自查；不调用 IMM 函数）

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Scintilla.h"  // Scintilla_RegisterClasses：静态链接时替代 DLL 自注册

#include "core/document.h"
#include "core/documentmanager.h"
#include "core/externalchangemonitor.h"
#include "core/format.h"
#include "core/draftcachemanager.h"
#include "core/encoding.h"
#include "core/localstorage.h"
#include "core/sessionmanager.h"
#include "core/textutil.h"
#include "editor/multicursorcontroller.h"
#include "editor/scintillaedit.h"
#include "editor/searchcontroller.h"
#include "platform/explorer.h"
#include "platform/shellintegration.h"
#include "platform/singleinstance.h"
#include "ui/commands.h"
#include "ui/dialog.h"
#include "ui/mainmenu.h"
#include "ui/menu.h"
#include "ui/numberingdialog.h"
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
// 列模式填充编号自测推进（--selftest-numbering；+10 在 MenuHost/对话框
// WM_APP..+9 测试透传段之外，与既有自测模式互斥不并发）
constexpr UINT kMsgNumberingSelftestStage = WM_APP + 10;
// DPI 变化后行号边宽度重算（主窗口 WM_DPICHANGED 处理内 PostMessage
// 「延迟一拍」触发：入队晚于 WM_DPICHANGED 的 sent-message 链，执行时
// Scintilla 子窗口已按新 DPI 重测字体（WM_DPICHANGED_AFTERPARENT））
constexpr UINT kMsgRefreshLineNumbers = WM_APP + 11;
// 外部文件改动检测自测阶段推进（--selftest-filewatch；+12 在 MenuHost/对话框
// WM_APP..+9 测试透传段与 +10/+11 已占编号之外顺延，与既有自测模式互斥不并发）
constexpr UINT kMsgFileWatchSelftestStage = WM_APP + 12;

// 主窗口宿主 timer id 分配（tabs.cpp / tabs.h / toolbar.h / statusbar.h 同源约定，
// 单窗口内必须互斥，否则 WM_TIMER 互相误路由）：
//   1 = 会话防抖轮询 / 2 = Tab tooltip / 3 = 工具栏 tooltip / 4 = 状态栏
//   tooltip / 5 = 转发队列消化重试 / 6 = Tab 拖拽让位动画（kDragAnimTimerId）
//   7 = 外部文件改动检测轮询（kTimerFileWatch）
constexpr UINT_PTR kTimerSessionFlush = 1;
// M5 转发队列消化重试定时器（模态对话框打开期间推迟消化，防重入叠加）
constexpr UINT_PTR kTimerForwardDrainRetry = 5;
static_assert(kTimerForwardDrainRetry >= 5,
              "宿主 timer id 须避开 1-4（会话轮询/Tab/工具栏/状态栏已占用）");
// 外部文件改动检测轮询定时器（间隔 g_watchIntervalMs，默认 1000ms；DEC-001）：
// 每 tick 对活动文档跑一轮检测（PollActiveDocumentNow）；切换标签/窗口激活的
// 立即检测不经此定时器。恢复期/模态期间 tick 内跳过检测（防重入）
constexpr UINT_PTR kTimerFileWatch = 7;

ui::TitleBar g_titleBar;
// 标题栏窗口控制按钮「按下会话」（防点击穿透，方案 A）：按下只记录 + SetCapture，
// 捕获期间移动/松开以客户区消息形态回到本窗口；松开时仍在原按钮上才执行动作。
// SetCapture 保证物理松开事件必被本窗口消化——修复旧「down 即执行」在窗口销毁
// 后 mouse-up 穿透到身后应用的缺陷（状态机见 ui::CaptionPressSession）
ui::CaptionPressSession g_captionPress;
ui::MenuHost g_menuHost;            // 菜单弹出层（模态 track；Tab 右键菜单复用）
ui::MainMenuState g_menuState;      // 弹出前按文档状态刷新
ui::TabStrip g_tabs;                // Tab 条（数据 = DocumentManager 快照）
ui::WelcomePage g_welcome;          // 欢迎页（无文档时显示，A5）
ui::Toolbar g_toolbar;              // 工具栏（E3）
ui::StatusBar g_statusBar;          // 状态栏（E3）
ui::DialogHost g_dialogHost;        // 基础对话框（E4 基座）
ui::SearchDialog g_searchDialog;    // 查找替换对话框（B5，M3-B）
ui::NumberingDialog g_numberingDialog;  // 插入编号对话框（DEC-009：全局实例，
                                        // 与 g_searchDialog 同语义接入 UiLocked，模态
                                        // 期间单实例转发推迟、活动编辑器不被切换/销毁）
editor::EditorHost g_editors;       // Scintilla 编辑器（每文档一实例）
editor::MultiCursorHost g_multiCursor;      // 多光标交互映射（B4，M3-B）
editor::SearchController g_searchCtl;       // 查找控制器（B5，M3-B）
core::DocumentManager g_docManager; // 全局唯一文档管理器（M2-A）
HWND g_mainWindow = nullptr;
// 窗口标题同值短路缓存（F1）：RefreshWindowTitle 位于选区拖拽的逐帧采样路径
// （SCN_UPDATEUI → metadata 链），标题未变化时跳过 SetWindowTextW。初始为空，
// 首次调用（无文档标题 L"qcnote"）会多设置一次，无害
std::wstring g_lastWindowTitle;
int g_stTitleSets = 0;  // 自测采样：SetWindowTextW 实际执行次数（F1 短路验证）
bool g_windowActive = true;    // E2 边界线活动/非活动换色（标题栏配色不随焦点变化，E1）
bool g_trackingClientMouse = false;  // 客户区 TrackMouseEvent（TME_LEAVE）挂载标志
bool g_fileDialogOpen = false;       // 文件对话框打开中（uiLocked 语义 + WM_CLOSE 拒绝）
bool g_wordWrap = false;             // 自动换行全局开关（旧版全局语义，B2）
bool g_lineNumbers = true;           // 显示行号全局开关（默认显示，会话持久化）
bool g_autoIndent = true;            // 回车自动缩进全局开关（默认开启，会话持久化）

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

// —— 外部文件改动检测（需求-外部文件改动检测）生产状态 ——
// 检测器生产注入（stat 三分采样 + LocalStorage 归一化内容哈希比对，实现见
// 「外部文件改动检测编排」段）：poll 由 timer 7 / 切换标签 / 窗口激活的
// PollActiveDocumentNow 驱动（DEC-001 单线程事件驱动，无竞态）
std::optional<core::FileStatSample> StatFileForWatch(const std::filesystem::path& path);
bool DiskContentMatchesBaseline(const std::filesystem::path& path, const core::Document& doc);
core::ExternalChangeMonitor g_fileWatch(&StatFileForWatch, &DiskContentMatchesBaseline);
int g_watchIntervalMs = 1000;  // 轮询间隔（--watch-interval-ms=<n> 测试钩子注入短值）
// 基线刷新 url 比对表（uid → 上次刷新时 url）：OnDocumentMetadataChanged 高频
// 触发（光标每次移动都发 metadata 事件），无条件刷新会在打字期间逐键重置快照
// 吸收外部改动——按 url 变化才刷新（DEC-010）
std::unordered_map<std::string, std::filesystem::path> g_fileWatchLastUrl;

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
// Tab 拖拽重排场景（TC-005..009；mock 直驱，DEC-003）
bool g_stTabsDragCommit = false, g_stTabsDragClick = false, g_stTabsDragEsc = false;
bool g_stTabsDragCapture = false, g_stTabsDragEdge = false;
int g_stReorderedFrom = -1;   // 最近一次 onTabReordered 的 from（自测采样）
int g_stReorderedTo = -1;     // 最近一次 onTabReordered 的 to
int g_stReorderedCount = 0;   // onTabReordered 触发计数

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

// —— 列模式填充编号自动化验证（--selftest-numbering=<结果文件>）状态 ——
bool g_selftestNumbering = false;
std::wstring g_selftestNumberingOut;
int g_numberingStage = 0;

// —— 外部文件改动检测自动化验证（--selftest-filewatch=<结果文件>）状态 ——
// 断言采样集合/阶段推进机制见「外部文件改动检测自动化验证」段（DEC-011）
bool g_selftestFileWatch = false;
std::wstring g_selftestFileWatchOut;
int g_fileWatchStage = 0;

// ---------------------------------------------------------------------------
// 状态查询/刷新（事件驱动）
// ---------------------------------------------------------------------------

// 持久化总开关：自测模式（menu/tabs/doc/search/theme/numbering/filewatch）
// 完全跳过会话/草稿读写——保持既有自测语义不被真实用户目录数据破坏，也不向
// 真实用户目录写入
bool SelftestModeActive() {
    return g_selftestMenu || g_selftestTabs || g_selftestDoc || g_selftestSearch ||
           g_selftestTheme || g_selftestNumbering || g_selftestFileWatch;
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

/// 立即落盘当前会话（文档 + normal 几何 + 最大化标志 + wordWrap/lineNumbers/autoIndent/darkTheme）
void PersistSessionNow() {
    if (!PersistenceActive()) return;
    core::SessionData data = core::SessionManager::buildSessionData(g_docManager);
    data.window = g_lastNormal;
    data.window->maximized = g_lastMaximized;
    data.wordWrap = g_wordWrap;
    data.lineNumbers = g_lineNumbers;
    data.autoIndent = g_autoIndent;
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
           g_numberingDialog.active() || g_menuHost.tracking();
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
    // 同值短路（F1）：拖拽选区等高频路径逐帧经过此函数，标题未变化时不再
    // 打扰系统（SetWindowTextW 触发系统侧文本重设）；只有标题真变才设置
    if (title == g_lastWindowTitle) return;
    g_lastWindowTitle = title;
    ++g_stTitleSets;  // 自测采样：实际调用次数（单测断言「未变 0 次 / 变化 1 次」）
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
    g_menuState.lineNumbers = g_lineNumbers;
    g_menuState.autoIndent = g_autoIndent;
    // 编辑▸插入编号使能条件之一：活动编辑器选区数 ≥ 2（多光标态）；无文档
    //（欢迎页）为 false。弹出前 RefreshMenuState 全量刷新（OpenMainMenu 唯一
    // 生产调用点，勾选/置灰实时），多光标增删后无需额外事件驱动
    g_menuState.multiCursorActive =
        editor::MultiCursorHost::active(g_editors.activeWindow());
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

// Tab 数据 = 管理器文档列表快照（displayName + modified + hasPath——hasPath
// 驱动右键菜单「文件重命名…/打开所在文件夹」可用性）
void RefreshTabsFromManager() {
    std::vector<ui::TabItem> items;
    for (core::Document* doc : g_docManager.documents()) {
        items.push_back({doc->displayName(), doc->modified(), !doc->url().empty()});
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
    // 拖拽重排（moveDocument）只发 documentsChanged 不发 activeDocumentChanged
    //（活动文档指针不动）：setItems 保留旧数值 activeIndex_ 会高亮到别的标签，
    // 此处从活动文档指针精确映射（与 RefreshAllAfterActiveChanged 同手法，
    // 对打开/关闭/新建等其余路径幂等）
    const int idx = IndexOfDocument(g_docManager.activeDocument());
    if (idx >= 0) g_tabs.setActiveIndex(idx);
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
// 外部文件改动检测一轮编排（定义见「外部文件改动检测编排」段）：切换标签后
// 立即检测活动文档一次（需求 3：非活动文档不做后台监测，切回时立即检测）
void PollActiveDocumentNow();

void OnActiveDocumentChanged() {
    RefreshAllAfterActiveChanged();
    // M4 会话：活动标签变化 → 防抖记忆
    TriggerSessionSave();
    // 外部文件改动检测：切换标签立即检测一次（不等下一轮 tick；恢复期跳过
    // ——启动恢复的文档逐个打开不触发检测，恢复完成后由首个 poll 兜底建快照；
    // 模态期间活动切换不可达，UiLocked 为防御性判定）
    if (!g_restoring && !UiLocked()) {
        PollActiveDocumentNow();
    }
}
void OnDocumentModifiedChanged(core::Document* doc) {
    const int idx = IndexOfDocument(doc);
    if (idx >= 0) g_tabs.setModified(idx, doc->modified());
    if (doc == g_docManager.activeDocument()) RefreshWindowTitle();
    // M4 草稿：脏→干净翻转（保存/另存为/编码重载路径）→ 删条目（DEC-003）
    if (PersistenceActive() && !g_restoring && !doc->modified()) {
        g_drafts->onModifiedCleared(doc);
    }
    // 外部文件改动检测：脏→干净翻转（保存/另存为/编码重载/外部重载路径）→
    // 刷新基线快照（防自触发，DEC-005 事件订阅分支）。filewatch 不属于持久化，
    // 不加 PersistenceActive 条件；恢复期跳过（首个 poll 兜底建快照）
    if (!g_restoring && !doc->modified() && !doc->url().empty()) {
        g_fileWatch.refreshBaseline(*doc);
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
    // url 空↔非空翻转（未命名→另存为/重命名取消回退）：右键菜单新项可用性同步
    if (idx >= 0) g_tabs.setHasPath(idx, !doc->url().empty());
    if (doc == g_docManager.activeDocument()) {
        RefreshWindowTitle();
        RefreshStatusBar();
    }
    RefreshMenuState();
    // 光标/显示名等元数据变化 → 会话记忆更新（500ms 防抖）
    TriggerSessionSave();
    // 外部文件改动检测：url 变化（另存为/重命名）→ 刷新基线快照（DEC-005 事件
    // 订阅分支 + poll 自动重建兜底）。metadata 事件不含 url 旧值，按 uid 记录
    // 上次刷新 url 比对——光标位置等高频 metadata 事件不触发（无条件刷新会在
    // 打字期间逐键重置快照吸收外部改动，DEC-010）。filewatch 独立于持久化，
    // 不加 PersistenceActive 条件；恢复期跳过
    if (!g_restoring && !doc->url().empty()) {
        std::filesystem::path& last = g_fileWatchLastUrl[doc->uid()];
        if (last != doc->url()) {
            last = doc->url();
            g_fileWatch.refreshBaseline(*doc);
        }
    }
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
// 编辑▸插入编号（列模式填充编号，任务C：editor 接线）
// ---------------------------------------------------------------------------

// 编辑▸插入编号…：模态对话框三参数 → 多光标批量填充。
// DEC-004：打开前**不**收缩多光标（与查找对话框 DEC-008 互斥语义相反——
// 编号必须保住选区集合；模态期间宿主 EnableWindow 禁用，选区安全）。
// 对话框参数的进程内记忆/预填在 ui 层完成（DEC-005），此处只消费确定结果。
void DoInsertNumbering() {
    HWND sci = g_editors.activeWindow();
    if (!sci) return;  // 无活动文档（欢迎页；菜单已置灰，防御）
    if (!editor::MultiCursorHost::active(sci)) return;  // 非多光标（理论不可达，防御）
    ui::NumberingParams params;
    // DEC-009：全局实例（原栈上局部）——模态期间 UiLocked() 生效，转发路径
    // 推迟消化，防止活动编辑器在模态中被切换/销毁导致确定路径使用过期 sci
    if (g_numberingDialog.showModal(g_mainWindow, params)) {
        g_multiCursor.insertNumbering(sci, params.start, params.step, params.padZero);
    }
    FocusActiveEditor();  // 模态归还后焦点回编辑器（与查找对话框一致）
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

// 过滤器：DocumentManager::nameFilters 5 项分组过滤器（所有文件 / 文本 /
// 结构化文档 / 配置 / 脚本）→ Win32 双 \0 分隔形态；通配 "*" 映射 "*.*"
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

// 默认过滤器索引：另存为/重命名默认选中"文本文件 (*.txt)"（与 lpstrDefExt 配套，
// 保持 txt 默认类型）；找不到时回退首项（Win32 nFilterIndex 为 1 基）
DWORD DefaultTextFilterIndex() {
    const auto& filters = g_docManager.nameFilters();
    for (std::size_t i = 0; i < filters.size(); ++i) {
        if (filters[i] == core::kTextFileFilter)
            return static_cast<DWORD>(i + 1);
    }
    return 1;
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
    ofn.nFilterIndex = DefaultTextFilterIndex();  // 默认选中"文本文件 (*.txt)"，与 lpstrDefExt 配套
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

// Tab 右键「文件重命名…」对话框：复用另存为形态（同过滤器/预填当前完整路径/
// g_fileDialogOpen 与 FocusActiveEditor 包夹），差别——
//   标题「重命名」；
//   不带 OFN_OVERWRITEPROMPT：目标已存在由核心层 rename 拒绝并给出明确错误，
//     避免「确认替换文件」的覆盖歧义（需求：不覆盖任何已存在文件）；
//   不带 OFN_NOREADONLYRETURN：重命名不写内容，只读文件也允许改名；
//   不设 lpstrDefExt：重命名语义下静默补 ".txt" 会意外改变文件类型，去掉更
//     符合「用户输入什么就是什么」的预期。
std::optional<std::filesystem::path> ShowRenameFileDialog(const core::Document* doc) {
    std::wstring filter = BuildFileDialogFilter();
    wchar_t buffer[4096] = {};
    const std::wstring initialPath =
        doc && !doc->url().empty() ? doc->url().wstring() : std::wstring();
    if (!initialPath.empty()) {
        wcsncpy_s(buffer, initialPath.c_str(), _TRUNCATE);
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_mainWindow;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = 4096;
    ofn.lpstrTitle = L"重命名";
    ofn.nFilterIndex = DefaultTextFilterIndex();  // 默认选中"文本文件 (*.txt)"，与另存为保持一致
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    g_fileDialogOpen = true;
    const BOOL ok = GetSaveFileNameW(&ofn);
    g_fileDialogOpen = false;
    FocusActiveEditor();
    if (!ok) {
        // 用户取消（CommDlgExtendedError()==0）维持静默；真失败不再吞掉（DEC-006）——
        // 预填路径含正斜杠等场景对话框直接失败，此前表现为「点击无反应」
        const DWORD err = CommDlgExtendedError();
        if (err != 0) {
            ShowErrorDialog(
                L"重命名",
                L"无法打开重命名对话框（错误码 " + std::to_wstring(err) + L"）\n" + initialPath);
        }
        return std::nullopt;
    }
    if (!buffer[0]) return std::nullopt;
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

// Tab 右键「文件重命名…」（改名/移动，A3 扩展）：核心层 rename 完成磁盘移动
// 与 url 更新（保留脏状态）；成功后经 documentMetadataChanged 既有管线自动
// 刷新 Tab 显示名/标题栏/状态栏/会话记忆，组装层无需手动刷新
void DoRenameDocument(core::Document* doc) {
    if (!doc || doc->url().empty()) return;  // 未命名不可达（菜单已置灰，防御）
    const auto picked = ShowRenameFileDialog(doc);
    if (!picked) return;  // 取消重命名：无任何变化
    const auto r = g_docManager.rename(doc, *picked);
    if (!r) {
        ShowErrorDialog(L"重命名失败", picked->wstring() + L"\n" + r.error().message);
    }
}

// Tab 右键「打开所在文件夹」：资源管理器打开并选中该文件（A3 扩展）；
// 文件不在磁盘上（被外部删除）弹错误框，不启动 explorer
void DoOpenContainingFolder(core::Document* doc) {
    if (!doc || doc->url().empty()) return;  // 未命名不可达（菜单已置灰，防御）
    std::error_code ec;
    if (!std::filesystem::exists(doc->url(), ec)) {
        ShowErrorDialog(L"打开所在文件夹", L"文件不在磁盘上：\n" + doc->url().wstring());
        return;
    }
    if (!platform::OpenSelectInExplorer(doc->url())) {
        ShowErrorDialog(L"打开所在文件夹", L"文件不在磁盘上：\n" + doc->url().wstring());
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
// 外部文件改动检测编排（需求-外部文件改动检测）：core::ExternalChangeMonitor
// 纯逻辑检测器 + 本层生产注入（stat/内容查询）、轮询定时器、立即触发点与
// 判定结果决策管线（干净自动重载 / 脏三选框 / 删除一次性提示）
// ---------------------------------------------------------------------------

/// stat 查询生产注入：std::filesystem 三分——not_found → 确认不存在（MSVC
/// 实测：不存在时 status 返回 file_type::not_found **且** ec 同步置
/// ERROR_FILE_NOT_FOUND/ERROR_PATH_NOT_FOUND——not_found 是「文件确认不在」
/// 的明确信号而非查询失败，三分判定必须先看 type 再看 ec，DEC-009）；其余
/// ec 错误（权限/网络抖动，type 多为 none）→ nullopt 本轮跳过不误报删除；
/// 存在但非常规文件（目录等）→ 按不存在处理（检测只关心常规文件）；常规
/// 文件 → mtime+size 采样。mtime 经 clock_cast 换算 epoch 毫秒（file_clock 的
/// Windows 纪元 1601 与 Unix 纪元由标准库换算，毫秒精度——stat 版本只做
/// 相对比较，绝对值精度无判定影响）
std::optional<core::FileStatSample> StatFileForWatch(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::file_status st = std::filesystem::status(path, ec);
    if (st.type() == std::filesystem::file_type::not_found) {
        return core::FileStatSample{false, 0, 0};  // 确认不存在（真实删除/移动）
    }
    if (ec) {
        return std::nullopt;  // 查询失败：本轮跳过，快照/判定状态不变
    }
    if (st.type() != std::filesystem::file_type::regular) {
        return core::FileStatSample{false, 0, 0};  // 目录等非常规文件：按不存在
    }
    std::error_code timeEc, sizeEc;
    const std::filesystem::file_time_type mtime = std::filesystem::last_write_time(path, timeEc);
    if (timeEc) return std::nullopt;
    const auto size = std::filesystem::file_size(path, sizeEc);
    if (sizeEc) return std::nullopt;
    const auto sysTime = std::chrono::clock_cast<std::chrono::system_clock>(mtime);
    const long long mtimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  sysTime.time_since_epoch())
                                  .count();
    return core::FileStatSample{true, mtimeMs, static_cast<long long>(size)};
}

/// 内容判定生产注入：storageForUrl 分发（file:// → LocalStorage）→ read 自动
/// 识别（BOM → 严格 UTF-8 校验 → GBK）得归一化内容（UTF-8/LF/无 BOM，与内存
/// 形态直接可比）→ SHA-256 与 baselineHash 比较。读失败按「内容不符」方向返回
/// false（误报走用户决策，比静默吸收外部修改安全——需求防覆盖目标）
bool DiskContentMatchesBaseline(const std::filesystem::path& path, const core::Document& doc) {
    if (core::DocumentStorage* storage = g_docManager.storageForUrl(path)) {
        if (auto loaded = storage->read(path, std::nullopt)) {
            return core::sha256Hex(loaded->content) == doc.baselineHash();
        }
    }
    return false;
}

/// 一轮检测编排（timer 7 tick / 切换标签 / 窗口激活共用入口）：只传活动文档
///（TC-106/108：非活动文档不做后台监测），按判定走管线——
///   Restored：静默（检测器已自清删除态，无需 UI 动作）；
///   Deleted：一次性提示（检测器删除态保证同一次删除只产出一条），文档保持
///            打开，保存时重建（LocalStorage 原子写路径）；
///   Modified：干净 → 静默自动重载（reloadFromDisk 自动识别编码，DEC-003）；
///             脏 → 三选框（重新加载=1 默认 / 保留修改=2=Esc / 另存为…=3）。
/// showModal 期间模态循环泵消息：timer 7 的 WM_TIMER 会重入到达，但 UiLocked
/// 守卫跳过（天然防重入，需求「模态期间跳过本轮检测」）
void PollActiveDocumentNow() {
    core::Document* doc = g_docManager.activeDocument();
    if (!doc || doc->url().empty()) return;
    for (const core::Finding& f : g_fileWatch.poll({doc})) {
        switch (f.verdict) {
        case core::Verdict::Restored:
            break;  // 静默：内容==基线（检测器已清除删除提示态）
        case core::Verdict::Deleted: {
            ShowErrorDialog(
                L"文件已被删除或移动",
                doc->url().wstring() + L"\n\n文档将保持打开，保存时会重新创建该文件。");
            break;
        }
        case core::Verdict::Modified: {
            if (!doc->modified()) {
                // 干净 → 静默自动重载（DEC-003 自动识别编码；事件管线刷新
                // Tab/标题/状态栏/编辑器/光标钳制——applyLoadedState 传播链）
                const auto r = g_docManager.reloadFromDisk(doc);
                if (r) {
                    g_fileWatch.refreshBaseline(*doc);  // 显式刷新（双保险之一）
                } else {
                    // 重载失败（写入中间态/被锁等低频边界）：静默降级（DEC-008）
                    // ——不弹框不打断；快照已推进到新 stat，同版本不重试，下次
                    // stat 变化重新走管线；OutputDebugStringW 留诊断线索
                    std::wstring diag = L"[qcnote] filewatch auto-reload failed: ";
                    diag += doc->url().wstring();
                    diag += L" ";
                    diag += r.error().message;
                    diag += L"\n";
                    OutputDebugStringW(diag.c_str());
                }
                break;
            }
            // 脏 → 三选框（重载将丢弃未保存修改；另存为先保住自己的修改）
            ui::DialogSpec spec;
            spec.title = L"文件已被外部修改";
            spec.message = doc->url().wstring() +
                           L" 已在磁盘上被其他程序修改。\n重新加载将丢弃未保存的修改。";
            spec.buttons.push_back({L"重新加载", 1, true});
            spec.buttons.push_back({L"保留修改", 2, false});
            spec.buttons.push_back({L"另存为…", 3, false});
            spec.escId = 2;  // Esc = 保留现状（等价「保留修改」）
            switch (g_dialogHost.showModal(g_mainWindow, spec)) {
            case 1: {  // 重新加载（丢弃未保存修改，走与干净自动重载同机制）
                const auto r = g_docManager.reloadFromDisk(doc);
                if (r) {
                    g_fileWatch.refreshBaseline(*doc);
                } else {
                    ShowErrorDialog(L"重新加载失败",
                                    doc->url().wstring() + L"\n" + r.error().message);
                }
                break;
            }
            case 2:  // 保留修改：同 stat 版本不再重复弹（DEC-004 抑制）
                g_fileWatch.acknowledge(*doc);
                break;
            case 3:  // 另存为…：先记录决策（新版本再变重新决策），再走既有另存为
                g_fileWatch.acknowledge(*doc);
                DoSaveAsDocument(doc);
                break;
            default:
                break;
            }
            break;
        }
        }
    }
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
    case ui::Command::InsertNumbering:
        DoInsertNumbering();
        break;
    case ui::Command::ToggleDarkTheme:
        // E5 双主题：token/控件/DWM/Scintilla 一次切换（SetDarkTheme 编排）
        SetDarkTheme(!ui::IsDarkTheme());
        break;
    case ui::Command::ToggleWordWrap:
        g_wordWrap = !g_wordWrap;
        g_editors.setWordWrap(g_wordWrap);  // 全局（旧版所有文档共享）
        g_toolbar.setWrapEnabled(g_wordWrap);  // F2：工具栏 toggle 激活态同步
        RefreshMenuState();
        break;
    case ui::Command::ToggleLineNumbers:
        // 显示行号：全局语义同 wordWrap；菜单唯一入口（无工具栏/快捷键）；
        // 不即时触发会话落盘——退出时 PersistSessionNow 统一写
        g_lineNumbers = !g_lineNumbers;
        g_editors.setLineNumbers(g_lineNumbers);  // 全局（所有文档共享）
        RefreshMenuState();
        break;
    case ui::Command::ToggleAutoIndent:
        // 自动缩进（回车复制上一行前导空白）：全局语义同 lineNumbers；
        // 菜单唯一入口（无工具栏/快捷键）；不即时触发会话落盘——退出时
        // PersistSessionNow 统一写
        g_autoIndent = !g_autoIndent;
        g_editors.setAutoIndent(g_autoIndent);  // 全局（所有文档共享）
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
            // 新命令（RenameFile/OpenContainingFolder）与未知命令：mock 数据上
            // 无文档系统语义，只记录 g_stCtxCmd 采样后直接返回（不执行锚点/
            // welcome 逻辑；正式路径见下方 switch）
            return;
        }
        // 仅三个批量关闭命令激活锚点（A3）；CloseCurrentTab 无锚点（邻居规则兜底）
        const bool batchClose = cmd == ui::Command::CloseTabsToRight ||
                                cmd == ui::Command::CloseTabsToLeft ||
                                cmd == ui::Command::CloseOtherTabs;
        if (batchClose && index >= 0 && index < g_tabs.count()) {
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
    case ui::Command::RenameFile:
        DoRenameDocument(anchor);
        return;
    case ui::Command::OpenContainingFolder:
        DoOpenContainingFolder(anchor);
        return;
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
        // 根菜单行序（列模式填充编号加入「编辑」组后）：文件/编辑/搜索/──/
        // 视图/──/退出——搜索=第 3 行，其上 4 行条目 + 2 分隔线后为退出
        const UINT dpi = ui::WindowDpi(g_mainWindow);
        const int ih = ui::Scale(ui::menu_metrics::ItemHeight, dpi);
        const int sh = ui::Scale(ui::menu_metrics::SeparatorHeight, dpi);
        const int ySearch = 1 + 2 * ih + ih / 2;
        const int yQuit = 1 + 4 * ih + 2 * sh + ih / 2;
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

// 解析 --selftest-menu / --selftest-tabs / --selftest-doc / --selftest-filewatch
// （[=<结果文件>]；互斥，首个命中生效）
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
        {L"--selftest-numbering", &g_selftestNumbering, &g_selftestNumberingOut},
        {L"--selftest-filewatch", &g_selftestFileWatch, &g_selftestFileWatchOut},
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
    }
    if (g_selftestFileWatch) {
        // 临时文件落点（场景文件写在结果文件旁；未携带路径时回退当前目录）
        std::error_code ec;
        if (!g_selftestFileWatchOut.empty()) {
            g_selftestBaseDir = std::filesystem::path(g_selftestFileWatchOut).parent_path();
        }
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
        // —— 场景 4：拖拽重排（TC-005..009；mock 直驱口径，DEC-003）——
        // 每个子场景自足重建规范 3 Tab 序（与场景 1 相同），互不残留
        const auto resetMockTabs = [] {
            g_tabs.setItems({{L"未命名-1", true},
                             {L"readme.md", false},
                             {L"很长的中文文件名测试用例.txt", false}});
            g_tabs.setActiveIndex(0);
        };
        // 拖拽阈值采样（与 TabStrip::onMouseMove 同口径：两轴取最大，0 退 4px）
        int dragThresh =
            std::max(GetSystemMetrics(SM_CXDRAG), GetSystemMetrics(SM_CYDRAG));
        if (dragThresh <= 0) dragThresh = 4;
        // onTabReordered 采样接管：mock 直驱下 moveDocument 无文档可动，本场景
        // 断言回调值即可；生产接线（WM_CREATE 挂接的 moveDocument 管线）场景
        // 结束原样恢复（DEC-003）
        const auto prodReorder = g_tabs.onTabReordered;
        g_tabs.onTabReordered = [](int from, int to) {
            g_stReorderedFrom = from;
            g_stReorderedTo = to;
            ++g_stReorderedCount;
        };
        const auto tabCenter = [](const RECT& b) {
            return POINT{(b.left + b.right) / 2, (b.top + b.bottom) / 2};
        };
        ui::TabStripGeometry geo = g_tabs.geometry();

        // —— TC-005：拖拽提交（越阈值拖起 → 压缩流中点跨线 → 松手一次上抛）。
        //    视觉偏移式（本次需求 DEC-001）：拖拽期间 items/active/focus 恒不动，
        //    mock 直驱下 up 后 items 保持原序是正确语义——真实换序由生产
        //    moveDocument 完成（见 WM_CREATE 接线）；跨线态经 DEC-004 测试缝
        //    dragInsertIndex() 断言 ——
        resetMockTabs();
        geo = g_tabs.geometry();
        const POINT p0 = tabCenter(geo.tabs[0].bounds);
        g_stReorderedFrom = g_stReorderedTo = -1;
        g_stReorderedCount = 0;
        g_tabs.onLButtonDown(p0);  // down 即激活 tab0（已是活动项：无激活回调）
        // 越阈值即拖起（数据零改动：items/active/focus 原样）
        g_tabs.onMouseMove({p0.x + dragThresh + 2, p0.y});
        bool ok = g_tabs.dragging() && g_tabs.activeIndex() == 0 &&
                  g_tabs.item(0).displayName == L"未命名-1";
        geo = g_tabs.geometry();  // 数据零改动 → bounds 未变
        const RECT& b1 = geo.tabs[1].bounds;
        const int dragW0 = geo.tabs[0].bounds.right - geo.tabs[0].bounds.left;
        const int gapPx = b1.left - geo.tabs[0].bounds.right;
        // tab1 压缩中点：抽掉 tab0 槽位后 tab1 的静息位置中点（DEC-002）
        const int compMid1 = (b1.left - (dragW0 + gapPx)) + (b1.right - b1.left) / 2;
        const POINT p1{compMid1 + 1, p0.y};  // 严格越过 tab1 压缩中点
        g_tabs.onMouseMove(p1);
        ok = ok && g_tabs.dragging() && g_tabs.dragInsertIndex() == 1 &&
             g_tabs.item(0).displayName == L"未命名-1" &&  // 数据零改动（视觉偏移式）
             g_tabs.item(1).displayName == L"readme.md" && !g_tabs.item(1).modified &&
             g_tabs.item(2).displayName == L"很长的中文文件名测试用例.txt" &&
             g_tabs.activeIndex() == 0 && g_tabs.focusIndex() == 0;  // 索引不漂移
        g_tabs.onLButtonUp(p1);
        g_stTabsDragCommit = ok && g_stReorderedCount == 1 && g_stReorderedFrom == 0 &&
                             g_stReorderedTo == 1 && !g_tabs.dragging() &&
                             g_tabs.dragInsertIndex() == -1 &&
                             g_tabs.item(0).displayName == L"未命名-1" &&
                             g_tabs.item(1).displayName == L"readme.md";

        // —— TC-006：纯点击（未超阈值）不重排，仅激活 ——
        resetMockTabs();
        geo = g_tabs.geometry();
        const POINT p2 = tabCenter(geo.tabs[2].bounds);
        g_stReorderedCount = 0;
        g_stActivatedCb = -1;
        g_tabs.onLButtonDown(p2);              // down 即激活 tab2（回调采样）
        g_tabs.onMouseMove({p2.x + 1, p2.y});  // 1px 微动：dist²=1 恒不越阈值
        g_tabs.onLButtonUp({p2.x + 1, p2.y});
        g_stTabsDragClick =
            !g_tabs.dragging() && g_stReorderedCount == 0 && g_stActivatedCb == 2 &&
            g_tabs.activeIndex() == 2 &&
            g_tabs.item(0).displayName == L"未命名-1" &&
            g_tabs.item(2).displayName == L"很长的中文文件名测试用例.txt";

        // —— TC-007：Esc 取消（视觉偏移式：数据从未变动——清状态 + 停表 + 重绘
        //    回原位即可，不回调）——
        resetMockTabs();
        geo = g_tabs.geometry();
        const POINT e0 = tabCenter(geo.tabs[0].bounds);
        g_stReorderedCount = 0;
        g_tabs.onLButtonDown(e0);
        g_tabs.onMouseMove({e0.x + dragThresh + 2, e0.y});
        geo = g_tabs.geometry();
        const RECT& eb1 = geo.tabs[1].bounds;
        const int eW = geo.tabs[0].bounds.right - geo.tabs[0].bounds.left;
        const int eGap = eb1.left - geo.tabs[0].bounds.right;
        const int eCompMid1 = (eb1.left - (eW + eGap)) + (eb1.right - eb1.left) / 2;
        g_tabs.onMouseMove({eCompMid1 + 1, e0.y});  // 跨线（insertIndex=1，items 恒原序）
        const bool escCrossed =
            g_tabs.dragging() && g_tabs.dragInsertIndex() == 1 &&
            g_tabs.item(0).displayName == L"未命名-1" && g_tabs.activeIndex() == 0;
        const bool escConsumed = g_tabs.onKeyDown(VK_ESCAPE, false);  // 拖拽中消费
        g_stTabsDragEsc = escCrossed && escConsumed && !g_tabs.dragging() &&
                          g_tabs.dragInsertIndex() == -1 && g_stReorderedCount == 0 &&
                          g_tabs.item(0).displayName == L"未命名-1" &&
                          g_tabs.item(1).displayName == L"readme.md" &&
                          g_tabs.activeIndex() == 0;

        // —— TC-008：捕获丢失取消（同 Esc 语义：数据恒不变，不回调）——
        resetMockTabs();
        geo = g_tabs.geometry();
        const POINT c0 = tabCenter(geo.tabs[0].bounds);
        g_stReorderedCount = 0;
        g_tabs.onLButtonDown(c0);
        g_tabs.onMouseMove({c0.x + dragThresh + 2, c0.y});
        geo = g_tabs.geometry();
        const RECT& cb1 = geo.tabs[1].bounds;
        const int cW = geo.tabs[0].bounds.right - geo.tabs[0].bounds.left;
        const int cGap = cb1.left - geo.tabs[0].bounds.right;
        const int cCompMid1 = (cb1.left - (cW + cGap)) + (cb1.right - cb1.left) / 2;
        g_tabs.onMouseMove({cCompMid1 + 1, c0.y});  // 跨线（insertIndex=1，items 恒原序）
        const bool capCrossed =
            g_tabs.dragging() && g_tabs.dragInsertIndex() == 1 &&
            g_tabs.item(0).displayName == L"未命名-1" && g_tabs.activeIndex() == 0;
        g_tabs.onCaptureChanged();
        g_stTabsDragCapture = capCrossed && !g_tabs.dragging() &&
                              g_tabs.dragInsertIndex() == -1 && g_stReorderedCount == 0 &&
                              g_tabs.item(0).displayName == L"未命名-1" &&
                              g_tabs.item(1).displayName == L"readme.md" &&
                              g_tabs.activeIndex() == 0;

        // —— TC-009：溢出边缘步进滚动（移动驱动；步进 + 钳制 [0, maxScroll]；
        //    拖拽/滚动全程 items 恒不变——视觉偏移式）——
        resetMockTabs();
        for (int i = 0; i < 18; ++i) {
            g_tabs.addTab({L"溢出测试文档-" + std::to_wstring(i) + L".txt", i % 3 == 0});
        }
        geo = g_tabs.geometry();
        const int step = ui::Scale(ui::tab_metrics::DragEdgeScrollStep, dpi);
        bool ok9 = g_tabs.count() == 21 && geo.overflow && geo.maxScroll > 0 &&
                   g_tabs.scrollOffset() == 0;
        const POINT r0 = tabCenter(geo.tabs[0].bounds);  // scroll=0：tab0 可见
        g_stReorderedCount = 0;
        g_tabs.onLButtonDown(r0);
        g_tabs.onMouseMove({r0.x + dragThresh + 2, r0.y});  // 拖起（不在边缘带内）
        ok9 = ok9 && g_tabs.dragging() && g_tabs.scrollOffset() == 0;
        const POINT rightEdge{geo.area.right - 1, r0.y};  // 条区右边缘带内
        g_tabs.onMouseMove(rightEdge);
        ok9 = ok9 && g_tabs.scrollOffset() ==
                         std::min(step, g_tabs.geometry().maxScroll);  // 步进一级
        for (int i = 0; i < 60; ++i) {
            g_tabs.onMouseMove(rightEdge);  // 反复步进 → 钳制在 maxScroll
        }
        ok9 = ok9 && g_tabs.scrollOffset() == g_tabs.geometry().maxScroll;
        const POINT leftEdge{geo.area.left + 1, r0.y};  // 条区左边缘带内
        for (int i = 0; i < 60; ++i) {
            g_tabs.onMouseMove(leftEdge);  // 反向步进 → 回滚到 0（钳制下界）
        }
        ok9 = ok9 && g_tabs.scrollOffset() == 0;
        g_stReorderedCount = 0;
        g_tabs.onLButtonUp(leftEdge);
        // 松手至多一次提交（up 用既成插入槽位判定；多次跨线也只上抛一次；
        // 回调语义已由 TC-005 精确断言），items 全程恒不变
        ok9 = ok9 && !g_tabs.dragging() && g_stReorderedCount <= 1 &&
              g_tabs.item(0).displayName == L"未命名-1" &&
              g_tabs.item(1).displayName == L"readme.md" &&
              g_tabs.item(20).displayName == L"溢出测试文档-17.txt" &&
              g_tabs.activeIndex() == 0 && g_tabs.focusIndex() == 0;
        g_stTabsDragEdge = ok9;

        // —— 恢复：生产接线 + 规范 3 Tab 序（active 对齐键盘导航场景后的进入态，
        // 后续右键菜单场景依赖该 mock 状态）——
        g_tabs.onTabReordered = prodReorder;
        resetMockTabs();
        g_tabs.setActiveIndex(2);
        PostMessageW(g_mainWindow, kMsgTabsSelftestStage, 0, 0);
    } else if (g_tabsStage == 4) {
        g_tabsStage = 5;
        // —— 场景 5：右键菜单弹出与命令回调 ——
        // 导航不变式：mock tab0 为未命名（hasPath=false），右键菜单尾部分隔线 +
        // 「文件重命名…/打开所在文件夹」均置灰，MenuHost 键盘导航跳过分隔线与
        // 置灰项——DOWN×2 仍选中第 1 项「关闭右边所有 (2)」，无需调整
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
    } else if (g_tabsStage == 5) {
        g_tabsStage = 6;
        // —— 场景 6：超量溢出 + 横向滚动 + 命中路由 ——
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
    } else if (g_tabsStage == 6) {
        g_tabsStage = 7;
        // —— 场景 7：清空 → 欢迎页 → 重新出现 Tab ——
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
    } else if (g_tabsStage == 7) {
        g_tabsStage = 8;
        const bool all = g_stTabsInit && g_stTabsClick && g_stTabsKeyboard &&
                         g_stTabsDragCommit && g_stTabsDragClick && g_stTabsDragEsc &&
                         g_stTabsDragCapture && g_stTabsDragEdge && g_stTabsContext &&
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
        f << "scenario=drag reorder commit (threshold, compressed-midpoint insert, "
             "zero data mutation, one-shot submit): "
          << (g_stTabsDragCommit ? "PASS" : "FAIL") << "\n";
        f << "scenario=drag below threshold is pure click without reorder: "
          << (g_stTabsDragClick ? "PASS" : "FAIL") << "\n";
        f << "scenario=drag cancel via escape clears drag state without callback (data "
             "never mutated): "
          << (g_stTabsDragEsc ? "PASS" : "FAIL") << "\n";
        f << "scenario=drag cancel via capture loss clears drag state without callback: "
          << (g_stTabsDragCapture ? "PASS" : "FAIL") << "\n";
        f << "scenario=drag edge auto scroll steps and clamps while overflow (items "
             "unchanged): "
          << (g_stTabsDragEdge ? "PASS" : "FAIL") << "\n";
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
bool g_stDocTitleShort = false;  // F1：标题同值短路（未变 0 次 / 变化恰 1 次）
bool g_stDocDragReorder = false;  // 拖拽重排后活动标签仍指向被拖文档（生产管线）
// —— 回车自动缩进（--selftest-doc 阶段 11；TC-002..010）——
bool g_stDocAiAlign = false;    // TC-002/003/006：行尾/缩进末尾回车对齐 + 光标位置 + 混合缩进原样复制
bool g_stDocAiFirst = false;    // TC-004：首行回车不缩进
bool g_stDocAiEmpty = false;    // TC-005：空行无空白不插入 / 纯空白行照常复制
bool g_stDocAiMulti = false;    // TC-007：多光标各自对齐 + 选区数不变
bool g_stDocAiToggle = false;   // TC-008：开关关闭行为不变（RouteCommand 分发验证）
bool g_stDocAiUndo = false;     // TC-010：单光标一次 Ctrl+Z 整体回退（coalesce）
bool g_stDocAiUndoReplace = false;  // DEC-005 固化：选区替换 Enter 一次 UNDO 整体回退（实测边界）
bool g_stDocAiUndoMulti = false;    // DEC-005 固化：多光标 Enter+缩进一次 UNDO 整体回退、选区数保持
bool g_stDocAiPaste = false;    // TC-009：粘贴含换行文本不追加缩进
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

// 测试内存剪贴板（经 MultiCursorHost 注入；系统剪贴板在本机被环境级
// ERROR_ACCESS_DENIED 拒绝——见 M3-B 报告「问题与决策」；注入只影响
// 测试进程内多光标控制器的剪贴板存取，分发/拼接判定逻辑与真实路径相同。
// doc selftest 阶段 11（回车自动缩进 TC-009 粘贴不触发）复用）
std::string g_testClipboard;

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
        {"drag reorder keeps active tab on dragged document (production pipeline)", &g_stDocDragReorder},
        {"close all documents shows welcome page and returns focus", &g_stDocCloseAll},
        {"title short-circuit: unchanged skips SetWindowTextW, change sets once", &g_stDocTitleShort},
        {"auto-indent: enter copies prev-line leading ws, caret at indent end (mixed tabs)", &g_stDocAiAlign},
        {"auto-indent: enter on first line inserts nothing", &g_stDocAiFirst},
        {"auto-indent: empty prev line no ws, whitespace-only prev line copies ws", &g_stDocAiEmpty},
        {"auto-indent: multi-caret each aligns own line, selection count kept", &g_stDocAiMulti},
        {"auto-indent: toggle off via command keeps plain newline behavior", &g_stDocAiToggle},
        {"auto-indent: single ctrl+z reverts newline+indent in one step", &g_stDocAiUndo},
        {"auto-indent: selection-replace enter fully reverts in one undo, selection restored (DEC-005)", &g_stDocAiUndoReplace},
        {"auto-indent: multi-caret enter+indent fully reverts in one undo, selection count kept (DEC-005)", &g_stDocAiUndoMulti},
        {"auto-indent: paste with newlines appends no indent", &g_stDocAiPaste},
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
        // —— F1 标题同值短路（计数采样）——
        {
            // 先撤销本阶段键入的 "\nx"（2 个撤销单元）回到磁盘加载态「中文a」：
            // 清脏翻转本身会设置一次标题（脏→干净的 "* " 摘除），随后从干净态
            // 开始计数，键入才能稳定产生「标题变化恰一次」的断言前提
            SendMessageW(sci, SCI_UNDO, 0, 0);
            SendMessageW(sci, SCI_UNDO, 0, 0);
            const int sets0 = g_stTitleSets;
            RefreshWindowTitle();  // 标题未变 → 短路（0 次设置）
            bool okTitle = g_stTitleSets == sets0;
            TypeIntoEditor(doc, L"t");  // 键入 → 干净→脏翻转 → 标题加 " *"（恰 1 次；
                                        // CHARADDED 光标回写的 metadata 链同值短路）
            okTitle = okTitle && g_stTitleSets == sets0 + 1;
            SendMessageW(sci, SCI_UNDO, 0, 0);  // 撤销 → 清脏 → 标题恢复（第 2 次）
            okTitle = okTitle && g_stTitleSets == sets0 + 2;
            RefreshWindowTitle();  // 恢复后未变 → 短路
            okTitle = okTitle && g_stTitleSets == sets0 + 2;
            g_docDetail += L"[stage7] titleSets0=" + std::to_wstring(sets0) +
                           L" titleSets1=" + std::to_wstring(g_stTitleSets) +
                           L" content=" + core::utf8ToWide(doc->content()) + L"\n";
            g_stDocTitleShort = okTitle;
        }
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
            RECT btns[4];  // F2：buttonRects 输出 4 按钮（写满 out[0..3]，须同容量）
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
        // —— 拖拽重排：活动标签跟随被拖文档（生产管线，TC-001/TC-002）——
        // 清场：直调 manager 关闭（干净文档无对话框），与「全部关闭」阶段同法
        for (core::Document* d : g_docManager.documents()) {
            g_docManager.closeDocument(d);
        }
        RouteCommand(ui::Command::NewDocument);
        RouteCommand(ui::Command::NewDocument);
        RouteCommand(ui::Command::NewDocument);
        const auto docs = g_docManager.documents();
        if (docs.size() != 3 || g_tabs.count() != 3) {
            // 前置失败：跳过驱动（防无条件索引访问 UB），干净落 FAIL 并推进
            g_docDetail += L"[stage10] precondition failed: docs=" +
                           std::to_wstring(docs.size()) + L" tabs=" +
                           std::to_wstring(g_tabs.count()) + L"\n";
            g_stDocDragReorder = false;
            PostMessageW(g_mainWindow, kMsgDocSelftestStage, 0, 0);
            return;
        }
        bool ok = g_docManager.activeDocument() == docs[2] && g_tabs.activeIndex() == 2;
        core::Document* A = docs[0];
        core::Document* B = docs[1];
        core::Document* C = docs[2];
        const std::wstring nameA = A->displayName();  // 未命名编号动态，只记不断言
        // 拖拽阈值采样（与 tabs selftest 同口径：两轴取最大，<=0 退 4px）
        int dragThresh =
            std::max(GetSystemMetrics(SM_CXDRAG), GetSystemMetrics(SM_CYDRAG));
        if (dragThresh <= 0) dragThresh = 4;
        // —— TC-001：右向拖 0→2 ——
        ui::TabStripGeometry geo = g_tabs.geometry();
        const POINT p0{(geo.tabs[0].bounds.left + geo.tabs[0].bounds.right) / 2,
                       (geo.tabs[0].bounds.top + geo.tabs[0].bounds.bottom) / 2};
        g_tabs.onLButtonDown(p0);  // 按下即激活 A
        ok = ok && g_docManager.activeDocument() == A && g_tabs.activeIndex() == 0;
        g_tabs.onMouseMove({p0.x + dragThresh + 2, p0.y});  // 越阈值拖起
        // 压缩流中点（与 tabs selftest TC-005 同法）：抽掉 tab0 槽位后 tab2
        // 的静息位置中点
        const RECT& b2 = geo.tabs[2].bounds;
        const int w0 = geo.tabs[0].bounds.right - geo.tabs[0].bounds.left;
        const int gap = geo.tabs[1].bounds.left - geo.tabs[0].bounds.right;
        const int shift = w0 + gap;
        const int compMid2 = (b2.left - shift) + (b2.right - b2.left) / 2;
        const int x2 = compMid2 + 1;  // 严格越过 tab2 压缩中点
        g_tabs.onMouseMove({x2, p0.y});
        ok = ok && g_tabs.dragging() && g_tabs.dragInsertIndex() == 2;
        g_tabs.onLButtonUp({x2, p0.y});
        {
            const auto after = g_docManager.documents();
            // 核心断言：顺序 {B,C,A}、活动指针不动、Tab 高亮跟随到 2
            ok = ok && !g_tabs.dragging() && after.size() == 3 &&
                 after[0] == B && after[1] == C && after[2] == A &&
                 g_docManager.activeDocument() == A && g_tabs.activeIndex() == 2 &&
                 g_tabs.item(2).displayName == nameA &&
                 g_tabs.item(0).displayName == B->displayName();
        }
        // —— TC-002：左向拖回 2→0 ——
        geo = g_tabs.geometry();
        const POINT p2t{(geo.tabs[2].bounds.left + geo.tabs[2].bounds.right) / 2,
                        (geo.tabs[2].bounds.top + geo.tabs[2].bounds.bottom) / 2};
        g_tabs.onLButtonDown(p2t);  // A 已活动
        g_tabs.onMouseMove({p2t.x - dragThresh - 2, p2t.y});
        const int compMid0 = geo.tabs[0].bounds.left +
                             (geo.tabs[0].bounds.right - geo.tabs[0].bounds.left) / 2;
        const int x0 = compMid0 - 1;  // 严格越过 tab0 压缩中点（左侧）
        g_tabs.onMouseMove({x0, p2t.y});
        ok = ok && g_tabs.dragInsertIndex() == 0;
        g_tabs.onLButtonUp({x0, p2t.y});
        {
            const auto after = g_docManager.documents();
            ok = ok && after.size() == 3 && after[0] == A && after[1] == B &&
                 after[2] == C && g_docManager.activeDocument() == A &&
                 g_tabs.activeIndex() == 0 &&
                 g_tabs.item(0).displayName == nameA;
        }
        g_stDocDragReorder = ok;
        g_docDetail += L"[stage10] drag reorder: order restored {A,B,C}, active ptr == A, activeIndex == 0, ok=" +
                       std::wstring(ok ? L"true" : L"false") + L"\n";
        PostMessageW(g_mainWindow, kMsgDocSelftestStage, 0, 0);
    } else if (g_docStage == 11) {
        g_docStage = 12;
        // —— 回车自动缩进（需求-回车自动缩进；经真实 WM_KEYDOWN VK_RETURN
        //    驱动——与 B4 列选 Enter 同一路径：子类化放行 → 原生 SCI_NEWLINE →
        //    SCN_CHARADDED 补缩进。活动文档 + setContent 设场景（程序化回灌
        //    编辑器，粘贴/装载路径不触发自动缩进），断言 doc->content() 与
        //    SCI_GETCURRENTPOS/选区）——
        core::Document* doc = g_docManager.activeDocument();
        HWND s = g_editors.activeWindow();
        if (!doc || !s) {
            // 前置失败：跳过驱动（防空指针），落 FAIL 并推进
            g_docDetail += L"[stage11] precondition failed: no active doc/editor\n";
            PostMessageW(g_mainWindow, kMsgDocSelftestStage, 0, 0);
            return;
        }
        const auto enter = [&] { SendMessageW(s, WM_KEYDOWN, VK_RETURN, 0); };
        const auto curPos = [&] {
            return static_cast<int>(SendMessageW(s, SCI_GETCURRENTPOS, 0, 0));
        };
        // —— TC-002：单光标行尾回车对齐（"  ab" → "  ab\n  "，光标 7）——
        doc->setContent("  ab");
        s = g_editors.activeWindow();
        SendMessageW(s, SCI_SETSEL, 4, 4);
        enter();
        bool alignOk = doc->content() == "  ab\n  " && curPos() == 7;
        // —— TC-003：缩进末尾断行（"\t\txy" 光标 x 前 → "\t\t\n\t\txy"，光标 5）——
        doc->setContent("\t\txy");
        s = g_editors.activeWindow();
        SendMessageW(s, SCI_SETSEL, 2, 2);
        enter();
        alignOk = alignOk && doc->content() == "\t\t\n\t\txy" && curPos() == 5;
        // —— TC-006：Tab/空格混合前导原样字节复制（不经 tabWidth 规范化）——
        doc->setContent(" \t ab");
        s = g_editors.activeWindow();
        SendMessageW(s, SCI_SETSEL, 5, 5);
        enter();
        alignOk = alignOk && doc->content() == " \t ab\n \t " && curPos() == 9;
        g_stDocAiAlign = alignOk;
        g_docDetail += L"[stage11-ai] align content=" + core::utf8ToWide(doc->content()) +
                       L" pos=" + std::to_wstring(curPos()) + L"\n";
        // —— TC-004：首行回车无上一行，不插入缩进（"ab\nc"，光标 3 = 新行行首）——
        doc->setContent("abc");
        s = g_editors.activeWindow();
        SendMessageW(s, SCI_SETSEL, 2, 2);
        enter();
        g_stDocAiFirst = doc->content() == "ab\nc" && curPos() == 3;
        g_docDetail += L"[stage11-first] content=" + core::utf8ToWide(doc->content()) +
                       L" pos=" + std::to_wstring(curPos()) + L"\n";
        // —— TC-005：上一行空行（无空白）不插入；纯空白行照常复制 ——
        doc->setContent("  a\n\n");
        s = g_editors.activeWindow();
        SendMessageW(s, SCI_SETSEL, 4, 4);  // L1 空行行首
        enter();
        bool emptyOk = doc->content() == "  a\n\n\n" && curPos() == 5;
        doc->setContent("  a\n    ");
        s = g_editors.activeWindow();
        SendMessageW(s, SCI_SETSEL, 8, 8);  // L1 纯空白行行尾
        enter();
        emptyOk = emptyOk && doc->content() == "  a\n    \n    " && curPos() == 13;
        g_stDocAiEmpty = emptyOk;
        g_docDetail += L"[stage11-empty] content=" + core::utf8ToWide(doc->content()) +
                       L" pos=" + std::to_wstring(curPos()) + L" ok=" +
                       std::wstring(emptyOk ? L"true" : L"false") + L"\n";
        // —— TC-007：多光标各自对齐 + 选区数保持（两光标 L0/L2 行尾）——
        doc->setContent("  a\nb\n  c");
        s = g_editors.activeWindow();
        SendMessageW(s, SCI_SETSEL, 3, 3);
        SendMessageW(s, SCI_ADDSELECTION, 9, 9);
        enter();
        const int selCount = static_cast<int>(SendMessageW(s, SCI_GETSELECTIONS, 0, 0));
        g_stDocAiMulti =
            selCount == 2 && doc->content() == "  a\n  \nb\n  c\n  " &&
            static_cast<int>(SendMessageW(s, SCI_GETSELECTIONNCARET, 0, 0)) == 6 &&
            static_cast<int>(SendMessageW(s, SCI_GETSELECTIONNCARET, 1, 0)) == 15;
        g_docDetail += L"[stage11-multi] sels=" + std::to_wstring(selCount) +
                       L" content=" + core::utf8ToWide(doc->content()) + L"\n";
        // —— TC-010：单光标一次 Ctrl+Z 整体回退（Scintilla undo coalesce：
        //    Enter 的 "\n" 与补缩进插入为紧邻同类型顶层插入自动合并）——
        doc->setContent("  ab");
        s = g_editors.activeWindow();
        SendMessageW(s, SCI_SETSEL, 4, 4);
        enter();
        const std::string afterEnter = doc->content();
        SendMessageW(s, SCI_UNDO, 0, 0);
        g_stDocAiUndo = afterEnter == "  ab\n  " && doc->content() == "  ab";
        g_docDetail += L"[stage11-undo] after=" + core::utf8ToWide(afterEnter) +
                       L" undid=" + core::utf8ToWide(doc->content()) + L"\n";
        // —— DEC-005 相关回归（应改-1）：选区替换 Enter 的 undo 边界固化 ——
        //    实测（Scintilla 5.6.9）：NewLine 的 NotifyChar 在 UndoGroup 析构前
        //    发出（Editor.cxx 通知段先于 ug 作用域结束）→ 补缩进插入落在 undo
        //    组内部，一次 UNDO 即整体回退（删选区+换行+补缩进），并恢复被替换
        //    文本与选区。断言按实测固化：1 步整体回退 + 1 步后 CANUNDO=0。
        doc->setContent("  ab");
        s = g_editors.activeWindow();
        SendMessageW(s, SCI_SETSEL, 2, 4);  // 选中 "ab"
        enter();
        const std::string replaceAfterEnter = doc->content();
        SendMessageW(s, SCI_UNDO, 0, 0);
        const std::string replaceUndo1 = doc->content();
        const int replaceCanUndo1 =
            static_cast<int>(SendMessageW(s, SCI_CANUNDO, 0, 0));
        const int replaceSelStart =
            static_cast<int>(SendMessageW(s, SCI_GETSELECTIONSTART, 0, 0));
        const int replaceSelEnd =
            static_cast<int>(SendMessageW(s, SCI_GETSELECTIONEND, 0, 0));
        SendMessageW(s, SCI_UNDO, 0, 0);  // 边界：已无可撤销步，须为无操作
        g_stDocAiUndoReplace =
            replaceAfterEnter == "  \n  " && replaceUndo1 == "  ab" &&
            replaceCanUndo1 == 0 && doc->content() == "  ab" &&
            replaceSelStart == 2 && replaceSelEnd == 4;
        g_docDetail += L"[stage11-ai-undo-replace] after=" + core::utf8ToWide(replaceAfterEnter) +
                       L" undo1=" + core::utf8ToWide(replaceUndo1) +
                       L" canUndo1=" + std::to_wstring(replaceCanUndo1) +
                       L" sel=" + std::to_wstring(replaceSelStart) + L"," +
                       std::to_wstring(replaceSelEnd) + L"\n";
        // —— DEC-005 相关回归（应改-2）：多光标 Enter+缩进的整体 UNDO 固化 ——
        //    同上：补缩进 BEGIN/ENDUNDOACTION 嵌套在 NewLine 的 UndoGroup 内，
        //    一次 UNDO 整体回退（多光标换行+全部补缩进），选区数保持 2，
        //    内容回到 Enter 前态。按实测固化：1 步整体回退 + 选区数保持。
        doc->setContent("  a\nb\n  c");
        s = g_editors.activeWindow();
        SendMessageW(s, SCI_SETSEL, 3, 3);
        SendMessageW(s, SCI_ADDSELECTION, 9, 9);
        enter();
        const std::string multiAfterEnter = doc->content();
        const int multiSelsAfter =
            static_cast<int>(SendMessageW(s, SCI_GETSELECTIONS, 0, 0));
        SendMessageW(s, SCI_UNDO, 0, 0);
        const std::string multiUndo1 = doc->content();
        const int multiSelsUndo1 =
            static_cast<int>(SendMessageW(s, SCI_GETSELECTIONS, 0, 0));
        const int multiCanUndo1 =
            static_cast<int>(SendMessageW(s, SCI_CANUNDO, 0, 0));
        SendMessageW(s, SCI_UNDO, 0, 0);  // 边界：已无可撤销步，须为无操作
        g_stDocAiUndoMulti =
            multiAfterEnter == "  a\n  \nb\n  c\n  " && multiSelsAfter == 2 &&
            multiUndo1 == "  a\nb\n  c" && multiSelsUndo1 == 2 &&
            multiCanUndo1 == 0 && doc->content() == "  a\nb\n  c" &&
            static_cast<int>(SendMessageW(s, SCI_GETSELECTIONS, 0, 0)) == 2;
        g_docDetail += L"[stage11-ai-undo-multi] after=" + core::utf8ToWide(multiAfterEnter) +
                       L" undo1=" + core::utf8ToWide(multiUndo1) +
                       L" sels=" + std::to_wstring(multiSelsAfter) + L"/" +
                       std::to_wstring(multiSelsUndo1) +
                       L" canUndo1=" + std::to_wstring(multiCanUndo1) + L"\n";
        // —— TC-009：粘贴含换行文本不追加缩进（B5 分发路径 + 内存剪贴板
        //    注入——机制前提：粘贴经 InsertString 直插，不经 SCN_CHARADDED）——
        doc->setContent("  a");
        s = g_editors.activeWindow();
        SendMessageW(s, SCI_SETSEL, 3, 3);
        g_testClipboard = "x\ny";
        g_multiCursor.setClipboardAccessForTest(
            [](HWND) { return g_testClipboard; },
            [](HWND, const std::string& v) { g_testClipboard = v; return true; });
        g_multiCursor.testPaste(s);
        g_multiCursor.setClipboardAccessForTest(nullptr, nullptr);
        g_stDocAiPaste = doc->content() == "  ax\ny";
        g_docDetail += L"[stage11-paste] content=" + core::utf8ToWide(doc->content()) + L"\n";
        // —— TC-008：开关关闭行为与改动前完全一致（经 RouteCommand 菜单
        //    命令路径分发验证；收场恢复开启态供后续）——
        RouteCommand(ui::Command::ToggleAutoIndent);
        bool toggleOk = !g_autoIndent && !g_editors.autoIndent() &&
                        g_menuState.autoIndent == false;
        doc->setContent("  ab");
        s = g_editors.activeWindow();
        SendMessageW(s, SCI_SETSEL, 4, 4);
        enter();
        toggleOk = toggleOk && doc->content() == "  ab\n" && curPos() == 5;
        RouteCommand(ui::Command::ToggleAutoIndent);  // 恢复开启（会话默认）
        toggleOk = toggleOk && g_autoIndent && g_editors.autoIndent();
        g_stDocAiToggle = toggleOk;
        g_docDetail += L"[stage11-toggle] off content=" + core::utf8ToWide(doc->content()) +
                       L" pos=" + std::to_wstring(curPos()) + L" ok=" +
                       std::wstring(toggleOk ? L"true" : L"false") + L"\n";
        PostMessageW(g_mainWindow, kMsgDocSelftestStage, 0, 0);
    } else if (g_docStage == 12) {
        g_docStage = 13;
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
// 外部文件改动检测自动化验证（--selftest-filewatch[=<结果文件>]，覆盖
// TC-101..108；--watch-interval-ms=50 注入短轮询间隔）
//
// 驱动方式（DEC-011）：生产 timer 7 每 tick 先跑 PollActiveDocumentNow（完整
// 生产管线），随后推进阶段等待计数——磁盘改动与断言之间保证确定轮数的生产
// poll 发生（posted 消息优先于 WM_TIMER 生成，故阶段消息在计数到 0 的 tick 内
// 投递，而非改动后立刻投递）。模态对话框经 g_dialogHost.onOpened 注入键盘
// （Enter=默认按钮=「重新加载」/「确定」，Esc=escId=「保留修改」）——真实
// 模态路径；「另存为…」（TC-104）经等价路径模拟（系统文件对话框不可自动化，
// 与 --selftest-doc 的 saveAs 阶段同约定：直调 acknowledge + g_docManager.saveAs
// + markSaved——与生产 choice-3 分支组合的同一组函数）。
// ---------------------------------------------------------------------------

// 断言采样集合（TC 编号对应测试用例文档）
bool g_stFwSetup = false;             // 前置：临时文件创建 + openPath 打开
bool g_stFwAutoReload = false;        // TC-101：干净文档外部修改静默自动重载
bool g_stFwOwnSave = false;           // TC-107：自己保存不触发提示
bool g_stFwDirtyReload = false;       // TC-102：脏文档弹三选框，选「重新加载」
bool g_stFwKeepModified = false;      // TC-103：选「保留修改」内容/脏保持
bool g_stFwKeepNoRepeat = false;      // TC-103：同 stat 版本多轮不重复弹
bool g_stFwKeepReappear = false;      // TC-103：文件再变三选框再现（DEC-004）
bool g_stFwSaveAs = false;            // TC-104：另存为等价路径（url 更新/干净/无提示残留）
bool g_stFwDeleteOnce = false;        // TC-105：删除提示恰一次 + 保存重建后不再提示
bool g_stFwInactiveDoc = false;       // TC-106：非活动文档不被检测
bool g_stFwSwitchImmediate = false;   // TC-108：切换标签立即检测（不等 tick）
bool g_stFwActivateImmediate = false; // TC-108：窗口激活立即检测（不等 tick）
int g_fwDialogCount = 0;   // 对话框出现次数（onOpened 采样；断言「无弹框」）
UINT g_fwAutoKey = VK_RETURN;  // 自动应答键（未期望对话框兜底关闭防悬挂）
int g_fileWatchWaitTicks = 0;   // 距下一阶段的生产 poll tick 数（tick 驱动推进）
std::wstring g_fwDetail;        // 各阶段采样明细（失败定位；写 <结果文件>.detail.txt）

// 自测文件写入（二进制 UTF-8 无 BOM、LF——与 LocalStorage 归一化内容直接可比）
bool FwWriteFile(const std::filesystem::path& path, const std::string& utf8) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    f.flush();
    return f.good();
}

// 场景内容（tag + 不同长度的填充保证各版本互异且字节数不同——stat 版本变化
// 不依赖 mtime 分辨率）
std::string FwContent(const char* tag, int pad) {
    std::string s = "外部改动检测 ";
    s += tag;
    s.push_back(' ');
    s.append(static_cast<size_t>(pad), '.');
    s.push_back('\n');
    return s;
}

// 自测辅助：光标先落文末再键入（WM_CHAR 在 Scintilla 当前光标处插入——加载
// 后光标停在 0/上次位置，场景语义 = 追加；SCI_SETSEL 同步设置，随后 CHARADDED
// 经既有链回写 doc 光标）
void FwTypeAtEnd(core::Document* doc, const std::wstring& text) {
    HWND sci = g_editors.windowForTest(doc);
    if (!sci) return;
    const LRESULT len = SendMessageW(sci, SCI_GETLENGTH, 0, 0);
    SendMessageW(sci, SCI_SETSEL, len, len);
    TypeIntoEditor(doc, text);
}

/// timer 7 tick 内生产 poll 之后调用（WM_TIMER 分支）：阶段等待计数推进，到 0
/// 投递下一阶段消息。模态对话框打开期间不消耗等待轮数（UiLocked 同门，DEC-011）
void FileWatchSelftestTick() {
    if (!g_selftestFileWatch || !g_mainWindow) return;
    if (g_fileWatchWaitTicks > 0 && --g_fileWatchWaitTicks == 0) {
        PostMessageW(g_mainWindow, kMsgFileWatchSelftestStage, 0, 0);
    }
}

void WriteFileWatchSelftestResult() {
    std::error_code ec;
    std::filesystem::path out(g_selftestFileWatchOut);
    if (g_selftestFileWatchOut.empty()) {
        out = std::filesystem::current_path(ec) / "qcnote-filewatch-selftest.txt";
    }
    const std::pair<const char*, bool*> named[] = {
        {"setup: temp file created and opened (snapshot auto-established)",
         &g_stFwSetup},
        {"TC-101 clean doc auto-reloads silently on external modify",
         &g_stFwAutoReload},
        {"TC-107 own save does not trigger any prompt",
         &g_stFwOwnSave},
        {"TC-102 dirty doc shows 3-choice dialog, reload discards edits",
         &g_stFwDirtyReload},
        {"TC-103 keep-modified preserves in-memory content and dirty flag",
         &g_stFwKeepModified},
        {"TC-103 same stat version does not re-prompt on later polls",
         &g_stFwKeepNoRepeat},
        {"TC-103 new disk version re-prompts (decision recorded per stat)",
         &g_stFwKeepReappear},
        {"TC-104 save-as equivalent path: url updated, clean, no prompt left",
         &g_stFwSaveAs},
        {"TC-105 deletion prompts once; save recreates file, no prompt after",
         &g_stFwDeleteOnce},
        {"TC-106 inactive document is not detected (content untouched)",
         &g_stFwInactiveDoc},
        {"TC-108 tab switch polls immediately (before next tick)",
         &g_stFwSwitchImmediate},
        {"TC-108 window activation polls immediately (before next tick)",
         &g_stFwActivateImmediate},
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
    detail << core::wideToUtf8(g_fwDetail);
    detail.close();
}

// 阶段机（kMsgFileWatchSelftestStage 逐段推进；每段末置 g_fileWatchWaitTicks，
// 由生产 poll tick 计数到 0 后投递下一段——磁盘改动与断言之间恒有确定轮数的
// 完整生产检测发生）
void RunFileWatchSelftestStage() {
    if (!g_mainWindow) return;
    std::error_code ec;

    if (g_fileWatchStage == 0) {
        g_fileWatchStage = 1;
        // —— 前置：创建 TC-101 临时文件并打开（openPath → 切换标签立即检测 →
        //     快照自动建立；TC-108 切换立即检测路径的顺带覆盖）——
        const auto fileA = g_selftestBaseDir / L"fw-tc101.txt";
        std::filesystem::remove(fileA, ec);
        g_stFwSetup = FwWriteFile(fileA, FwContent("A", 4));
        const auto opened = g_docManager.openPath(fileA);
        g_stFwSetup = g_stFwSetup && opened.has_value() &&
                      g_docManager.activeDocument() == *opened;
        // 对话框采样 + 自动应答挂全程（未期望对话框兜底关闭防悬挂：Enter=
        // 默认按钮，断言经 g_fwDialogCount 判定失败）
        g_fwAutoKey = VK_RETURN;
        g_fwDialogCount = 0;
        g_dialogHost.onOpened = [] {
            ++g_fwDialogCount;
            if (g_fwAutoKey != 0) {
                if (HWND h = g_dialogHost.dialogWindowForTest()) {
                    PostMessageW(h, WM_KEYDOWN, g_fwAutoKey, 0);
                }
            }
        };
        g_fwDetail += L"[stage0] setup=" + std::wstring(g_stFwSetup ? L"1" : L"0") +
                      L" docs=" + std::to_wstring(g_docManager.documentCount()) + L"\n";
        g_fileWatchWaitTicks = 2;  // 稳态余量（快照已由 openPath 立即检测建立）
    } else if (g_fileWatchStage == 1) {
        g_fileWatchStage = 2;
        // —— TC-101：外部改写为 B（不同字节数保 stat 版本变化）——
        core::Document* doc = g_docManager.activeDocument();
        bool ok = doc != nullptr && doc->url() == (g_selftestBaseDir / L"fw-tc101.txt");
        if (ok) {
            ok = doc->content() == FwContent("A", 4) && !doc->modified();
            FwWriteFile(doc->url(), FwContent("B", 40));
        }
        g_fwDetail += L"[stage1] tc101 overwrite ok=" + std::wstring(ok ? L"1" : L"0") +
                      L" dialogs=" + std::to_wstring(g_fwDialogCount) + L"\n";
        g_fileWatchWaitTicks = 4;  // 4 轮 poll：首轮检测 + 自动重载同步完成
    } else if (g_fileWatchStage == 2) {
        g_fileWatchStage = 3;
        // —— 断言 TC-101：静默自动重载（内容/编辑器/标题/状态栏），无对话框 ——
        core::Document* doc = g_docManager.activeDocument();
        wchar_t title[256] = {};
        GetWindowTextW(g_mainWindow, title, 256);
        bool ok = doc != nullptr && doc->content() == FwContent("B", 40) &&
                  !doc->modified() && EditorTextOf(doc) == FwContent("B", 40) &&
                  g_fwDialogCount == 0 && wcsstr(title, L"*") == nullptr;
        ok = ok && g_statusBar.data().hasDoc &&
             g_statusBar.data().totalChars ==
                 static_cast<long long>(core::utf8CharCount(FwContent("B", 40)));
        g_stFwAutoReload = ok;
        g_fwDetail += L"[stage2] tc101 autoReload=" + std::wstring(ok ? L"1" : L"0") +
                      L" dialogs=" + std::to_wstring(g_fwDialogCount) + L"\n";
        // —— TC-107 前置：编辑置脏 → 自己保存（真实 DoSave 路径）——
        if (doc) {
            FwTypeAtEnd(doc, L"用户编辑");
            DoSave();
        }
        g_fileWatchWaitTicks = 4;  // 保存后多轮 poll：基线已刷新（事件 + 显式双保险）
    } else if (g_fileWatchStage == 3) {
        g_fileWatchStage = 4;
        // —— 断言 TC-107：自己保存不触发提示；磁盘已写回内存内容 ——
        core::Document* doc = g_docManager.activeDocument();
        const std::string saved = FwContent("B", 40) + core::wideToUtf8(L"用户编辑");
        bool ok = doc != nullptr && !doc->modified() && g_fwDialogCount == 0 &&
                  doc->content() == saved && ReadFileBytes(doc->url()) == saved;
        g_stFwOwnSave = ok;
        g_fwDetail += L"[stage3] tc107 ownSave=" + std::wstring(ok ? L"1" : L"0") +
                      L" dialogs=" + std::to_wstring(g_fwDialogCount) + L"\n";
        // —— 清场 + TC-102 前置：打开新文件 C ——
        if (doc) g_docManager.closeDocument(doc);  // 干净文档直接关（无对话框）
        const auto fileC = g_selftestBaseDir / L"fw-tc102.txt";
        std::filesystem::remove(fileC, ec);
        FwWriteFile(fileC, FwContent("C", 6));
        const auto openedC = g_docManager.openPath(fileC);  // [[nodiscard]]：捕获结果
        if (!openedC.has_value() || *openedC == nullptr) {
            g_fwDetail += L"[stage3] openPath(fileC) failed\n";
        }
        g_fileWatchWaitTicks = 2;
    } else if (g_fileWatchStage == 4) {
        g_fileWatchStage = 5;
        // —— TC-102：编辑置脏 → 外部改写 D → 等三选框出现（Enter=重新加载）——
        core::Document* doc = g_docManager.activeDocument();
        bool ok = doc != nullptr && doc->url() == (g_selftestBaseDir / L"fw-tc102.txt") &&
                  doc->content() == FwContent("C", 6) && !doc->modified();
        if (ok) {
            FwTypeAtEnd(doc, L"用户编辑");
            FwWriteFile(doc->url(), FwContent("D", 44));
        }
        g_fwAutoKey = VK_RETURN;  // 三选框默认按钮 = 重新加载
        g_fwDetail += L"[stage4] tc102 setup ok=" + std::wstring(ok ? L"1" : L"0") +
                      L" dialogs=" + std::to_wstring(g_fwDialogCount) + L"\n";
        g_fileWatchWaitTicks = 4;  // 首轮 poll 弹框 + 模态内自动应答 + 重载同步完成
    } else if (g_fileWatchStage == 5) {
        g_fileWatchStage = 6;
        // —— 断言 TC-102：重载丢弃编辑、干净、编辑器同步、对话框恰一次 ——
        core::Document* doc = g_docManager.activeDocument();
        bool ok = doc != nullptr && doc->content() == FwContent("D", 44) &&
                  !doc->modified() && EditorTextOf(doc) == FwContent("D", 44) &&
                  g_fwDialogCount == 1;
        g_stFwDirtyReload = ok;
        g_fwDetail += L"[stage5] tc102 dirtyReload=" + std::wstring(ok ? L"1" : L"0") +
                      L" dialogs=" + std::to_wstring(g_fwDialogCount) + L"\n";
        // —— TC-103 前置：再置脏 → 外部改写 E → Esc=保留修改 ——
        if (ok) {
            FwTypeAtEnd(doc, L"再编辑");
            FwWriteFile(doc->url(), FwContent("E", 8));
        }
        g_fwAutoKey = VK_ESCAPE;  // escId=2 = 保留修改
        g_fileWatchWaitTicks = 4;
    } else if (g_fileWatchStage == 6) {
        g_fileWatchStage = 7;
        // —— 断言 TC-103（保留修改）：内存内容/脏保持；对话框第二次出现 ——
        core::Document* doc = g_docManager.activeDocument();
        const std::string kept = FwContent("D", 44) + core::wideToUtf8(L"再编辑");
        bool ok = doc != nullptr && doc->content() == kept && doc->modified() &&
                  g_fwDialogCount == 2;
        g_stFwKeepModified = ok;
        g_fwDetail += L"[stage6] tc103 keep=" + std::wstring(ok ? L"1" : L"0") +
                      L" dialogs=" + std::to_wstring(g_fwDialogCount) + L"\n";
        // —— 同 stat 版本多轮 poll：不重复弹（DEC-004 抑制）——
        g_fileWatchWaitTicks = 5;
    } else if (g_fileWatchStage == 7) {
        g_fileWatchStage = 8;
        // —— 断言 TC-103（不重复弹）——
        const bool ok = (g_fwDialogCount == 2);
        g_stFwKeepNoRepeat = ok;
        g_fwDetail += L"[stage7] tc103 noRepeat=" + std::wstring(ok ? L"1" : L"0") +
                      L" dialogs=" + std::to_wstring(g_fwDialogCount) + L"\n";
        // —— 文件再变 F（新 stat 版本）→ 三选框再现，Esc=再次保留 ——
        core::Document* doc = g_docManager.activeDocument();
        if (doc) {
            FwWriteFile(doc->url(), FwContent("F", 48));
        }
        g_fwAutoKey = VK_ESCAPE;
        g_fileWatchWaitTicks = 4;
    } else if (g_fileWatchStage == 8) {
        g_fileWatchStage = 9;
        // —— 断言 TC-103（再现）：文件再变重新进入决策（第三次对话框）——
        core::Document* doc = g_docManager.activeDocument();
        const std::string kept = FwContent("D", 44) + core::wideToUtf8(L"再编辑");
        const bool ok = doc != nullptr && g_fwDialogCount == 3 && doc->content() == kept &&
                        doc->modified();
        g_stFwKeepReappear = ok;
        g_fwDetail += L"[stage8] tc103 reappear=" + std::wstring(ok ? L"1" : L"0") +
                      L" dialogs=" + std::to_wstring(g_fwDialogCount) + L"\n";
        // —— TC-104（等价路径）：三选框「另存为…」= acknowledge + saveAs +
        //     markSaved（系统文件对话框不可自动化；生产 choice-3 分支组合的
        //     同一组函数，直接注入目标路径，DEC-011）——
        if (doc) {
            const auto fileAs = g_selftestBaseDir / L"fw-tc104-另存为.txt";
            std::filesystem::remove(fileAs, ec);
            g_fileWatch.acknowledge(*doc);
            const core::SaveAsResult sr = g_docManager.saveAs(doc, fileAs);
            if (sr.written) g_editors.markSaved(doc);
        }
        g_fileWatchWaitTicks = 4;  // 新 url 快照（metadata url 变化 + 干净翻转双保险）
    } else if (g_fileWatchStage == 9) {
        g_fileWatchStage = 10;
        // —— 断言 TC-104：url 更新、干净、磁盘内容即内存内容、无提示残留 ——
        core::Document* doc = g_docManager.activeDocument();
        const std::string saved = FwContent("D", 44) + core::wideToUtf8(L"再编辑");
        const auto fileAs = g_selftestBaseDir / L"fw-tc104-另存为.txt";
        bool ok = doc != nullptr && doc->url() == fileAs && !doc->modified() &&
                  doc->content() == saved && ReadFileBytes(fileAs) == saved &&
                  g_fwDialogCount == 3;
        g_stFwSaveAs = ok;
        g_fwDetail += L"[stage9] tc104 saveAs=" + std::wstring(ok ? L"1" : L"0") +
                      L" dialogs=" + std::to_wstring(g_fwDialogCount) + L"\n";
        // —— TC-105 前置：置脏 → 删除磁盘文件（删除提示=单按钮框，Enter=确定）——
        if (doc) {
            FwTypeAtEnd(doc, L"删除前编辑");
        }
        g_fwAutoKey = VK_RETURN;
        if (doc) {
            const bool removed = std::filesystem::remove(doc->url(), ec);
            g_fwDetail += L"[stage9] remove=" + std::wstring(removed ? L"1" : L"0") +
                          L" ec=" + std::to_wstring(ec.value()) +
                          L" url=" + doc->url().wstring() +
                          L" snapshots=" + std::to_wstring(static_cast<int>(g_fileWatch.snapshotCount())) +
                          L"\n";
        }
        g_fileWatchWaitTicks = 4;
    } else if (g_fileWatchStage == 10) {
        g_fileWatchStage = 11;
        // —— 断言 TC-105（提示恰一次）：文档仍打开、内容未变、对话框第四次 ——
        core::Document* doc = g_docManager.activeDocument();
        const std::string kept = FwContent("D", 44) + core::wideToUtf8(L"再编辑") +
                                 core::wideToUtf8(L"删除前编辑");
        bool ok = doc != nullptr && g_fwDialogCount == 4 && doc->content() == kept &&
                  doc->modified() && g_docManager.documentCount() == 1;
        g_fwDetail += L"[stage10] tc105 deleteNotice=" + std::wstring(ok ? L"1" : L"0") +
                      L" dialogs=" + std::to_wstring(g_fwDialogCount) + L"\n";
        g_fileWatchWaitTicks = 5;  // 多轮 poll：删除态不再重复提示
    } else if (g_fileWatchStage == 11) {
        g_fileWatchStage = 12;
        // —— 断言 TC-105（不重复提示）+ 保存重建（DoSave 真实路径）——
        core::Document* doc = g_docManager.activeDocument();
        bool ok = (g_fwDialogCount == 4);
        g_fwDetail += L"[stage11] tc105 noRepeat=" + std::wstring(ok ? L"1" : L"0") +
                      L" dialogs=" + std::to_wstring(g_fwDialogCount) + L"\n";
        if (doc) {
            DoSave();  // 文件重建（LocalStorage 原子写；modified 翻转刷新基线）
        }
        g_fileWatchWaitTicks = 4;  // 保存后多轮 poll：不再提示
    } else if (g_fileWatchStage == 12) {
        g_fileWatchStage = 13;
        // —— 断言 TC-105（保存重建后不再提示）——
        core::Document* doc = g_docManager.activeDocument();
        const std::string saved = FwContent("D", 44) + core::wideToUtf8(L"再编辑") +
                                  core::wideToUtf8(L"删除前编辑");
        bool ok = doc != nullptr && g_fwDialogCount == 4 && !doc->modified() &&
                  std::filesystem::exists(doc->url(), ec) &&
                  ReadFileBytes(doc->url()) == saved;
        g_stFwDeleteOnce = ok;
        g_fwDetail += L"[stage12] tc105 rebuild=" + std::wstring(ok ? L"1" : L"0") +
                      L" dialogs=" + std::to_wstring(g_fwDialogCount) + L"\n";
        // —— 清场 + TC-106 前置：两个文件 A1（活动）/B1 ——
        if (doc) g_docManager.closeDocument(doc);
        const auto fileA1 = g_selftestBaseDir / L"fw-tc106-a.txt";
        const auto fileB1 = g_selftestBaseDir / L"fw-tc106-b.txt";
        std::filesystem::remove(fileA1, ec);
        std::filesystem::remove(fileB1, ec);
        FwWriteFile(fileA1, FwContent("A1", 10));
        FwWriteFile(fileB1, FwContent("B1", 12));
        const auto openedA1 = g_docManager.openPath(fileA1);  // [[nodiscard]]：捕获结果
        if (!openedA1.has_value() || *openedA1 == nullptr) {
            g_fwDetail += L"[stage12] openPath(fileA1) failed\n";
        }
        core::Document* docB = g_docManager.openPath(fileB1).value_or(nullptr);
        if (docB) g_docManager.setActiveDocument(g_docManager.documentForPath(fileA1));
        // 非活动文档 B1 外部改写：多轮 poll 只查活动文档 A1（TC-106）
        if (docB) {
            FwWriteFile(docB->url(), FwContent("B2", 52));
        }
        g_fileWatchWaitTicks = 6;
    } else if (g_fileWatchStage == 13) {
        g_fileWatchStage = 14;
        // —— 断言 TC-106：B1 内容未被重载、无任何提示 ——
        const auto fileB1 = g_selftestBaseDir / L"fw-tc106-b.txt";
        core::Document* docB = g_docManager.documentForPath(fileB1);
        core::Document* docA = g_docManager.activeDocument();
        bool ok = docB != nullptr && docA != nullptr &&
                  docB->content() == FwContent("B1", 12) &&
                  g_docManager.activeDocument() == docA && g_fwDialogCount == 4;
        g_stFwInactiveDoc = ok;
        g_fwDetail += L"[stage13] tc106 inactive=" + std::wstring(ok ? L"1" : L"0") +
                      L" dialogs=" + std::to_wstring(g_fwDialogCount) + L"\n";
        // —— TC-108（切换标签立即检测）：激活 B1 → 同步立即检测 → 干净 →
        //     静默自动重载（断言在切换返回后立即采样，未等 tick）——
        if (docB) {
            g_docManager.setActiveDocument(docB);
            ok = docB->content() == FwContent("B2", 52) && !docB->modified() &&
                 EditorTextOf(docB) == FwContent("B2", 52) && g_fwDialogCount == 4;
        } else {
            ok = false;
        }
        g_stFwSwitchImmediate = ok;
        g_fwDetail += L"[stage13] tc108 switchImmediate=" + std::wstring(ok ? L"1" : L"0") +
                      L" content=" + core::utf8ToWide(docB ? docB->content() : std::string()) +
                      L"\n";
        // —— TC-108（窗口激活）前置：B1 再改一次（新 stat 版本）——
        if (docB) {
            FwWriteFile(docB->url(), FwContent("B3", 56));
        }
        // 模拟窗口失活→激活（SendMessage 直驱 = 真实 WM_ACTIVATE 处理路径；
        // 先发 WA_INACTIVE 置非活动态，保证「非激活→激活」翻转成立）
        SendMessageW(g_mainWindow, WM_ACTIVATE, MAKEWPARAM(WA_INACTIVE, 0), 0);
        SendMessageW(g_mainWindow, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0), 0);
        // —— 断言 TC-108（激活立即检测）：同步重载完成（未等 tick）——
        bool okA = docB != nullptr && docB->content() == FwContent("B3", 56) &&
                   !docB->modified() && g_fwDialogCount == 4;
        g_stFwActivateImmediate = okA;
        g_fwDetail += L"[stage13] tc108 activateImmediate=" + std::wstring(okA ? L"1" : L"0") +
                      L" dialogs=" + std::to_wstring(g_fwDialogCount) + L"\n";
        // —— 收尾：清场 + 写结果 + 退出 ——
        for (core::Document* d : g_docManager.documents()) {
            g_docManager.closeDocument(d);  // 均干净：直接关（无对话框）
        }
        g_dialogHost.onOpened = nullptr;
        WriteFileWatchSelftestResult();
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
// 列选鼠标按下/拖拽（修复 1+3：剥 MK_SHIFT 转发 + 矩形虚拟列；DEC-007 后
// Alt 一律物理键状态，注入形态与真实系统输入重合）
bool g_stSchNativeFlags = false, g_stSchMouseRect = false, g_stSchRectVirtual = false;
bool g_stSchKbColEmpty = false;
bool g_stSchMouseTap = false;   // C-3：按下即松开（无拖动）→ 非矩形、无残留
bool g_stSchX1NotAlt = false;   // A-2 锁定：X1 侧键（wParam 0x20）不误触发 Alt+click

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
        {"mouse-down forward flags strip MK_SHIFT (alt-only, pure fn)", &g_stSchNativeFlags},
        {"alt+shift mouse down starts empty rect at press point", &g_stSchMouseRect},
        {"rect drag keeps virtual col on empty line + typing pads", &g_stSchRectVirtual},
        {"keyboard column select keeps virtual col on empty line", &g_stSchKbColEmpty},
        {"mouse down+up without drag leaves single caret at press point (C-3)", &g_stSchMouseTap},
        {"X1 button (MK_XBUTTON1) click not misread as alt+click (DEC-007/A-2)", &g_stSchX1NotAlt},
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
        PostMessageW(g_mainWindow, kMsgSearchSelftestStage, 0, 0);
    } else if (g_searchStage == 11) {
        g_searchStage = 12;
        // —— 列选鼠标按下/拖拽集成（修复 1+3：按下转发剥 MK_SHIFT + 矩形虚拟列）——
        // 纯函数断言（转发 flags 调整；纯函数不读键状态，alt/ctrl 由调用方
        // 传入）。0x20 在真实鼠标消息 wParam 中是 MK_XBUTTON1 侧键（WinUser.h）
        // 而非 Alt——MK_ALT 属 OLE 拖放协议常量（oleidl.h），DEC-007
        constexpr UINT kMkXButton1 = 0x20;
        bool ok =
            editor::NativeMouseDownFlags(kMkXButton1 | MK_SHIFT, true, false) == kMkXButton1 &&
            editor::NativeMouseDownFlags(kMkXButton1 | MK_SHIFT | MK_CONTROL, true, true) ==
                (kMkXButton1 | MK_SHIFT | MK_CONTROL) &&
            editor::NativeMouseDownFlags(MK_SHIFT, false, false) == MK_SHIFT &&
            editor::NativeMouseDownFlags(kMkXButton1, true, false) == kMkXButton1;
        g_stSchNativeFlags = ok;

        core::Document* doc = g_docManager.activeDocument();
        doc->setContent("abcdef\n\nxy");  // L0 "abcdef" [0,6) L1 空行 [7,8) L2 "xy" [8,10)
        HWND s = g_editors.activeWindow();
        auto selCount = [&] {
            return static_cast<int>(SendMessageW(s, SCI_GETSELECTIONS, 0, 0));
        };
        // 虚拟空间配置（修复 3 的基础）
        ok = ok && static_cast<int>(SendMessageW(s, SCI_GETVIRTUALSPACEOPTIONS, 0, 0)) ==
                        SCVS_RECTANGULARSELECTION;

        // 旧非空选区 [0,3)（caret=3 ≠ 按下点）；按下点取 L0 列 4（旧选区外——
        // 点在已有非空选区内会走原生选区拖放分支，DEC-001 已知权衡路径，不测）
        SendMessageW(s, SCI_SETSEL, 0, 3);
        const int px = static_cast<int>(SendMessageW(s, SCI_POINTXFROMPOSITION, 0, 4));
        const int py = static_cast<int>(SendMessageW(s, SCI_POINTYFROMPOSITION, 0, 4));
        // 本层与 Scintilla 的 Alt 修饰键均从物理键盘状态读取（GetKeyState
        // (VK_MENU)，ScintillaWin.cxx:1142；DEC-007——合成消息 wParam 不携带
        // Alt，0x20 是 MK_XBUTTON1）——置本线程键状态表 Alt down（仅影响本
        // 线程 GetKeyState，不注入系统输入流；防扰约定），测后恢复
        BYTE keyState[256] = {}, savedState[256] = {};
        GetKeyboardState(savedState);
        for (int i = 0; i < 256; ++i) keyState[i] = savedState[i];
        keyState[VK_MENU] |= 0x80;
        SetKeyboardState(keyState);

        // C-3（DEC-007 伴随项）：按下即松开（无拖动）——真实形态 DOWN→UP
        // 立即成对：断言非矩形、无残留（单光标空选区定格于按下点 pos5，不是
        // 旧选区扩展、不是矩形）。按下点取 pos5：与拖拽按下点 pos4 相邻列但
        // 相距一个字宽（> Editor.cxx:4834 multiClick 判定的 3px 容差），合成
        // 注入的微秒级间隔不会被判成双击（真实用户两次按压亦远超该窗口）
        const int qx = static_cast<int>(SendMessageW(s, SCI_POINTXFROMPOSITION, 0, 5));
        const int qy = static_cast<int>(SendMessageW(s, SCI_POINTYFROMPOSITION, 0, 5));
        SendMessageW(s, WM_LBUTTONDOWN, MK_LBUTTON | MK_SHIFT, MAKELPARAM(qx, qy));
        SendMessageW(s, WM_LBUTTONUP, 0, MAKELPARAM(qx, qy));
        const bool tapOk =
            SendMessageW(s, SCI_SELECTIONISRECTANGLE, 0, 0) == 0 && selCount() == 1 &&
            static_cast<int>(SendMessageW(s, SCI_GETSELECTIONNCARET, 0, 0)) == 5 &&
            static_cast<int>(SendMessageW(s, SCI_GETSELECTIONNANCHOR, 0, 0)) == 5;
        g_stSchMouseTap = tapOk;
        g_searchDetail += L"[mousetap] rect=" +
                          std::to_wstring(static_cast<int>(SendMessageW(
                              s, SCI_SELECTIONISRECTANGLE, 0, 0))) +
                          L" sels=" + std::to_wstring(selCount()) +
                          L" caret=" +
                          std::to_wstring(static_cast<int>(
                              SendMessageW(s, SCI_GETSELECTIONNCARET, 0, 0))) + L"\n";

        // Alt+Shift 按下（子类化放行路径剥 MK_SHIFT 后转发）：空矩形于按下点，
        // 不拉出旧选区（修复前普通 Shift+click 扩展语义 → 矩形锚落在旧
        // anchor=0）。真实形态 wParam=MK_LBUTTON|MK_SHIFT（无 0x20——真实
        // 消息该位是 MK_XBUTTON1，DEC-007），Alt 经上方物理键状态注入；
        // 按下点 pos4 与 C-3 的 pos5 错开（规避 multiClick 判定，见上）
        SendMessageW(s, SCI_SETSEL, 0, 3);  // C-3 定格单光标后恢复旧选区前提
        SendMessageW(s, WM_LBUTTONDOWN, MK_LBUTTON | MK_SHIFT, MAKELPARAM(px, py));
        const bool downOk =
            SendMessageW(s, SCI_SELECTIONISRECTANGLE, 0, 0) != 0 &&
            selCount() == 1 &&
            static_cast<int>(SendMessageW(s, SCI_GETRECTANGULARSELECTIONANCHOR, 0, 0)) == 4 &&
            static_cast<int>(SendMessageW(s, SCI_GETRECTANGULARSELECTIONCARET, 0, 0)) == 4;
        g_stSchMouseRect = downOk;
        g_searchDetail += L"[mousedn] rect=" +
                          std::to_wstring(static_cast<int>(SendMessageW(s, SCI_SELECTIONISRECTANGLE, 0, 0))) +
                          L" sels=" + std::to_wstring(selCount()) +
                          L" anchor=" +
                          std::to_wstring(static_cast<int>(SendMessageW(s, SCI_GETRECTANGULARSELECTIONANCHOR, 0, 0))) +
                          L" caret=" +
                          std::to_wstring(static_cast<int>(SendMessageW(s, SCI_GETRECTANGULARSELECTIONCARET, 0, 0))) +
                          L"\n";

        // 拖到 L2：矩形逐行展开 L0..L2（一次 move 直达——后续 move 会被原生
        // autoScroll 节流吞掉，ButtonMoveWithModifiers 的 ticksToWait=200/tickSize=10）；
        // x 同为列 4：L1 空行 / L2 "xy"（2 字符）均超行宽 → 端点得虚拟列（修复 3）
        const int dx = px;
        const int fy = static_cast<int>(SendMessageW(s, SCI_POINTYFROMPOSITION, 0, 9));
        SendMessageW(s, WM_MOUSEMOVE, MK_LBUTTON | MK_SHIFT, MAKELPARAM(dx, fy));
        const int rectVs = static_cast<int>(
            SendMessageW(s, SCI_GETRECTANGULARSELECTIONCARETVIRTUALSPACE, 0, 0));
        const bool dragOk = selCount() == 3 && rectVs > 0;
        g_searchDetail += L"[rectdrag] sels=" + std::to_wstring(selCount()) +
                          L" rectVs=" + std::to_wstring(rectVs) + L"\n";

        // 抬起 → 先原生 ButtonUp（caret 角行虚拟列保留）→ 定格普通多选区
        SendMessageW(s, WM_LBUTTONUP, 0, MAKELPARAM(dx, fy));
        const bool upOk = selCount() == 3 &&
                          SendMessageW(s, SCI_SELECTIONISRECTANGLE, 0, 0) == 0;
        int emptyVs = 0, cornerVs = 0;  // 空行（pos7）与 caret 角行（pos10）
        for (int i = 0; i < selCount(); ++i) {
            const int caretPos = static_cast<int>(SendMessageW(
                s, SCI_GETSELECTIONNCARET, static_cast<WPARAM>(i), 0));
            const int vs = static_cast<int>(SendMessageW(
                s, SCI_GETSELECTIONNCARETVIRTUALSPACE, static_cast<WPARAM>(i), 0));
            if (caretPos == 7) emptyVs = vs;
            if (caretPos == 10) cornerVs = vs;
        }
        // 空行与 caret 角行（主选区）虚拟列均保留（DEC-005「松手后终点不回弹」；
        // cornerVs==rectVs：定格只换 selType 不动选区集合）
        g_stSchRectVirtual = ok && dragOk && upOk &&
                             emptyVs > 0 && cornerVs == rectVs;
        g_searchDetail += L"[rectup] sels=" + std::to_wstring(selCount()) +
                          L" emptyVs=" + std::to_wstring(emptyVs) +
                          L" cornerVs=" + std::to_wstring(cornerVs) +
                          L" rectVs=" + std::to_wstring(rectVs) + L"\n";

        // P1（TC-004）：定格后键入自动补列 + 字符（InsertCharacter 的
        // RealizeVirtualSpace；ADDITIONALSELECTIONTYPING 分发所有光标）——
        // L0 光标 pos4 → "abcdZef"；L1 空行光标在缩进位（vs=4）→ Scintilla 经
        // SetLineIndentation 按 useTabs（tabWidth=4，产品默认）补 "\t" 再插 'Z'；
        // L2 光标 pos10 非缩进位（vs=2）→ 补 2 空格再插 'Z'
        SetKeyboardState(savedState);  // 恢复线程键状态（Alt up）
        TypeIntoEditor(doc, L"Z");
        const bool typeOk = doc->content() == "abcdZef\n\tZ\nxy  Z";
        g_stSchRectVirtual = g_stSchRectVirtual && typeOk;
        g_searchDetail += L"[recttype] content=" + core::utf8ToWide(doc->content()) +
                          L" typeOk=" + std::to_wstring(typeOk ? 1 : 0) + L"\n";

        // TC-005：键盘列选（Ctrl+Alt+Shift+↓）在空行/短行上列终点不钳回第 1 列。
        // SCI_LINEDOWNRECTEXTEND 原生路径按 lastXChosen 粘性列定位
        // （Editor.cxx CursorUpOrDown 矩形分支 → PositionUpOrDown），
        // UserVirtualSpace 允许矩形端点落虚拟列。SCI_SETSEL 不刷新
        // lastXChosen，先用常规右移（SCI_CHARRIGHT 每步 SetLastXChosen）把
        // 粘性列同步到 col4——与真实用户移动光标/点选路径等价
        doc->setContent("abcdef\n\nxy");  // L0 [0,6) L1 空 [7,7) L2 "xy" [8,10)
        SendMessageW(s, SCI_SETSEL, 0, 0);
        for (int i = 0; i < 4; ++i) SendMessageW(s, SCI_CHARRIGHT, 0, 0);  // col4
        g_multiCursor.testKeyDown(s, VK_DOWN, true, true, true);  // 矩形扩到空行 L1
        int kbEmptyVs = 0;
        for (int i = 0; i < selCount(); ++i) {
            if (static_cast<int>(SendMessageW(s, SCI_GETSELECTIONNCARET,
                                              static_cast<WPARAM>(i), 0)) == 7) {
                kbEmptyVs = static_cast<int>(SendMessageW(
                    s, SCI_GETSELECTIONNCARETVIRTUALSPACE, static_cast<WPARAM>(i), 0));
            }
        }
        const bool kbDown1 = selCount() == 2 &&
                             SendMessageW(s, SCI_SELECTIONISRECTANGLE, 0, 0) != 0 &&
                             kbEmptyVs == 4;  // col4 粘性：空行 0 实字符 → vs=4
        auto dumpRectSels = [&]() {
            for (int i = 0; i < selCount(); ++i) {
                g_searchDetail += L"[kbsel" + std::to_wstring(i) + L"] caret=" +
                                  std::to_wstring(static_cast<int>(SendMessageW(
                                      s, SCI_GETSELECTIONNCARET, static_cast<WPARAM>(i), 0))) +
                                  L"vs" + std::to_wstring(static_cast<int>(SendMessageW(
                                      s, SCI_GETSELECTIONNCARETVIRTUALSPACE,
                                      static_cast<WPARAM>(i), 0))) +
                                  L" anchor=" +
                                  std::to_wstring(static_cast<int>(SendMessageW(
                                      s, SCI_GETSELECTIONNANCHOR, static_cast<WPARAM>(i), 0))) +
                                  L"vs" + std::to_wstring(static_cast<int>(SendMessageW(
                                      s, SCI_GETSELECTIONNANCHORVIRTUALSPACE,
                                      static_cast<WPARAM>(i), 0))) + L"\n";
            }
        };
        dumpRectSels();  // 第 1 次 DOWN 后现场（证据）
        g_multiCursor.testKeyDown(s, VK_DOWN, true, true, true);  // 扩到 L2 "xy"
        int kbShortVs = 0;
        for (int i = 0; i < selCount(); ++i) {
            if (static_cast<int>(SendMessageW(s, SCI_GETSELECTIONNCARET,
                                              static_cast<WPARAM>(i), 0)) == 10) {
                kbShortVs = static_cast<int>(SendMessageW(
                    s, SCI_GETSELECTIONNCARETVIRTUALSPACE, static_cast<WPARAM>(i), 0));
            }
        }
        // 短行 "xy"（2 实字符）：col4 = 2 实 + 2 虚 → vs=2；钳制（修复前
        // SCVS_NONE）时两处 vs 均为 0
        g_stSchKbColEmpty = kbDown1 && selCount() == 3 && kbShortVs == 2;
        dumpRectSels();  // 第 2 次 DOWN 后现场（证据）
        g_searchDetail += L"[kbempty] sels=" + std::to_wstring(selCount()) +
                          L" rect=" +
                          std::to_wstring(static_cast<int>(SendMessageW(
                              s, SCI_SELECTIONISRECTANGLE, 0, 0))) +
                          L" emptyVs=" + std::to_wstring(kbEmptyVs) +
                          L" shortVs=" + std::to_wstring(kbShortVs) + L"\n";

        // —— A-2 锁定（DEC-007）：X1 侧键不误触发 Alt+单击 ——真实 X1+左键
        // 单击 wParam 恒含 MK_XBUTTON1(0x20)（WinUser.h:2735），修复前被当
        // MK_ALT：纯 X1 点击误加光标、X1+Shift 误剥 MK_SHIFT。本线程物理 Alt
        // 已恢复 up（上方 savedState），不消费 → 原生普通/Shift+click 语义
        doc->setContent("abcdef\n\nxy");  // L0 [0,6) L1 空 [7,8) L2 "xy" [8,10)
        SendMessageW(s, SCI_SETSEL, 1, 1);  // 单光标 pos1（≠点击点）
        const int x1x = static_cast<int>(SendMessageW(s, SCI_POINTXFROMPOSITION, 0, 9));
        const int x1y = static_cast<int>(SendMessageW(s, SCI_POINTYFROMPOSITION, 0, 9));
        // 纯 X1+左键点击：不加光标（选区数不变），原生单击移光标到点击点 pos9
        SendMessageW(s, WM_LBUTTONDOWN, MK_LBUTTON | kMkXButton1, MAKELPARAM(x1x, x1y));
        SendMessageW(s, WM_LBUTTONUP, 0, MAKELPARAM(x1x, x1y));
        const bool x1PlainOk = selCount() == 1 &&
            static_cast<int>(SendMessageW(s, SCI_GETSELECTIONNCARET, 0, 0)) == 9;
        // X1+左键+Shift：Shift 不被误剥 → 原生 Shift+click 从 pos1 扩展到
        // 点击点 pos5。点击点与上次 X1 点击（pos9，L2 行）错开行（规避
        // Editor.cxx:4834 multiClick 同点判定——同点重按会触发双击词选分支）
        const int x5x = static_cast<int>(SendMessageW(s, SCI_POINTXFROMPOSITION, 0, 5));
        const int x5y = static_cast<int>(SendMessageW(s, SCI_POINTYFROMPOSITION, 0, 5));
        SendMessageW(s, SCI_SETSEL, 1, 1);
        SendMessageW(s, WM_LBUTTONDOWN, MK_LBUTTON | kMkXButton1 | MK_SHIFT,
                     MAKELPARAM(x5x, x5y));
        SendMessageW(s, WM_LBUTTONUP, 0, MAKELPARAM(x5x, x5y));
        g_stSchX1NotAlt = x1PlainOk && selCount() == 1 &&
            SendMessageW(s, SCI_SELECTIONISRECTANGLE, 0, 0) == 0 &&
            static_cast<int>(SendMessageW(s, SCI_GETSELECTIONSTART, 0, 0)) == 1 &&
            static_cast<int>(SendMessageW(s, SCI_GETSELECTIONEND, 0, 0)) == 5;
        g_searchDetail += L"[x1click] plain=" + std::to_wstring(x1PlainOk ? 1 : 0) +
                          L" sels=" + std::to_wstring(selCount()) +
                          L" start=" + std::to_wstring(static_cast<int>(
                              SendMessageW(s, SCI_GETSELECTIONSTART, 0, 0))) +
                          L" end=" + std::to_wstring(static_cast<int>(
                              SendMessageW(s, SCI_GETSELECTIONEND, 0, 0))) + L"\n";

        WriteSearchSelftestResult();
        PostMessageW(g_mainWindow, WM_CLOSE, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// 列模式填充编号自动化验证（--selftest-numbering[=<结果文件>]）
//
// 驱动方式（对齐查找 selftest 先例）：打开对话框前投递 kMsgNumberingSelftestStage，
// 模态循环 GetMessage 派发之（宿主 EnableWindow 禁用不影响消息投递）。对话框为
// DEC-009 全局实例 g_numberingDialog，经 dialogWindowForTest 定位。EDIT 用
// EM_SETSEL+WM_CHAR 录入、Enter/Tab/Space 用 WM_KEYDOWN
// （EDIT 子类化与对话框键盘流转的真实路径）。另直调 g_multiCursor.insertNumbering
// 覆盖非空选区替换与路由防御两层。
// ---------------------------------------------------------------------------

bool g_stNumMenuState = false, g_stNumFill = false, g_stNumUndoRedo = false;
bool g_stNumPadNeg = false, g_stNumReplace = false, g_stNumGuard = false;
bool g_stNumPrefill = false;  // 对话框预填（默认 1/1；DEC-005 会话内记忆一致性）
std::wstring g_numberingDetail;  // 采样明细（失败定位；写 <结果文件>.detail.txt）

// 测试内容：L0 alpha[0,5) \n5 L1 beta[6,10) \n10 L2 gamma[11,16)
const std::string kNumberingContent = "alpha\nbeta\ngamma";

void WriteNumberingSelftestResult() {
    std::error_code ec;
    std::filesystem::path out(g_selftestNumberingOut);
    if (g_selftestNumberingOut.empty()) {
        out = std::filesystem::current_path(ec) / "qcnote-numbering-selftest.txt";
    }
    const std::pair<const char*, bool*> named[] = {
        {"menu gating: welcome/single-caret greyed, multi-cursor enables item",
         &g_stNumMenuState},
        {"dialog prefills 1/1 defaults, focus on start edit", &g_stNumPrefill},
        {"dialog ok fills numbers top-down, carets at number ends (3 carets)",
         &g_stNumFill},
        {"single undo reverts whole batch, redo restores all numbers",
         &g_stNumUndoRedo},
        {"pad-zero auto width with negatives: -15/-05/05 (sign outside width)",
         &g_stNumPadNeg},
        {"non-empty selections replaced in place, outside text untouched",
         &g_stNumReplace},
        {"route guards: single-caret/no-document command is a no-op", &g_stNumGuard},
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
    detail << core::wideToUtf8(g_numberingDetail);
    detail.close();
}

// 3 行行首空光标（L0 行首 + Ctrl+Alt+↓×2 —— 真实多光标手势直调入口）
void NumberingSelftestLineStartCarets(HWND s) {
    SendMessageW(s, SCI_SETSEL, 0, 0);
    g_multiCursor.testKeyDown(s, VK_DOWN, true, false, true);
    g_multiCursor.testKeyDown(s, VK_DOWN, true, false, true);
}

// 各选区光标位置（CollectSelections 同序，升序）
std::vector<int> NumberingSelftestCarets(HWND s) {
    const int n = static_cast<int>(SendMessageW(s, SCI_GETSELECTIONS, 0, 0));
    std::vector<int> out;
    out.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        out.push_back(static_cast<int>(
            SendMessageW(s, SCI_GETSELECTIONNCARET, static_cast<WPARAM>(i), 0)));
    }
    return out;
}

// 定位打开中的编号对话框（DEC-009 起为全局实例，直接取测试句柄；
// 保留 owner/WS_POPUP 校验以确认确为打开中的编号对话框）
HWND NumberingSelftestDialog() {
    HWND dlg = g_numberingDialog.dialogWindowForTest();
    if (!dlg || !IsWindow(dlg)) return nullptr;
    if (::GetParent(dlg) != g_mainWindow) return nullptr;
    if (!(::GetWindowLongPtrW(dlg, GWL_STYLE) & WS_POPUP)) return nullptr;
    return dlg;
}

// 对话框的两个 EDIT 子控件（上=起始值、下=增量；按几何取，不依赖控件 id/Z 序；
// 类名用不区分大小写比较——注册名 "EDIT" 的规范名是 "Edit"）
void NumberingSelftestEdits(HWND dlg, HWND& startEdit, HWND& stepEdit) {
    startEdit = stepEdit = nullptr;
    RECT rStart{}, rStep{};
    for (HWND c = ::GetWindow(dlg, GW_CHILD); c; c = ::GetWindow(c, GW_HWNDNEXT)) {
        wchar_t cls[16] = {};
        if (!GetClassNameW(c, cls, 16) || _wcsicmp(cls, L"EDIT") != 0) continue;
        RECT rc{};
        GetWindowRect(c, &rc);
        if (!startEdit || rc.top < rStart.top) {
            stepEdit = startEdit;  // 更靠上的成为起始框，原候选让位增量框
            rStep = rStart;
            startEdit = c;
            rStart = rc;
        } else if (!stepEdit || rc.top < rStep.top) {
            stepEdit = c;
            rStep = rc;
        }
    }
}

void RunNumberingSelftestStage() {
    if (!g_mainWindow) return;

    if (g_numberingStage == 0) {
        g_numberingStage = 1;
        // —— 场景1 菜单状态（TC-011）：无文档/单光标置灰、多光标可用 ——
        // 无文档（自测模式不恢复会话，初始即欢迎页）
        RefreshMenuState();
        bool ok = !g_menuState.hasActiveDocument && !g_menuState.multiCursorActive &&
                  !ui::BuildMainMenu(g_menuState)
                       .isEnabled(ui::Command::InsertNumbering);
        // 单光标文档：仍置灰
        g_docManager.newDocument();
        core::Document* doc = g_docManager.activeDocument();
        doc->setContent(kNumberingContent);
        HWND s = g_editors.windowForTest(doc);
        RefreshMenuState();
        ok = ok && g_menuState.hasActiveDocument && !g_menuState.multiCursorActive &&
             !ui::BuildMainMenu(g_menuState).isEnabled(ui::Command::InsertNumbering);
        // 多光标（3 空光标，Ctrl+Alt+↓ 真实手势直调）：可用
        NumberingSelftestLineStartCarets(s);
        RefreshMenuState();
        ok = ok && editor::MultiCursorHost::active(s) && g_menuState.multiCursorActive &&
             ui::BuildMainMenu(g_menuState).isEnabled(ui::Command::InsertNumbering);
        g_stNumMenuState = ok;

        // —— 场景2 对话框填充（TC-001）——
        // 先投递阶段消息再进模态：模态循环 GetMessage 派发（宿主禁用不影响
        // 消息投递——查找 selftest 同款先例；对话框为栈上局部实例，无 onOpened
        // 可注入，阶段消息即注入通道）
        PostMessageW(g_mainWindow, kMsgNumberingSelftestStage, 0, 0);
        RouteCommand(ui::Command::InsertNumbering);  // 模态（阻塞至阶段 1 确定）
        const std::string kFilled = "1alpha\n2beta\n3gamma";
        g_stNumFill = doc->content() == kFilled && EditorTextOf(doc) == kFilled &&
                      NumberingSelftestCarets(s) == std::vector<int>{1, 8, 14};
        g_numberingDetail +=
            L"[fill] content=" + core::utf8ToWide(doc->content()) + L" carets=";
        for (int c : NumberingSelftestCarets(s)) {
            g_numberingDetail += std::to_wstring(c) + L",";
        }
        g_numberingDetail += L"\n";
        // —— TC-010 单批撤销/重做：一步整体回滚、一步恢复 ——
        SendMessageW(s, SCI_UNDO, 0, 0);
        bool undoOk = doc->content() == kNumberingContent;
        SendMessageW(s, SCI_REDO, 0, 0);
        undoOk = undoOk && doc->content() == kFilled;
        SendMessageW(s, SCI_UNDO, 0, 0);  // 回滚收场，供场景 2b 复用干净文本
        undoOk = undoOk && doc->content() == kNumberingContent;
        g_stNumUndoRedo = undoOk;
        PostMessageW(g_mainWindow, kMsgNumberingSelftestStage, 0, 0);
    } else if (g_numberingStage == 1) {
        g_numberingStage = 2;
        // （模态循环内）默认预填 1/1 → Enter 确定（对话框键盘流转真实路径：
        // 焦点在输入框上，Enter = 默认按钮「确定」→ accept 校验通过关闭）
        HWND dlg = NumberingSelftestDialog();
        if (!dlg) {
            PostMessageW(g_mainWindow, WM_CLOSE, 0, 0);  // 防御逃生（结果缺文件→FAIL）
            return;
        }
        HWND startEdit = nullptr, stepEdit = nullptr;
        NumberingSelftestEdits(dlg, startEdit, stepEdit);
        wchar_t sbuf[32] = {}, tbuf[32] = {};
        if (startEdit) GetWindowTextW(startEdit, sbuf, 32);
        if (stepEdit) GetWindowTextW(stepEdit, tbuf, 32);
        g_stNumPrefill = startEdit && stepEdit && wcscmp(sbuf, L"1") == 0 &&
                         wcscmp(tbuf, L"1") == 0 && GetFocus() == startEdit;
        g_numberingDetail += L"[dlg1] start=" + std::wstring(sbuf) + L" step=" +
                             std::wstring(tbuf) + L"\n";
        SendMessageW(dlg, WM_KEYDOWN, VK_RETURN, 0);  // Enter = 确定
    } else if (g_numberingStage == 2) {
        g_numberingStage = 3;
        // —— 场景2b 补零+负数（TC-005；含 TC-003 补零自动位数语义，等价小规模）——
        core::Document* doc = g_docManager.activeDocument();
        HWND s = g_editors.activeWindow();
        NumberingSelftestLineStartCarets(s);
        PostMessageW(g_mainWindow, kMsgNumberingSelftestStage, 0, 0);
        RouteCommand(ui::Command::InsertNumbering);  // 模态（阶段 3 驱动）
        const std::string kPadded = "-15alpha\n-05beta\n05gamma";
        g_stNumPadNeg = doc->content() == kPadded && EditorTextOf(doc) == kPadded &&
                        NumberingSelftestCarets(s) == std::vector<int>{3, 12, 19};
        g_numberingDetail +=
            L"[padneg] content=" + core::utf8ToWide(doc->content()) + L" carets=";
        for (int c : NumberingSelftestCarets(s)) {
            g_numberingDetail += std::to_wstring(c) + L",";
        }
        g_numberingDetail += L"\n";
        SendMessageW(s, SCI_UNDO, 0, 0);  // 回滚收场（单批撤销断言已在场景 2 覆盖）
        PostMessageW(g_mainWindow, kMsgNumberingSelftestStage, 0, 0);
    } else if (g_numberingStage == 3) {
        g_numberingStage = 4;
        // （模态循环内）预填=上次确定值（1/1；DEC-005 会话内记忆，与默认一致）
        // → 录入 -15/10 → Tab 流转到复选（EDIT 子类化转发）→ Space 勾补零
        // → Enter 确定
        HWND dlg = NumberingSelftestDialog();
        if (!dlg) {
            PostMessageW(g_mainWindow, WM_CLOSE, 0, 0);
            return;
        }
        HWND startEdit = nullptr, stepEdit = nullptr;
        NumberingSelftestEdits(dlg, startEdit, stepEdit);
        wchar_t sbuf[32] = {}, tbuf[32] = {};
        if (startEdit) GetWindowTextW(startEdit, sbuf, 32);
        if (stepEdit) GetWindowTextW(stepEdit, tbuf, 32);
        g_numberingDetail += L"[dlg2] start=" + std::wstring(sbuf) + L" step=" +
                             std::wstring(tbuf) + L"\n";
        if (!startEdit || !stepEdit) {
            PostMessageW(g_mainWindow, WM_CLOSE, 0, 0);
            return;
        }
        SearchSelftestTypePattern(startEdit, L"-15");  // EM_SETSEL 全选+逐字符 WM_CHAR
        SearchSelftestTypePattern(stepEdit, L"10");
        SendMessageW(startEdit, WM_KEYDOWN, VK_TAB, 0);  // → 增量框（子类化流转）
        SendMessageW(stepEdit, WM_KEYDOWN, VK_TAB, 0);   // → 复选（虚拟焦点）
        SendMessageW(dlg, WM_KEYDOWN, VK_SPACE, 0);      // Space 勾选补零
        SendMessageW(dlg, WM_KEYDOWN, VK_RETURN, 0);     // Enter = 确定
    } else if (g_numberingStage == 4) {
        g_numberingStage = 5;
        // —— 场景3 非空选区替换（TC-008；编辑器层直调）——
        core::Document* doc = g_docManager.activeDocument();
        doc->setContent("xx\nyy\nzz");  // L0 [0,2) L1 [3,5) L2 [6,8)
        HWND s = g_editors.activeWindow();
        SendMessageW(s, SCI_SETSEL, 0, 2);           // 选中 "xx"
        SendMessageW(s, SCI_ADDSELECTION, 3, 5);     // "yy"
        SendMessageW(s, SCI_ADDSELECTION, 6, 8);     // "zz"
        g_multiCursor.insertNumbering(s, 1, 1, false);
        g_stNumReplace = doc->content() == "1\n2\n3" && EditorTextOf(doc) == "1\n2\n3" &&
                         NumberingSelftestCarets(s) == std::vector<int>{1, 3, 5};
        g_numberingDetail +=
            L"[replace] content=" + core::utf8ToWide(doc->content()) + L"\n";
        // —— 路由防御：单光标 / 无文档 no-op（不弹对话框、同步返回）——
        g_multiCursor.testKeyDown(s, VK_ESCAPE, false, false, false);  // 收缩单光标
        RouteCommand(ui::Command::InsertNumbering);  // 非多光标：DoInsertNumbering 直接返回
        bool guard = doc->content() == "1\n2\n3";    // 文本不变（若误弹模态则超时暴露）
        g_docManager.closeDocument(doc);  // 直调 manager 关闭（doc 自测同款先例）
        RouteCommand(ui::Command::InsertNumbering);  // 无活动编辑器：直接返回
        guard = guard && g_docManager.documentCount() == 0 &&
                g_editors.activeWindow() == nullptr;
        g_stNumGuard = guard;
        WriteNumberingSelftestResult();
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
        g_toolbar.setWrapEnabled(g_wordWrap);  // F2：会话恢复后按钮激活态一致
        RefreshMenuState();
    }

    // —— [editor] lineNumbers（同款先于文档打开；无记录保持默认显示）——
    if (session.lineNumbers) {
        g_lineNumbers = *session.lineNumbers;
    }
    g_editors.setLineNumbers(g_lineNumbers);  // 此时无实例：仅落标志，后续 configure 继承

    // —— [editor] autoIndent（回车自动缩进；无记录保持默认开启）——
    if (session.autoIndent) {
        g_autoIndent = *session.autoIndent;
    }
    g_editors.setAutoIndent(g_autoIndent);  // 仅落标志：SCN_CHARADDED 处理时读取

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

// 窗口控制按钮按下开始（方案 A：标准 caption 按钮语义，松开才执行）：只记录
// 会话 + 按下视觉 + SetCapture，不执行动作——动作在捕获会话的 WM_LBUTTONUP
// 中收口。SetCapture 保证移动与最终松开全部投递本窗口（客户区消息形态），
// 物理松开事件必被本窗口消化，杜绝窗口销毁后 mouse-up 穿透到身后应用。
void BeginCaptionButtonPress(HWND hwnd, ui::CaptionButton button) {
    g_captionPress.begin(button);
    g_titleBar.setPressed(g_captionPress.visualButton());
    SetCapture(hwnd);
}

// 菜单按钮触发（维持 down 触发，与窗口控制按钮不同）：TrackPopupMenu 的模态
// 循环会消化物理松开事件，且按下不销毁窗口，无穿透风险；菜单自动化
//（verify-linenumbers 等 selftest 以 WM_NCLBUTTONDOWN(HTBORDER) 驱动）依赖此行为。
void TriggerMenuButton() {
    g_titleBar.setPressed(ui::CaptionButton::Menu);
    g_titleBar.performAction(ui::CaptionButton::Menu);
    // TrackPopupMenu 模态循环消化物理松开，不会走捕获会话的 WM_LBUTTONUP
    // 复位路径；菜单关闭后（含 ESC）在此显式清除 pressed 残留视觉。
    g_titleBar.setPressed(ui::CaptionButton::None);
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
        // Tab 拖拽重排提交（DEC-001：拖拽全程仅本地预览，松手一次提交）：上抛
        // → 数据层 moveDocument 重排（活动文档指针不动，仅 emit documentsChanged
        // 一次）；既有订阅自动完成 Tab 整表重建（OnDocumentsChanged →
        // RefreshAllAfterDocumentsChanged）+ 会话 500ms 防抖落盘
        //（TriggerSessionSave）——本接线无需新增管线代码。
        g_tabs.onTabReordered = [](int from, int to) {
            g_docManager.moveDocument(static_cast<std::size_t>(from),
                                      static_cast<std::size_t>(to));
        };
        g_welcome.attach(hwnd);
        g_welcome.setChromeInsets(ui::toolbar_metrics::Height, ui::statusbar_metrics::Height);
        g_welcome.onOpenClicked = [] { RouteCommand(ui::Command::OpenDialog); };
        // —— 工具栏 / 状态栏（E3）——
        g_toolbar.attach(hwnd);
        g_toolbar.onCommand = &RouteCommand;
        g_toolbar.setSaveEnabled(false);  // 初始无文档
        g_toolbar.setWrapEnabled(g_wordWrap);  // F2：启动态一致（会话恢复处会再同步）
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
        // 文档关闭（移除后、销毁前）：草稿「不保存」放弃清理 + 会话记忆 +
        // 外部改动检测快照清理（uid 遗留不泄漏，TC-011；与草稿清理同位置同
        // 前提——uid 仍可安全读；filewatch 独立于持久化，无条件执行）
        g_docManager.documentClosed.subscribe([](core::Document* doc) {
            if (PersistenceActive() && !g_restoring) g_drafts->onDocumentClosed(doc);
            g_fileWatch.forget(doc->uid());
            g_fileWatchLastUrl.erase(doc->uid());
            TriggerSessionSave();
        });
        // —— M4 防抖驱动定时器（200ms tick；会话 500ms / 草稿 1000ms 到期轮询）——
        SetTimer(hwnd, kTimerSessionFlush, 200, nullptr);
        // —— 外部文件改动检测轮询（timer 7；间隔 g_watchIntervalMs，默认 1000ms；
        //     仅正常模式与 filewatch 自测模式启动——其他自测模式不启动，防干扰
        //     既有自测语义；filewatch 模式需要本定时器驱动检测与阶段推进）——
        if (!SelftestModeActive() || g_selftestFileWatch) {
            SetTimer(hwnd, kTimerFileWatch, static_cast<UINT>(g_watchIntervalMs), nullptr);
        }
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
    case WM_NCPAINT:
        // 客户区 = 整窗（WM_NCCALCSIZE 返回 0），帧完全自绘/DWM 负责（E2）。
        // 跳过 DefWindowProc 的标准帧绘制：防止其按「系统想象的 NC 区域」重绘
        // 侵入客户区像素（状态栏条带贴窗口底缘，历史缺陷的兜底）
        return 0;
    case WM_NCACTIVATE:
        return DefWindowProcW(hwnd, msg, wParam, static_cast<LPARAM>(-1));
    case WM_ACTIVATE: {
        // 外部文件改动检测：非激活→激活翻转时立即检测一次（需求 3：不等下一
        // 轮 tick；恢复期/模态期间跳过——模态对话框打开中 WM_ACTIVATE 仍可能
        // 因 z 序切换到达）
        const bool wasActive = g_windowActive;
        g_windowActive = LOWORD(wParam) != WA_INACTIVE;
        if (g_windowActive && !wasActive && !g_restoring && !UiLocked()) {
            PollActiveDocumentNow();
        }
        // C3 失焦即写：草稿立即落盘（把强杀丢失窗口从「最后 1 秒防抖」压缩到
        // 「失焦前 1 秒」）；挂起中的会话一并冲刷
        if (LOWORD(wParam) == WA_INACTIVE && PersistenceActive() && !g_restoring) {
            g_drafts->flushNow();
            if (g_sessionDebounce.pending()) {
                g_sessionDebounce.cancel();
                PersistSessionNow();
            }
        }
        // 失活即清自绘组件悬停与 tooltip：模态对话框（打开/另存为/MessageBox
        // 等）期间 WM_MOUSEMOVE 不再到达主窗口，且 TrackMouseEvent 的
        // WM_MOUSELEAVE 为纯几何判定（对话框覆盖主窗口时光标仍在客户矩形
        // 内，不触发）——悬停高亮与 TOPMOST tooltip 会残留到对话框关闭后；
        // 切换其他程序同样适用
        if (LOWORD(wParam) == WA_INACTIVE) {
            if (!g_tabs.dragging()) g_tabs.onMouseLeave();  // 拖拽防御（同 WM_NCMOUSEMOVE 口径）
            g_toolbar.onMouseLeave();
            g_statusBar.onMouseLeave();
            g_welcome.onMouseLeave();
        }
        InvalidateRect(hwnd, nullptr, FALSE);  // E2 边界线换色
        break;
    }
    case WM_NCLBUTTONDOWN:
        switch (static_cast<int>(wParam)) {
        case HTCAPTION:
            if (IsZoomed(hwnd)) {
                HandleMaximizedCaptionPress(hwnd, lParam);
                return 0;
            }
            break;
        case HTCLOSE:
            // 方案 A：按下只记录会话 + 捕获，不执行（松开才执行，防穿透）
            BeginCaptionButtonPress(hwnd, ui::CaptionButton::Close);
            return 0;
        case HTMINBUTTON:
            BeginCaptionButtonPress(hwnd, ui::CaptionButton::Minimize);
            return 0;
        case HTMAXBUTTON:
            BeginCaptionButtonPress(hwnd, ui::CaptionButton::MaximizeRestore);
            return 0;
        case HTBORDER:
            TriggerMenuButton();
            return 0;
        default:
            break;
        }
        break;
    case WM_NCLBUTTONUP:
        if (g_captionPress.active()) {
            // 防御路径：SetCapture 失败等异常情形下松开以 NC 形态到达——按
            // 屏幕坐标换算客户区后与捕获路径同款收口（先判定后释放捕获，
            // 见 WM_LBUTTONUP 会话分支注释）
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);
            const ui::CaptionButton fire = g_captionPress.complete(g_titleBar.hitButton(pt));
            if (GetCapture() == hwnd) ReleaseCapture();
            g_titleBar.endCaptureSession();
            if (fire != ui::CaptionButton::None) {
                g_titleBar.performAction(fire);
            }
            return 0;
        }
        g_titleBar.onLButtonUp();
        break;
    case WM_NCMOUSEMOVE:
        g_titleBar.onNcMouseMove({GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
        if (!g_tabs.dragging()) {
            // 拖拽中防御性跳过（捕获期间不会收到 NC 移动；即便到达也不清状态）
            g_tabs.onMouseLeave();
        }
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
    case kMsgNumberingSelftestStage:
        RunNumberingSelftestStage();
        return 0;
    case kMsgFileWatchSelftestStage:
        // 外部文件改动检测自测阶段推进（--selftest-filewatch；阶段等待由
        // timer 7 tick 计数驱动，见 RunFileWatchSelftestStage 段注释）
        RunFileWatchSelftestStage();
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
        if (g_captionPress.active()) {
            // 按下会话捕获期间：移动以客户区消息形态到达（坐标可为负/窗外），
            // 重新命中测试维持/撤销按下视觉（拖离取消、拖回恢复），跳过
            // tabs/toolbar 等常规鼠标路由
            g_captionPress.update(g_titleBar.hitButton(pt));
            g_titleBar.setPressed(g_captionPress.visualButton());
            return 0;
        }
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
        if (g_tabs.dragging()) {
            // 拖拽路由优先：捕获持续送坐标（光标可到条外/窗外），直接驱动拖拽，
            // 跳过下方 onMouseLeave 分支
            g_tabs.onMouseMove(pt);
        } else if (pt.y < capH) {
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
    case WM_CAPTURECHANGED:
        // 主窗口鼠标捕获被夺走/释放（lParam = 获得捕获的新窗口句柄，无需处理）
        // → 取消标题栏窗口控制按钮按下会话（弹窗/系统夺走捕获：清按下状态、
        // 不执行动作；本窗口自己 ReleaseCapture 收口时会话已先结束，此处 no-op）
        // 与 Tab 条拖拽（DEC-002：清状态回原位不回调；非拖拽态 no-op）
        if (g_captionPress.active()) {
            g_captionPress.cancel();
            g_titleBar.endCaptureSession();
        }
        g_tabs.onCaptureChanged();
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
    case WM_LBUTTONUP: {
        const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (g_captionPress.active()) {
            // 按下会话的松开（捕获把物理松开重定向到本窗口，客户区消息形态）：
            // 先判定后释放捕获——ReleaseCapture 会同步派发 WM_CAPTURECHANGED，
            // 顺序颠倒会话会被其取消分支清掉导致动作永不执行；仍在原按钮上
            // 才执行动作（拖离取消语义），随后复位 NC hover 跟踪
            const ui::CaptionButton fire = g_captionPress.complete(g_titleBar.hitButton(pt));
            if (GetCapture() == hwnd) ReleaseCapture();
            g_titleBar.endCaptureSession();
            if (fire != ui::CaptionButton::None) {
                g_titleBar.performAction(fire);
            }
            return 0;
        }
        const UINT dpi = ui::WindowDpi(hwnd);
        const int capH = ui::Scale(ui::metrics::CaptionHeight, dpi);
        // 拖拽中（捕获把松手重定向到主窗口，可发生在条外/编辑区上方）或标题栏
        // Tab 区内的纯点击松手（判定与 WM_LBUTTONDOWN 同款）→ 收口 TabStrip
        //（提交拖拽 / 清按下记录）；非拖拽且标题栏外不拦截——编辑区
        //（Scintilla 子窗口）等维持既有 DefWindowProc 原路径。
        if (g_tabs.dragging() || pt.y < capH) {
            g_tabs.onLButtonUp(pt);
            return 0;
        }
        break;
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
        // 外部文件改动检测轮询（timer 7）：恢复期/模态期间跳过检测（模态循环
        // 泵消息时 WM_TIMER 照常到达，UiLocked 防重入；showModal 内部的三选框
        // 重入即此场景）；filewatch 自测等待计数随生产 poll 同门推进——对话框
        // 打开期间不消耗等待轮数
        if (wParam == kTimerFileWatch && !g_restoring && !UiLocked()) {
            PollActiveDocumentNow();
            if (g_selftestFileWatch) FileWatchSelftestTick();
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
        // 行号边宽度按新字体重测（PostMessage 延迟一拍：保证子窗口字体已就绪）
        PostMessageW(hwnd, kMsgRefreshLineNumbers, 0, 0);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case kMsgRefreshLineNumbers:
        g_editors.refreshLineNumberWidths();
        return 0;
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
                } else if (arg.rfind(L"--watch-interval-ms=", 0) == 0) {
                    // 外部文件改动检测轮询间隔（测试钩子；默认 1000ms）：正值
                    // 直接采用（filewatch 自测需要短值如 50 加速推进），≤0/
                    // 非法/超界回退默认（解析失败 _wtoi 返回 0 → 回退）
                    constexpr wchar_t kWatchPrefix[] = L"--watch-interval-ms=";
                    const int parsed = _wtoi(arg.c_str() + wcslen(kWatchPrefix));
                    if (parsed >= 1 && parsed <= 60000) {
                        g_watchIntervalMs = parsed;
                    }
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
    // 事件接线已在 WM_CREATE 完成）。filewatch 自测自建场景文件，不消费位置参数
    if (!g_initialPaths.empty() && !g_selftestMenu && !g_selftestTabs &&
        !g_selftestSearch && !g_selftestTheme && !g_selftestNumbering &&
        !g_selftestFileWatch) {
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
    } else if (g_selftestNumbering) {
        PostMessageW(hwnd, kMsgNumberingSelftestStage, 0, 0);
    } else if (g_selftestFileWatch) {
        PostMessageW(hwnd, kMsgFileWatchSelftestStage, 0, 0);
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
        // 拖拽取消失效修复（DEC-004 方案①）：焦点在 Scintilla 编辑器子窗口时，
        // 裸 Esc 经消息循环直达子窗口，主窗口 WM_KEYDOWN 收不到，拖拽无法取消；
        // 故在消息循环层预拦截（与上方快捷键拦截同理，焦点在编辑器时同样生效）。
        // 仅 g_tabs.dragging() 态独占：非拖拽态裸 Esc 语义完全不变（编辑器内
        // Esc/取消等既有行为不受影响）。selftest-tabs 为同步直驱，消息循环泵
        // 消息时不可能处于拖拽态，故无需 selftest 守卫。
        if (msg.message == WM_KEYDOWN && g_tabs.dragging() &&
            static_cast<UINT>(msg.wParam) == VK_ESCAPE) {
            g_tabs.onKeyDown(VK_ESCAPE, false);  // 拖拽态下取消拖拽
            continue;  // 已消费：不产生 WM_CHAR、不分发到任何子窗口
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
