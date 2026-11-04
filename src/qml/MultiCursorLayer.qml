import QtQuick

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

    // ---- 选区段模型：[{x, y, w, h}]（selectionRanges / 文本 / 布局变化时重算） ----

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
        // positionToRectangle 是方法调用，其内部读取的文档布局状态不会被
        // QML 依赖追踪捕获；开启自动换行后布局随 wrapMode 切换、编辑器宽度
        // 变化而重排，显式读取下列属性保证重排后重算本绑定。wrap 关闭时
        // 这些值平时稳定，重算结果不变。
        const _layoutDeps = [ed.wrapMode, ed.width, ed.contentHeight]
        const contentRight = ed.width - ed.rightPadding

        // 行内子段统一出口：sR / eR 为子段起点、终点的光标矩形（同一可视行），
        // 宽 = eR.x − sR.x，h 取行高；空段（空行）不产生高亮，仅由光标表达。
        // toEdge：子段终点是折行断点且断点处字符可见时，本行精确右缘无法经
        // positionToRectangle 取得（见 push 内说明），以内容区右缘兜底。
        const emit = (sR, eR, toEdge) => {
            let w = eR.x - sR.x
            if (toEdge && w < contentRight - sR.x)
                w = contentRight - sR.x
            if (w > 0)
                segs.push({ x: sR.x, y: sR.y, w: w, h: sR.height })
        }

        const push = (a, b) => {
            if (b <= a)
                return
            const r1 = ed.positionToRectangle(a)
            const r2 = ed.positionToRectangle(b)
            // 同一可视行的行内矩形。wrap 关闭时每个逻辑行恒为单可视行，恒走
            // 此路径，计算与旧实现完全一致（零回归）；wrap 开启但段内无折行
            // 断点时同理。
            if (r1.y === r2.y) {
                emit(r1, r2, false)
                return
            }
            // 跨可视行（仅自动换行开启时出现）：同一逻辑行折成多个可视行，
            // 终点矩形 r2 已落到后续可视行、x 回到行首，需在 y 跳变处拆成
            // 每可视行一段，保证每个子段的起止矩形都落在同一可视行内，再
            // 各自按上面的行内矩形逻辑计算。
            //
            // 断点定位用二分而非逐 position 扫描：同一逻辑行内
            // positionToRectangle(p).y 随 p 单调不减（文本布局折行只进
            // 不退，已验证），故「相邻对 (q-1, q) 处 y 跳变」等价于「q
            // 是 (p, b] 内第一个 y 大于当前行 y 的位置」，二分即可定位。
            // 单个断点 O(log n)、整段 O(k·log n)（k=段内可视行数，
            // n=段长），大文档 wrap + 多光标下每键整体重算不再随段长线性
            // 放大（逐位扫描版每 position 一次 QML→C++ 调用，10 万字符
            // 逻辑行每键可达 5 万~20 万次调用、主线程阻塞数十至数百毫秒）。
            let p = a                        // 当前子段起点 position
            let sR = r1                      // 当前子段起点矩形（= R(p)）
            while (true) {
                // 在 (p, b] 内二分找第一个 y 大于 sR.y 的 position。
                // 不变量：该 position（若存在）总落在 (lo, hi] 内，且
                // y(lo) <= sR.y。mid 处 y 更大则收缩 hi，否则（单调性
                // 下只能相等）收缩 lo；收敛到 hi = lo+1 后，y(hi) > sR.y
                // 则 hi 即断点后首位置，否则 (p, b] 内无跳变、hi 必为 b。
                let lo = p
                let hi = b
                while (hi - lo > 1) {
                    const mid = (lo + hi) >> 1
                    if (ed.positionToRectangle(mid).y > sR.y)
                        hi = mid
                    else
                        lo = mid
                }
                const nR = hi === b ? r2 : ed.positionToRectangle(hi)
                if (nR.y === sR.y) {
                    // 末段终点不在折行断点上（断点已逐个在下方处理），
                    // 行内精确。
                    emit(sR, r2, false)
                    return
                }
                // hi-1 是本可视行最后一个光标位置，行右缘分两种情况：
                // - 折行发生在空白处（单词边界换行，空白留行尾且不显示）：
                //   hi-1 即空白位置，其矩形 x 已是可见文本右缘，精确；
                // - 折行发生在可见字符后（中文逐字折行 / 超长单词内强制
                //   断行）：Qt 把 hi 归下一可视行，任何 position 的矩形
                //   都取不到本行精确右缘（R(hi-1).x 少算最后一个可见
                //   字符），以内容区右缘兜底——强制断行发生在行宽耗尽
                //   处，超界不超过一个字符宽，视觉上等同“选中到行尾”。
                emit(sR, ed.positionToRectangle(hi - 1),
                     !(/\s/.test(text[hi - 1])))
                sR = nR
                p = hi
                if (p === b) {
                    // 段终点 b 恰为下一可视行行首：末段为空（sR === nR），
                    // emit 按 w<=0 跳过，与逐位扫描版出口一致。
                    emit(sR, nR, false)
                    return
                }
            }
        }
        const ranges = layer.selectionPairs(ctrl.selectionRanges)
        for (let i = 0; i < ranges.length; ++i) {
            const pair = ranges[i]
            const start = pair[0]
            const end = pair[1]
            let segStart = start
            // 跨行选区按 '\n' 拆成每逻辑行一段（逻辑行内再由 push 按可视行拆段）
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
