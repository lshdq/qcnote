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
 *
 * 多光标（列式编辑）集成：
 * - 每编辑器一个 MultiCursorController（状态按文档独立保留）；
 * - Keys.onPressed 拦截控制器消费的按键；覆盖 MouseArea 拦截
 *   Alt+单击 / Alt+Shift+拖拽，其余事件一律放行（单光标零回归）；
 * - MultiCursorLayer 渲染额外光标与选区高亮（纯视觉）；
 * - active==false 时把原生光标 / 选区同步给引擎（触发键基准），
 *   接管期反向同步主光标并清原生选区（见下方防循环说明）；
 * - 状态栏光标数（cursorCount）随活动编辑器的控制器状态更新；
 *   接管期“已选 N 字符”改取各多光标选区长度之和（syncSelectionLength）。
 */
Item {
    id: root

    // 状态栏数据（由活动编辑器维护）
    property int cursorLine: 1
    property int cursorColumn: 1
    property int selectionLength: 0
    // 光标数：>1 时状态栏显示“光标 ×N”（多光标 / 列选激活时由控制器更新）
    property int cursorCount: 1

    signal newRequested()
    signal openRequested()

    readonly property var documentsModel: DocumentManager.documentsModel

    FontMetrics {
        id: monoMetrics
        font.family: AppTheme.fontFamilyMono
        font.pointSize: 11
    }

    Repeater {
        model: root.documentsModel

        delegate: Item {
            id: editorHolder

            required property Document document
            readonly property Document doc: editorHolder.document
            readonly property bool isActive: doc === DocumentManager.activeDocument
            /// 自同步标志：onTextChanged 回写 doc.content 期间为真，
            /// 用于 contentChanged 处理器区分“自身回写”与“外部重置”
            property bool selfContentSync: false

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

                    // ---- 多光标：每编辑器一个控制器实例 ----
                    MultiCursorController {
                        id: mc
                        target: editor
                        textDocument: editor.textDocument
                    }

                    // 覆盖层：额外光标 + 选区高亮（纯视觉，不消费事件）
                    MultiCursorLayer {
                        textEditor: editor
                        controller: mc
                    }

                    // 按键拦截：控制器消费触发键（Ctrl+D / Ctrl+Alt+方向等）
                    // 与接管期的全部编辑键；单光标未接管时恒返回 false，
                    // 事件照常交原生处理，行为零变化
                    Keys.onPressed: (event) => {
                        // 纯修饰键 / 锁定键按下不构成编辑命令，不经控制器：
                        // 否则其"未知命令退单光标"语义会在按住 Ctrl/Shift/Alt
                        // 准备组合键时错误退出多光标（如连续 Ctrl+D 丢光标）。
                        // 同时必须 accepted=true 接受事件：Windows 按住 Alt 会
                        // 自动重复产生系统键，未接受的事件交给 DefWindowProc
                        // 会触发 WM_CANCELMODE，Qt 随即取消所有鼠标抓取，
                        // 中断 Alt+Shift 列选拖拽（MouseArea 收到 onCanceled）；
                        // 按住 Ctrl 等其他修饰键时同理避免同类系统级干扰
                        switch (event.key) {
                        case Qt.Key_Control:
                        case Qt.Key_Shift:
                        case Qt.Key_Alt:
                        case Qt.Key_AltGr:
                        case Qt.Key_Meta:
                        case Qt.Key_CapsLock:
                        case Qt.Key_NumLock:
                        case Qt.Key_ScrollLock:
                            event.accepted = true
                            return
                        default:
                            break
                        }
                        if (mc.handleKey(event.key, event.modifiers, event.text))
                            event.accepted = true
                    }

                    // 修饰键释放对称接受：防止 Alt 释放路径经
                    // DefWindowProc 触发同类系统消息干扰鼠标抓取
                    Keys.onReleased: (event) => {
                        switch (event.key) {
                        case Qt.Key_Control:
                        case Qt.Key_Shift:
                        case Qt.Key_Alt:
                        case Qt.Key_AltGr:
                        case Qt.Key_Meta:
                        case Qt.Key_CapsLock:
                        case Qt.Key_NumLock:
                        case Qt.Key_ScrollLock:
                            event.accepted = true
                            return
                        default:
                            break
                        }
                    }

                    // 鼠标拦截：仅 Alt 系交互接管；其余一律 accepted=false
                    // 放行，原生单击定位 / 拖拽选择 / 滚动完全不变
                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton
                        // 与 TextArea 一致的光标形状（拖拽期间亦为文本光标）
                        cursorShape: Qt.IBeamCursor
                        // ScrollView 内部 Flickable 会在拖拽超过阈值（约 10px）
                        // 后经子事件过滤器抢夺子项鼠标抓取；原生 TextArea 选择
                        // 自带保持抓取不受影响，本覆盖层必须显式 preventStealing
                        // 才能全程持有拖拽（真机 Alt+Shift 列选拖拽缺陷的根因修复）
                        preventStealing: true

                        onPressed: (mouse) => {
                            const pos = editor.positionAt(mouse.x, mouse.y)
                            if ((mouse.modifiers & Qt.AltModifier)
                                    && (mouse.modifiers & Qt.ShiftModifier)) {
                                // Alt+Shift：矩形列选拖拽（锚 = 游标 = 按下点）
                                editor.forceActiveFocus()
                                mc.columnDragBegin(pos, pos)
                                mouse.accepted = true
                            } else if (mouse.modifiers & Qt.AltModifier) {
                                // Alt：单击加光标
                                editor.forceActiveFocus()
                                mc.altClick(pos)
                                mouse.accepted = true
                            } else {
                                // 普通单击：多光标时先退出，再放行原生
                                //（回到单击处单光标的原生语义）
                                if (mc.active)
                                    mc.reset()
                                mouse.accepted = false
                            }
                        }
                        onPositionChanged: (mouse) => {
                            if (mc.columnSelecting)
                                mc.columnDragUpdate(editor.positionAt(mouse.x, mouse.y))
                        }
                        onReleased: {
                            if (mc.columnSelecting)
                                mc.columnDragEnd()
                        }
                        onCanceled: {
                            // 拖拽被系统打断（如窗口失活）：结束列选，状态定格
                            if (mc.columnSelecting)
                                mc.columnDragEnd()
                        }
                    }

                    onTextChanged: {
                        if (editorHolder.doc && text !== editorHolder.doc.content) {
                            // 用户输入回写：置自同步标志，避免 contentChanged
                            // 被误判为外部重置（那会错误退出多光标）
                            editorHolder.selfContentSync = true
                            editorHolder.doc.content = text
                            editorHolder.selfContentSync = false
                        }
                        if (editorHolder.isActive)
                            syncStatus()
                    }

                    onCursorPositionChanged: syncStatus()
                    onSelectionStartChanged: syncStatus()
                    onSelectionEndChanged: syncStatus()

                    Connections {
                        target: mc

                        // 引擎状态统一通知（所有属性共用 stateChanged）：
                        // - 接管期（active 或列选拖拽中）：原生仅保留主光标
                        //   （排序最末）并清空原生选区，选区全部交给覆盖层渲染；
                        // - 未接管：只刷新状态栏数据（光标数；已选字符数
                        //   经 syncSelectionLength 走原生语义）。
                        // 注意：这里不调用 syncStatus——syncNativeCursor 会
                        // 无条件再发 stateChanged，经 syncStatus 回写会形成
                        // 信号死循环；行/列状态由 cursorPosition 赋值触发的
                        // onCursorPositionChanged → syncStatus 自然刷新。
                        function onStateChanged() {
                            if (mc.active || mc.columnSelecting) {
                                editor.deselect()
                                editor.cursorPosition = mc.primaryPosition
                            }
                            if (editorHolder.isActive) {
                                root.cursorCount = mc.active ? mc.cursorCount : 1
                                // 接管期原生选区已清空，“已选 N 字符”改取
                                // 各多光标选区长度之和（增删选区但主光标未动
                                // 时不会触发 syncStatus，须在此主动刷新）
                                editor.syncSelectionLength()
                            }
                        }
                    }

                    Connections {
                        target: editorHolder.doc
                        function onContentChanged() {
                            if (!editorHolder.doc || editorHolder.selfContentSync)
                                return // 自身回写：内容已一致，非外部重置
                            // 外部内容重置（重新加载/另存覆盖）：退出多光标。
                            // 注意不能只在 text 不一致分支里做：初始绑定
                            // text: doc.content 会先于本处理器把文本同步成
                            // 一致，走不到该分支
                            mc.reset()
                            if (editor.text !== editorHolder.doc.content) {
                                editor.text = editorHolder.doc.content
                                editor.cursorPosition = Math.min(editor.cursorPosition, editor.length)
                            }
                            editor.syncStatus()
                        }
                    }

                    // 状态栏“已选 N 字符”数据源：
                    // - 接管期（active / 列选拖拽中）：原生选区已被上方
                    //   editor.deselect() 清空，改取控制器各选区长度之和
                    //  （覆盖层渲染的即这些选区）；
                    // - 非接管期：保持原生 selectionEnd - selectionStart 语义。
                    // selectionRanges 经 QML 引擎边界可能呈嵌套 [[s,e],...]
                    // 或展平 [s,e,s,e,...] 两种形态（同 MultiCursorLayer
                    // .selectionPairs 的兼容解析）
                    function syncSelectionLength() {
                        if (mc.active || mc.columnSelecting) {
                            let total = 0
                            const raw = mc.selectionRanges
                            if (raw.length > 0 && raw[0] instanceof Array) {
                                for (let i = 0; i < raw.length; ++i)
                                    total += raw[i][1] - raw[i][0]
                            } else {
                                for (let i = 0; i + 1 < raw.length; i += 2)
                                    total += raw[i + 1] - raw[i]
                            }
                            root.selectionLength = total
                        } else {
                            root.selectionLength = editor.selectionEnd - editor.selectionStart
                        }
                    }

                    function syncStatus() {
                        if (!editorHolder.isActive)
                            return
                        // 引擎未接管时：把原生光标/选区同步给控制器，
                        // 作为触发键（Ctrl+D 等）的状态基准；接管期/列选中
                        // 不回写（引擎为准），也避免与 stateChanged 互发循环
                        if (!mc.active && !mc.columnSelecting) {
                            const selStart = editor.selectionStart
                            const selEnd = editor.selectionEnd
                            if (selEnd > selStart) {
                                // 锚点放在光标所在选区的另一端，保留选择方向
                                const anchor = editor.cursorPosition === selStart
                                               ? selEnd : selStart
                                mc.syncNativeCursor(editor.cursorPosition, anchor)
                            } else {
                                mc.syncNativeCursor(editor.cursorPosition, editor.cursorPosition)
                            }
                        }
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
                        syncSelectionLength()
                        root.cursorCount = mc.active ? mc.cursorCount : 1
                    }
                }
            }
        }
    }

    WelcomePage {
        anchors.fill: parent
        visible: root.documentsModel.count === 0
        onNewRequested: root.newRequested()
        onOpenRequested: root.openRequested()
    }
}
