// qcnote 纯逻辑核心层：多光标纯逻辑（M3-A）。
//
// 语义移植自 Qt 版 MultiCursorEngine 的可纯化部分；按方案 6.3（DEC-010），
// Scintilla 原生 multiple selection 承担渲染/同步编辑/撤销合并，本层只计算
// 「目标光标/选区集合」，交互映射（Alt+Click/拖拽列选/期望 x 粘性等）在
// M3-B 的编辑器接线层完成。
//
// 坐标约定（与查找引擎一致）：偏移一律 UTF-8 字节偏移（int，文档 < 2GB）；
// 「列」一律字符列（Unicode 码点列，非字节列——对齐旧版「保持当前列」语义）。
// 行 = 以 '\n' 分隔（内容契约恒 LF），行尾 = 行分隔符之前。
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace core {

/// 光标/选区：anchor == caret 为空选区（纯光标）；UTF-8 字节偏移。
struct SelRange {
    int anchor = 0;
    int caret = 0;

    bool operator==(const SelRange&) const = default;
};

/// Ctrl+Alt+Up/Down：每个光标上/下移一行同字符列处各加一个空光标（短行
/// 钳制行末、不越界到下一行；已在首/末行的光标跳过）。返回合并后的集合
///（按 caret 升序、anchor 升序去重）；无新增时与输入集合等价。
std::vector<SelRange> multicursorAddLine(const std::string& content,
                                         const std::vector<SelRange>& current, bool up);

/// Shift+Alt+I：单个选区 [min(anchor,caret), max(anchor,caret)] 覆盖到的每一行
///（含最后一行）行尾（换行符之前）各插一个空光标。
std::vector<SelRange> multicursorLineEnds(const std::string& content, int anchor, int caret);

/// Ctrl+D：返回下一个要追加的相同词选区（环绕整个文档、大小写敏感精确
/// 字节匹配、目标不含换行不跨行；与任一现有选区**严格重叠**的命中跳过——
/// 端点相接不算重叠、允许追加，如 "foofoo" 选 [0,3) 后可追加 [3,6)）。
/// 目标取按位置排序最末的非空选区；无任何非空选区时直接返回最末光标处的
/// 单词本身（wordRangeAt——首次 Ctrl+D「先选当前词」；多光标全空时 M3-B
/// 应按此逐光标选词，本函数只取最末一个）。无可追加目标（全部命中被占用/
/// 无词/跨行）返回 nullopt。
std::optional<SelRange> multicursorNextOccurrence(const std::string& content,
                                                  const std::vector<SelRange>& selections);

/// Ctrl+Shift+L：目标文本（最末选区文本；空选区则最末光标处单词）的全部
/// 非重叠出现（从左到右）各生成一个选区，替换整个选区集合。无目标时
/// 原样返回输入集合。
std::vector<SelRange> multicursorAllOccurrences(const std::string& content,
                                                const std::vector<SelRange>& selections);

/// Esc 退出多光标：保留按（caret, anchor）升序排序最末的一个光标
///（保留其 anchor/caret，可能是选区）。空输入返回 {0,0}（调用方不应在
/// 空选区集上调用）。
SelRange multicursorCollapse(const std::vector<SelRange>& selections);

/// 粘贴分发判定（纯判定）：剪贴板行数 == 光标数 → 逐行分发；否则每个光标
/// 整份插入。行数为 0（空剪贴板）恒不分发。
bool multicursorPasteDistributes(int clipboardLines, int caretCount);

/// 光标处单词范围 [anchor=start, caret=end)：中英文词边界按旧版 WordBoundary
/// 语义——字母/数字/下划线/符号类（L*/N*/M*/S*，如 $、￥）并入词字符，
/// 标点（P*，含中文标点 ，。、）与空白（Z*，含换行）为分隔符；中文按
/// 「连续非空白非标点」成词。不在词上（空白/标点/文末/负越界）返回
/// nullopt；position 落在多字节序列中间时解析为该字符（防御，契约下光标
/// 不会落在序列中间）。
std::optional<SelRange> wordRangeAt(const std::string& content, int position);

}  // namespace core
