import QtQuick

/**
 * 多光标覆盖层（纯视觉组件）。
 *
 * 职责：把 MultiCursorController 的光标 / 选区状态渲染为覆盖图形：
 * - 额外光标：controller.cursorPositions 中与 textEditor.cursorPosition
 *   重合的主光标由 TextArea 原生渲染，其余每个光标画一条 1px 竖线，
 *   整体以 500ms 显 / 隐节奏闪烁（与原生光标节奏一致）；
 * - 选区高亮：controller.selectionRanges 的每段 [start,end] 按 '\n' 拆成
 *   每行一段，逐段取 positionToRectangle 的行内矩形填充
 *   （x=段起光标 x，宽=段末光标 x − 段起 x；段末为行末时该 x 即行末
 *   文本右缘，全行选中自然延伸到本行文本宽度），z 低于光标。
 *
 * 坐标约定：必须作为 TextArea 的子 Item 使用；positionToRectangle 返回
 * 值已含 padding，直接作为子 Item 坐标使用即可。本项目 TextArea 位于
 * ScrollView 内（整项随内容滚动），覆盖层随文本自然跟随。
 *
 * 接口细节：controller.selectionRanges 在 C++ 侧为内嵌 [start,end] 对的
 * QVariantList，但经 QML 引擎边界可能被展平为 [s1,e1,s2,e2,...]，
 * selectionPairs() 对两种形态做兼容解析。
 *
 * 边界：纯视觉组件，不含任何 MouseArea、不消费任何事件；
 * controller 未接管（active 为 false）或无光标时整体隐藏。
 */
Item {
    id: layer

    /// 目标编辑器（TextArea，经 TextEdit 类型引用）
    required property TextEdit textEditor
    /// 多光标控制器
    required property MultiCursorController controller

    /// 引擎未接管时无任何可见元素：原生渲染即全部状态（单光标零变化）
    visible: layer.controller && layer.controller.active
             && layer.controller.cursorCount > 0

    anchors.fill: parent

    // ---- 选区段模型：[{x, y, w, h}]（selectionRanges / 文本变化时重算） ----

    /// 兼容解析 selectionRanges：控制器交付的 QVariantList 内嵌 [start,end]
    /// 对，但经 QML 引擎边界可能被展平为 [s1,e1,s2,e2,...]；
    /// 两种形态（嵌套数组 / 展平数字串）都还原为 [[start,end],...]
    function selectionPairs(raw) {
        const pairs = []
        if (!(raw instanceof Array) || raw.length === 0)
            return pairs
        if (raw[0] instanceof Array) {
            for (let i = 0; i < raw.length; ++i)
                pairs.push([raw[i][0], raw[i][1]])
        } else {
            for (let i = 0; i + 1 < raw.length; i += 2)
                pairs.push([raw[i], raw[i + 1]])
        }
        return pairs
    }

    readonly property var selectionSegments: {
        const segs = []
        const ed = layer.textEditor
        const ctrl = layer.controller
        if (!ed || !ctrl)
            return segs
        const text = ed.text
        const push = (a, b) => {
            if (b <= a)
                return
            const r1 = ed.positionToRectangle(a)
            const r2 = ed.positionToRectangle(b)
            const w = r2.x - r1.x
            // 同一行的行内矩形；空段（空行）不产生高亮，仅由光标表达
            if (w > 0 && r1.y === r2.y)
                segs.push({ x: r1.x, y: r1.y, w: w, h: r1.height })
        }
        const ranges = layer.selectionPairs(ctrl.selectionRanges)
        for (let i = 0; i < ranges.length; ++i) {
            const pair = ranges[i]
            const start = pair[0]
            const end = pair[1]
            let segStart = start
            // 跨行选区按 '\n' 拆成每行一段
            for (let p = start; p < end; ++p) {
                if (text[p] === "\n") {
                    push(segStart, p)
                    segStart = p + 1
                }
            }
            push(segStart, end)
        }
        return segs
    }

    // ---- 额外光标模型：[{x, y, h}]（跳过与原生主光标重合者） ----
    readonly property var extraCursorRects: {
        const rects = []
        const ed = layer.textEditor
        const ctrl = layer.controller
        if (!ed || !ctrl)
            return rects
        const primary = ed.cursorPosition
        const positions = ctrl.cursorPositions
        for (let i = 0; i < positions.length; ++i) {
            if (positions[i] === primary)
                continue
            const r = ed.positionToRectangle(positions[i])
            rects.push({ x: r.x, y: r.y, h: r.height })
        }
        return rects
    }

    // ---- 选区高亮（z 低于光标） ----
    Repeater {
        model: layer.selectionSegments

        Rectangle {
            x: modelData.x
            y: modelData.y
            width: modelData.w
            height: modelData.h
            color: AppTheme.multiSelection
        }
    }

    // ---- 额外光标 ----
    Item {
        id: cursorLayer

        Repeater {
            model: layer.extraCursorRects

            Rectangle {
                x: modelData.x
                y: modelData.y
                width: 1
                height: modelData.h
                color: AppTheme.multiCursor
            }
        }

        // 整体闪烁；隐藏（退出多光标）时停在完全不透明，重新进入即立即可见
        SequentialAnimation {
            running: layer.visible
            loops: Animation.Infinite
            alwaysRunToEnd: true

            NumberAnimation {
                target: cursorLayer
                property: "opacity"
                from: 1
                to: 0
                duration: 500
                easing.type: Easing.Linear
            }
            NumberAnimation {
                target: cursorLayer
                property: "opacity"
                from: 0
                to: 1
                duration: 500
                easing.type: Easing.Linear
            }
            onStopped: cursorLayer.opacity = 1
        }
    }
}
