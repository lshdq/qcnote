#include "platform/gbkcodec.h"

#include <QByteArray>
#include <QChar>

#include <climits>

// windows.h 的 min/max 宏会破坏标准库头，NOMINMAX + LEAN_AND_MEAN 是 Qt 下的常规防御
#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef Q_OS_WIN

static_assert(sizeof(wchar_t) == 2,
              "Windows 构建下 wchar_t 必须为 16 位，QString(UTF-16) 才能与 WideChar 系 API 直接互转");

namespace {

/// Windows CP936 代码页编号（GBK 的 Windows 官方实现，兼容 GBK/GB2312 文件）
constexpr UINT kCp936 = 936;

/// CP936 是否可用（进程内一次探测）。
/// 代码页 936 是 Windows 内置 NLS 表、任意 locale 下均应存在；此处防御极端裁剪系统，
/// 用 GBK 标准样本字节（“你”= C4 E3）做严格解码探测，失败即认定编解码完全无法工作。
bool cp936Available()
{
    static const bool available = [] {
        const char probe[] = "\xC4\xE3"; // “你”的 GBK 编码
        wchar_t buf[2] = {};
        return MultiByteToWideChar(kCp936, MB_ERR_INVALID_CHARS, probe, 2, buf, 2) == 1
               && buf[0] == 0x4F60;
    }();
    return available;
}

/// 整块严格解码（MB_ERR_INVALID_CHARS：遇非法或未指派序列整体失败）。
/// 尺寸由调用方保证 <= INT_MAX；size 为 0 时视为成功（产出空串）。
bool decodeStrict(const char *data, int size, QString *out)
{
    if (size == 0) {
        out->clear();
        return true;
    }
    const int len = MultiByteToWideChar(kCp936, MB_ERR_INVALID_CHARS, data, size, nullptr, 0);
    if (len <= 0)
        return false;
    out->resize(len);
    const int written = MultiByteToWideChar(kCp936, MB_ERR_INVALID_CHARS, data, size,
                                             reinterpret_cast<wchar_t *>(out->data()), len);
    return written == len;
}

/// GBK 双字节结构的次字节判定：0x40–0xFE 且非 0x7F（GBK 规范区间）
inline bool isGbkTrailByte(unsigned char b)
{
    return b >= 0x40 && b <= 0xFE && b != 0x7F;
}

/// 单个字符单元（1 个 BMP 字符或 1 对代理项）能否直接编码为 CP936（不含 best-fit 近似）
bool unitEncodable(const wchar_t *unit, int unitLen)
{
    char buf[4] = {};
    BOOL usedDefault = FALSE;
    const char defChar = '?';
    const int n = WideCharToMultiByte(kCp936, WC_NO_BEST_FIT_CHARS, unit, unitLen,
                                      buf, sizeof(buf), &defChar, &usedDefault);
    return n > 0 && !usedDefault;
}

} // namespace

bool GbkCodec::decode(const QByteArray &raw, QString *out, QString *error)
{
    if (out == nullptr) { // 调用契约：out/error 必须有效（内部工具类，判空防御即可）
        if (error)
            *error = QStringLiteral("解码输出参数无效");
        return false;
    }
    if (error)
        error->clear();
    out->clear();

    if (raw.size() > INT_MAX) {
        if (error)
            *error = QStringLiteral("文件过大，无法以 GBK 解码");
        return false;
    }
    const int size = static_cast<int>(raw.size());
    if (size == 0)
        return true; // 空输入即合法空内容

    // 快路径：绝大多数真实 GBK 文件整块严格解码即成功
    QString whole;
    if (decodeStrict(raw.constData(), size, &whole)) {
        *out = whole;
        return true;
    }

    // 整块失败但 CP936 本身可用：按 GBK 双字节结构扫描回退，
    // 非法序列以单个 U+FFFD 呈现并继续（保证文件始终可打开、后续内容不丢）
    if (!cp936Available()) {
        if (error)
            *error = QStringLiteral("当前系统缺少 Windows 代码页 936（GBK）支持，无法解码");
        return false;
    }

    const char *d = raw.constData();
    QString result;
    result.reserve(size); // 上界估计：每字节至多产出 1 个字符
    int i = 0;
    while (i < size) {
        const unsigned char lead = static_cast<unsigned char>(d[i]);
        if (lead == 0xFF) {
            // 0xFF 不在 GBK 首字节区间（0x81–0xFE）：非法单字节
            result.append(QChar(0xFFFD));
            ++i;
            continue;
        }

        // 收集从 i 开始连续“结构合法”的最长区段 [i, j)：
        // 单字节（< 0x80 的 ASCII、0x80 欧元符）与结构合法的双字节对
        int j = i;
        bool structurallyValid = true;
        while (j < size) {
            const unsigned char b = static_cast<unsigned char>(d[j]);
            if (b <= 0x80) {
                ++j; // 单字节单元（ASCII 或 0x80 欧元符）
            } else if (b <= 0xFE) {
                if (j + 1 < size && isGbkTrailByte(static_cast<unsigned char>(d[j + 1])))
                    j += 2; // 结构合法双字节对
                else {
                    structurallyValid = false; // 次字节非法或末尾孤立首字节
                    break;
                }
            } else {
                structurallyValid = false; // 0xFF
                break;
            }
        }
        if (j > i) {
            // 区段整块严格解码；失败（区段含未指派对）时逐单元回退，失败单元置 U+FFFD
            QString chunk;
            if (decodeStrict(d + i, j - i, &chunk)) {
                result.append(chunk);
            } else {
                for (int k = i; k < j;) {
                    const unsigned char b = static_cast<unsigned char>(d[k]);
                    const int unitLen = (b >= 0x81 && b <= 0xFE) ? 2 : 1;
                    QString unit;
                    if (decodeStrict(d + k, unitLen, &unit))
                        result.append(unit);
                    else
                        result.append(QChar(0xFFFD)); // 结构合法但 CP936 未指派
                    k += unitLen;
                }
            }
            i = j;
        }
        if (!structurallyValid) {
            // 位置 j 结构非法（非法次字节/孤立首字节/0xFF）：单个 U+FFFD，仅前进 1 字节
            // ——若非法首字节后跟合法 ASCII，后续内容仍正常解码
            result.append(QChar(0xFFFD));
            ++i;
        }
    }

    *out = result;
    return true; // 只要产出内容即成功（非法序列已替换呈现），error 保持为空
}

bool GbkCodec::encode(const QString &text, QByteArray *out, QString *error)
{
    if (out == nullptr) { // 调用契约：out/error 必须有效（内部工具类，判空防御即可）
        if (error)
            *error = QStringLiteral("编码输出参数无效");
        return false;
    }
    if (error)
        error->clear();
    out->clear();

    if (text.size() > INT_MAX) {
        if (error)
            *error = QStringLiteral("内容过长，无法以 GBK 编码");
        return false;
    }
    const int len = static_cast<int>(text.size());
    if (len == 0) {
        return true; // 空内容合法
    }
    const wchar_t *w = reinterpret_cast<const wchar_t *>(text.constData());

    // 快路径：整串编码一次完成，同时用 default-char 机制做全量可编码校验。
    // WC_NO_BEST_FIT_CHARS：近似映射（best-fit）也计为不可直接映射——
    // 若系统存在近似表（现代 Windows 的 CP936 已移除，防御性保留），
    // 不加该标志会静默把不可编码字符替换成近似字符，破坏数据。
    const int need = WideCharToMultiByte(kCp936, WC_NO_BEST_FIT_CHARS, w, len,
                                         nullptr, 0, nullptr, nullptr);
    if (need > 0) {
        QByteArray bytes(need, Qt::Uninitialized);
        BOOL usedDefault = FALSE;
        const char defChar = '?';
        const int written = WideCharToMultiByte(kCp936, WC_NO_BEST_FIT_CHARS, w, len,
                                                bytes.data(), need, &defChar, &usedDefault);
        if (written == need && !usedDefault) {
            *out = bytes;
            return true; // 全部字符均有直接 CP936 映射
        }
    }

    // 失败路径：定位首个不可编码字符及其行号（从 1 起，按 '\n' 计数）。
    // 逐“字符单元”探测：代理项对（如 emoji）必须作为一个完整码点整体编码，
    // 拆成半个代理各自探测必然失败，会导致行号/字符定位错误。
    int line = 1;
    QString badChar;
    for (int i = 0; i < len;) {
        const QChar c = text.at(i);
        wchar_t unit[2] = {};
        int unitLen = 1;
        if (c.isHighSurrogate() && i + 1 < len && text.at(i + 1).isLowSurrogate()) {
            unit[0] = c.unicode();
            unit[1] = text.at(i + 1).unicode();
            unitLen = 2;
        } else {
            unit[0] = c.unicode();
        }
        if (!unitEncodable(unit, unitLen)) {
            badChar = text.mid(i, unitLen);
            break;
        }
        i += unitLen;
        if (c == QChar(u'\n'))
            ++line;
    }

    if (error) {
        if (badChar.isEmpty()) {
            // 未定位到具体字符（极端情况：整串调用失败但逐单元均可编码）
            *error = QStringLiteral("无法以 GBK 编码保存该内容，请改用 UTF-8");
        } else {
            *error = QStringLiteral("无法以 GBK 编码保存：第 %1 行的字符“%2”在 GBK 中没有对应编码，"
                                    "请改用 UTF-8 或删除该字符")
                         .arg(QString::number(line), badChar);
        }
    }
    return false; // out 不产出：调用方不得写盘
}

#else // !Q_OS_WIN

// 非 Windows 桩实现：项目当前仅发布 Windows 版；GBK 依赖 Win32 NLS（CP936）。
// 返回失败保证上层（LocalStorage）按“读取/写出失败”路径处理，不会静默产生
// 错误数据。未来跨平台时在此接入对应平台的 GBK 实现即可（DEC-009）。
bool GbkCodec::decode(const QByteArray &raw, QString *out, QString *error)
{
    Q_UNUSED(raw)
    Q_UNUSED(out)
    if (error)
        *error = QStringLiteral("当前平台未提供 GBK 编解码实现（仅支持 Windows）");
    return false;
}

bool GbkCodec::encode(const QString &text, QByteArray *out, QString *error)
{
    Q_UNUSED(text)
    Q_UNUSED(out)
    if (error)
        *error = QStringLiteral("当前平台未提供 GBK 编解码实现（仅支持 Windows）");
    return false;
}

#endif // Q_OS_WIN
