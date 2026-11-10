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
 *   双击最大化/还原；最大化时拖拽先还原窗口（标题栏吸附光标下）再继续系统拖拽；
 * - 菜单项快捷键通过自绘右对齐文本显示（Qt Quick Controls 2 的 MenuItem
 *   无 shortcut 属性，按键由窗口级 Shortcut 统一处理）；
 * - 主菜单分组：文件（含编码三级级联子菜单）→ 搜索（查找/替换/上一个/
 *   下一个，仅转发既有动作）→ 视图 → 退出。
 */
Rectangle {
    id: root

    required property var window

    signal requestNew()
    signal requestOpen()
    signal requestSave()
    signal requestSaveAs()
    signal requestCloseTab(var doc)
    /// 批量关闭标签（docs 为目标文档数组，anchorDoc 为完成后需激活的
    /// 锚点文档，即被右键的标签）
    signal requestCloseRange(var docs, var anchorDoc)
    signal requestQuit()
    /// 按指定编码重新加载（参数为 Document.Encoding 枚举值）
    signal requestReloadWithEncoding(int encoding)
    /// 按指定编码保存（编码转换；参数为 Document.Encoding 枚举值）
    signal requestSaveWithEncoding(int encoding)
    /// 打开查找对话框（同 Ctrl+F）
    signal requestFind()
    /// 打开替换对话框（同 Ctrl+H）
    signal requestReplace()
    /// 跳转下一个匹配（对话框关闭态 F3 语义）
    signal requestFindNext()
    /// 跳转上一个匹配（对话框关闭态 Shift+F3 语义）
    signal requestFindPrev()

    /// 自绘右对齐快捷键文本的菜单项：Qt Quick Controls 2 的 MenuItem 无
    /// shortcut 属性，按键由窗口级 Shortcut 统一处理，这里仅显示文本。
    /// 提升至根作用域，供主菜单与各子菜单复用。
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

    readonly property int barHeight: 40

    /// 主菜单或标签右键菜单是否打开（供 Main 屏蔽快捷键）；
    /// documentTabs 的 id 引用与声明顺序无关，绑定可用
    readonly property bool menuVisible: mainMenu.visible
                                       || documentTabs.contextMenuVisible

    color: AppTheme.titleBarBackground
    implicitHeight: barHeight

    // 空白区：拖拽移动 + 双击最大化/还原（先于按钮声明，置于下层）；
    // 最大化时拖拽：先还原并让标题栏吸附到光标下，再继续系统拖拽
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
        // 移动超过阈值才启动系统拖拽：纯点击不启动，双击手势不受影响；
        // 最大化时拖拽：先还原并让标题栏吸附到光标下（水平按按下点比例），再继续系统拖拽
        onPositionChanged: (mouse) => {
            if (!pressed)
                return
            if (Math.abs(mouse.x - pressPos.x) <= 4 && Math.abs(mouse.y - pressPos.y) <= 4)
                return
            if (maximized) {
                const globalPos = mapToGlobal(mouse.x, mouse.y)
                const ratio = pressPos.x / root.width
                root.window.showNormal()
                root.window.x = Math.round(globalPos.x - ratio * root.window.width)
                root.window.y = Math.round(globalPos.y - pressPos.y)
            }
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

        // 主菜单分组结构：文件（含编码子菜单）→ 搜索 → 视图 → 退出。
        // 各子菜单条目仅转发既有动作，快捷键仍由 Main.qml 窗口级 Shortcut 统一处理。

        Menu {
            id: fileMenu
            title: "文件"

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

            // 编码子菜单（GBK 与 UTF-8 编码支持）：与文件操作同组。
            // 「通过编码重新加载」对未命名文档（无磁盘内容可重载）整组
            // 禁用（TC-017）；「通过编码保存」为一步完成编码转换（DEC-005），
            // 当前文档编码对应项打勾，未命名文档由 Main 转既有另存为流程。
            Menu {
                id: encodingMenu
                title: "编码"

                Menu {
                    id: reloadEncodingMenu
                    title: "通过编码重新加载"
                    enabled: DocumentManager.activeDocument !== null
                             && DocumentManager.activeDocument.url.toString().length > 0

                    MenuItem {
                        text: "UTF-8"
                        onTriggered: root.requestReloadWithEncoding(Document.Encoding.Utf8)
                    }
                    MenuItem {
                        text: "GBK"
                        onTriggered: root.requestReloadWithEncoding(Document.Encoding.Gbk)
                    }
                }

                Menu {
                    id: saveEncodingMenu
                    title: "通过编码保存"
                    enabled: DocumentManager.activeDocument !== null

                    MenuItem {
                        text: "UTF-8"
                        checkable: true
                        checked: DocumentManager.activeDocument
                                 && DocumentManager.activeDocument.encoding === Document.Encoding.Utf8
                        onTriggered: root.requestSaveWithEncoding(Document.Encoding.Utf8)
                    }
                    MenuItem {
                        text: "GBK"
                        checkable: true
                        checked: DocumentManager.activeDocument
                                 && DocumentManager.activeDocument.encoding === Document.Encoding.Gbk
                        onTriggered: root.requestSaveWithEncoding(Document.Encoding.Gbk)
                    }
                }
            }
        }

        // 搜索子菜单：仅转发既有动作，快捷键仍由 Main.qml 窗口级 Shortcut
        // 统一处理（不新增按键绑定）。「下一个/上一个」为对话框关闭态 F3
        // 语义：沿用上次查找参数与控制器保留的匹配状态，不恢复全部高亮，
        // 无匹配时无操作。查找对话框为模态、打开期间菜单不可达，故无需
        // 处理对话框打开分支。四项均要求有活动文档，欢迎页时置灰。
        Menu {
            id: searchMenu
            title: "搜索"

            KeyMenuItem {
                text: "查找…"
                keyText: "Ctrl+F"
                enabled: DocumentManager.activeDocument !== null
                onTriggered: root.requestFind()
            }
            KeyMenuItem {
                text: "替换…"
                keyText: "Ctrl+H"
                enabled: DocumentManager.activeDocument !== null
                onTriggered: root.requestReplace()
            }
            KeyMenuItem {
                text: "下一个"
                keyText: "F3"
                enabled: DocumentManager.activeDocument !== null
                onTriggered: root.requestFindNext()
            }
            KeyMenuItem {
                text: "上一个"
                keyText: "Shift+F3"
                enabled: DocumentManager.activeDocument !== null
                onTriggered: root.requestFindPrev()
            }
        }

        MenuSeparator {}

        Menu {
            id: viewMenu
            title: "视图"

            MenuItem {
                text: "深色主题"
                checkable: true
                checked: AppTheme.dark
                onTriggered: AppTheme.dark = !AppTheme.dark
            }

            // 自动换行：全局编辑器设置（SessionManager 跨会话持久化），
            // 与工具栏右侧切换按钮共用同一数据源，两处状态始终一致
            MenuItem {
                text: "自动换行"
                checkable: true
                checked: SessionManager.wordWrap
                onTriggered: SessionManager.wordWrap = !SessionManager.wordWrap
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
        // 贴底：标签底边与标题栏底边（= 工具栏顶边）重合，活动标签与工具栏连通
        anchors.bottom: parent.bottom
        height: 34

        onCloseRequested: (doc) => root.requestCloseTab(doc)
        onCloseRangeRequested: (docs, anchor) => root.requestCloseRange(docs, anchor)
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
