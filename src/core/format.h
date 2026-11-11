// qcnote 纯逻辑核心层：文档格式抽象（M2-A，架构扩展点 F）。
//
// 每种格式（纯文本、未来的 Markdown/JSON）实现 DocumentFormat 并在
// DocumentManager 注册；PlainTextFormat 是兜底格式（恒真、不可移除），
// 未知扩展名的文件最终由它处理（需求 F：兜底行为不得移除）。
#pragma once

#include <string>
#include <vector>

namespace core {

class DocumentFormat {
public:
    virtual ~DocumentFormat() = default;

    /// 格式标识（稳定 ASCII，如 "plaintext"）
    virtual std::string id() const = 0;

    /// 用户可见名称（如 L"纯文本"）
    virtual std::wstring displayName() const = 0;

    /// 文件对话框名过滤器（如 L"文本文件 (*.txt)"、L"所有文件 (*)"）
    virtual std::vector<std::wstring> nameFilters() const = 0;

    /// 文件名是否由本格式处理。默认实现：按过滤器括号内 "*.ext" 通配符对
    /// 扩展名做大小写不敏感匹配（"*" 匹配任意）；无扩展名返回 false
    /// （由兜底格式接手）。兜底格式可重写为恒真。
    virtual bool matchesFileName(const std::wstring& fileName) const;
};

/// 纯文本格式：默认兜底格式，任何文件都可按纯文本打开（不可移除）
class PlainTextFormat : public DocumentFormat {
public:
    std::string id() const override;
    std::wstring displayName() const override;
    std::vector<std::wstring> nameFilters() const override;

    /// 兜底格式：恒返回 true
    bool matchesFileName(const std::wstring& fileName) const override;
};

} // namespace core
