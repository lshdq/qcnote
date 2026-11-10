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
 * 光标同步（会话恢复数据源，与内容同步同模式）：
 * - delegate 创建与外部变化（doc.cursorPositionChanged）→ 应用到 TextArea
 *   并滚动可见（selfCursorSync 防环，防止下行应用被误判为用户输入回写）；
 * - 用户移动光标 → 回写 doc.cursorPosition（Document 同值设置不发信号，
 *   环自然终止）。
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
 *
 * 查找替换集成（见 SearchDialog / SearchController / SearchHighlightLayer）：
 * - 会话级 SearchController 单实例：target/textDocument 绑活动编辑器
 *   （activeEditor 由活动 delegate 维护），活动文档切换时重绑 → 控制器
 *   对新文档 restartSearch 重算（旧文档匹配区间不残留、索引归零），查找
 *   串/选项保留（会话内）；查找参数与匹配状态的唯一持有者是控制器，
 *   对话框只做输入与展示；
 * - 每编辑器一个 SearchHighlightLayer（TextArea 子 Item，与
 *   MultiCursorLayer 同位）：仅活动编辑器实例激活，可见性由 searchActive
 *   控制（对话框关闭即隐藏，DEC-009：不清控制器匹配状态，F3 可继续跳转）；
 * - 跳转滚动：监听控制器 stateChanged，currentIndex 变化时对活动编辑器
 *   调 scrollCursorIntoView（findNext/findPrev 已先把当前匹配设为原生
 *   选区；此处只读滚动，不改任何状态，无循环）；
 * - 多光标互斥（DEC-008）：对话框打开前经 exitMultiCursor() 退出活动
 *   编辑器的多光标。
 */
Item {
    id: root

    // 状态栏数据（由活动编辑器维护）
    property int cursorLine: 1
    property int cursorColumn: 1
    property int selectionLength: 0
    // 光标数：>1 时状态栏显示“光标 ×N”（多光标 / 列选激活时由控制器更新）
    property int cursorCount: 1

    // ---- 查找替换集成状态 ----
    /// 查找功能是否激活（对话框打开期间为真）：各编辑器的
    /// SearchHighlightLayer 以 searchActive && isActive 决定是否渲染匹配
    /// 高亮；Main.qml 在打开时置真、对话框 rejected 时置假（DEC-009）
    property bool searchActive: false
    /// 活动编辑器（活动 delegate 维护；查找控制器 target/textDocument、
    /// 选区预填读取、关闭后回焦均经此引用；delegate 销毁时置空）
    property TextArea activeEditor: null
    /// 活动编辑器的多光标控制器（exitMultiCursor 用，随 activeEditor 维护）
    property MultiCursorController activeMc: null
    /// 查找跳转滚动去重：上次已滚动到的 currentIndex（-2 保证首个匹配
    /// （索引 0）也会触发一次滚动）
    property int lastSearchScrollIndex: -2

    signal newRequested()
    signal openRequested()

    readonly property var documentsModel: DocumentManager.documentsModel

    // ---- 查找控制器（会话级单实例） ----
    // target/textDocument 绑活动编辑器：活动文档切换时重绑 → 控制器
    // setTextDocument 对新文档 restartSearch 重算（旧文档区间不残留、
    // 索引归零），查找串/替换串/选项保留（会话内，需求）。
    // 查找参数变更即重算并归零索引；文档 contentsChange（外部键入/撤销/
    // 重置，及本控制器替换编辑之外的修改）由控制器重算并钳制索引。
    SearchController {
        id: searchCtl
        target: root.activeEditor
        textDocument: root.activeEditor ? root.activeEditor.textDocument : null
    }

    /// 查找控制器（Main.qml 查找对话框的参数/状态/动作入口）
    readonly property alias searchController: searchCtl

    // 跳转滚动：控制器把当前匹配经 select 设为原生选区（findNext/
    // findPrev/replaceCurrent 内先 select 后发 stateChanged），光标随选区
    // 落到匹配末端；此处监听 stateChanged 在 currentIndex 变化时对活动
    // 编辑器滚动到可见。只读滚动（不改匹配/选区/文档状态），无循环；
    // Qt.callLater 等本轮 polish 布局稳定后执行（与既有滚动调用同时序）
    Connections {
        target: searchCtl

        function onStateChanged() {
            const idx = searchCtl.currentIndex
            if (idx < 0 || idx === root.lastSearchScrollIndex)
                return
            root.lastSearchScrollIndex = idx
            if (root.activeEditor) {
                Qt.callLater(function() {
                    // 活动编辑器或索引在此期间又变化时交给后续通知处理
                    if (root.activeEditor && searchCtl.currentIndex === idx)
                        root.activeEditor.scrollCursorIntoView()
                })
            }
        }
    }

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
            /// 光标自同步标志：doc.cursorPositionChanged → editor 应用期间为真，
            /// 用于 editor.onCursorPositionChanged 区分“下行应用”与“用户输入回写”
            property bool selfCursorSync: false
            /// 原生拖拽守卫：编辑器按下期间为真。用于按下期间禁用 ScrollView
            /// 内部 Flickable 的 interactive，阻止其在 TextArea 延迟按下窗口内
            /// 抢夺鼠标抓取（竖直先行拖拽选区失效的根因，见 docs/已知问题）
            property bool nativeDragGuard: false
            /// 原生拖拽锚点：左键按下时的文档位置（onPressed 记录）。
            /// Qt 的 QQuickPressHandler 延迟按下机制下 delayed press 可能被
            /// 800ms 定时器销毁而永不补放，锚点必须在按下时自持，由下方
            /// PointHandler 被动驱动选区扩展（见 onPressed / PointHandler 注释）
            property int nativeDragAnchor: -1
            /// 原生拖拽终点：PointHandler 最后一次点更新的文档位置。
            /// 释放时原生 replay 会清空选区（延迟按下窗口内 release 先补放
            /// press 再放 release），onReleased 据此延迟一拍重放选区
            property int nativeDragLastPos: -1
            /// 拖拽期间最近鼠标位置（TextArea 局部坐标，<0 为无效）：
            /// onPressed 记录、PointHandler.onPointChanged 更新（active 过滤
            /// 内，防释放时 point 归零 (0,0) 污染判定）。钳制检测的输入
            property real dragMouseLocalX: -1
            property real dragMouseLocalY: -1
            /// 横向抑制回滚基准：最近一次"认可的"contentX。合法滚动（右缘
            /// 前瞻爬行）每次认可后更新基准，钳制跳滚以此值回滚——保留拖拽
            /// 前段右缘爬行的合法滚动进度，仅消除钳制引发的单次跳变
            /// （DEC-002：基准取"变化前认可值"而非按下时快照）
            property real dragContentXBase: 0
            /// 横向抑制窗口标志：从左键按下起，到释放后选区重放
            /// （Qt.callLater）完成之后一拍止——比 nativeDragGuard 多存活
            /// 一拍，覆盖"释放时原生补放/重放把光标设到钳制端点"仍触发
            /// ensureCursorVisible 的窗口（DEC-003）；失焦兜底同步复位，
            /// 防标志卡死导致横向滚动长期失效
            property bool hDragScrollGuard: false

            /// 钳制检测：判定"本次光标位置是否来自命中钳制"。
            /// 拖拽期间鼠标位于末行以下/行尾右侧空白时，positionAt/hitTest
            /// （FuzzyHit）把光标钳到行尾/文档末尾——NoWrap 长行末尾远在
            /// 视口右侧，光标矩形与最近鼠标位置（同为 TextArea 局部坐标）
            /// 严重分离；而须保留的右缘前瞻爬行中光标始终紧邻鼠标。以容差
            /// （横向 2 字符宽、纵向约 0.3 行高（≥3px））四向判定二者分离即认定钳制。
            /// 纵向容差取小值：hitTest 的 Y 向行归属判定是精确行界（Fuzzy 仅
            /// 作用于 X），真实命中的纵向偏差仅为行距 leading（约 3~4px）；
            /// 整行高容差会在末行以下留出约一行高的漏检带（带内钳制跳滚不被检测）。
            /// 鼠标位置无效（<0，未拖拽/已复位）时保守放行（不抑制）
            function contentXFromClampScroll() {
                if (editorHolder.dragMouseLocalX < 0 || editorHolder.dragMouseLocalY < 0)
                    return false
                const rect = editor.cursorRectangle
                const tolX = monoMetrics.advanceWidth("00")
                const tolY = Math.max(3, monoMetrics.height * 0.3)
                return editorHolder.dragMouseLocalX < rect.x - tolX
                        || editorHolder.dragMouseLocalX > rect.x + rect.width + tolX
                        || editorHolder.dragMouseLocalY < rect.y - tolY
                        || editorHolder.dragMouseLocalY > rect.y + rect.height + tolY
            }

            visible: isActive
            anchors.fill: parent

            // 活动编辑器引用上移到 EditorArea 根（查找控制器绑定源）；
            // 失活/销毁时若仍指向自己则置空（确定性清理，不依赖引擎
            // 对已销毁对象的置空行为）
            onIsActiveChanged: {
                if (isActive) {
                    root.activeEditor = editor
                    root.activeMc = mc
                    editor.forceActiveFocus()
                    editor.syncStatus()
                } else {
                    if (root.activeEditor === editor)
                        root.activeEditor = null
                    if (root.activeMc === mc)
                        root.activeMc = null
                }
            }

            Component.onDestruction: {
                if (root.activeEditor === editor)
                    root.activeEditor = null
                if (root.activeMc === mc)
                    root.activeMc = null
            }

            // 初始光标：delegate 创建时 doc.cursorPosition 可能已非 0
            // （如会话恢复路径 restoreDocuments 先 openUrls 后写光标，
            // 创建早于写光标时由下方 Connections 兜底，两路径互补）；
            // 钳制到内容长度，防止越界
            Component.onCompleted: {
                editor.cursorPosition = Math.min(editorHolder.doc.cursorPosition, editor.length)
                // 延迟一拍再滚动：创建期 cursorRectangle / 视口高度尚未经首次
                // polish / 布局最终化（height=0 时 scrollCursorIntoView 的分支
                // 计算失真），Qt.callLater 等布局稳定后执行；Qt 原生 attach
                // 滚动为兜底，此处消除对兜底的时序依赖
                Qt.callLater(editor.scrollCursorIntoView)
            }

            ScrollView {
                id: editorScroll
                anchors.fill: parent

                // 滚动条常驻显示：Material 默认 AsNeeded 为 transient 指示条（交互结束
                // 约 2.45s 后滑块淡出为 0，风格状态机：active 仅当
                // policy===AlwaysOn || active&&size<1.0）。必须用链式写法
                // （ScrollBar.vertical.policy: ...）仅覆盖属性——若写成
                // ScrollBar.vertical: ScrollBar { ... } 整体替换，风格 ScrollView.qml
                // 的定位绑定（parent/x/y/height）随之丢失，滚动条退化 0 尺寸完全
                // 不可见（Qt 6 官方 ScrollView 文档 "Scroll Bars" 章写法）。size > 0
                // 下限：未布局/空 delegate 时 size==0 按隐藏处理，避免短文档首帧闪现
                ScrollBar.vertical.interactive: true
                ScrollBar.vertical.policy: root.residentScrollBarPolicy(editorScroll.ScrollBar.vertical.size)
                ScrollBar.horizontal.interactive: true
                ScrollBar.horizontal.policy: root.residentScrollBarPolicy(editorScroll.ScrollBar.horizontal.size)

                // 滚轮到顶/底（及 NoWrap 长行水平到左/右）不做过冲回弹：
                // ScrollView 内部 Flickable 默认 DragAndOvershootBounds，滚轮越过
                // 边界会过冲再回弹；桌面编辑器应停在边界（与 DocumentTabs 标签栏
                // 的 boundsBehavior: Flickable.StopAtBounds 同款修复）。内部
                // Flickable 只能经 contentItem 访问（见下方 scrollCursorIntoView
                // 注释，Qt 6.11.2 实测其为 Flickable）；用 Binding 而非一次性
                // 赋值，contentItem 变化时自动重挂
                Binding {
                    target: editorScroll.contentItem
                    property: "boundsBehavior"
                    value: Flickable.StopAtBounds
                }

                // 按下期间禁用内部 Flickable 交互：阻断其子事件过滤器抢夺鼠标
                // （TextArea 的 QQuickPressHandler 延迟按下机制下 keepMouseGrab
                // 未及时设置，竖直先行拖拽会被抢为视图滚动）。代价：编辑区内
                // "按住拖动=滚动视图"的触摸式交互被禁用（桌面编辑器惯例，滚轮
                // 与滚动条不受影响）
                Binding {
                    target: editorScroll.contentItem
                    property: "interactive"
                    value: !editorHolder.nativeDragGuard
                }

                TextArea {
                    id: editor

                    textFormat: TextEdit.PlainText
                    // 自动换行：跟随全局设置 SessionManager.wordWrap（所有文档共享，
                    // 菜单与工具栏切换）；开启时优先在单词边界断行，超长单词内强制
                    // 断行（WrapAtWordBoundaryOrAnywhere）；关闭时 NoWrap，与原
                    // 行为一致（长行水平滚动）
                    wrapMode: SessionManager.wordWrap ? TextEdit.WrapAtWordBoundaryOrAnywhere : TextEdit.NoWrap
                    // 切换换行开关后的两个动作：
                    // 1) 命令式重放 QQuickTextArea 私有内建的 resizeFlickableControl
                    //    宽度逻辑。TextArea 进入 ScrollView 时被 Qt 自动
                    //    attachFlickable，此后宽度由 Qt 以命令式 setSize 管理
                    //    （任何 QML width 绑定都会被其覆盖，实测确认），内建
                    //    逻辑仅在 Flickable 内容宽/高变化时重跑；NoWrap→Wrap
                    //    切换时 implicitWidth 已是自然宽（内容宽），内容宽不变、
                    //    级联不触发，宽度停在数千像素，文本布局宽度不收缩 →
                    //    长行不折行（P0 缺陷根因）。此处按新 wrapMode 手动重放
                    //    同款计算：wrap 态收窄到视口宽触发重排折行，NoWrap 态
                    //    取 max(视口宽, 内容宽) 保持水平滚动（与
                    //    resizeFlickableControl 的宽度公式同构：wrap 态=视口宽，
                    //    NoWrap 态=max(视口宽, 内容宽)）。切换后由内建级联
                    //    自然接管，终值与“创建即 wrap”路径一致（实测存在
                    //    视口宽 17px 滚动条瞬态差，终值收敛依赖内建级联）。
                    // 2) contentHeight 骤变（光标上方超长行折为多个可视行），
                    //    Flickable 仅做边界钳制、不跟随光标，会失去光标位置
                    //    参照；延迟一拍再滚动回视口：wrapMode 变化后
                    //    cursorRectangle / 布局需经一次 polish 才最终化，与
                    //    Component.onCompleted 的 Qt.callLater 同一时序理由。
                    onWrapModeChanged: {
                        const flick = editorScroll.contentItem
                        width = wrapMode === TextEdit.NoWrap
                                ? Math.max(editorScroll.width, flick ? flick.contentWidth : 0)
                                : editorScroll.width
                        Qt.callLater(editor.scrollCursorIntoView)
                    }
                    font.family: AppTheme.fontFamilyMono
                    font.pointSize: 11
                    // Tab 宽度 = 4 个等宽字符
                    tabStopDistance: monoMetrics.advanceWidth("0000")
                    color: AppTheme.textPrimary
                    // 选区：半透明强调蓝（与多光标选区同色）；文字保持原色，避免默认白字在半透明底上对比不足
                    selectionColor: AppTheme.selection
                    selectedTextColor: AppTheme.textPrimary

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

                    // 搜索匹配高亮覆盖层（纯视觉，不消费事件）：全部匹配
                    // 半透明底色 + 当前匹配描边（DEC-007 形状线索）。数据来
                    // 自 EditorArea 会话级查找控制器；仅活动编辑器实例激活，
                    // 可见性由 searchActive 控制（对话框关闭即隐藏，DEC-009）。
                    // overflowed 时不再传入全部区间（DEC-004：全部底色停止
                    // 绘制；当前匹配描边仅 1 段，开销可忽略，保留）
                    SearchHighlightLayer {
                        textEditor: editor
                        matchRanges: searchCtl.overflowed ? [] : searchCtl.matchRanges
                        currentRange: searchCtl.currentRange
                        active: root.searchActive && editorHolder.isActive
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
                    // 放行，原生单击定位 / 拖拽选择 / 滚动完全不变。
                    // Alt+Shift 列选拖拽为坐标驱动：MouseArea 与 TextArea
                    // 同坐标系（anchors.fill: parent），mouse.x/y 原样交
                    // 控制器，由其经真实布局逐行视觉命中（中文/全角宽度、
                    // 短行钳制均由布局保证，QML 层不做位置换算）
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
                            if ((mouse.modifiers & Qt.AltModifier)
                                    && (mouse.modifiers & Qt.ShiftModifier)) {
                                // Alt+Shift：视觉列选拖拽（锚 = 游标 = 按下点）
                                editor.forceActiveFocus()
                                mc.columnDragBeginAt(mouse.x, mouse.y)
                                mouse.accepted = true
                            } else if (mouse.modifiers & Qt.AltModifier) {
                                // Alt：单击加光标
                                const pos = editor.positionAt(mouse.x, mouse.y)
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
                                mc.columnDragUpdateAt(mouse.x, mouse.y)
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

                    // 原生拖拽的被动驱动：QQuickPressHandler 的 800ms 延迟定时器只被
                    // 横向位移取消，竖直拖拽超时后保存的 press 被销毁、control 侧
                    // mousePressed 恒 false，原生 move 不再扩展选区。PointHandler 为
                    // Qt 6 的被动点跟踪（不抢独占抓取，TextArea 抓取期间照常更新），
                    // 按下期间以锚点 + 当前点直接驱动 select；与原生补放路径并存时
                    // 锚点/终点一致、逐步收敛，无双头冲突。双击/三击为独立 dblclick
                    // 事件不经 onPressed（守卫不置位），原生选词/扩展不受影响
                    PointHandler {
                        acceptedButtons: Qt.LeftButton
                        onPointChanged: {
                            // active 过滤：释放时 point 会重置为 (0,0)（此时守卫
                            // 尚未复位），不过滤会以 select(锚点, 0) 清空选区
                            if (!active || !editorHolder.nativeDragGuard
                                    || editorHolder.nativeDragAnchor < 0)
                                return
                            // 先记录鼠标位置再 select：select 同步触发
                            // cursorRectangleChanged → ensureCursorVisible →
                            // contentXChanged，钳制检测须与选区驱动读到
                            // 同一批最新鼠标/光标值；point.position 与
                            // onPressed 的 event.x/y 同为 TextArea 局部坐标
                            editorHolder.dragMouseLocalX = point.position.x
                            editorHolder.dragMouseLocalY = point.position.y
                            const pos = editor.positionAt(point.position.x, point.position.y)
                            editorHolder.nativeDragLastPos = pos
                            editor.select(editorHolder.nativeDragAnchor, pos)
                        }
                    }

                    // 按下/释放维护守卫：连接 pressed 信号使 press 被 TextArea 接受、
                    // 成为独占抓取者（拖出边缘 move/release 仍送达，选区可跨边缘持续，
                    // 释放必达，守卫可靠复位）。守卫为真期间内部 Flickable 非交互，
                    // 其 childMouseEventFilter 早退、自身拖动分支不生效，两条抢夺/滚动
                    // 路径同时被阻断；释放即恢复，滚轮滚动不受影响
                    //（QQuickFlickable::wheelEvent 仅在 interactive 时处理，故不能永久禁用）
                    onPressed: (event) => {
                        // 仅左键置守卫：QQuickPressHandler 对任意按键都发 pressed，
                        // 右/中键单击不得触发（否则其 release 被下方左键过滤挡住，
                        // 守卫卡死导致滚轮长期失效）
                        if (event.button === Qt.LeftButton) {
                            editorHolder.nativeDragGuard = true
                            // 横向抑制窗口同步开启，且必须先于下方光标定位
                            // 赋值/Shift 分支 select——按下即位于末行以下空白
                            // 时，positionAt 把光标钳到文档末尾，随之而来的
                            // ensureCursorVisible 右滚须已被本窗口覆盖
                            //（验收 2）；基准取当前认可值（非抑制期持续跟随）
                            editorHolder.hDragScrollGuard = true
                            editorHolder.dragContentXBase = editorScroll.contentItem
                                    ? editorScroll.contentItem.contentX : 0
                            // event.x/y 即 TextArea 局部坐标，钳制检测同系
                            editorHolder.dragMouseLocalX = event.x
                            editorHolder.dragMouseLocalY = event.y
                            if (event.modifiers & Qt.ShiftModifier) {
                                // Shift+按下：原生 KeepAnchor 扩展语义——锚保持当前
                                // 光标；已有选区时锚为光标对端（连续 Shift+click 锚
                                // 不跟随更新，与 qquicktextcontrol 原生基线一致）。
                                // 不得 MoveAnchor 定位到按下点，否则扩展长度恒 0
                                // 且清掉旧选区；立即以锚 + 按下点 select，随后
                                // PointHandler 以此锚持续扩展（被动驱动）
                                const anchor = editor.selectedText !== ""
                                        ? (editor.cursorPosition === editor.selectionStart
                                           ? editor.selectionEnd
                                           : editor.selectionStart)
                                        : editor.cursorPosition
                                editorHolder.nativeDragAnchor = anchor
                                const shiftPos = editor.positionAt(event.x, event.y)
                                editor.select(anchor, shiftPos)
                            } else {
                                // 按下即定位光标并自持锚点：原生路径要到 release 或首个
                                // 横向位移超阈值的 move 才补放 press；竖直拖拽超 800ms 后
                                // delayed press 被定时器销毁，原生 move 永不扩展选区
                                //（光标定位亦由 release 提前到 press，桌面编辑器惯例）
                                const pos = editor.positionAt(event.x, event.y)
                                editor.cursorPosition = pos
                                editorHolder.nativeDragAnchor = pos
                            }
                        }
                    }
                    onReleased: (event) => {
                        // 仅左键释放复位：QQuickPressHandler 对任意按键的释放都发 released，
                        // 左键拖拽中点按右/中键不得提前解除守卫
                        if (event.button === Qt.LeftButton) {
                            // 延迟按下窗口内 release 会先补放 press 再放 release，
                            // 原生处理将清空 PointHandler 驱动的选区；延迟一拍
                            //（原生事件处理完成后）以自持的锚点/终点重放选区
                            const anchor = editorHolder.nativeDragAnchor
                            const last = editorHolder.nativeDragLastPos
                            if (anchor >= 0 && last >= 0 && last !== anchor) {
                                Qt.callLater(function() {
                                    editor.select(anchor, last)
                                })
                            }
                            // 横向抑制窗口须覆盖释放重放：重放终点可能来自钳制
                            //（如释放于末行以下，重放仍触发一次
                            // ensureCursorVisible 右滚）。上方重放的 callLater
                            // 先入队、下方复位后入队，FIFO 保证重放先执行、
                            // 复位后执行——抑制多存活一拍（毫秒级），期间无
                            // 其他合法 contentX 写入方（DEC-003）。鼠标位置
                            // 必须随守卫同拍失效：重放终点的钳制检测依赖最后
                            // 一次鼠标位置仍有效，同步提前复位会使检测读到
                            // 无效值而保守放行，重放的钳制跳滚不被回滚
                            Qt.callLater(function() {
                                editorHolder.hDragScrollGuard = false
                                editorHolder.dragMouseLocalX = -1
                                editorHolder.dragMouseLocalY = -1
                            })
                            editorHolder.nativeDragGuard = false
                            editorHolder.nativeDragAnchor = -1
                            editorHolder.nativeDragLastPos = -1
                        }
                    }
                    // 防御性复位：窗口失活等事件中断可能丢失 release，焦点转移时兜底，
                    // 防止守卫卡死导致滚轮长期失效（下次按下亦有自愈兜底）
                    onActiveFocusChanged: {
                        if (!activeFocus) {
                            editorHolder.nativeDragGuard = false
                            editorHolder.nativeDragAnchor = -1
                            editorHolder.nativeDragLastPos = -1
                            // 横向抑制标志同步兜底复位：丢失 release 的中断
                            // 同样会使 hDragScrollGuard 卡死，导致横向滚动
                            // 长期失效（下次按下自愈，此处消除窗口期）
                            editorHolder.hDragScrollGuard = false
                            editorHolder.dragMouseLocalX = -1
                            editorHolder.dragMouseLocalY = -1
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

                    onCursorPositionChanged: {
                        // 用户移动光标（键盘/鼠标）→ 回写 doc.cursorPosition，
                        // 作为会话恢复的光标数据源。selfCursorSync 期间是
                        // doc→editor 的下行应用而非用户输入，跳过；
                        // Document 同值设置不发信号，环自然终止
                        if (editorHolder.doc && !editorHolder.selfCursorSync
                                && editor.cursorPosition !== editorHolder.doc.cursorPosition) {
                            // 回写期间置 selfCursorSync：doc 信号同步回落到下行
                            // 处理器时跳过——对 editor.cursorPosition 的同值赋值
                            // 仍会按 MoveAnchor 语义重置锚点、清空用户选区
                            //（与 onContentChanged 回写的 selfContentSync 同模式）
                            editorHolder.selfCursorSync = true
                            try {
                                editorHolder.doc.cursorPosition = editor.cursorPosition
                            } finally {
                                editorHolder.selfCursorSync = false
                            }
                        }
                        syncStatus()
                    }
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
                        function onCursorPositionChanged() {
                            // C++（会话恢复等）设置 doc 光标 → 应用到编辑器
                            // 并滚动到可见；置 selfCursorSync 防止上方
                            // editor.onCursorPositionChanged 把本次下行应用
                            // 误判为用户输入而回写 doc（与 onContentChanged 的
                            // selfContentSync 防环同一模式）；值相等的下行
                            // 应用不触发 editor 信号，无环
                            if (!editorHolder.doc || editorHolder.selfCursorSync)
                                return
                            editorHolder.selfCursorSync = true
                            try {
                                editor.cursorPosition = Math.min(editorHolder.doc.cursorPosition, editor.length)
                                editor.scrollCursorIntoView()
                            } finally {
                                // 复位放 finally：应用/滚动任何一步抛异常也不得卡死标志，
                                // 否则用户光标回写链路永久失效（本缺陷的教训）
                                editorHolder.selfCursorSync = false
                            }
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
                    // 滚动到光标可见：TextArea 的 ensureCursorVisible 为私有方法不可从 QML
                    // 调用（Qt 6.11.2 实测抛 TypeError），按光标矩形手动调整 ScrollView
                    // 滚动位。滚动载体取 contentItem：ScrollView 无 flickable 属性
                    // （运行时取值为 undefined——flickable 实为 TextArea 的附加属性，
                    // 旧实现取它恒早退、恢复光标后不滚动），其 contentItem 即内部
                    // Flickable，contentX/contentY 可直接读写
                    function scrollCursorIntoView() {
                        const flick = editorScroll.contentItem
                        if (!flick)
                            return
                        const rect = editor.cursorRectangle
                        if (rect.y < flick.contentY)
                            flick.contentY = rect.y
                        else if (rect.y + rect.height > flick.contentY + editorScroll.height)
                            flick.contentY = rect.y + rect.height - editorScroll.height
                        // 横向（NoWrap 长行）：光标列超出可视区时同样滚动到位
                        if (rect.x < flick.contentX)
                            flick.contentX = rect.x
                        else if (rect.x + rect.width > flick.contentX + editorScroll.width)
                            flick.contentX = rect.x + rect.width - editorScroll.width
                    }

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

            // 拖拽期横向钳制跳滚的选择性回滚（DEC-001 方案 3）：TextArea 进入
            // ScrollView 时被 Qt 私有连接 cursorRectangleChanged →
            // ensureCursorVisible 附着（QML 不可解绑），拖拽中光标被
            // positionAt/hitTest 钳到行尾/文档末尾时会一次大幅右滚 contentX。
            // 此处在抑制窗口内检测"光标是否来自钳制"（与鼠标位置分离），
            // 仅回滚钳制引发的跳变；右缘前瞻爬行（光标紧邻鼠标）照旧放行。
            // 拖拽期间 contentX 的其他写入方（Flickable 拖动/滚轮）已被
            // nativeDragGuard（interactive=false）禁用，回滚不会误伤合法
            // 滚动。任何路径都不触碰 contentY（纵向行为完全不变）
            Connections {
                target: editorScroll.contentItem

                function onContentXChanged() {
                    const flick = editorScroll.contentItem
                    if (!flick)
                        return
                    if (!editorHolder.hDragScrollGuard) {
                        // 非抑制期：跟随认可（键盘移动光标/查找跳转/会话
                        // 恢复/wrap 切换等合法滚动一律放行并更新基准）
                        editorHolder.dragContentXBase = flick.contentX
                        return
                    }
                    // 回滚写回自身/微抖动（<0.5px）：退出防递归
                    if (Math.abs(flick.contentX - editorHolder.dragContentXBase) < 0.5)
                        return
                    if (editorHolder.contentXFromClampScroll()) {
                        // 钳制跳滚：同步写回认可值（同帧不上屏，无闪烁）
                        flick.contentX = editorHolder.dragContentXBase
                    } else {
                        // 合法滚动（右缘前瞻爬行）：更新认可基准（DEC-002）
                        editorHolder.dragContentXBase = flick.contentX
                    }
                }
            }
        }
    }

    // ---- 查找集成接口（供 Main.qml 的打开/关闭流程调用） ----

    /// 滚动条 policy：可滚动（0 < size < 1）→ AlwaysOn 常驻；否则 AlwaysOff 隐藏
    /// （size==1 不可滚动；size==0 未布局/空 delegate 亦按隐藏，防首帧闪现）
    function residentScrollBarPolicy(barSize) {
        return barSize > 0.0 && barSize < 1.0 ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
    }

    /// 退出活动编辑器的多光标（DEC-008：查找对话框打开前调用，互斥；
    /// 单光标时无操作，既有行为零变化）
    function exitMultiCursor() {
        if (activeMc && activeMc.active)
            activeMc.reset()
    }

    /// 活动编辑器当前选区文本（无选区返回 null）。调用方（Main 预填逻辑）
    /// 自行判断含换行不预填；不在此处过滤，保持取值语义纯粹
    function activeSelectionText() {
        if (!activeEditor)
            return null
        const s = activeEditor.selectionStart
        const e = activeEditor.selectionEnd
        if (e <= s)
            return null
        return activeEditor.getText(s, e)
    }

    /// 焦点回活动编辑器（查找对话框关闭后调用；无活动编辑器无操作）
    function focusActiveEditor() {
        if (activeEditor)
            activeEditor.forceActiveFocus()
    }

    WelcomePage {
        anchors.fill: parent
        visible: root.documentsModel.count === 0
        onNewRequested: root.newRequested()
        onOpenRequested: root.openRequested()
    }
}
