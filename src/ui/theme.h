// qcnote UI 主题 token（M1）：语义命名与取值对齐旧 Qt 版 src/qml/AppTheme.qml
// （git 历史可查；浅色值照抄，深色结构预留 M6 填值接入）。
//
// 颜色统一用 0xAARRGGBB 存储：GDI 的 COLORREF 不含 alpha，hover/pressed 等
// 半透明叠加 token 在绘制处经 BlendOver() 与已知底色预合成为不透明色再落笔，
// 既保住 token 的「叠加」语义（换底色自动得到新合成色），又不需要引入
// GDI+/AlphaBlend 的合成路径（单 exe 小体积优先，M1 绘制全是纯色块+线段）。
#pragma once

#include <windows.h>

#include <cstdint>

// 前向声明（core/syntax.h 的语义色 token 枚举；底层类型 int 与定义一致）：
// ui/theme.h 是全 ui 层公共头（titlebar/tabs/toolbar/… 全量包含），不在此引入
// core 头依赖链（core/document.h → <filesystem> 等）；token→色板映射的 switch
// 实现于 theme.cpp（该 TU include core/syntax.h 取完整枚举定义）
namespace core {
enum class SyntaxToken : int;
}

namespace ui {

// —— 语义色板（字段与 AppTheme.qml token 一一对应，便于对照维护）——
struct ThemePalette {
    // 表面
    uint32_t titleBarBackground;   // 标题栏/标签条背景：深于工具栏一档（活动标签「浮起」可辨）
    uint32_t toolbarBackground;    // 工具栏 = 活动标签底色（连通式约束）
    uint32_t contentBackground;    // 内容区
    uint32_t statusBarBackground;  // 状态栏（不随标题栏加深联动，DEC-102）
    uint32_t activeTab;            // 活动标签（恒等于 toolbarBackground）
    // 文本
    uint32_t textPrimary;
    uint32_t textSecondary;
    uint32_t textDisabled;
    // 语义
    uint32_t accent;               // 强调色/焦点环/额外光标
    uint32_t focus;                // 恒等于 accent
    uint32_t error;
    uint32_t hover;                // 悬停半透明叠加（叠于表面色之上）
    uint32_t pressed;              // 按下半透明叠加
    uint32_t closeHover;           // 关闭按钮悬停（不透明红）
    uint32_t closePressed;         // 关闭按钮按下（不透明红）
    uint32_t closeGlyphForeground; // 红底上的前景字形（白，两主题同值）
    uint32_t onAccentForeground;   // 强调底色上的前景字色（白）
    // 边框
    uint32_t border;               // 内部分隔线
    uint32_t windowBorder;         // 窗口外轮廓（活动窗口）
    uint32_t windowBorderInactive; // 窗口外轮廓（非活动窗口，Windows 桌面惯例变淡）
    uint32_t unsavedMark;          // 未保存 ● 标记（形状即线索，颜色仅辅助）
    // 选区 / 多光标
    uint32_t multiCursor;
    uint32_t selection;
    uint32_t selectionBackground;  // 选区底色（不透明实底；文字保持原字色）
    uint32_t multiSelection;
    // 工具栏图标（彩色扁平，DEC-003；两主题各一档深浅适配）
    uint32_t iconNewBadge;    // 新建「+」角标（绿）
    uint32_t iconOpenBack;    // 打开文件夹后板（琥珀深）
    uint32_t iconOpenFront;   // 打开文件夹前盖（琥珀浅）
    uint32_t iconSaveBody;    // 保存软盘机身（蓝）
    uint32_t iconSaveLabel;   // 保存软盘标签（浅蓝）
    uint32_t iconWrapArrow;   // 自动换行折行箭头（teal）
    // 搜索
    uint32_t searchMatch;
    uint32_t searchMatchBorder;
    // —— 语法着色（代码着色 T3）——
    // core::SyntaxToken → 本字段组的归并映射集中一处可查：theme.cpp 的
    // SyntaxTokenColor()（枚举值→字段 + 归并到既有 token 的注释）。
    // 取值参考 VS Code Light+/Dark+ 语法配色（关键字蓝/紫蓝、字符串暖红、
    // 注释灰绿、数字绿等），与 contentBackground/textPrimary 和谐。
    // 未列字段者经映射归并既有 token：Operator/Identifier/Value → textPrimary
    //（VS Code 同款不另着色）、Error → error、Strong → syntaxKeyword
    uint32_t syntaxComment;       // 注释（Comment/CommentDoc；灰绿）
    uint32_t syntaxKeyword;       // 主关键字（Keyword/Keyword2/Label/Strong；蓝/紫蓝）
    uint32_t syntaxPreprocessor;  // 预处理/指令（Preprocessor；浅色与关键字同蓝、深色紫蓝，VS Code 同款）
    uint32_t syntaxString;        // 字符串（String/String2/Regex；暖红/沙棕）
    uint32_t syntaxNumber;        // 数字（Number；绿）
    uint32_t syntaxType;          // 类型/类名（Type；青蓝）
    uint32_t syntaxFunction;      // 函数名（Function；棕黄/芥末黄）
    uint32_t syntaxVariable;      // 变量/属性名（Variable/Attribute/Em；深蓝/浅蓝）
    uint32_t syntaxKey;           // 键/链接（Key/Link；JSON 属性名蓝，VS Code JSON 同款）
    uint32_t syntaxEscape;        // 转义序列（Escape；红/金棕）
    uint32_t syntaxTag;           // 标签/节/标题（Tag/Section/Header；褐红/蓝）
};

// 浅色色板（M1 默认）：取值照抄 AppTheme.qml 浅色分支。
const ThemePalette& LightPalette();

// 深色色板（M6，E5）：取值照抄 AppTheme.qml 深色分支（git: 4e313f0）。
const ThemePalette& DarkPalette();

// 当前生效色板（按 SetTheme 的全局标志在浅/深间分发；默认浅色）
const ThemePalette& CurrentPalette();

// —— 主题切换（E5）——
// 全局主题标志翻转（进程内唯一状态；影响 CurrentPalette 分发）。仅改标志，
// 不触碰窗口——重绘/DWM/编辑器重配置由组装层 ApplyTheme 一站式完成（main.cpp），
// 保证「token → 全部控件 → DWM → Scintilla」一次完成无中间态。
bool IsDarkTheme();
void SetTheme(bool dark);

// overlay 按 alpha 叠加到 base（均 0xAARRGGBB），返回不透明 ARGB。
// 半透明 token 在纯 GDI 绘制前的预合成通道；四舍五入取整。
uint32_t BlendOver(uint32_t base, uint32_t overlay);

// —— 语法着色（代码着色 T3）——
// core::SyntaxToken → 当前色板语义色（0xAARRGGBB 不透明）。editor 装配层
// 消费：样式槽 → token（core 注册表映射表）→ 本函数取色 → SCI_STYLESETFORE。
// token→色板字段/既有 token 的归并关系集中在本实现（theme.cpp，单一可查）
uint32_t SyntaxTokenColor(core::SyntaxToken token);

// 0xAARRGGBB → COLORREF（配合 BlendOver 的不透明输出；alpha 丢弃）
COLORREF ToColorRef(uint32_t argb);

// —— 非客户区暗色联动（E5：DWMWA_USE_IMMERSIVE_DARK_MODE / DarkMode_Explorer）——

/// DWM 沉浸式暗色属性（标题栏/滚动条等系统表面）：Win11 / Win10 1809+ 生效，
/// 属性 id 20（旧 Win10 build 为 19，失败静默回退尝试）；再旧系统调用失败
/// 返回错误码、无视觉影响。调用方无须判错。
void SetImmersiveDark(HWND hwnd, bool dark);

/// 查询 DWMWA_USE_IMMERSIVE_DARK_MODE 当前值（自测断言用；失败返回 dark=false）
bool QueryImmersiveDark(HWND hwnd);

/// 窗口视觉样式类切换（uxtheme）：dark → "DarkMode_Explorer"（Win11 深色
/// 滚动条/EDIT 光标等系统绘制元素随动），light → 恢复默认主题类。
/// uxtheme 动态解析（与 WindowDpi 同模式：exe/测试目标不新增链接依赖）。
/// 应用后由调用方触发 WM_NCPAINT（SetWindowPos FRAMECHANGED / WM_THEMECHANGED）。
void ApplyDarkExplorerThemeClass(HWND hwnd, bool dark);

// 窗口当前 DPI：GetDpiForWindow 动态解析（exe 不静态导入该符号，
// 更早的系统也能启动；hwnd 为空或 API 不可用时按主屏 DC 兜底）。
UINT WindowDpi(HWND hwnd);

// —— 尺寸 token（逻辑单位，96 DPI 基准；使用处一律经 Scale() 按窗口 DPI 换算）——
namespace metrics {

constexpr int CaptionHeight = 40;       // 标题栏高度（旧 QML barHeight）
constexpr int CaptionButtonWidth = 44;  // 窗口控制按钮 44×40（Win11 惯例）
constexpr int CaptionButtonHeight = 40;
constexpr int CaptionGlyphSize = 10;    // 自绘图标 10×10 逻辑像素（Win11 风格）
constexpr int TabAreaLeftGap = 4;       // Tab 占位区与菜单按钮间距（旧 QML leftMargin）
constexpr int TabAreaRightGap = 8;      // Tab 占位区与窗口控制按钮间距（旧 QML rightMargin）
constexpr int ResizeEdge = 5;           // 8 向缩放边缘命中宽
constexpr int ResizeCorner = 12;        // 四角缩放命中宽
constexpr int MinWindowWidth = 800;     // 最小窗口尺寸（需求 C1/E1，随 DPI 换算）
constexpr int MinWindowHeight = 500;

}  // namespace metrics

// 逻辑像素 → 物理像素（MulDiv 就近舍入）
inline int Scale(int logical, UINT dpi) {
    return ::MulDiv(logical, static_cast<int>(dpi), 96);
}

}  // namespace ui
