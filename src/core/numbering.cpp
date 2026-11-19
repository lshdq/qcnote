// qcnote 列模式填充编号文本生成实现。要点：
// - 溢出钳制（DEC-007）：安全范围 [-10^18, 10^18]；start/step 先各自钳制，
//   之后逐元素递增前先判断（加法只对 |v|、|step| ≤ 10^18 的操作数执行，
//   判断式中的 LIMIT±step ∈ [-2e18, 2e18] 未超 long long，无有符号溢出 UB）；
//   一旦触界且继续同向递增则保持边界值；
// - 补零宽度：全体编号绝对值最大者的十进制位数；负号不计入宽度。
#include "core/numbering.h"

#include <algorithm>
#include <string>

namespace core {
namespace {

// 安全边界 ±10^18（DEC-007）：远小于 LLONG_MAX ≈ 9.2×10^18，
// 保证边界值上的加减判断不溢出。
constexpr long long kLimit = 1'000'000'000'000'000'000LL;  // 10^18

long long clampToLimit(long long v) {
    return std::clamp(v, -kLimit, kLimit);
}

// |v| 的十进制位数（0 → 1）。
int digitCount(long long v) {
    unsigned long long a = v < 0 ? static_cast<unsigned long long>(-(v + 1)) + 1
                                 : static_cast<unsigned long long>(v);
    int n = 1;
    while (a >= 10) {
        a /= 10;
        ++n;
    }
    return n;
}

// v → 可选补零的十进制文本（负号在补零宽度之外）。
std::string formatNumber(long long v, int padWidth) {
    const bool neg = v < 0;
    unsigned long long a = neg ? static_cast<unsigned long long>(-(v + 1)) + 1
                               : static_cast<unsigned long long>(v);
    std::string digits;
    do {
        digits.push_back(static_cast<char>('0' + a % 10));
        a /= 10;
    } while (a > 0);
    while (static_cast<int>(digits.size()) < padWidth)
        digits.push_back('0');
    std::reverse(digits.begin(), digits.end());
    return neg ? "-" + digits : digits;
}

}  // namespace

std::vector<std::string> numberingTexts(int count, long long start, long long step,
                                        bool padZero) {
    std::vector<std::string> out;
    if (count <= 0)
        return out;
    out.reserve(static_cast<std::size_t>(count));

    long long v = clampToLimit(start);
    const long long s = clampToLimit(step);

    // 先算全体值（含钳制），再统一求补零宽度（绝对值最大者的位数）。
    std::vector<long long> values;
    values.reserve(static_cast<std::size_t>(count));
    int width = 1;
    for (int i = 0; i < count; ++i) {
        width = std::max(width, digitCount(v));
        values.push_back(v);
        // 递增前先判断（v、s 已在 ±10^18 内，判断式不溢出）
        if (s > 0 && v > kLimit - s)
            v = kLimit;
        else if (s < 0 && v < -kLimit - s)
            v = -kLimit;
        else
            v += s;
    }

    for (const long long value : values)
        out.push_back(formatNumber(value, padZero ? width : 1));
    return out;
}

}  // namespace core
