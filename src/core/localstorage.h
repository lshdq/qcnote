// qcnote 纯逻辑核心层：本地文件存储（M2-A，旧 LocalStorage 语义移植）。
//
// 读：自动识别（BOM → 严格 UTF-8 校验 → GBK）或强制编码；UTF-8 剥离 BOM
//     （不进内容）；GBK 经 CP936 解码，非法序列以 U+FFFD 呈现；
//     CRLF/CR 归一 LF 并记录原换行风格。
// 写：原子写（同目录临时文件 + MoveFileEx REPLACE_EXISTING，对齐 QSaveFile
//     语义）；GBK 不可编码校验在写盘前（磁盘不被触碰）。
#pragma once

#include "core/storage.h"

namespace core {

class LocalStorage : public DocumentStorage {
public:
    std::wstring scheme() const override; // L"file"

    /// 仅认本地 path 形态：携带其他 scheme（http:// 等）或 file://host 网络
    /// 主机形态的 URL 不由本存储处理——后注册的对应 scheme 存储可达，
    /// 不被先注册的本实现吞掉（storageForUrl 分发语义）
    bool supports(const std::filesystem::path& path) const override;

    std::expected<LoadedData, StorageError>
    read(const std::filesystem::path& path, std::optional<TextEncoding> forced) const override;

    std::expected<void, StorageError>
    write(const std::filesystem::path& path, const std::string& content, TextEncoding encoding,
          bool hasBom, LineEnding lineEnding) const override;

private:
    /// 校验路径（存在且是常规文件，目录不算）并读取全部原始字节
    std::expected<std::string, StorageError> readRawBytes(const std::filesystem::path& path) const;
};

} // namespace core
