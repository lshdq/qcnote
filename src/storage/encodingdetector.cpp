#include "storage/encodingdetector.h"

#include <QStringDecoder>

EncodingDetector::DetectionResult EncodingDetector::detect(const QByteArray &raw)
{
    DetectionResult result;

    // ① BOM 优先：显式字节比对（不依赖解码器自身的 BOM 处理行为，行为确定）。
    // 带 BOM 即权威判定为 UTF-8：BOM 是写入方主动添加的编码声明，即使后续字节
    // 含非法 UTF-8 序列，也按 UTF-8 解码（非法序列呈现为 U+FFFD），不再回退 GBK。
    static const QByteArray bom("\xEF\xBB\xBF", 3);
    if (raw.startsWith(bom)) {
        result.encoding = Document::Encoding::Utf8;
        result.hasBom = true;
        return result;
    }

    // ② 严格 UTF-8 校验：全量解码无错误（含纯 ASCII——ASCII 是 UTF-8 子集）。
    // 要点 1：decode() 返回惰性包装（EncodedData），必须落到 QString 才真正解码，
    //         丢弃返回值时什么都不做、hasError() 恒 false（Qt 6.11 实证行为）；
    // 要点 2：Flag::Stateless 使“尾部截断的多字节序列”也计为错误——默认有状态
    //         解码器会静默吞掉悬空字节并判为合法 UTF-8，导致内容被静默截断；
    //         Stateless 下此类文件回退 GBK 路径，悬空字节以 U+FFFD/汉字呈现。
    //         非法序列本身仍按 Qt 默认替换为 U+FFFD 并置 hasError，正好用作校验器。
    QStringDecoder decoder(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    const QString text = decoder.decode(raw); // 显式落盘到 QString，触发真实解码
    if (!decoder.hasError()) {
        Q_UNUSED(text);
        result.encoding = Document::Encoding::Utf8;
        result.hasBom = false;
        return result;
    }

    // ③ 其余按 GBK 处理：GBK 双字节结构几乎总能容纳任意高位字节序列，
    // 无法靠“解码成功”进一步区分，回退判定即最终判定（DEC-002）
    result.encoding = Document::Encoding::Gbk;
    result.hasBom = false;
    return result;
}
