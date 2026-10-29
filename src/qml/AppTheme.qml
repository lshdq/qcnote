pragma Singleton
import QtQuick

/**
 * 语义主题 token 单例。
 *
 * 全部颜色集中于此，QML 中禁止散落硬编码色值；
 * dark 属性驱动浅色/深色两套取值，ApplicationWindow 的
 * Material.theme 绑定 AppTheme.dark。
 *
 * 状态表达遵循"不得仅依赖颜色"：未保存用 ● 与标题 * 作为形状线索，
 * 颜色仅是辅助。
 */
QtObject {
    id: root

    /// 当前是否深色主题（可写，切换后所有 token 自动重算）
    property bool dark: false

    // ---- 表面 ----
    readonly property color titleBarBackground: dark ? "#1d222b" : "#e8ebf1"
    readonly property color toolbarBackground:  dark ? "#232833" : "#f2f4f8"
    readonly property color contentBackground:  dark ? "#262b34" : "#ffffff"
    readonly property color statusBarBackground: dark ? "#1d222b" : "#e8ebf1"

    // ---- 标签 ----
    /// 活动标签：与内容区同色（浅色主题为白色，形成与内容区的连续感）
    readonly property color activeTab: dark ? "#262b34" : "#ffffff"
    /// 非活动标签：灰蓝色
    readonly property color inactiveTab: dark ? "#39404e" : "#adb8cc"
    /// 非活动标签悬停
    readonly property color inactiveTabHover: dark ? "#424a5a" : "#9daac2"

    // ---- 文本 ----
    readonly property color textPrimary:   dark ? "#e8eaf0" : "#212734"
    readonly property color textSecondary: dark ? "#9aa3b5" : "#5c6678"
    readonly property color textDisabled:  dark ? "#5d6472" : "#a3aab7"

    // ---- 语义 ----
    /// 强调色 / 焦点环（同步作为 Material.primary）
    readonly property color accent: dark ? "#6f9bff" : "#3564d4"
    readonly property color focus: accent
    readonly property color error: dark ? "#ff7a68" : "#c8402c"
    /// 悬停叠加（半透明，叠在表面色之上）
    readonly property color hover:   dark ? "#22ffffff" : "#16000000"
    readonly property color pressed: dark ? "#33ffffff" : "#26000000"
    /// 窗口关闭按钮悬停 / 按下（红）
    readonly property color closeHover:   "#c42b1c"
    readonly property color closePressed: "#a12316"
    /// 边框 / 分隔线
    readonly property color border: dark ? "#3a4150" : "#c3cad6"
    /// 未保存 ● 标记（形状本身即线索，颜色仅辅助）
    readonly property color unsavedMark: dark ? "#ffb454" : "#b37700"

    // ---- 字体 ----
    /// 编辑区等宽字体（Windows 自带；缺字时 Qt 自动回退系统字体）
    readonly property string fontFamilyMono: "Consolas"
}
