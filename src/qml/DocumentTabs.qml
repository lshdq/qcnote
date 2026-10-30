pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

/**
 * 文档 Tab 区（自绘，非默认 TabBar）。
 *
 * - 活动 Tab 白底（AppTheme.activeTab）、非活动 Tab 灰蓝底（AppTheme.inactiveTab）；
 * - Tab 宽度自适应：按文字自然宽度取宽（上限 minTabWidth~cap），空间不足按可收缩量
 *   等比收缩，收缩饱和后回退最小宽度 + 既有横向滚动；
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
    /// （宽度自适应天然兼容：收缩能放下时 contentWidth 贴合 width 不滚，饱和后自动转滚动）
    readonly property bool tabsOverflow: tabList.contentWidth > tabList.width

    // ===== 宽度分配（Tab 宽度自适应）=====
    // 装饰尺寸常量：单点定义，宽度计算与下方 delegate 布局锚点共用，禁止两处魔法数字
    readonly property int tabSpacing: 2           // Tab 间距
    readonly property int minTabWidth: 100        // Tab 最小宽度（固定下限）
    readonly property int leftMarginWithMark: 8   // 有未保存 ● 标记时的左边距
    readonly property int leftMarginPlain: 10     // 无标记时的左边距
    readonly property int markToLabelSpacing: 5   // ● 标记与标签文字的间距
    readonly property int labelToCloseSpacing: 2  // 标签文字与关闭按钮的间距
    readonly property int closeButtonSize: 20     // 关闭按钮边长
    readonly property int closeRightEdge: 4       // 关闭按钮距 Tab 右缘
    readonly property int markPixelSize: 9        // ● 标记字号
    readonly property int labelPixelSize: 13      // Tab 标签字号，测量与渲染共用

    // 各 Tab 的自然宽度（文字自然宽度 + 装饰宽度），由 recomputeNaturalWidths() 集中计算。
    // 不用绑定求值：绑定内写共享 TextMetrics.text 会同步触发 advanceWidthChanged，
    // 被 Qt 判定为绑定重入（刷 "Binding loop detected" 警告）；改为命令式重算后
    // 写入无绑定订阅者，警告消除。重算触发源（一帧内同步完成）：
    // - 行增删：下方 Connections.onCountChanged（Q_INVOKABLE at() 无绑定依赖，
    //   countChanged 显式转发，同 activeIndex 的“双依赖”思路）；
    // - 重命名/未保存状态切换：下方 Repeater（delegate id: docRelay）逐文档转发
    //   displayNameChanged/modifiedChanged（两者无聚合信号，ListView 只实例化
    //   可视行，故用全量实例化的 0 尺寸 Repeater 桥接）。
    // 窗口缩放不影响自然宽度，仅影响 distributedWidths（纯绑定，无需参与本函数）。
    property var naturalWidths: []

    function recomputeNaturalWidths() {
        const widths = []
        for (let i = 0; i < root.documentsModel.count; ++i) {
            const doc = root.documentsModel.at(i)
            labelMetrics.text = doc ? doc.displayName : ""
            // 装饰宽：● 标记出现时（左边距 + 标记宽 + 间距）或纯左边距，
            // 再加 标签-关闭按钮间距 + 关闭按钮宽 + 右缘
            const decor = (doc && doc.modified
                           ? root.leftMarginWithMark + markMetrics.advanceWidth
                             + root.markToLabelSpacing
                           : root.leftMarginPlain)
                          + root.labelToCloseSpacing + root.closeButtonSize + root.closeRightEdge
            widths.push(labelMetrics.advanceWidth + decor)
        }
        root.naturalWidths = widths
    }

    // 宽度分配结果（delegate 实际取宽），依赖 naturalWidths 与 tabList.width（窗口缩放重算）：
    // 1) 单 Tab 动态上限 cap = max(minTabWidth, floor(W/4))，极窄窗口退化为下限；
    // 2) n_i 夹取到 [minTabWidth, cap]（自然宽度不足下限时抬到下限，恒有 100 ≤ w_i）；
    // 3) 空间充足（Σn_i + gaps ≤ W）：w_i = n_i，各 Tab 独立适配、文字完整显示；
    // 4) 空间不足：缺口 D 按各 Tab 可收缩量 (n_i − minTabWidth) 等比分配，
    //    w_i = n_i − (D/C)×(n_i − minTabWidth)（不低于 minTabWidth）；
    //    收缩结果向下取整：保证 Σw_i + gaps ≤ W 恒成立，contentWidth 不因舍入虚增
    //    被 tabsOverflow 误判溢出、滚动条闪烁；
    // 5) 收缩饱和（D ≥ C）：全部取 minTabWidth，既有横向滚动自动接管。
    readonly property var distributedWidths: {
        const n = root.naturalWidths.length
        if (n === 0)
            return []
        const W = Math.max(0, tabList.width)
        const gaps = root.tabSpacing * (n - 1)
        const cap = Math.max(root.minTabWidth, Math.floor(W / 4))
        const capped = []
        let sum = 0
        for (let i = 0; i < n; ++i) {
            capped.push(Math.max(root.minTabWidth, Math.min(root.naturalWidths[i], cap)))
            sum += capped[i]
        }
        if (sum + gaps <= W)
            return capped
        const deficit = sum + gaps - W
        let shrinkable = 0
        for (let i = 0; i < n; ++i)
            shrinkable += capped[i] - root.minTabWidth
        if (deficit >= shrinkable)
            return new Array(n).fill(root.minTabWidth)
        const ratio = deficit / shrinkable
        const result = []
        for (let i = 0; i < n; ++i)
            result.push(Math.floor(capped[i] - ratio * (capped[i] - root.minTabWidth)))
        return result
    }

    /// 键盘焦点所在的 Tab 索引（仅焦点指示，不代表活动文档）
    property int focusIndex: activeIndex

    onActiveIndexChanged: {
        if (!root.activeFocus)
            focusIndex = activeIndex
        root.clampFocus()
    }

    Connections {
        target: root.documentsModel
        function onCountChanged() {
            root.clampFocus()
            root.recomputeNaturalWidths()
        }
    }

    // 逐文档信号转发桥：displayName/modified 无聚合信号，借全量实例化的
    // 0 尺寸 Repeater 把行级 NOTIFY 转发到根节点集中重算自然宽度
    Repeater {
        model: root.documentsModel
        delegate: Item {
            id: docRelay
            required property Document document
            width: 0
            height: 0
            Connections {
                target: docRelay.document
                function onDisplayNameChanged() { root.recomputeNaturalWidths() }
                function onModifiedChanged() { root.recomputeNaturalWidths() }
            }
        }
    }

    Component.onCompleted: root.recomputeNaturalWidths()

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

    // 共享文字测量：labelMetrics 与 tabLabel 同字体（仅 pixelSize、默认字体族）；
    // markMetrics 实测 ● 标记宽（同 unsavedMark 字号）
    TextMetrics {
        id: labelMetrics
        font.pixelSize: root.labelPixelSize
    }

    TextMetrics {
        id: markMetrics
        text: "●"
        font.pixelSize: root.markPixelSize
    }

    ListView {
        id: tabList
        anchors.fill: parent
        orientation: ListView.Horizontal
        clip: true
        spacing: root.tabSpacing
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

            // 宽度取分配结果；模型增删的瞬态窗口内数组可能尚未覆盖该行，回退最小宽度
            width: root.distributedWidths[tab.index] !== undefined
                   ? root.distributedWidths[tab.index] : root.minTabWidth
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
                font.pixelSize: root.markPixelSize
                color: AppTheme.unsavedMark
                anchors.left: parent.left
                anchors.leftMargin: root.leftMarginWithMark
                anchors.verticalCenter: parent.verticalCenter
            }

            Text {
                id: tabLabel
                text: tab.doc ? tab.doc.displayName : ""
                color: tab.isActive ? AppTheme.textPrimary : AppTheme.textSecondary
                font.pixelSize: root.labelPixelSize
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
                anchors.left: unsavedMark.visible ? unsavedMark.right : parent.left
                anchors.leftMargin: unsavedMark.visible ? root.markToLabelSpacing : root.leftMarginPlain
                anchors.right: closeButton.left
                anchors.rightMargin: root.labelToCloseSpacing
                anchors.top: parent.top
                anchors.bottom: parent.bottom
            }

            // 关闭按钮（Canvas 绘制 ×，悬停可见）
            Item {
                id: closeButton
                width: root.closeButtonSize
                height: root.closeButtonSize
                anchors.right: parent.right
                anchors.rightMargin: root.closeRightEdge
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
