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
    uint32_t multiSelection;
    // 搜索
    uint32_t searchMatch;
    uint32_t searchMatchBorder;
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
