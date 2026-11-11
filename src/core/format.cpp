#include "core/format.h"

#include <algorithm>

namespace core {

namespace {

/// 取文件名扩展名（最后一个 '.' 之后；无扩展名返回空）
std::wstring fileSuffix(const std::wstring& fileName) {
    const std::size_t dot = fileName.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= fileName.size())
        return {};
    return fileName.substr(dot + 1);
}

/// 大小写不敏感宽字符串比较（仅用于扩展名/通配符；towlower 处理 ASCII 扩展名足够）
bool equalsCaseInsensitive(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (towlower(static_cast<wint_t>(a[i])) != towlower(static_cast<wint_t>(b[i])))
            return false;
    }
    return true;
}

} // namespace

bool DocumentFormat::matchesFileName(const std::wstring& fileName) const {
    const std::wstring suffix = fileSuffix(fileName);
    if (suffix.empty())
        return false; // 无扩展名：具体格式不认，交兜底格式

    // 旧版语义移植：解析每个过滤器括号内的通配符列表
    for (const std::wstring& filter : nameFilters()) {
        const std::size_t open = filter.find_last_of(L'(');
        const std::size_t close = filter.find_last_of(L')');
        if (open == std::wstring::npos || close == std::wstring::npos || close <= open)
            continue;
        std::wstring inner = filter.substr(open + 1, close - open - 1);
        std::size_t pos = 0;
        while (pos <= inner.size()) {
            const std::size_t space = inner.find(L' ', pos);
            const std::wstring pattern =
                inner.substr(pos, space == std::wstring::npos ? std::wstring::npos : space - pos);
            if (!pattern.empty()) {
                if (pattern == L"*")
                    return true; // 通配一切
                if (pattern.size() > 2 && pattern.starts_with(L"*.")
                    && equalsCaseInsensitive(pattern.substr(2), suffix))
                    return true;
            }
            if (space == std::wstring::npos)
                break;
            pos = space + 1;
        }
    }
    return false;
}

std::string PlainTextFormat::id() const {
    return "plaintext";
}

std::wstring PlainTextFormat::displayName() const {
    return L"纯文本";
}

std::vector<std::wstring> PlainTextFormat::nameFilters() const {
    return {L"文本文件 (*.txt)", L"所有文件 (*)"};
}

bool PlainTextFormat::matchesFileName(const std::wstring& fileName) const {
    (void)fileName;
    return true; // 兜底格式：任何文件都可用纯文本打开
}

} // namespace core
