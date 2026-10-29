import QtQuick
import QtQuick.Controls

/**
 * 欢迎空状态页：无文档时显示。
 * 提供新建 / 打开入口与快捷键提示（非颜色线索 + 文字说明）。
 */
Item {
    id: root

    signal newRequested()
    signal openRequested()

    Rectangle {
        anchors.fill: parent
        color: AppTheme.contentBackground

        Column {
            anchors.centerIn: parent
            spacing: 20

            Text {
                text: "qcnote"
                font.pixelSize: 44
                font.bold: true
                color: AppTheme.textPrimary
                anchors.horizontalCenter: parent.horizontalCenter
            }

            Text {
                text: "轻量纯文本编辑器"
                font.pixelSize: 14
                color: AppTheme.textSecondary
                anchors.horizontalCenter: parent.horizontalCenter
            }

            Row {
                spacing: 12
                anchors.horizontalCenter: parent.horizontalCenter

                Button {
                    text: "新建文档"
                    onClicked: root.newRequested()
                }

                Button {
                    text: "打开文件…"
                    onClicked: root.openRequested()
                }
            }

            Text {
                text: "Ctrl+N 新建　Ctrl+O 打开　Ctrl+S 保存　Ctrl+W 关闭标签页"
                font.pixelSize: 12
                color: AppTheme.textSecondary
                anchors.horizontalCenter: parent.horizontalCenter
            }
        }
    }
}
