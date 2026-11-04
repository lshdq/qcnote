import QtQuick
import QtQuick.Controls

/**
 * Win11 风格对话框按钮（供 DialogButtonBox 使用，配合 buttonRole 附加属性接线）。
 *
 * - primary=true：默认按钮，AppTheme.accent 填充底 + onAccentForeground 白字；
 * - primary=false：次级按钮，透明底 + 1px AppTheme.border 描边 + textPrimary 文字；
 * - 悬停 / 按下用半透明叠加层（AppTheme.hover / pressed）表达，两类按钮通用；
 * - 禁用态文字取 textDisabled，去描边且无交互反馈；
 * - 键盘焦点以形状线索表达（非仅颜色）：次级按钮描边加粗为 2px AppTheme.focus，
 *   默认按钮平时无描边，焦点态用外圈焦点环；
 * - 统一尺寸：高 32、最小宽 96（水平 padding 16），圆角 4；
 * - 聚焦时按 Enter / Return 触发 click()（补齐平台主题下 Enter 不激活按钮的缺口）。
 * 颜色一律取 AppTheme 语义 token，禁止硬编码色值。
 */
Button {
    id: root

    /// 是否默认按钮（accent 填充）
    property bool primary: false

    hoverEnabled: true

    // 背景：Qt 6.11 平台主题的 ButtonPressKeys 默认仅 {Space, Select}，按钮持焦点时按
    // Enter 不会激活。这里在组件内补齐 Windows 对话框惯例——回车激活当前聚焦按钮；
    // auto-repeat 只消费事件不重复触发。
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            if (!event.isAutoRepeat)
                root.click()
            event.accepted = true
        }
    }

    implicitHeight: 32
    implicitWidth: Math.max(96, contentItem.implicitWidth + leftPadding + rightPadding)
    leftPadding: 16
    rightPadding: 16

    contentItem: Text {
        text: root.text
        font: root.font
        color: !root.enabled ? AppTheme.textDisabled
                             : root.primary ? AppTheme.onAccentForeground
                                            : AppTheme.textPrimary
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        id: bg

        radius: 4
        color: root.primary ? AppTheme.accent : "transparent"
        // 次级按钮描边：焦点态加粗换 focus 色；默认按钮 / 禁用态不描边
        border.width: !root.enabled || root.primary ? 0
                                                   : root.visualFocus ? 2 : 1
        border.color: root.visualFocus ? AppTheme.focus : AppTheme.border

        // 悬停 / 按下半透明叠加层（默认与次级按钮通用）
        Rectangle {
            anchors.fill: parent
            radius: bg.radius
            color: !root.enabled ? "transparent"
                    : root.pressed ? AppTheme.pressed
                    : root.hovered ? AppTheme.hover
                    : "transparent"
        }

        // 默认按钮无描边，键盘焦点用外圈焦点环表达（非颜色唯一线索）
        Rectangle {
            visible: root.enabled && root.primary && root.visualFocus
            anchors.centerIn: parent
            width: bg.width + 4
            height: bg.height + 4
            radius: bg.radius + 2
            color: "transparent"
            border.color: AppTheme.focus
            border.width: 2
        }
    }
}
