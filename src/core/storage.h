// qcnote 纯逻辑核心层：存储抽象（M2-A，架构扩展点 F）。
//
// 按 URL scheme 分发：file://（本地路径）由 LocalStorage 处理；未来网络存储
// 按 scheme 新增实现注册（storageForUrl 分发，未注册报「不支持的存储类型」）。
// 当前 URL 用 std::filesystem::path 直接表达（file:// 前缀经 parseFileUrl 解析；
// 网络 scheme 需要 URL 类型时再扩展，接口不堵死）。
// 错误统一 std::expected<T, StorageError>（C++23）；消息为用户可见中文（宽字符）。
#pragma once

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/encoding.h"
#include "core/textutil.h"

namespace core {

/// 失败类别（供 UI 决定后续动作：如 InvalidUrl 的保存 → 转另存为流程）
enum class StorageErrorKind {
    InvalidUrl,        // 空/非法路径
    NotFound,          // 文件不存在
    IsDirectory,       // 目标是目录而不是文件（isFile 预检，旧 DEC-015 对齐）
    OpenFailed,        // 打开/读取失败（占用、权限等）
    WriteFailed,       // 写入/替换失败（原子写的临时文件或替换阶段）
    EncodingFailed,    // GBK 不可编码（写盘前校验失败，磁盘未被触碰）
    DecodeFailed,      // 解码失败（系统缺 CP936 支持等）
    UnsupportedScheme, // 无匹配存储（scheme 未注册）
};

struct StorageError {
    StorageErrorKind kind = StorageErrorKind::OpenFailed;
    std::wstring message; ///< 用户可见中文错误消息
};

/// 读取结果：内容（UTF-8、LF 归一、无 BOM）+ 磁盘事实元数据
struct LoadedData {
    std::string content;
    LineEnding lineEnding = LineEnding::Lf;
    TextEncoding encoding = TextEncoding::Utf8; ///< 实际使用的编码（自动识别结果或强制值）
    bool hasBom = false; ///< 磁盘是否带 UTF-8 BOM（已剥离，不进内容）；GBK 恒 false
};

/// 存储扩展点抽象（不转移所有权：DocumentManager 持裸指针注册）
class DocumentStorage {
public:
    virtual ~DocumentStorage() = default;

    /// 本存储负责的 URL scheme（如 L"file"）
    virtual std::wstring scheme() const = 0;

    /// path 型 URL 是否由本存储处理（storageForUrl 遍历依此分发）：默认全部
    /// 认领；具体存储应按自身 scheme 形态重写（LocalStorage 仅认本地 path，
    /// 其他 scheme 的 URL 形态留给对应存储，不被先注册者吞掉）
    virtual bool supports(const std::filesystem::path& path) const { (void)path; return true; }

    /// 读取：forced 有值按指定编码强制解码（按编码重新加载/会话记忆编码），
    /// 否则自动识别（BOM → 严格 UTF-8 校验 → GBK）。解码失败的序列以替换字符
    /// 呈现（保证文件可打开），不算读取失败。
    virtual std::expected<LoadedData, StorageError>
    read(const std::filesystem::path& path, std::optional<TextEncoding> forced) const = 0;

    /// 写入（实现方保证原子写）。encoding==Utf8 时 hasBom 控制 BOM 写回
    /// （有 BOM 历史的文档保持）；encoding==Gbk 时忽略 hasBom（GBK 无 BOM 概念）。
    /// GBK 不可编码校验必须在写盘前完成：任一字符无直接 CP936 映射即返回
    /// EncodingFailed（消息含字符与行号），磁盘不被触碰。
    virtual std::expected<void, StorageError>
    write(const std::filesystem::path& path, const std::string& content, TextEncoding encoding,
          bool hasBom, LineEnding lineEnding) const = 0;
};

/// file:// URL → 本地 path：file:/// 剥前缀；无 scheme 前缀的原样；
/// 其他 scheme（http:// 等）返回 nullopt（未注册的存储类型）
std::optional<std::filesystem::path> parseFileUrl(const std::wstring& url);

} // namespace core
