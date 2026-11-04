import QtQuick
import QtQuick.Controls

/**
 * 第二栏工具栏：左侧新建 / 打开 / 保存（高频操作）；右侧自动换行切换。
 * 图标按钮有 accessible name 与 tooltip；无活动文档时保存禁用（图标置灰可辨识）。
 * 键盘：Tab 进入，左右方向键在同组按钮间移动（仅左侧组，见 focusSiblingToolButton）。
 */
Rectangle {
    id: root

    signal requestNew()
    signal requestOpen()
    signal requestSave()

    color: AppTheme.toolbarBackground
    implicitHeight: 44

    Row {
        anchors.fill: parent
        anchors.leftMargin: 4
        anchors.topMargin: 2
        anchors.bottomMargin: 2
        spacing: 2

        ToolButton {
            id: newButton
            icon.source: "icons/new.svg"
            icon.color: AppTheme.textPrimary
            Accessible.name: "新建文档"
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: "新建文档（Ctrl+N）"
            onClicked: root.requestNew()
        }

        ToolButton {
            id: openButton
            icon.source: "icons/open.svg"
            icon.color: AppTheme.textPrimary
            Accessible.name: "打开文件"
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: "打开文件（Ctrl+O）"
            onClicked: root.requestOpen()
        }

        ToolButton {
            id: saveButton
            icon.source: "icons/save.svg"
            icon.color: enabled ? AppTheme.textPrimary : AppTheme.textDisabled
            enabled: DocumentManager.activeDocument !== null
            Accessible.name: "保存文档"
            Accessible.description: enabled ? "" : "没有打开的文档"
            ToolTip.visible: hovered
            ToolTip.delay: 600
            ToolTip.text: enabled ? "保存文档（Ctrl+S）" : "保存文档（当前没有文档）"
            onClicked: root.requestSave()
        }
    }

    // 右侧独立按钮：自动换行全局开关（设置类低频操作，锚定右缘，
    // 不挤占左侧高频操作区）；与「查看 → 自动换行」菜单项共用
    // SessionManager.wordWrap 数据源，状态始终一致。
    // 选中态以强调色图标表达；tooltip / Accessible 文本随状态显示
    // 「自动换行：开 / 关」，提供状态的非颜色线索
    //（不纳入 focusSiblingToolButton：该组仅服务左侧新建/打开/保存）
    ToolButton {
        id: wrapButton
        anchors.right: parent.right
        anchors.rightMargin: 4
        // 与左侧按钮组同高对齐（Row topMargin 2 + 子项顶部对齐）
        anchors.top: parent.top
        anchors.topMargin: 2
        checkable: true
        checked: SessionManager.wordWrap
        icon.source: "icons/wrap.svg"
        icon.color: checked ? AppTheme.accent : AppTheme.textPrimary
        Accessible.name: checked ? "自动换行：开" : "自动换行：关"
        ToolTip.visible: hovered
        ToolTip.delay: 600
        ToolTip.text: checked ? "自动换行：开" : "自动换行：关"
        onClicked: SessionManager.wordWrap = !SessionManager.wordWrap
    }

    // 底部分隔线
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: AppTheme.border
    }

    function focusSiblingToolButton(delta) {
        const buttons = [newButton, openButton, saveButton].filter(b => b.enabled)
        const i = buttons.findIndex(b => b.activeFocus)
        const next = i + delta
        if (i >= 0 && next >= 0 && next < buttons.length)
            buttons[next].forceActiveFocus()
    }

    Keys.onLeftPressed: focusSiblingToolButton(-1)
    Keys.onRightPressed: focusSiblingToolButton(1)
}
