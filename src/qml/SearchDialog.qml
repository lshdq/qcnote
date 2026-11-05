import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

/**
 * 查找与替换对话框（模态，DEC-002 顶部对齐，不居中——避免遮挡编辑区，
 * 保证背后匹配高亮可见）。
 *
 * - 数据/操作入口为 EditorArea 的会话级 SearchController 单实例（target/
 *   textDocument 随活动文档重绑）；本对话框只做参数输入、状态展示与
 *   动作转发，不持有匹配状态；
 * - 查找输入框输入即实时查找（控制器 pattern 变更即重算全部匹配）；
 *   Enter=下一个、Shift+Enter=上一个（环绕由控制器实现）；
 * - 关闭（Esc / 关闭按钮）不调 reset()（DEC-009）：控制器匹配状态保留
 *   供 F3 继续跳转，高亮层隐藏与焦点回编辑器由 Main.qml 的 onRejected
 *   处理（关闭按钮为 RejectRole，与 Esc 汇入同一 rejected 信号）；
 * - 状态行以文本表达状态（错误 / 无匹配 / 计数 / 已替换 N 处），错误文本
 *   另取 AppTheme.error 着色，颜色仅是辅助线索；
 * - 「查找下一个 / 替换 / 全部替换」为 ActionRole：点击不触发对话框
 *   accepted/rejected，对话框保持打开（查找类对话框惯例）。
 */
Dialog {
    id: root

    /// 查找控制器（EditorArea 会话级单实例，查找参数与匹配状态的唯一持有者）
    required property SearchController controller

    // ---- 内部状态 ----

    /// 全新查找会话标志：pattern / 选项变更、活动文档重绑、对话框重开时
    /// 复位。复位后的首次跳转先定位到当前匹配——控制器 restartSearch 只
    /// 把索引归零、不设原生选区，直接 findNext 会从第 2 个开始（控制器
    /// 注释明示 QML 侧可依 currentRange 二次调整）；此后按环绕导航推进
    property bool hasJumped: false

    /// 最近一次「全部替换」的处数（-1 = 不显示）；需求：显示到下一次
    /// 查找/替换操作前为止
    property int lastReplacedCount: -1

    /// 控制器 textDocument 重绑检测：该属性 NOTIFY 为共用的 stateChanged，
    /// 经本绑定做值比较，文档对象实际变化时才复位会话标志
    property var trackedDocument: controller.textDocument
    onTrackedDocumentChanged: hasJumped = false

    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape
    width: 520
    title: "查找与替换"

    // DEC-002：顶部对齐于窗口（Popup 不支持任意 anchors，仅 centerIn 有
    // 特殊支持，故以 x/y 绑定计算）；y 取 标题栏 40 + 工具栏 44 + 间距 12，
    // 落在编辑区上方
    x: (parent.width - width) / 2
    y: 96

    contentItem: ColumnLayout {
        spacing: 8

        // 对话框打开期间 F3 / Shift+F3 = 查找下一个/上一个。Main.qml 的
        // 窗口级 F3 Shortcut 已含此转发分支，但模态 Popup 打开时窗口级
        // Shortcut 不被键事件激活（QML Shortcut 匹配发生在条目派发路径，
        // 模态弹层重定向派发），故在对话框内容区补 Keys 转发保证该路径
        // 生效；焦点在输入框/选项/按钮时未处理的 F3 沿父链上浮至此。
        // 转发逻辑与 footer 侧共享（forwardSearchKey，DEC-013）
        Keys.onPressed: (event) => root.forwardSearchKey(event)

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label { text: "查找：" }

            TextField {
                id: findField
                Layout.fillWidth: true

                // 输入即实时查找（控制器重算全部匹配并归零索引）；
                // 同值不回写（控制器 setPattern 同值本就早退，此处再省
                // 一次跨边界调用）
                onTextChanged: {
                    root.hasJumped = false
                    root.lastReplacedCount = -1
                    if (text !== root.controller.pattern)
                        root.controller.pattern = text
                }

                // Enter=下一个、Shift+Enter=上一个。Keys 附加处理器默认
                // BeforeItem 优先级：先于输入框内部处理并接受事件，阻止
                // 其再走 accepted 信号的重复路径
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        if (event.modifiers & Qt.ShiftModifier)
                            root.doFindPrev()
                        else
                            root.doFindNext()
                        event.accepted = true
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label { text: "替换：" }

            TextField {
                id: replaceField
                Layout.fillWidth: true
                // 捕获组引用语法提示（DEC-006：$n 与 \n 等价、$$ 字面化）
                placeholderText: "支持 $1 与 \\1 捕获组引用"

                onTextChanged: {
                    if (text !== root.controller.replacement)
                        root.controller.replacement = text
                }

                // 替换框内 Enter 同样执行查找跳转（提交替换串后继续定位）
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        if (event.modifiers & Qt.ShiftModifier)
                            root.doFindPrev()
                        else
                            root.doFindNext()
                        event.accepted = true
                    }
                }
            }
        }

        RowLayout {
            spacing: 24

            CheckBox {
                text: "区分大小写"
                // DEC-003：默认关闭（记事本 / VS Code 惯例）
                checked: root.controller.caseSensitive
                onToggled: {
                    root.hasJumped = false
                    root.lastReplacedCount = -1
                    root.controller.caseSensitive = checked
                }
            }
            CheckBox {
                text: "全字匹配"
                checked: root.controller.wholeWord
                onToggled: {
                    root.hasJumped = false
                    root.lastReplacedCount = -1
                    root.controller.wholeWord = checked
                }
                // 仅纯文本模式生效：正则开启时禁用置灰（需求选项表）
                enabled: !root.controller.useRegex
            }
            CheckBox {
                text: "正则表达式"
                checked: root.controller.useRegex
                onToggled: {
                    root.hasJumped = false
                    root.lastReplacedCount = -1
                    root.controller.useRegex = checked
                }
            }
        }

        // 状态行：错误文本（非仅颜色线索）→ 已替换 N 处（保留到下一次
        // 查找/替换操作）→ 无匹配 → 溢出（DEC-004）→ 计数
        Label {
            Layout.fillWidth: true
            elide: Text.ElideRight
            color: root.controller.errorMessage !== "" ? AppTheme.error
                                                       : AppTheme.textSecondary
            text: {
                const c = root.controller
                if (c.errorMessage !== "")
                    return c.errorMessage
                if (root.lastReplacedCount >= 0)
                    return "已替换 " + root.lastReplacedCount + " 处"
                if (c.matchCount === 0)
                    return "无匹配"
                if (c.overflowed)
                    return ">10000"
                if (c.currentIndex < 0)
                    return "共 " + c.matchCount + " 个"
                return "第 " + (c.currentIndex + 1) + " 个 / 共 " + c.matchCount + " 个"
            }
        }
    }

    // 无匹配（matchCount==0，含无效正则）时三个动作按钮全部禁用
    //（无匹配无可跳转、无可替换）；关闭恒可用。
    // DEC-013：footer 按钮点击后焦点落在按钮上（DialogButton 默认
    // StrongFocus），键事件父链为 按钮→DialogButtonBox→弹层面板，
    // 不经过 contentItem，故 footer 侧需补同款 F3/Shift+F3 转发
    footer: DialogButtonBox {
        buttonLayout: DialogButtonBox.WinLayout

        // 与 contentItem 共享的转发（forwardSearchKey）：焦点在 footer
        // 按钮上时未处理的 F3 沿父链上浮至此
        Keys.onPressed: (event) => root.forwardSearchKey(event)

        DialogButton {
            text: "查找下一个"
            primary: true
            enabled: root.controller.matchCount > 0
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: root.doFindNext()
        }
        DialogButton {
            text: "替换"
            enabled: root.controller.matchCount > 0
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: root.doReplaceCurrent()
        }
        DialogButton {
            text: "全部替换"
            enabled: root.controller.matchCount > 0
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: root.doReplaceAll()
        }
        DialogButton {
            text: "关闭"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }

    // ---- 打开 / 聚焦 ----

    function openFind() {
        open()
        focusFindField()
    }

    function openReplace() {
        open()
        focusReplaceField()
    }

    /// 对话框已打开时重复 Ctrl+F：聚焦查找框并全选（便于直接重新输入）
    function focusFindField() {
        findField.forceActiveFocus()
        findField.selectAll()
    }

    /// 对话框已打开时重复 Ctrl+H：聚焦替换框
    function focusReplaceField() {
        replaceField.forceActiveFocus()
    }

    // open() 同步发 aboutToShow：先同步输入框内容（Main 打开前已按选区 /
    // 上次查找串预填 pattern），再在 openFind/openReplace 中聚焦
    onAboutToShow: {
        lastReplacedCount = -1
        hasJumped = false
        findField.text = controller.pattern
        replaceField.text = controller.replacement
    }

    // ---- 查找 / 替换动作 ----

    /// F3/Shift+F3 共享转发（DEC-013）：contentItem 与 footer 两处
    /// Keys.onPressed 调用。修饰键过滤：Ctrl/Alt/Meta 组合不转发（保留
    /// 组合键语义空间）；容忍 KeypadModifier（Windows 开 NumLock 时按键
    /// 常带该位，过滤会误杀纯 F3）；Shift 仅用于 Shift+F3 后退，与
    /// Main.qml 关闭态 Shortcut 的精确序列语义对称
    function forwardSearchKey(event) {
        if (event.key !== Qt.Key_F3)
            return
        if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
            return
        if (event.modifiers & Qt.ShiftModifier)
            doFindPrev()
        else
            doFindNext()
        event.accepted = true
    }

    function doFindNext() {
        lastReplacedCount = -1
        jump(true)
    }

    function doFindPrev() {
        lastReplacedCount = -1
        jump(false)
    }

    function doReplaceCurrent() {
        lastReplacedCount = -1
        controller.replaceCurrent()
        // 替换后控制器已把索引指向下一个匹配（光标在替换文本之后），
        // 后续跳转按环绕导航推进
        hasJumped = true
    }

    function doReplaceAll() {
        lastReplacedCount = controller.replaceAll()
        hasJumped = true
    }

    /// 跳转统一出口（对话框内 Enter/按钮 与 窗口级 F3 转发共用）：
    /// 全新会话首跳先定位当前匹配（见 hasJumped 注释），已跳转后按
    /// 控制器环绕 findNext/findPrev（跳转后控制器已把当前匹配设为原生
    /// 选区，滚动由 EditorArea 的 stateChanged 监听完成）
    function jump(forward) {
        if (controller.matchCount === 0)
            return
        const r = controller.currentRange
        const ed = controller.target
        if (!hasJumped && ed && r.length === 2
                && !(ed.selectionStart === r[0] && ed.selectionEnd === r[1])) {
            ed.select(r[0], r[1])
            // 索引未变不会触发 EditorArea 的滚动监听，此处显式滚动；
            // typeof 防护：target 为无该函数的普通 TextArea 时不滚而不报错
            Qt.callLater(function() {
                const t = controller.target
                if (t && typeof t.scrollCursorIntoView === "function")
                    t.scrollCursorIntoView()
            })
        } else {
            if (forward)
                controller.findNext()
            else
                controller.findPrev()
        }
        hasJumped = true
    }
}
