import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

/**
 * 顶部窗口栏：菜单按钮 → 文档 Tab 区 → 最小化/最大化/关闭。
 *
 * - 窗口控制按钮固定在最右（独立 Row，锚定右缘），永不被 Tab 挤压；
 * - Tab 区独立组件横向滚动（见 DocumentTabs）；
 * - 空白区拖拽移动窗口（阈值触发 startSystemMove，保证双击仍可最大化/还原）、
 *   双击最大化/还原；最大化时禁用拖拽；
 * - 菜单项快捷键通过自绘右对齐文本显示（Qt Quick Controls 2 的 MenuItem
 *   无 shortcut 属性，按键由窗口级 Shortcut 统一处理）。
 */
Rectangle {
    id: root

    required property var window

    signal requestNew()
    signal requestOpen()
    signal requestSave()
    signal requestSaveAs()
    signal requestCloseTab(var doc)
    signal requestQuit()

    readonly property int barHeight: 40

    /// 主菜单是否打开（供 Main 屏蔽快捷键）
    readonly property alias menuVisible: mainMenu.visible

    color: AppTheme.titleBarBackground
    implicitHeight: barHeight

    // 空白区：拖拽移动 + 双击最大化/还原（先于按钮声明，置于下层）
    MouseArea {
        id: dragArea
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton

        readonly property bool maximized: root.window.visibility === Window.Maximized
        property point pressPos: Qt.point(0, 0)

        onPressed: (mouse) => {
            mouse.accepted = true
            pressPos = Qt.point(mouse.x, mouse.y)
        }
        // 移动超过阈值才启动系统拖拽：纯点击不启动，双击手势不受影响
        onPositionChanged: (mouse) => {
            if (!pressed || maximized)
                return
            if (Math.abs(mouse.x - pressPos.x) > 4 || Math.abs(mouse.y - pressPos.y) > 4)
                root.window.startSystemMove()
        }
        onDoubleClicked: {
            if (root.window.visibility === Window.Maximized)
                root.window.showNormal()
            else
                root.window.showMaximized()
        }
    }

    CaptionButton {
        id: menuButton
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        glyph: "menu"
        accessibleName: "主菜单"
        showToolTip: true
        activeFocusOnTab: true
        onClicked: mainMenu.popup(menuButton, 0, menuButton.height)
    }

    Menu {
        id: mainMenu
        title: "主菜单"

        component KeyMenuItem: MenuItem {
            id: keyItem
            property string keyText: ""
            contentItem: RowLayout {
                spacing: 12
                Label {
                    text: keyItem.text
                    font: keyItem.font
                    color: keyItem.enabled ? AppTheme.textPrimary : AppTheme.textDisabled
                }
                Item { Layout.fillWidth: true }
                Label {
                    visible: keyItem.keyText !== ""
                    text: keyItem.keyText
                    font: keyItem.font
                    color: AppTheme.textSecondary
                }
            }
        }

        KeyMenuItem {
            text: "新建"
            keyText: "Ctrl+N"
            onTriggered: root.requestNew()
        }
        KeyMenuItem {
            text: "打开…"
            keyText: "Ctrl+O"
            onTriggered: root.requestOpen()
        }
        KeyMenuItem {
            text: "保存"
            keyText: "Ctrl+S"
            enabled: DocumentManager.activeDocument !== null
            onTriggered: root.requestSave()
        }
        KeyMenuItem {
            text: "另存为…"
            keyText: "Ctrl+Shift+S"
            enabled: DocumentManager.activeDocument !== null
            onTriggered: root.requestSaveAs()
        }
        KeyMenuItem {
            text: "关闭标签页"
            keyText: "Ctrl+W"
            enabled: DocumentManager.activeDocument !== null
            onTriggered: root.requestCloseTab(DocumentManager.activeDocument)
        }

        MenuSeparator {}

        Menu {
            id: viewMenu
            title: "查看"

            MenuItem {
                text: "深色主题"
                checkable: true
                checked: AppTheme.dark
                onTriggered: AppTheme.dark = !AppTheme.dark
            }
        }

        MenuSeparator {}

        MenuItem {
            text: "退出"
            onTriggered: root.requestQuit()
        }
    }

    DocumentTabs {
        id: documentTabs
        anchors.left: menuButton.right
        anchors.right: windowControls.left
        anchors.leftMargin: 4
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        height: 32

        onCloseRequested: (doc) => root.requestCloseTab(doc)
    }

    Row {
        id: windowControls
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter

        CaptionButton {
            glyph: "minimize"
            accessibleName: "最小化"
            activeFocusOnTab: true
            onClicked: root.window.showMinimized()
        }
        CaptionButton {
            glyph: root.window.visibility === Window.Maximized ? "restore" : "maximize"
            accessibleName: root.window.visibility === Window.Maximized ? "还原" : "最大化"
            activeFocusOnTab: true
            onClicked: {
                if (root.window.visibility === Window.Maximized)
                    root.window.showNormal()
                else
                    root.window.showMaximized()
            }
        }
        CaptionButton {
            glyph: "close"
            accessibleName: "关闭"
            activeFocusOnTab: true
            onClicked: root.window.close()
        }
    }
}
