#pragma once

#include <QString>

class QByteArray;

/**
 * @brief GBK（Windows 代码页 936）编解码封装（DEC-009）。
 *
 * 为什么独立成组件：本机官方 Qt 6.11.2 二进制包未编译 big_codecs 特性
 * （QStringConverter::GB18030 不可用），GBK 编解码改走 Windows 内置 NLS API
 * （MultiByteToWideChar / WideCharToMultiByte，代码页 936）。全部平台相关实现
 * 封装在本文件内，未来跨平台时仅替换本组件，其余层不感知（与 shellintegration
 * 的条件编译降级模式一致）。
 *
 * 契约：
 * - decode：CP936 → QString。优先整块 MB_ERR_INVALID_CHARS 严格解码；失败则按
 *   GBK 双字节结构（首字节 0x81–0xFE，次字节 0x40–0xFE 除 0x7F）扫描回退，
 *   非法序列以单个 U+FFFD 呈现并继续（不崩溃、不丢后续内容）。单字节 0x80
 *   解码为 U+20AC（CP936 定义）。只要能产出内容即成功；仅当系统缺少 CP936
 *   支持等"完全无法工作"情形才返回 false。
 * - encode：QString → CP936。写盘前全量可编码校验：任一字符无直接 CP936
 *   映射（含 best-fit 近似，用 WC_NO_BEST_FIT_CHARS + usedDefaultChar 判定）
 *   即失败，error 为中文并含首个不可编码字符及其行号（从 1 起，按 '\n' 计数）；
 *   失败时 out 不产出、调用方不得写盘。
 *   注意：WideCharToMultiByte 的 WC_ERR_INVALID_CHARS 标志仅对 UTF-8 目标代码页
 *   有效（CP936 下返回 ERROR_INVALID_FLAGS，已实证），不可编码检测必须走
 *   default-char 机制。
 *
 * 纯静态工具类，无 QObject/QML 注册；非 Windows 平台提供返回失败的桩实现
 * （保证跨平台可编译），见 gbkcodec.cpp。
 */
class GbkCodec
{
public:
    /// CP936 字节流 → UTF-16 文本。失败（系统缺少代码页支持）返回 false 并置 *error。
    static bool decode(const QByteArray &raw, QString *out, QString *error);

    /// UTF-16 文本 → CP936 字节流。存在不可编码字符时返回 false 并置 *error
    /// （含首个不可编码字符与其所在行号），此时 *out 不产出。
    static bool encode(const QString &text, QByteArray *out, QString *error);
};
