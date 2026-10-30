import QtQuick
import QtQuick.Controls

/**
 * 标题栏图形按钮（菜单 / 最小化 / 最大化-还原 / 关闭）。
 *
 * 图标用 Canvas 矢量绘制（不依赖字体符号与图片资源，跨主题/跨平台稳定）；
 * 悬停、按下、键盘焦点状态可辨识；关闭按钮悬停为红色（AppTheme token）。
 * 键盘：Space / Enter 触发。
 */
Item {
    id: root

    /// minimize | maximize | restore | close | menu
    property string glyph: "minimize"
    property string accessibleName: ""
    property bool showToolTip: false

    readonly property bool isClose: glyph === "close"

    /// glyph 变化时触发 Canvas 重绘（信号属于根 Item，需在根部处理）
    onGlyphChanged: canvas.requestPaint()

    signal clicked()

    width: 44
    height: 40

    Accessible.role: Accessible.Button
    Accessible.name: accessibleName
    Accessible.onPressAction: root.activate()

    function activate() {
        root.clicked()
    }

    Rectangle {
        id: background
        anchors.fill: parent
        color: {
            if (!root.enabled)
                return "transparent"
            if (mouseArea.pressed)
                return root.isClose ? AppTheme.closePressed : AppTheme.pressed
            if (mouseArea.containsMouse)
                return root.isClose ? AppTheme.closeHover : AppTheme.hover
            return "transparent"
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: root.activeFocus
        color: "transparent"
        border.color: AppTheme.focus
        border.width: 2
    }

    Canvas {
        id: canvas
        anchors.centerIn: parent
        width: 10
        height: 10

        property color paintColor: root.isClose && mouseArea.containsMouse
                                    ? AppTheme.closeGlyphForeground : AppTheme.textPrimary
        onPaintColorChanged: requestPaint()

        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.strokeStyle = paintColor
            ctx.lineWidth = 1.2
            ctx.beginPath()
            switch (root.glyph) {
            case "minimize":
                ctx.moveTo(0, 5)
                ctx.lineTo(10, 5)
                break
            case "maximize":
                ctx.rect(0.5, 0.5, 9, 9)
                break
            case "restore":
                ctx.rect(0.5, 2.5, 7, 7)
                ctx.moveTo(3, 2.5)
                ctx.lineTo(3, 0.5)
                ctx.lineTo(9.5, 0.5)
                ctx.lineTo(9.5, 7)
                ctx.lineTo(7.5, 7)
                break
            case "close":
                ctx.moveTo(0.5, 0.5)
                ctx.lineTo(9.5, 9.5)
                ctx.moveTo(9.5, 0.5)
                ctx.lineTo(0.5, 9.5)
                break
            case "menu":
                ctx.moveTo(0, 1)
                ctx.lineTo(10, 1)
                ctx.moveTo(0, 5)
                ctx.lineTo(10, 5)
                ctx.moveTo(0, 9)
                ctx.lineTo(10, 9)
                break
            }
            ctx.stroke()
        }
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: true
        onClicked: root.activate()
    }

    Keys.onSpacePressed: root.activate()
    Keys.onReturnPressed: root.activate()
    Keys.onEnterPressed: root.activate()

    ToolTip.visible: showToolTip && mouseArea.containsMouse
    ToolTip.delay: 600
    ToolTip.text: accessibleName
}
