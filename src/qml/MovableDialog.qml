import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

/**
 * 可拖动对话框（应用内弹窗统一基类，弹窗拖动需求）。
 *
 * - 对外属性 dialogTitle：标题渲染在自绘 header 把手（Dialog.title 保持
 *   空，避免框架 header 双标题）；header 视觉对齐 Material 风格 Dialog
 *   标题（同字号 font.pixelSize、同边距 padding 24 / 底 0），文本色取
 *   AppTheme.textPrimary，无分隔线与背景色变化（保持现状卡片样式）；
 * - 位置结构（DEC-003）：未拖动时 x/y 为默认位置绑定（随窗口 resize
 *   跟随重算，并防御性 clamp）；首次拖动起切换为字面存储值；重开
 *   （aboutToShow）复位为未拖动态（拖动仅当次打开期间生效）；
 * - 默认位置：居中；覆写点为 defaultX/defaultY 属性（NaN = 自动居中），
 *   如 SearchDialog 顶部对齐覆写 defaultY: 96（DEC-002）；
 * - 边界约束（把手不丢失）：x ∈ [0, parent.width - keepWidth]、
 *   y ∈ [0, parent.height - headerHeight]——把手整体留在窗口内可抓回，
 *   弹窗身体允许向右/向下部分移出窗口。左/上不可越出是 Qt 对 Item 型
 *   Popup 的框架级强制（x/y 为 effective 位置，越界请求被
 *   QQuickPopupPositioner 钳回窗口内，实测 Qt 6.11 负 x 恒回落 0），故
 *   此处下界取 0：若仍存储负值，会与框架实际位置脱节，产生反向拖动
 *   死区；已拖动的可见弹窗在窗口 resize 时重新施加 clamp；
 * - 拖拽：header 内 DragHandler（target: null，鼠标左键）直改 x/y，
 *   无动画；不设特殊光标形状（Windows 标题栏惯例）；
 * - 模态、closePolicy、footer 结构与焦点行为完全不变；header 为 Label
 *   （非可聚焦控件，不进入 Tab 焦点链）；无障碍：header 标题提供
 *   Accessible.name / StaticText 角色，弹窗可访问名不回退。
 */
Dialog {
    id: root

    /// 标题文本（渲染在自绘 header；Dialog.title 保持空，避免双标题）
    property string dialogTitle: ""

    /// 默认位置覆写（NaN = 自动居中）：子组件 / 实例按需覆写
    property real defaultX: NaN
    property real defaultY: NaN

    /// 把手在窗口内保留的最小可抓宽度（px，水平边界约束用）
    readonly property int keepWidth: 80

    // ---- 位置状态（DEC-003）----

    /// 当次打开期间是否已被拖动：false 时 x/y 为默认位置绑定；true 时
    /// 为字面存储值（不随 resize 重算，改为重新施加边界约束）
    property bool userMoved: false
    property real storedX: 0
    property real storedY: 0

    // 未拖动：默认位置绑定（防御性 clamp）；首次拖动起：字面存储值
    x: userMoved ? storedX : clampX(resolveDefaultX())
    y: userMoved ? storedY : clampY(resolveDefaultY())

    function resolveDefaultX() {
        return Number.isNaN(defaultX) ? (parent.width - width) / 2 : defaultX
    }
    function resolveDefaultY() {
        return Number.isNaN(defaultY) ? (parent.height - height) / 2 : defaultY
    }

    /// 边界约束：把手整体留在窗口内（弹窗身体允许向右/下移出窗口）。
    /// 下界取 0（Qt 对 Item 型 Popup 的框架级强制 effective x/y ≥ 0，
    /// 存储负值会与实际位置脱节产生反向拖动死区）
    function clampX(v) {
        const hi = parent.width - keepWidth
        return Math.min(Math.max(v, 0), hi)
    }
    function clampY(v) {
        const hi = parent.height - (header ? header.height : 0)
        return Math.min(Math.max(v, 0), hi)
    }

    // 重开复位为未拖动态（x/y 回默认位置绑定）。经 Connections 挂接而非
    // 根级 onAboutToShow：根级信号处理器在 QML 派生组件中会被覆写
    // （SearchDialog 自带 onAboutToShow），Connections 连接与其并存不互斥
    Connections {
        target: root
        function onAboutToShow() {
            root.userMoved = false
        }
    }

    // 窗口 resize（popup 父项 = 窗口 contentItem 尺寸变化）：未拖动由
    // x/y 绑定自动跟随默认位置；已拖动的可见弹窗重新施加边界约束，
    // 保证把手仍在窗口内
    Connections {
        target: root.parent
        function onWidthChanged() { root.reclampIfMoved() }
        function onHeightChanged() { root.reclampIfMoved() }
    }

    function reclampIfMoved() {
        if (!visible || !userMoved)
            return
        storedX = clampX(storedX)
        storedY = clampY(storedY)
    }

    // ---- 自绘 header：标题 + 拖拽把手 ----
    // 视觉对齐 Material 风格 Dialog 标题：同字号（Material.dialogTitle-
    // FontPixelSize）、同边距（padding 24 / bottomPadding 0）；背景保持
    // 透明（弹窗背景卡片已带圆角与底色，无分隔线 / 背景色变化）。
    // 把手高度 = Label 自身高度（内容高 + 顶部 24px 边距），满足抓取
    header: Label {
        id: dragHandle

        text: root.dialogTitle
        elide: Label.ElideRight
        font.pixelSize: Material.dialogTitleFontPixelSize
        padding: 24
        bottomPadding: 0
        color: AppTheme.textPrimary

        // 无障碍：标题文本可访问（名称 + StaticText 角色）
        Accessible.name: root.dialogTitle
        Accessible.role: Accessible.StaticText

        DragHandler {
            id: handleDrag

            target: null

            // 拖动会话状态：激活且为鼠标左键按下才有效
            property bool dragging: false
            // 激活（越过拖动阈值）时弹窗位置（此后 x/y 切换为字面存储值）
            property real originX: 0
            property real originY: 0

            onActiveChanged: {
                // 质心跟踪点（centroid: HandlerPoint）：pressedButtons 为
                // 当前按住的按钮集合（1 = 左键）；DragHandler 对鼠标手势
                // 仅在左键拖动时激活，此处显式守卫双保险
                dragging = active && (centroid.pressedButtons & Qt.LeftButton) !== 0
                if (dragging) {
                    originX = root.x
                    originY = root.y
                }
            }
            onActiveTranslationChanged: {
                if (!dragging)
                    return
                // 首次真实位移起切换为字面存储值（未移动的按下/释放
                // 不打断未拖动态的 resize 跟随语义）
                if (!root.userMoved) {
                    root.storedX = originX
                    root.storedY = originY
                    root.userMoved = true
                }
                // 位移实时更新（activeTranslation 为自手势起点的累计
                // 位移；无动画）
                root.storedX = root.clampX(originX + activeTranslation.x)
                root.storedY = root.clampY(originY + activeTranslation.y)
            }
        }
    }
}
