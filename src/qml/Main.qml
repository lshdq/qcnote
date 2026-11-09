import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts

/**
 * 主窗口：Frameless 四层结构（标题栏 / 工具栏 / 内容区 / 状态栏）。
 *
 * - 快捷键统一为窗口级 Shortcut；菜单仅展示快捷键文本；
 * - 未保存关闭/退出流程：三选（保存 / 不保存 / 取消），取消中止；
 * - 打开/保存失败统一错误对话框（排队展示），内容不丢失；
 * - 查找替换（Ctrl+F/Ctrl+H）：模态对话框（顶部对齐）+ 全部匹配高亮 +
 *   F3/Shift+F3 关闭态跳转，与多光标互斥，见 SearchDialog/SearchController；
 * - 边缘 + 四角隐形命中区触发系统缩放，保留 Win+方向贴靠等原生行为；
 * - 会话恢复：启动时先恢复文档集（含光标/活动标签）再应用窗口几何、
 *   最后显示窗口（几何先就位再显示，避免默认几何闪跳）；运行期窗口
 *   几何与最大化标志变化经 SessionManager 防抖写盘；
 * - 启动时序：会话恢复 → 草稿缓存恢复（静默应用未保存修改，磁盘被
 *   外部修改/文件缺失时弹汇总对话框决策）→ 单实例启动参数刷出（叠加
 *   打开）→ 首启右键菜单提示；单实例激活请求（再次启动/打开）仅前置窗口。
 */
ApplicationWindow {
    id: root

    width: 1000
    height: 640
    minimumWidth: 800
    minimumHeight: 500

    flags: Qt.Window | Qt.FramelessWindowHint

    // 初始不可见：Component.onCompleted 完成会话恢复与几何应用后再显示
    // （见下方"会话恢复"块），避免先按默认几何闪跳再跳到恢复几何
    visible: false

    color: AppTheme.contentBackground
    Material.theme: AppTheme.dark ? Material.Dark : Material.Light
    Material.primary: AppTheme.accent

    title: DocumentManager.activeDocument
           ? DocumentManager.activeDocument.displayName
             + (DocumentManager.activeDocument.modified ? " *" : "") + " — qcnote"
           : "qcnote"

    // ---- 流程状态 ----
    /// 正在处理的关闭请求文档（null = 无流程）
    property var closeFlowDoc: null
    /// 该流程是否由退出触发（保存/放弃后继续处理下一个未保存文档）
    property bool closeFlowIsExit: false
    /// 退出已确认，放行 onClosing
    property bool exiting: false
    /// 正在另存为的文档
    property var saveAsTarget: null
    /// 待展示错误队列
    property var errorQueue: []
    /// 按编码重载确认流程的目标文档与编码（确认框展示期间有效）
    property var reloadConfirmDoc: null
    property int reloadEncoding: 0

    function uiLocked() {
        return unsavedDialog.visible || errorDialog.visible
               || openFileDlg.visible || saveFileDialog.visible
               || titleBar.menuVisible || ctxMenuPromptDialog.visible
               || reloadConfirmDialog.visible || searchDialog.visible
               || draftConflictDialog.visible
    }

    function firstModifiedDoc() {
        const m = DocumentManager.documentsModel
        for (let i = 0; i < m.count; ++i) {
            if (m.at(i).modified)
                return m.at(i)
        }
        return null
    }

    // ---- 动作 ----
    function doNewDocument() {
        if (uiLocked())
            return
        DocumentManager.newDocument()
    }

    function doOpenDialog() {
        if (uiLocked())
            return
        openFileDlg.open()
    }

    function doSave() {
        if (uiLocked())
            return
        const doc = DocumentManager.activeDocument
        if (!doc)
            return
        // save 返回 false：url 为空（转另存为）或写入失败（saveFailed 已弹错误）
        if (!DocumentManager.save(doc) && doc.url.toString().length === 0) {
            saveAsTarget = doc
            prefillSaveDialog(doc)
            saveFileDialog.open()
        }
    }

    function doSaveAs() {
        if (uiLocked())
            return
        const doc = DocumentManager.activeDocument
        if (!doc)
            return
        saveAsTarget = doc
        prefillSaveDialog(doc)
        saveFileDialog.open()
    }

    // 按指定编码重新加载（纠正自动识别错误）。有未保存修改时先弹确认框
    // （DEC-008：确认只在 QML 层，C++ 重载直接执行、以磁盘内容覆盖内存）
    function doReloadWithEncoding(encoding) {
        if (uiLocked())
            return
        const doc = DocumentManager.activeDocument
        if (!doc || doc.url.toString().length === 0)
            return
        if (doc.modified) {
            reloadConfirmDoc = doc
            reloadEncoding = encoding
            reloadConfirmDialog.open()
            return
        }
        DocumentManager.reloadWithEncoding(doc, encoding)
    }

    // 按指定编码保存（编码转换，DEC-005 一步完成）。返回 false：
    // url 为空（未命名，C++ 已保留编码设置）→ 转既有另存为流程，按该编码写出；
    // 有 url 但写入失败（含 GBK 不可编码）→ saveFailed 信号 → errorDialog
    function doSaveWithEncoding(encoding) {
        if (uiLocked())
            return
        const doc = DocumentManager.activeDocument
        if (!doc)
            return
        if (!DocumentManager.saveWithEncoding(doc, encoding)
                && doc.url.toString().length === 0) {
            saveAsTarget = doc
            prefillSaveDialog(doc)
            saveFileDialog.open()
        }
    }

    function doCloseActive() {
        if (uiLocked())
            return
        const doc = DocumentManager.activeDocument
        if (doc)
            closeDocumentWithConfirm(doc)
    }

    function doQuit() {
        if (uiLocked())
            return
        root.close() // 统一走 onClosing → 退出确认流程
    }

    function cycleTab(delta) {
        if (uiLocked())
            return
        const m = DocumentManager.documentsModel
        if (m.count === 0)
            return
        const i = m.indexOf(DocumentManager.activeDocument)
        const n = m.count
        DocumentManager.activeDocument = m.at(((i + delta) % n + n) % n)
    }

    // ---- 查找与替换 ----
    // Ctrl+F / Ctrl+H 打开同一对话框（聚焦框不同）。打开逻辑：
    // - 查找对话框自身可见（亦属 uiLocked）时不重开，改为聚焦对应输入框
    //   （查找框全选，便于直接重新输入）；其他模态打开时无操作；
    // - 无活动文档（欢迎页）无操作（需求边界）；
    // - 多光标激活时先退出（DEC-008 互斥）；
    // - 预填：活动编辑器有不含换行的选区 → 查找串（含换行不预填），
    //   否则沿用上次查找串；两种情况均在对话框内全选（其 onAboutToShow
    //   自控制器同步输入框）。
    function doFind() {
        openSearchDialog("find")
    }

    function doReplace() {
        openSearchDialog("replace")
    }

    function openSearchDialog(mode) {
        if (uiLocked()) {
            if (searchDialog.visible) {
                if (mode === "replace")
                    searchDialog.focusReplaceField()
                else
                    searchDialog.focusFindField()
            }
            return
        }
        if (!DocumentManager.activeDocument)
            return
        editorArea.exitMultiCursor()
        const sel = editorArea.activeSelectionText()
        if (sel !== null && sel.indexOf("\n") === -1)
            editorArea.searchController.pattern = sel
        // 高亮层激活（对话框关闭时由 searchDialog.onRejected 复位）
        editorArea.searchActive = true
        if (mode === "replace")
            searchDialog.openReplace()
        else
            searchDialog.openFind()
    }

    // F3 / Shift+F3 在对话框关闭时的跳转（DEC-009：沿用上次查找参数与
    // 控制器保留的匹配状态，不恢复全部高亮）；无活动文档、无查找串或
    // 无匹配时无操作。经对话框的跳转出口执行（全新会话首跳先定位当前
    // 匹配，语义与对话框内 Enter 一致）；对话框打开期间 F3 由窗口级
    // Shortcut 直接转发（见下方快捷键），不经本函数
    function searchJumpGlobal(forward) {
        if (uiLocked())
            return
        if (!DocumentManager.activeDocument)
            return
        const c = editorArea.searchController
        if (c.pattern.length === 0 || c.matchCount === 0)
            return
        if (forward)
            searchDialog.doFindNext()
        else
            searchDialog.doFindPrev()
    }

    // ---- 关闭 / 退出流程 ----
    function closeDocumentWithConfirm(doc) {
        if (doc.modified) {
            closeFlowDoc = doc
            closeFlowIsExit = false
            unsavedDialog.open()
        } else {
            DocumentManager.closeDocument(doc)
        }
    }

    function continueExit() {
        const next = firstModifiedDoc()
        if (!next) {
            exiting = true
            root.close()
            return
        }
        closeFlowDoc = next
        closeFlowIsExit = true
        unsavedDialog.open()
    }

    function finishCloseFlow(doc) {
        const wasExit = closeFlowIsExit
        resetCloseFlow()
        DocumentManager.closeDocument(doc)
        if (wasExit)
            continueExit()
    }

    function resetCloseFlow() {
        closeFlowDoc = null
        closeFlowIsExit = false
        saveAsTarget = null
    }

    function prefillSaveDialog(doc) {
        // 有路径时预填原路径文件名；未命名文档由系统对话框默认位置开始
        saveFileDialog.selectedFile = (doc && doc.url && doc.url.toString().length > 0)
                                       ? doc.url : ""
    }

    onClosing: (close) => {
        // 退出已确认（未保存流程走完）→ 放行
        if (root.exiting) {
            close.accepted = true
            return
        }
        // 有模态 UI 打开时拒绝关闭
        if (uiLocked()) {
            close.accepted = false
            return
        }
        // 无未保存修改 → 直接放行关闭；
        // 不可嵌套调用 close()：QWindow::close() 在 close 事件派发期间有重入保护（inClose），会被静默忽略
        if (!DocumentManager.hasUnsavedChanges()) {
            close.accepted = true
            return
        }
        // 有未保存修改 → 拒绝本次关闭，弹确认框逐个处理
        close.accepted = false
        closeFlowDoc = firstModifiedDoc()
        closeFlowIsExit = true
        unsavedDialog.open()
    }

    // ---- 会话恢复 ----
    // 最近一次 Windowed（普通）状态的窗口几何。仅在 Windowed 时更新，
    // 最大化期间几何变化不落入缓存——保证"还原"回到最大化前的几何。
    // 初始值为当前默认几何（x/y 未显式设置，QQuickWindow 默认 0）
    property int lastNormalX: 0
    property int lastNormalY: 0
    property int lastNormalWidth: 1000
    property int lastNormalHeight: 640
    // 最近一次非最小化状态是否最大化。QML 的 visibility 在最小化时报
    // Minimized（丢失"此前最大化"信息），上报时若实时取
    // visibility === Window.Maximized 会把缓存覆盖为 false，故以本标志
    // 为准：仅在 Maximized / Windowed 迁移时更新，Minimized 保持不变
    property bool lastMaximized: false

    /// 统一上报窗口状态（500ms 防抖写盘；maximized 取 lastMaximized，
    /// lastNormal* 保持上次 Windowed 几何）
    function syncWindowState() {
        SessionManager.saveWindowState(lastNormalX, lastNormalY, lastNormalWidth,
                                       lastNormalHeight, lastMaximized)
    }

    Component.onCompleted: {
        // 时序：先恢复文档（各 doc 光标随 openUrls 后写入，EditorArea
        // delegate 经 Connections 同步到 TextArea），再应用窗口几何，
        // 最后显示窗口；无会话/几何离屏时 windowState() 返回空 map，
        // 不设置任何几何，保持默认 1000×640 行为与现状一致
        SessionManager.restoreDocuments()
        const g = SessionManager.windowState()
        if (Object.keys(g).length > 0) {
            // 数据源先行：此时窗口未显示，visibility 仍为 Hidden，下方几何
            // 赋值触发的 onXChanged 走不进 Windowed 分支，恢复几何永远不会
            // 写入 lastNormal*；而 visible = true 会同步发出 visibilityChanged，
            // 若不在此直接更新数据源，首次上报将以初始默认值 (0,0,1000,640)
            // 覆盖会话缓存（P0 缺陷根因：恢复后不动窗口的会话丢失几何）
            lastNormalX = g.x
            lastNormalY = g.y
            lastNormalWidth = g.width
            lastNormalHeight = g.height
            lastMaximized = g.maximized === true
            x = g.x
            y = g.y
            width = g.width
            height = g.height
            if (g.maximized === true)
                visibility = Window.Maximized
        }
        visible = true
        // 首次上报：保证首启也写入 [window] 节（此后窗口创建会话文件窗口项）
        syncWindowState()
        // 草稿缓存恢复：会话恢复之后进行（静默应用未保存修改；
        // 有外部修改/文件缺失冲突时由 conflictsFound 弹统一对话框）
        DraftCacheManager.restoreDrafts()
        // 单实例：恢复会话后叠加打开启动参数中的文件
        SingleInstance.openPendingUrls()
        // 首启提示：恢复与参数打开完成后再弹，避免被覆盖
        if (ShellIntegration.contextMenuPromptNeeded())
            ctxMenuPromptDialog.open()
    }

    // 几何变化仅在普通窗口状态记录并上报（最大化/还原过程的中间几何
    // 不是 normal 几何，跳过）；每处变化均触发防抖上报
    onXChanged: {
        if (visibility === Window.Windowed) {
            lastNormalX = x
            syncWindowState()
        }
    }
    onYChanged: {
        if (visibility === Window.Windowed) {
            lastNormalY = y
            syncWindowState()
        }
    }
    onWidthChanged: {
        if (visibility === Window.Windowed) {
            lastNormalWidth = width
            syncWindowState()
        }
    }
    onHeightChanged: {
        if (visibility === Window.Windowed) {
            lastNormalHeight = height
            syncWindowState()
        }
    }
    // 可见性变化：lastNormal* 保持上次 Windowed 几何；lastMaximized 仅在
    // Maximized / Windowed 迁移时更新（Minimized 报告丢失"此前最大化"
    // 信息，直接取 visibility 会误写 false），随后统一上报
    onVisibilityChanged: {
        if (visibility === Window.Maximized)
            lastMaximized = true
        else if (visibility === Window.Windowed)
            lastMaximized = false
        syncWindowState()
    }

    // ---- 布局（四层结构） ----
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TitleBar {
            id: titleBar
            Layout.fillWidth: true
            window: root

            onRequestNew: root.doNewDocument()
            onRequestOpen: root.doOpenDialog()
            onRequestSave: root.doSave()
            onRequestSaveAs: root.doSaveAs()
            onRequestCloseTab: (doc) => root.closeDocumentWithConfirm(doc)
            onRequestQuit: root.doQuit()
            onRequestReloadWithEncoding: (encoding) => root.doReloadWithEncoding(encoding)
            onRequestSaveWithEncoding: (encoding) => root.doSaveWithEncoding(encoding)
            onRequestFind: root.doFind()
            onRequestReplace: root.doReplace()
            onRequestFindNext: root.searchJumpGlobal(true)
            onRequestFindPrev: root.searchJumpGlobal(false)
        }

        Toolbar {
            Layout.fillWidth: true

            onRequestNew: root.doNewDocument()
            onRequestOpen: root.doOpenDialog()
            onRequestSave: root.doSave()
        }

        EditorArea {
            id: editorArea
            Layout.fillWidth: true
            Layout.fillHeight: true

            onNewRequested: root.doNewDocument()
            onOpenRequested: root.doOpenDialog()
        }

        StatusBar {
            Layout.fillWidth: true
            doc: DocumentManager.activeDocument
            cursorLine: editorArea.cursorLine
            cursorColumn: editorArea.cursorColumn
            selectionLength: editorArea.selectionLength
            cursorCount: editorArea.cursorCount
        }
    }

    // ---- 窗口级快捷键 ----
    // 显式序列而非 StandardKey 枚举：避免标准键多绑定歧义警告
    Shortcut {
        sequence: "Ctrl+N"
        onActivated: root.doNewDocument()
    }
    Shortcut {
        sequence: "Ctrl+O"
        onActivated: root.doOpenDialog()
    }
    Shortcut {
        sequence: "Ctrl+S"
        onActivated: root.doSave()
    }
    Shortcut {
        sequence: "Ctrl+Shift+S"
        onActivated: root.doSaveAs()
    }
    Shortcut {
        sequence: "Ctrl+W"
        onActivated: root.doCloseActive()
    }
    Shortcut {
        sequence: "Ctrl+Tab"
        onActivated: root.cycleTab(1)
    }
    Shortcut {
        sequence: "Ctrl+Shift+Tab"
        onActivated: root.cycleTab(-1)
    }
    Shortcut {
        sequence: "Ctrl+F"
        onActivated: root.doFind()
    }
    Shortcut {
        sequence: "Ctrl+H"
        onActivated: root.doReplace()
    }
    // F3 / Shift+F3：对话框关闭时全局跳转（DEC-009，沿用上次查找参数）；
    // 对话框打开时转发给对话框动作（与输入框 Enter 等价，此时焦点可能在
    // 选项开关或按钮上——选择转发而非仅靠输入框 Enter 处理）
    Shortcut {
        sequence: "F3"
        onActivated: {
            if (searchDialog.visible)
                searchDialog.doFindNext()
            else
                root.searchJumpGlobal(true)
        }
    }
    Shortcut {
        sequence: "Shift+F3"
        onActivated: {
            if (searchDialog.visible)
                searchDialog.doFindPrev()
            else
                root.searchJumpGlobal(false)
        }
    }

    // ---- 对话框 ----
    FileDialog {
        id: openFileDlg
        title: "打开文件"
        fileMode: FileDialog.OpenFiles
        nameFilters: DocumentManager.nameFilters()
        onAccepted: DocumentManager.openUrls(selectedFiles)
    }

    FileDialog {
        id: saveFileDialog
        title: "另存为"
        fileMode: FileDialog.SaveFile
        nameFilters: DocumentManager.nameFilters()

        onAccepted: {
            const doc = root.saveAsTarget
            root.saveAsTarget = null
            if (!doc)
                return
            if (DocumentManager.saveAs(doc, selectedFile)) {
                // 保存成功：若该保存服务于关闭/退出流程，继续流程
                if (root.closeFlowDoc === doc)
                    root.finishCloseFlow(doc)
            } else if (root.closeFlowDoc === doc) {
                // 写入失败：saveFailed 已弹错误对话框，中止关闭/退出，内容保留
                root.resetCloseFlow()
            }
        }
        onRejected: {
            // 取消另存为：中止关闭/退出流程，内容保留
            root.resetCloseFlow()
        }
    }

    MovableDialog {
        id: unsavedDialog
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        width: 380
        dialogTitle: "未保存的修改"

        contentItem: Label {
            text: root.closeFlowDoc
                  ? "是否要将更改保存到 " + root.closeFlowDoc.displayName + "？"
                  : ""
            wrapMode: Text.Wrap
            width: unsavedDialog.availableWidth
        }

        footer: DialogButtonBox {
            buttonLayout: DialogButtonBox.WinLayout
            DialogButton {
                text: "保存"
                primary: true
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            DialogButton {
                text: "不保存"
                DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
            }
            DialogButton {
                text: "取消"
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
        }

        onAccepted: {
            const doc = root.closeFlowDoc
            if (!doc)
                return
            if (doc.url && doc.url.toString().length > 0) {
                if (DocumentManager.save(doc)) {
                    root.finishCloseFlow(doc)
                } else {
                    // 保存失败：saveFailed 已弹错误，中止流程，文档保留
                    root.resetCloseFlow()
                }
            } else {
                // 无路径：自动转另存为（取消则中止关闭/退出）
                root.saveAsTarget = doc
                root.prefillSaveDialog(doc)
                saveFileDialog.open()
            }
        }
        onDiscarded: {
            const doc = root.closeFlowDoc
            if (doc)
                root.finishCloseFlow(doc)
        }
        onRejected: root.resetCloseFlow()
    }

    MovableDialog {
        id: errorDialog
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        width: 440
        dialogTitle: currentError ? currentError.title : ""

        property var currentError: null

        contentItem: Label {
            text: errorDialog.currentError ? errorDialog.currentError.message : ""
            wrapMode: Text.Wrap
            width: errorDialog.availableWidth
        }

        footer: DialogButtonBox {
            buttonLayout: DialogButtonBox.WinLayout
            DialogButton {
                text: "确定"
                primary: true
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
        }

        onAccepted: showNextError()
        onRejected: showNextError()

        function showNextError() {
            if (root.errorQueue.length > 0) {
                errorDialog.currentError = root.errorQueue.shift()
                errorDialog.open()
            } else {
                errorDialog.currentError = null
            }
        }
    }

    MovableDialog {
        id: ctxMenuPromptDialog
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        width: 380
        dialogTitle: "添加右键菜单"

        contentItem: Label {
            text: "是否将『用qcnote打开』加入资源管理器右键菜单？"
            wrapMode: Text.Wrap
            width: ctxMenuPromptDialog.availableWidth
        }

        footer: DialogButtonBox {
            buttonLayout: DialogButtonBox.WinLayout
            DialogButton {
                text: "添加"
                primary: true
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            DialogButton {
                text: "不添加"
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
        }

        onAccepted: {
            const r = ShellIntegration.applyContextMenuChoice(true)
            if (!r.ok)
                root.queueError("右键菜单设置失败", r.error)
        }
        onRejected: {
            const r = ShellIntegration.applyContextMenuChoice(false)
            if (!r.ok)
                root.queueError("右键菜单设置失败", r.error)
        }
    }

    // 按编码重新加载的未保存修改确认（DEC-008：确认在 QML 层；
    // 确认后 C++ 以磁盘内容覆盖内存未保存修改，取消则不动作）
    MovableDialog {
        id: reloadConfirmDialog
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        width: 380
        dialogTitle: "按编码重新加载"

        contentItem: Label {
            text: "通过 "
                  + (root.reloadEncoding === Document.Encoding.Gbk ? "GBK" : "UTF-8")
                  + " 重新加载将丢弃未保存的修改"
            wrapMode: Text.Wrap
            width: reloadConfirmDialog.availableWidth
        }

        footer: DialogButtonBox {
            buttonLayout: DialogButtonBox.WinLayout
            DialogButton {
                text: "重新加载"
                primary: true
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            DialogButton {
                text: "取消"
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
        }

        onAccepted: {
            const doc = root.reloadConfirmDoc
            root.reloadConfirmDoc = null
            if (doc && doc.url.toString().length > 0)
                DocumentManager.reloadWithEncoding(doc, root.reloadEncoding)
        }
        // 取消（含 Esc）：不动作，未保存修改保留
        onRejected: root.reloadConfirmDoc = null
    }

    // 草稿缓存冲突汇总对话框（DEC-006：单一对话框，「恢复」全量应用、
    // 「丢弃」全量丢弃，不做逐文件交互）：启动恢复时磁盘被外部修改/
    // 文件缺失的条目经 conflictsFound 汇总到此
    MovableDialog {
        id: draftConflictDialog
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape
        width: 440
        dialogTitle: "恢复未保存的修改"

        property var draftConflicts: []

        contentItem: Label {
            text: {
                const list = draftConflictDialog.draftConflicts
                if (list.length === 0)
                    return "" // 对话框只在有冲突时打开，空列表仅初始态
                const lines =
                        ["下列文件在上次退出时有未保存的修改，可以恢复为未保存的修改或丢弃："]
                for (let i = 0; i < list.length; ++i)
                    lines.push(list[i].name
                               + (list[i].missing ? "（文件已被删除或移动）" : ""))
                return lines.join("\n")
            }
            wrapMode: Text.Wrap
            width: draftConflictDialog.availableWidth
        }

        footer: DialogButtonBox {
            buttonLayout: DialogButtonBox.WinLayout
            DialogButton {
                text: "恢复"
                primary: true
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            DialogButton {
                text: "丢弃"
                DialogButtonBox.buttonRole: DialogButtonBox.DestructiveRole
            }
        }

        onAccepted: DraftCacheManager.resolveConflicts(true)
        onDiscarded: DraftCacheManager.resolveConflicts(false)
        // Esc = 稍后处理：不动作，条目保留在缓存目录，下次启动再次提示
        onRejected: {
        }
    }

    // 查找与替换对话框（DEC-002 顶部对齐模态）：参数与匹配状态由
    // EditorArea 会话级 SearchController 持有，本实例只做输入/展示/转发；
    // 打开与预填逻辑见 doFind/doReplace（openSearchDialog）
    SearchDialog {
        id: searchDialog
        controller: editorArea.searchController

        // 关闭统一出口（Esc 与「关闭」按钮均为 RejectRole → rejected）：
        // DEC-009——不调 reset()，控制器匹配状态保留供 F3 继续跳转；
        // 仅隐藏高亮层并把焦点还给活动编辑器
        onRejected: {
            editorArea.searchActive = false
            editorArea.focusActiveEditor()
        }
    }

    Connections {
        target: SingleInstance

        // 单实例激活请求（再次启动/打开转发）：仅前置窗口，URL 已在 C++ 打开
        function onActivationRequested() {
            root.raise()
            root.requestActivate()
        }
    }

    Connections {
        target: DocumentManager

        function onOpenFailed(url, error) {
            root.queueError("打开失败", url.toString() + "\n" + error)
        }
        function onSaveFailed(doc, error) {
            const target = doc && doc.url && doc.url.toString().length > 0
                           ? doc.url.toString()
                           : (doc ? doc.displayName : "")
            root.queueError("保存失败", target + "\n" + error)
        }
    }

    // 草稿缓存恢复冲突：汇总到 draftConflictDialog（DEC-006 统一决策）
    Connections {
        target: DraftCacheManager

        function onConflictsFound(conflicts) {
            draftConflictDialog.draftConflicts = conflicts
            draftConflictDialog.open()
        }
    }

    function queueError(title, message) {
        errorQueue.push({ title: title, message: message })
        if (!errorDialog.visible)
            errorDialog.showNextError()
    }

    // ---- 边缘缩放命中区（四边 + 四角） ----
    component ResizeHandle: Item {
        id: handle
        required property int edges

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            hoverEnabled: true
            cursorShape: {
                const e = handle.edges
                const horizontal = (e & Qt.LeftEdge) || (e & Qt.RightEdge)
                const vertical = (e & Qt.TopEdge) || (e & Qt.BottomEdge)
                if (horizontal && vertical) {
                    const forward = ((e & Qt.LeftEdge) && (e & Qt.TopEdge))
                                    || ((e & Qt.RightEdge) && (e & Qt.BottomEdge))
                    return forward ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor
                }
                if (horizontal)
                    return Qt.SizeHorCursor
                return Qt.SizeVerCursor
            }
            onPressed: (mouse) => {
                mouse.accepted = root.startSystemResize(handle.edges)
            }
        }
    }

    // 仅普通窗口状态可缩放（最大化/全屏时禁用）
    readonly property bool resizable: root.visibility === Window.Windowed

    // 四角（12px，优先命中，置于最上层）
    ResizeHandle {
        edges: Qt.LeftEdge | Qt.TopEdge
        width: 12; height: 12
        anchors.left: parent.left; anchors.top: parent.top
        z: 1000
        enabled: root.resizable
    }
    ResizeHandle {
        edges: Qt.RightEdge | Qt.TopEdge
        width: 12; height: 12
        anchors.right: parent.right; anchors.top: parent.top
        z: 1000
        enabled: root.resizable
    }
    ResizeHandle {
        edges: Qt.LeftEdge | Qt.BottomEdge
        width: 12; height: 12
        anchors.left: parent.left; anchors.bottom: parent.bottom
        z: 1000
        enabled: root.resizable
    }
    ResizeHandle {
        edges: Qt.RightEdge | Qt.BottomEdge
        width: 12; height: 12
        anchors.right: parent.right; anchors.bottom: parent.bottom
        z: 1000
        enabled: root.resizable
    }

    // 四边（6px）
    ResizeHandle {
        edges: Qt.LeftEdge
        width: 6
        anchors.left: parent.left
        anchors.top: parent.top; anchors.bottom: parent.bottom
        anchors.topMargin: 12; anchors.bottomMargin: 12
        z: 1000
        enabled: root.resizable
    }
    ResizeHandle {
        edges: Qt.RightEdge
        width: 6
        anchors.right: parent.right
        anchors.top: parent.top; anchors.bottom: parent.bottom
        anchors.topMargin: 12; anchors.bottomMargin: 12
        z: 1000
        enabled: root.resizable
    }
    ResizeHandle {
        edges: Qt.TopEdge
        height: 6
        anchors.top: parent.top
        anchors.left: parent.left; anchors.right: parent.right
        anchors.leftMargin: 12; anchors.rightMargin: 12
        z: 1000
        enabled: root.resizable
    }
    ResizeHandle {
        edges: Qt.BottomEdge
        height: 6
        anchors.bottom: parent.bottom
        anchors.left: parent.left; anchors.right: parent.right
        anchors.leftMargin: 12; anchors.rightMargin: 12
        z: 1000
        enabled: root.resizable
    }
}
