import QtQuick
import "HighlightSegments.js" as HighlightSegments

/**
 * 搜索匹配高亮覆盖层（纯视觉组件）。
 *
 * 职责：渲染查找功能的全部匹配与当前匹配强化：
 * - 全部匹配：matchRanges 每段 [start,end] 拆成每可视行一段的行内矩形，
 *   填半透明底色 AppTheme.searchMatch；
 * - 当前匹配：currentRange 各拆段矩形上叠绘 1px 描边
 *   AppTheme.searchMatchBorder——「有描边 = 当前」是非颜色线索的区分
 *   （形状线索），底色 / 描边的色相差异仅是辅助。
 *
 * 拆段算法与 MultiCursorLayer 复用同一份共享实现
 * （HighlightSegments.js：按 '\n' 拆逻辑行、wrap 后按 y 跳变二分拆可视
 * 行、逐段取行内矩形、toEdge 兜底、空段不产生高亮——零长度匹配即便
 * 传入也不产生高亮）。
 *
 * 坐标约定：必须作为 TextArea 的子 Item 使用（anchors.fill: parent）；
 * positionToRectangle 返回值已含 padding，直接作为子 Item 坐标使用即可。
 * 本项目 TextArea 位于 ScrollView 内（整项随内容滚动），覆盖层随文本
 * 自然跟随。matchSegments / currentSegments 经
 * HighlightSegments.segmentsForRanges 显式读取 textEditor 的
 * wrapMode / width / contentHeight，保证自动换行开关切换 / 编辑器宽度
 * 变化重排后重算（依赖追踪注意点见该文件头）。
 *
 * z 序说明：本层是 TextArea 的子 Item，绘制于文本绘制层（含原生选区）
 * 之上；但底色为半透明，其下的原生选区仍透出可见。当前匹配的主表达
 * 是 TextArea 原生选区（由查找逻辑选中并滚动到可见），本层描边只是
 * 强化定位的非颜色线索，两者叠加不冲突。
 *
 * 性能边界：本组件不做匹配数上限逻辑（纯视觉，按传入区间如实绘制）；
 * 匹配数超大时由调用方裁剪 matchRanges（需求约定：匹配数 > 10000 时
 * 不再传入全部区间，导航与替换仍可用）。万级矩形逐个实例化的绘制与
 * 绑定开销显著，建议调用方严格遵守该约定。
 *
 * 接口细节：matchRanges 的来源区间列表经 QML 引擎边界可能被展平为
 * [s1,e1,s2,e2,...]，兼容解析同 MultiCursorLayer.selectionPairs
 * （HighlightSegments.parsePairs 对嵌套 / 展平两种形态都还原为
 * [[start,end],...]）。
 *
 * 边界：纯视觉组件，不含任何 MouseArea、不消费任何事件；
 * active 为 false 时整体隐藏且不计算任何段。
 */
Item {
    id: layer

    /// 目标编辑器（TextArea，经 TextEdit 类型引用）
    required property TextEdit textEditor
    /// 全部匹配区间：[[start,end],...]（兼容展平 [s1,e1,s2,e2,...] 形态）
    required property var matchRanges
    /// 当前匹配区间：[start,end]；null / undefined（或非法形态）表示无
    /// 当前匹配——仅绘制全部匹配底色，不绘制描边
    required property var currentRange
    /// 查找功能是否激活（false 时整体隐藏、不计算段）
    required property bool active

    visible: layer.active

    anchors.fill: parent

    // ---- 匹配段模型：[{x, y, w, h},...]（matchRanges / 文本 / 布局变化时重算） ----

    readonly property var matchSegments: {
        const ed = layer.textEditor
        if (!layer.active || !ed)
            return []
        return HighlightSegments.segmentsForRanges(
                    ed, HighlightSegments.parsePairs(layer.matchRanges))
    }

    // ---- 当前匹配段模型：[{x, y, w, h},...]（currentRange / 文本 / 布局变化时重算） ----

    readonly property var currentSegments: {
        const ed = layer.textEditor
        if (!layer.active || !ed)
            return []
        const cur = layer.currentRange
        // null / undefined / 非法形态都视为无当前匹配（不绘制描边）
        if (!(cur instanceof Array) || cur.length === 0)
            return []
        // 单对 [start,end]；顺带兼容引擎边界可能包裹出的嵌套
        // [[start,end]] 形态（嵌套时取内层对）
        const pair = cur[0] instanceof Array ? cur[0] : cur
        if (pair.length < 2)
            return []
        return HighlightSegments.segmentsForRanges(ed, [pair])
    }

    // ---- 全部匹配底色（半透明；先声明 → 位于描边之下） ----

    Repeater {
        model: layer.matchSegments

        Rectangle {
            x: modelData.x
            y: modelData.y
            width: modelData.w
            height: modelData.h
            color: AppTheme.searchMatch
        }
    }

    // ---- 当前匹配描边（1px；后声明 → 位于底色之上） ----

    Repeater {
        model: layer.currentSegments

        Rectangle {
            x: modelData.x
            y: modelData.y
            width: modelData.w
            height: modelData.h
            // 透明为结构性用色（仅描边不填充），非主题色
            color: "transparent"
            border.width: 1
            border.color: AppTheme.searchMatchBorder
        }
    }
}
