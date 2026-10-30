import QtQuick
import QtQuick.Controls

/**
 * 文档 Tab 区（自绘，非默认 TabBar）。
 *
 * - 活动 Tab 白底（AppTheme.activeTab）、非活动 Tab 灰蓝底（AppTheme.inactiveTab）；
 * - Tab 含：未保存 ● 标记（非颜色线索）、文档名（超长省略 + tooltip 完整名）、关闭按钮；
 * - 溢出时横向滚动（水平 ListView + 滚动条），窗口控制按钮在本组件之外、永不被挤压；
 * - 键盘：左右方向键在 Tab 间移动焦点（循环），Enter/Space 激活对应文档；
 * - accessibleName 含未保存状态。
 */
Item {
    id: root

    signal closeRequested(var doc)

    readonly property var documentsModel: DocumentManager.documentsModel
    // 双依赖重算：activeDocument 变化（NOTIFY activeDocumentChanged）或行增删
    // （count NOTIFY countChanged）时重求 indexOf —— Q_INVOKABLE 调用本身不产生绑定依赖
    readonly property int activeIndex: {
        root.documentsModel.count
        return root.documentsModel.indexOf(DocumentManager.activeDocument)
    }

    /// Tab 是否溢出（需滚动）；ListView 与附着 ScrollBar 的 interactive 均由它联动：
    /// 未溢出时空白区按压事件不被 Flickable/ScrollBar 吞掉，向下传播给
    /// TitleBar.dragArea（窗口拖拽/双击最大化）；溢出时恢复滚动交互
    readonly property bool tabsOverflow: tabList.contentWidth > tabList.width

    /// 键盘焦点所在的 Tab 索引（仅焦点指示，不代表活动文档）
    property int focusIndex: activeIndex

    onActiveIndexChanged: {
        if (!root.activeFocus)
            focusIndex = activeIndex
        root.clampFocus()
    }

    Connections {
        target: root.documentsModel
        function onCountChanged() { root.clampFocus() }
    }

    onActiveFocusChanged: {
        if (activeFocus)
            focusIndex = activeIndex
    }

    Accessible.role: Accessible.PageTabList
    Accessible.name: "文档标签页"

    activeFocusOnTab: root.documentsModel.count > 0

    function clampFocus() {
        if (root.documentsModel.count === 0) {
            focusIndex = -1
            return
        }
        if (focusIndex < 0)
            focusIndex = 0
        else if (focusIndex >= root.documentsModel.count)
            focusIndex = root.documentsModel.count - 1
    }

    function moveFocus(delta) {
        if (root.documentsModel.count === 0)
            return
        const n = root.documentsModel.count
        focusIndex = ((focusIndex + delta) % n + n) % n
        tabList.currentIndex = focusIndex
        tabList.positionViewAtIndex(focusIndex, ListView.Contain)
    }

    function activateIndex(index) {
        if (index < 0 || index >= root.documentsModel.count)
            return
        focusIndex = index
        DocumentManager.activeDocument = root.documentsModel.at(index)
        tabList.currentIndex = index
        tabList.positionViewAtIndex(index, ListView.Contain)
    }

    Keys.onLeftPressed: moveFocus(-1)
    Keys.onRightPressed: moveFocus(1)
    Keys.onReturnPressed: activateIndex(focusIndex)
    Keys.onEnterPressed: activateIndex(focusIndex)
    Keys.onSpacePressed: activateIndex(focusIndex)

    ListView {
        id: tabList
        anchors.fill: parent
        orientation: ListView.Horizontal
        clip: true
        spacing: 2
        boundsBehavior: Flickable.StopAtBounds
        // 见 root.tabsOverflow 说明：未溢出时空白区事件穿透
        interactive: root.tabsOverflow
        model: root.documentsModel

        ScrollBar.horizontal: ScrollBar {
            policy: ScrollBar.AsNeeded
            // 见 root.tabsOverflow 说明：未溢出时空白区事件穿透
            interactive: root.tabsOverflow
        }

        delegate: Item {
            id: tab

            required property Document document
            required property int index

            readonly property Document doc: tab.document
            readonly property bool isActive: doc === DocumentManager.activeDocument
            readonly property bool isFocused: root.activeFocus && root.focusIndex === tab.index

            width: 168
            height: tabList.height

            Accessible.role: Accessible.PageTab
            Accessible.name: (doc ? doc.displayName : "")
                             + (doc && doc.modified ? "（未保存）" : "")

            Rectangle {
                id: tabBackground
                anchors.fill: parent
                radius: 3
                color: tab.isActive
                       ? AppTheme.activeTab
                       : (tabMouseArea.containsMouse || closeButtonHover.containsMouse
                          ? AppTheme.inactiveTabHover : AppTheme.inactiveTab)
                border.width: 1
                border.color: AppTheme.border
            }

            // 键盘焦点环（不依赖颜色深浅，双线框 + accent 色）
            Rectangle {
                anchors.fill: parent
                anchors.margins: 1
                radius: 2
                visible: tab.isFocused
                color: "transparent"
                border.color: AppTheme.focus
                border.width: 2
            }

            MouseArea {
                id: tabMouseArea
                anchors.fill: parent
                hoverEnabled: true
                onClicked: root.activateIndex(tab.index)
            }

            // 未保存标记：● 字形本身即线索（另有标题 * 线索）
            Text {
                id: unsavedMark
                visible: tab.doc && tab.doc.modified
                text: "●"
                font.pixelSize: 9
                color: AppTheme.unsavedMark
                anchors.left: parent.left
                anchors.leftMargin: 8
                anchors.verticalCenter: parent.verticalCenter
            }

            Text {
                id: tabLabel
                text: tab.doc ? tab.doc.displayName : ""
                color: tab.isActive ? AppTheme.textPrimary : AppTheme.textSecondary
                font.pixelSize: 13
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
                anchors.left: unsavedMark.visible ? unsavedMark.right : parent.left
                anchors.leftMargin: unsavedMark.visible ? 5 : 10
                anchors.right: closeButton.left
                anchors.rightMargin: 2
                anchors.top: parent.top
                anchors.bottom: parent.bottom
            }

            // 关闭按钮（Canvas 绘制 ×，悬停可见）
            Item {
                id: closeButton
                width: 20
                height: 20
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter

                Accessible.role: Accessible.Button
                Accessible.name: tab.doc ? "关闭标签页 " + tab.doc.displayName : "关闭标签页"
                Accessible.onPressAction: root.closeRequested(tab.doc)

                Rectangle {
                    anchors.fill: parent
                    radius: 2
                    color: closeButtonHover.containsMouse ? AppTheme.pressed : "transparent"
                }

                Canvas {
                    id: closeCanvas
                    anchors.centerIn: parent
                    width: 8
                    height: 8
                    property color paintColor: AppTheme.textSecondary
                    onPaintColorChanged: requestPaint()
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.reset()
                        ctx.strokeStyle = paintColor
                        ctx.lineWidth = 1.1
                        ctx.beginPath()
                        ctx.moveTo(0.5, 0.5)
                        ctx.lineTo(7.5, 7.5)
                        ctx.moveTo(7.5, 0.5)
                        ctx.lineTo(0.5, 7.5)
                        ctx.stroke()
                    }
                }

                MouseArea {
                    id: closeButtonHover
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: root.closeRequested(tab.doc)
                }
            }

            ToolTip.visible: tabMouseArea.containsMouse
            ToolTip.delay: 600
            ToolTip.text: (tab.doc ? tab.doc.displayName : "")
                          + (tab.doc && tab.doc.modified ? "（未保存）" : "")
        }
    }
}
