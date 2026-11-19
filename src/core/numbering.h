// qcnote 纯逻辑核心层：列模式填充编号文本生成（纯函数，不依赖 UI/Win32/Scintilla）。
//
// 供「编辑▸插入编号」功能的多光标填充使用：按位置顺序第 i 个选区（i 从 0 起）
// 填入 起始值 + i × 增量 的十进制文本。对话框层已校验 |起始值| ≤ 10^15、
// |增量| ≤ 10^6（DEC-008），本层对极端参数做防御性钳制（不信任调用方，DEC-007）。
#pragma once

#include <string>
#include <vector>

namespace core {

/// 生成 count 个编号文本，第 i 个 = start + i×step（i 从 0 起）。
///
/// - count ≤ 0：返回空 vector；
/// - padZero=false：普通十进制文本（如 "-3"、"1"）；
/// - padZero=true（DEC-002）：补零宽度 = 全体编号绝对值最大者的十进制位数，
///   各编号**数字部分**左侧补 '0' 至该宽度，负号在宽度之外
///   （{1..100} → "001".."100"；{-12,-6,0,6} → "-12","-06","00","06"；
///    宽度 3 时 -5 → "-005"）；
/// - 溢出防护（DEC-007）：long long 运算，若某元素值超出安全范围
///   [-10^18, 10^18] 则钳制到边界值；运算先判断再执行，无有符号溢出 UB；
/// - 文本为 UTF-8 ASCII 数字，无本地化分组符。
std::vector<std::string> numberingTexts(int count, long long start, long long step,
                                        bool padZero);

}  // namespace core
