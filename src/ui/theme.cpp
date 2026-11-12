// 主题 token 取值与换算工具实现。
#include "theme.h"

#include <dwmapi.h>  // DwmSetWindowAttribute / DwmGetWindowAttribute（E5 暗色联动）

#include <cmath>

namespace ui {

namespace {
// 当前主题标志（E5）：默认浅色；SetTheme 翻转后 CurrentPalette 随即分发
bool g_darkTheme = false;

// DWMWA_USE_IMMERSIVE_DARK_MODE：Win10 1809（17763）之前不存在（引入时 id 19，
// 1903 起为 20）；先试 20，E_INVALIDARG 再试 19。两档都失败的旧系统无视觉
// 影响（调用静默），返回值仅用于诊断。
constexpr DWORD kDwmwaImmersiveDarkMode = 20;
constexpr DWORD kDwmwaImmersiveDarkModeLegacy = 19;
}  // namespace

const ThemePalette& LightPalette() {
    // 值逐项照抄 AppTheme.qml 浅色分支（git: 4e313f0 src/qml/AppTheme.qml）；
    // 深色对应值见 DarkPalette()。
    static const ThemePalette kLight{
        0xFFDDE2ECu,  // titleBarBackground  浅 #dde2ec
        0xFFF2F4F8u,  // toolbarBackground   浅 #f2f4f8
        0xFFFFFFFFu,  // contentBackground   浅 #ffffff
        0xFFE8EBF1u,  // statusBarBackground 浅 #e8ebf1
        0xFFF2F4F8u,  // activeTab           = toolbarBackground（连通式约束）
        0xFF212734u,  // textPrimary
        0xFF5C6678u,  // textSecondary
        0xFFA3AAB7u,  // textDisabled
        0xFF3564D4u,  // accent
        0xFF3564D4u,  // focus               = accent
        0xFFC8402Cu,  // error
        0x16000000u,  // hover               浅：8.6% 黑叠加
        0x26000000u,  // pressed             浅：14.9% 黑叠加
        0xFFC42B1Cu,  // closeHover          #c42b1c（红，两主题同值）
        0xFFA12316u,  // closePressed        #a12316
        0xFFFFFFFFu,  // closeGlyphForeground 白
        0xFFFFFFFFu,  // onAccentForeground  白
        0xFFC3CAD6u,  // border（内部分隔线）
        0xFFA8B3C4u,  // windowBorder（活动）
        0xFFC9CFDAu,  // windowBorderInactive（非活动）
        0xFFB37700u,  // unsavedMark
        0xFF3564D4u,  // multiCursor = accent
        0x293564D4u,  // selection（alpha 0x29 半透明叠加）
        0xFFADD6FFu,  // selectionBackground 选区实底（VS Code 浅色同款）
        0x293564D4u,  // multiSelection
        0xFF2E9E4Fu,  // iconNewBadge    浅 #2e9e4f（新建「+」角标绿）
        0xFFE2A52Eu,  // iconOpenBack    浅 #e2a52e（文件夹后板琥珀深）
        0xFFF6C460u,  // iconOpenFront   浅 #f6c460（文件夹前盖琥珀浅）
        0xFF4A8DD8u,  // iconSaveBody    浅 #4a8dd8（软盘机身蓝）
        0xFFDAE8F8u,  // iconSaveLabel   浅 #dae8f8（软盘标签浅蓝）
        0xFF2FA08Cu,  // iconWrapArrow   浅 #2fa08c（折行箭头 teal）
        0x59FFB454u,  // searchMatch（alpha 0x59 橙黄半透明）
        0xFFB37700u,  // searchMatchBorder（与 unsavedMark 同值、独立 token）
    };
    return kLight;
}

const ThemePalette& DarkPalette() {
    // 值逐项照抄 AppTheme.qml 深色分支（git: 4e313f0 src/qml/AppTheme.qml）。
    static const ThemePalette kDark{
        0xFF171C24u,  // titleBarBackground  深 #171c24
        0xFF232833u,  // toolbarBackground   深 #232833
        0xFF262B34u,  // contentBackground   深 #262b34
        0xFF1D222Bu,  // statusBarBackground 深 #1d222b（DEC-102：不随标题栏联动）
        0xFF232833u,  // activeTab           = toolbarBackground（连通式约束）
        0xFFE8EAF0u,  // textPrimary
        0xFF9AA3B5u,  // textSecondary
        0xFF5D6472u,  // textDisabled
        0xFF6F9BFFu,  // accent
        0xFF6F9BFFu,  // focus               = accent
        0xFFFF7A68u,  // error
        0x22FFFFFFu,  // hover               深：13.3% 白叠加
        0x33FFFFFFu,  // pressed             深：20% 白叠加
        0xFFC42B1Cu,  // closeHover          #c42b1c（红，两主题同值）
        0xFFA12316u,  // closePressed        #a12316
        0xFFFFFFFFu,  // closeGlyphForeground 白
        0xFFFFFFFFu,  // onAccentForeground  白
        0xFF3A4150u,  // border（内部分隔线）
        0xFF4A5364u,  // windowBorder（活动）
        0xFF333947u,  // windowBorderInactive（非活动）
        0xFFFFB454u,  // unsavedMark
        0xFF6F9BFFu,  // multiCursor = accent
        0x336F9BFFu,  // selection（alpha 0x33 半透明叠加）
        0xFF264F78u,  // selectionBackground 选区实底（VS Code 深色同款）
        0x336F9BFFu,  // multiSelection
        0xFF4CBF6Bu,  // iconNewBadge    深 #4cbf6b（新建「+」角标绿）
        0xFFC9973Bu,  // iconOpenBack    深 #c9973b（文件夹后板琥珀深）
        0xFFF0BC5Au,  // iconOpenFront   深 #f0bc5a（文件夹前盖琥珀浅）
        0xFF6AA7E0u,  // iconSaveBody    深 #6aa7e0（软盘机身蓝）
        0xFFD8E6F6u,  // iconSaveLabel   深 #d8e6f6（软盘标签浅蓝）
        0xFF46B9A2u,  // iconWrapArrow   深 #46b9a2（折行箭头 teal）
        0x4DFFB454u,  // searchMatch（alpha 0x4d 橙黄半透明）
        0xFFFFB454u,  // searchMatchBorder（与 unsavedMark 同值、独立 token）
    };
    return kDark;
}

// —— 主题标志（进程内单例；默认浅色）——
bool IsDarkTheme() {
    return g_darkTheme;
}

void SetTheme(bool dark) {
    g_darkTheme = dark;
}

const ThemePalette& CurrentPalette() {
    return g_darkTheme ? DarkPalette() : LightPalette();
}

uint32_t BlendOver(uint32_t base, uint32_t overlay) {
    const auto channel = [](uint32_t v, int shift) -> int {
        return static_cast<int>((v >> shift) & 0xFFu);
    };
    const int alpha = channel(overlay, 24);
    if (alpha <= 0) {
        return base | 0xFF000000u;
    }
    if (alpha >= 255) {
        return overlay | 0xFF000000u;
    }
    const double a = alpha / 255.0;
    const auto mix = [&](int shift) -> uint32_t {
        const double b = channel(base, shift);
        const double o = channel(overlay, shift);
        return static_cast<uint32_t>(std::lround(b * (1.0 - a) + o * a));
    };
    return 0xFF000000u | (mix(16) << 16) | (mix(8) << 8) | mix(0);
}

COLORREF ToColorRef(uint32_t argb) {
    return RGB((argb >> 16) & 0xFFu, (argb >> 8) & 0xFFu, argb & 0xFFu);
}

void SetImmersiveDark(HWND hwnd, bool dark) {
    if (!hwnd) return;
    const BOOL value = dark ? TRUE : FALSE;
    HRESULT hr = ::DwmSetWindowAttribute(hwnd, kDwmwaImmersiveDarkMode,
                                         &value, sizeof(value));
    if (FAILED(hr)) {
        // 旧 Win10 build：属性 id 为 19（1903 前引入形态），静默重试
        ::DwmSetWindowAttribute(hwnd, kDwmwaImmersiveDarkModeLegacy,
                                &value, sizeof(value));
    }
}

bool QueryImmersiveDark(HWND hwnd) {
    if (!hwnd) return false;
    BOOL value = FALSE;
    HRESULT hr = ::DwmGetWindowAttribute(hwnd, kDwmwaImmersiveDarkMode,
                                         &value, sizeof(value));
    if (FAILED(hr)) {
        hr = ::DwmGetWindowAttribute(hwnd, kDwmwaImmersiveDarkModeLegacy,
                                     &value, sizeof(value));
    }
    return SUCCEEDED(hr) && value != FALSE;
}

void ApplyDarkExplorerThemeClass(HWND hwnd, bool dark) {
    if (!hwnd) return;
    // uxtheme 动态解析：exe 与单测目标不新增 uxtheme.lib 链接依赖（与
    // WindowDpi 的 GetDpiForWindow 同模式）；系统必有 uxtheme.dll，失败静默。
    using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static const auto fn = reinterpret_cast<SetWindowThemeFn>(
        reinterpret_cast<void*>(::GetProcAddress(
            ::GetModuleHandleW(L"uxtheme.dll"), "SetWindowTheme")));
    if (!fn) return;
    // dark："DarkMode_Explorer"（Win11 深色滚动条等系统表面）；light：主题子类
    // 置空恢复默认。失败（类不存在等）返回错误码，视觉保持默认，无副作用。
    fn(hwnd, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
}


UINT WindowDpi(HWND hwnd) {
    // 动态解析：exe 导入表不出现 GetDpiForWindow，缺该导出的旧系统仍可启动；
    // PMv2 语境在 1703 前的 Windows 上 manifest 声明也会被忽略（回到系统虚拟化）。
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const auto fn = reinterpret_cast<GetDpiForWindowFn>(
        ::GetProcAddress(::GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (fn && hwnd) {
        const UINT dpi = fn(hwnd);
        if (dpi != 0) {
            return dpi;
        }
    }
    const HDC dc = ::GetDC(nullptr);
    const int dpi = dc ? ::GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) {
        ::ReleaseDC(nullptr, dc);
    }
    return dpi > 0 ? static_cast<UINT>(dpi) : 96u;
}

}  // namespace ui
