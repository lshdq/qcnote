import QtQuick
import "HighlightSegments.js" as HighlightSegments

/**
 * 多光标覆盖层（纯视觉组件）。
 *
 * 职责：把 MultiCursorController 的光标 / 选区状态渲染为覆盖图形：
 * - 额外光标：controller.cursorPositions 中与 textEditor.cursorPosition
 *   重合的主光标由 TextArea 原生渲染，其余每个光标画一条 1px 竖线，
 *   整体以 500ms 显 / 隐节奏闪烁（与原生光标节奏一致）；
 * - 选区高亮：controller.selectionRanges 的每段 [start,end] 先按 '\n' 拆成
 *   每逻辑行一段；开启自动换行后逻辑行还会跨多个可视行，再按
 *   positionToRectangle 的 y 跳变处拆成每可视行一段（逻辑行内 y 随
 *   position 单调不减，断点经二分定位，整段 O(k·log n)），逐段取行内
 *   矩形填充
 *   （x=段起光标 x，宽=段末光标 x − 段起 x；段末为行末/折行点时该 x 即
 *   行末文本右缘，全行选中自然延伸到本行文本宽度），z 低于光标。
 *   拆段算法共享于 HighlightSegments.js（与 SearchHighlightLayer 复用
 *   同一份），实现细节与依赖追踪注意点见该文件。
 *
 * 坐标约定：必须作为 TextArea 的子 Item 使用；positionToRectangle 返回
 * 值已含 padding，直接作为子 Item 坐标使用即可。本项目 TextArea 位于
 * ScrollView 内（整项随内容滚动），覆盖层随文本自然跟随。
 *
 * 接口细节：controller.selectionRanges 在 C++ 侧为内嵌 [start,end] 对的
 * QVariantList，但经 QML 引擎边界可能被展平为 [s1,e1,s2,e2,...]，
 * selectionPairs()（委托 HighlightSegments.parsePairs）对两种形态做
 * 兼容解析。
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

    // ---- 选区段模型：[{x, y, w, h}]（selectionRanges / 文本 / 布局变化时重算） ----

    /// 兼容解析 selectionRanges：控制器交付的 QVariantList 内嵌 [start,end]
    /// 对，但经 QML 引擎边界可能被展平为 [s1,e1,s2,e2,...]；两种形态
    /// （嵌套数组 / 展平数字串）都还原为 [[start,end],...]。
    /// 实现共享于 HighlightSegments.parsePairs。
    function selectionPairs(raw) {
        return HighlightSegments.parsePairs(raw)
    }

    /// 拆段算法（按 '\n' 拆逻辑行、wrap 后按 y 跳变二分拆可视行、逐段
    /// 取行内矩形、toEdge 兜底、空段不产生高亮）共享于
    /// HighlightSegments.segmentsForRanges；其内部显式读取
    /// wrapMode / width / contentHeight 保证重排后重算本绑定（依赖
    /// 追踪注意点见该文件头）。
    readonly property var selectionSegments: {
        const ed = layer.textEditor
        const ctrl = layer.controller
        if (!ed || !ctrl)
            return []
        return HighlightSegments.segmentsForRanges(
                    ed, HighlightSegments.parsePairs(ctrl.selectionRanges))
    }

    // ---- 额外光标模型：[{x, y, h}]（跳过与原生主光标重合者） ----
    readonly property var extraCursorRects: {
        const rects = []
        const ed = layer.textEditor
        const ctrl = layer.controller
        if (!ed || !ctrl)
            return rects
        // 同 selectionSegments：positionToRectangle 的内部布局状态不被依赖
        // 追踪，显式读取布局相关属性，保证换行开关切换 / 宽度变化重排后
        // 额外光标位置随之重算。
        const _layoutDeps = [ed.wrapMode, ed.width, ed.contentHeight]
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
