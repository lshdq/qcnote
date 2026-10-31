import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

/**
 * 底部状态栏：光标行/列、选中字符数、光标数（多光标时）、总字符数、
 * 编码、换行风格、文件路径。
 * 路径超长省略显示，悬停 tooltip 显示完整路径。
 */
Rectangle {
    id: root

    property var doc: null
    property int cursorLine: 1
    property int cursorColumn: 1
    property int selectionLength: 0
    /// 光标数（多光标激活时 > 1）
    property int cursorCount: 1

    color: AppTheme.statusBarBackground
    implicitHeight: 28

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 1
        color: AppTheme.border
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 16

        Label {
            font.pointSize: 9
            color: AppTheme.textSecondary
            text: root.doc ? ("行 " + root.cursorLine + "，列 " + root.cursorColumn) : "行 —，列 —"
        }

        Label {
            visible: root.doc && root.selectionLength > 0
            font.pointSize: 9
            color: AppTheme.textSecondary
            text: "已选 " + root.selectionLength + " 字符"
        }

        Label {
            visible: root.doc && root.cursorCount > 1
            font.pointSize: 9
            color: AppTheme.textSecondary
            text: "光标 ×" + root.cursorCount
        }

        Label {
            visible: root.doc !== null
            font.pointSize: 9
            color: AppTheme.textSecondary
            text: "共 " + (root.doc ? root.doc.content.length : 0) + " 字符"
        }

        Item { Layout.fillWidth: true }

        Label {
            visible: root.doc !== null
            font.pointSize: 9
            color: AppTheme.textSecondary
            text: "UTF-8"
        }

        Label {
            visible: root.doc !== null
            font.pointSize: 9
            color: AppTheme.textSecondary
            text: root.doc
                   ? (root.doc.lineEnding === Document.LineEnding.Crlf ? "CRLF" : "LF")
                   : "—"
        }

        Label {
            id: pathLabel
            visible: root.doc !== null
            font.pointSize: 9
            color: AppTheme.textSecondary
            elide: Text.ElideMiddle
            Layout.maximumWidth: 320
            text: root.doc ? root.doc.filePath : ""

            HoverHandler {
                id: pathHover
                enabled: pathLabel.visible
            }

            ToolTip.visible: root.doc !== null && pathHover.hovered && pathLabel.text !== ""
            ToolTip.delay: 400
            ToolTip.text: root.doc ? root.doc.filePath : ""
        }
    }
}
