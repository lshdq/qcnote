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
    /// 强调底色（关闭按钮悬停/按下红底）上的前景字形；两主题同值（红底上均取白）
    readonly property color closeGlyphForeground: "#ffffff"
    /// 强调底色（accent 填充按钮等）上的前景字色；两主题同值（强调底上均取白）。
    /// 语义通用（与 closeGlyphForeground 同值，但后者专属关闭按钮，不复用）
    readonly property color onAccentForeground: "#ffffff"
    /// 边框 / 分隔线
    readonly property color border: dark ? "#3a4150" : "#c3cad6"
    /// 未保存 ● 标记（形状本身即线索，颜色仅辅助）
    readonly property color unsavedMark: dark ? "#ffb454" : "#b37700"

    // ---- 选区 / 多光标（列式编辑） ----
    /// 额外光标：与原生光标一致取强调色
    readonly property color multiCursor: accent
    /// 原生文本选区高亮（半透明，叠于内容背景之上；与 multiSelection 同色，观感统一）
    readonly property color selection: dark ? "#336f9bff" : "#293564d4"
    /// 多光标选区高亮（带透明度，叠于文本之上）
    readonly property color multiSelection: dark ? "#336f9bff" : "#293564d4"

    // ---- 搜索（查找/替换） ----
    /// 全部匹配底色（半透明，叠于文本之上）。色相取橙黄系而非选区蓝：
    /// 搜索高亮与原生选区 / 多光标选区同屏出现（当前匹配本身即原生选区），
    /// 蓝系会与选区蓝混淆；橙黄与蓝互补、同屏可辨，且与 unsavedMark
    /// 同属"注意力/标记"语义。透明度参照 selection / multiSelection
    /// （0x33 / 0x29）的风格略上调至 0x4d / 0x59：橙黄在浅色主题白色
    /// 内容背景上低于约 25% 透明度时几乎不可辨，仍属半透明档，文本可读。
    readonly property color searchMatch: dark ? "#4dffb454" : "#59ffb454"
    /// 当前匹配描边（不透明，同色系较底色加深——底色是同色相约 30% 透明度
    /// 的浅渍，描边取全强度橙黄 / 深琥珀，浅深两主题下均清晰可辨）。
    /// 「有描边 = 当前匹配」本身即非颜色区分线索，颜色仅辅助。
    /// 与 unsavedMark 同值但独立 token（语义不同：未保存标记 vs 搜索描边，
    /// 便于各自单独调整，参照 onAccentForeground / closeGlyphForeground 先例）。
    readonly property color searchMatchBorder: dark ? "#ffb454" : "#b37700"

    // ---- 字体 ----
    /// 编辑区等宽字体（Windows 自带；缺字时 Qt 自动回退系统字体）
    readonly property string fontFamilyMono: "Consolas"
}
