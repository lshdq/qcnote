.pragma library

/**
 * 高亮覆盖层共享拆段算法（纯函数库，无状态，不依赖导入方上下文）。
 *
 * 职责：把「文档区间对列表」转换为「每可视行一段的矩形列表」。
 * 输入：textEditor（TextEdit）引用 + [[start,end],...]；
 * 输出：[{x, y, w, h},...]。
 * 供 MultiCursorLayer（多光标选区高亮）与 SearchHighlightLayer（搜索
 * 匹配高亮）共用，算法自 MultiCursorLayer.selectionSegments 原样提取
 * （含 y 跳变二分、toEdge 兜底、空段不产生高亮等全部细节），两处调用
 * 不得引入语义分叉；如需调整，改这里并同步验证两个调用方。
 *
 * 坐标约定：textEditor 必须是覆盖层所在的 TextArea（覆盖层作为其子
 * Item 使用）；positionToRectangle 返回值已含 padding，直接作为子 Item
 * 坐标使用。
 *
 * 依赖追踪注意：positionToRectangle 是方法调用，其内部读取的文档布局
 * 状态不会被 QML 依赖追踪捕获；开启自动换行后布局随 wrapMode 切换、
 * 编辑器宽度变化而重排，segmentsForRanges 在函数体内显式读取
 * wrapMode / width / contentHeight（见 _layoutDeps），因此调用方必须在
 * property 绑定求值期间同步调用本函数，这些读取才能被绑定捕获、重排后
 * 重算绑定（绑定求值期间经由被调函数访问 QObject 属性同样计入依赖，
 * 此为 QML 既有行为，.pragma library 不影响该追踪）。wrap 关闭时这些
 * 值平时稳定，重算结果不变。
 */

/**
 * 兼容解析区间列表：来源（C++ QVariantList 或 QML 数组）内嵌
 * [start,end] 对，但经 QML 引擎边界可能被展平为 [s1,e1,s2,e2,...]；
 * 两种形态（嵌套数组 / 展平数字串）都还原为 [[start,end],...]。
 * 非数组 / 空数组返回 []。
 */
function parsePairs(raw) {
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

/**
 * 区间对列表 → 每可视行矩形段列表 [{x,y,w,h},...]。
 *
 * 拆段规则（与提取自 MultiCursorLayer 的原实现一致）：
 * - 先按 '\n' 拆成每逻辑行一段；开启自动换行后逻辑行还会跨多个可视行，
 *   再按 positionToRectangle 的 y 跳变处拆成每可视行一段（逻辑行内 y
 *   随 position 单调不减，断点经二分定位，整段 O(k·log n)），逐段取
 *   行内矩形（x=段起光标 x，宽=段末光标 x − 段起 x；段末为行末/折行点
 *   时该 x 即行末文本右缘，全行选中自然延伸到本行文本宽度）。
 * - 空段（空行 / 零长度区间 b<=a 或 w<=0）不产生高亮。
 * - 折行断点若发生在可见字符后（中文逐字折行 / 超长单词强制断行），
 *   本行精确右缘无法经 positionToRectangle 取得，以内容区右缘兜底。
 */
function segmentsForRanges(textEditor, pairs) {
    const segs = []
    const ed = textEditor
    if (!ed || !(pairs instanceof Array))
        return segs
    const text = ed.text
    // 见文件头「依赖追踪注意」：这三项读取必须在绑定求值期间发生，
    // 调用方须在 property 绑定内同步调用本函数。
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
        // 此路径，计算与提取前的内联实现完全一致（零回归）；wrap 开启但
        // 段内无折行断点时同理。
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
    for (let i = 0; i < pairs.length; ++i) {
        const pair = pairs[i]
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
