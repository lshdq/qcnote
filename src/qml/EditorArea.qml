import QtQuick
import QtQuick.Controls

/**
 * 内容区：每文档一个 TextArea（Repeater，仅活动项可见、不销毁），
 * 光标 / 滚动 / 未保存内容在标签切换时全部保留；
 * 无文档时显示欢迎页。
 *
 * 内容同步：
 * - 初始与外部变化（doc.contentChanged）→ 写入 TextArea；
 * - 用户输入 → 写回 doc.content（值相等判断防回写环）。
 */
Item {
    id: root

    // 状态栏数据（由活动编辑器维护）
    property int cursorLine: 1
    property int cursorColumn: 1
    property int selectionLength: 0

    signal newRequested()
    signal openRequested()

    readonly property var documents: DocumentManager.documents

    FontMetrics {
        id: monoMetrics
        font.family: AppTheme.fontFamilyMono
        font.pointSize: 11
    }

    Repeater {
        model: root.documents

        delegate: Item {
            id: editorHolder

            readonly property Document doc: modelData
            readonly property bool isActive: doc === DocumentManager.activeDocument

            visible: isActive
            anchors.fill: parent

            onIsActiveChanged: {
                if (isActive) {
                    editor.forceActiveFocus()
                    editor.syncStatus()
                }
            }

            ScrollView {
                id: editorScroll
                anchors.fill: parent

                TextArea {
                    id: editor

                    textFormat: TextEdit.PlainText
                    wrapMode: TextEdit.NoWrap
                    font.family: AppTheme.fontFamilyMono
                    font.pointSize: 11
                    // Tab 宽度 = 4 个等宽字符
                    tabStopDistance: monoMetrics.advanceWidth("0000")

                    background: Rectangle {
                        color: AppTheme.contentBackground
                    }

                    // 初始绑定：delegate 创建时取当前内容
                    text: editorHolder.doc ? editorHolder.doc.content : ""

                    onTextChanged: {
                        if (editorHolder.doc && text !== editorHolder.doc.content)
                            editorHolder.doc.content = text
                        if (editorHolder.isActive)
                            syncStatus()
                    }

                    onCursorPositionChanged: syncStatus()
                    onSelectionStartChanged: syncStatus()
                    onSelectionEndChanged: syncStatus()

                    Connections {
                        target: editorHolder.doc
                        function onContentChanged() {
                            if (!editorHolder.doc)
                                return
                            if (editor.text !== editorHolder.doc.content) {
                                editor.text = editorHolder.doc.content
                                editor.cursorPosition = Math.min(editor.cursorPosition, editor.length)
                                syncStatus()
                            }
                        }
                    }

                    function syncStatus() {
                        if (!editorHolder.isActive)
                            return
                        const pos = editor.cursorPosition
                        const before = editor.text.substring(0, pos)
                        let line = 1
                        for (let i = 0; i < before.length; ++i) {
                            if (before[i] === "\n")
                                ++line
                        }
                        const lastNl = before.lastIndexOf("\n")
                        root.cursorLine = line
                        // 列号 1 起：pos - (lastNl + 1) + 1
                        root.cursorColumn = pos - lastNl
                        root.selectionLength = editor.selectionEnd - editor.selectionStart
                    }
                }
            }
        }
    }

    WelcomePage {
        anchors.fill: parent
        visible: !root.documents || root.documents.length === 0
        onNewRequested: root.newRequested()
        onOpenRequested: root.openRequested()
    }
}
