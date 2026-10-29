import QtQuick
import QtQuick.Controls

/**
 * 第二栏工具栏：新建 / 打开 / 保存（高频操作）。
 * 图标按钮有 accessible name 与 tooltip；无活动文档时保存禁用（图标置灰可辨识）。
 * 键盘：Tab 进入，左右方向键在同组按钮间移动。
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
