#pragma once

#include <QByteArray>

#include "document/document.h"

/**
 * @brief 文件编码自动识别（DEC-002/DEC-003：识别在存储层、独立成类）。
 *
 * 纯静态工具类（无 QObject）：未来网络存储扩展复用同一识别逻辑。
 * 策略（按优先级）：
 * ① 前 3 字节为 UTF-8 BOM（EF BB BF）→ UTF-8 + BOM（BOM 为权威标记，不再校验）；
 * ② 剥离 BOM 后的字节经严格 UTF-8 校验全部合法（含纯 ASCII）→ UTF-8 无 BOM；
 * ③ 其余 → GBK（本类只判定候选，GBK 解码由 LocalStorage 调 GbkCodec 完成）。
 *
 * 已知误判（预期行为，DEC-002）：GBK 字节序列恰好构成合法 UTF-8 时判为 UTF-8，
 * 由用户手动“按编码重新加载”兜底纠正。
 */
class EncodingDetector
{
public:
    struct DetectionResult
    {
        Document::Encoding encoding = Document::Encoding::Utf8;
        bool hasBom = false; ///< 仅 UTF-8 有意义（GBK 无 BOM 概念）
    };

    /// 识别原始字节流的编码。空输入判为 UTF-8（无 BOM）——空文件是合法 UTF-8。
    static DetectionResult detect(const QByteArray &raw);
};
