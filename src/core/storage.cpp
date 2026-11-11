#include "core/storage.h"

namespace core {

std::optional<std::filesystem::path> parseFileUrl(const std::wstring& url) {
    if (url.starts_with(L"file:///"))
        return std::filesystem::path(url.substr(8)); // file:///C:/x → C:/x
    if (url.starts_with(L"file://"))
        return std::nullopt; // file://host/... 网络主机路径：当前无对应存储
    if (url.find(L"://") != std::wstring::npos)
        return std::nullopt; // 其他 scheme（http 等）：未注册的存储类型
    return std::filesystem::path(url); // 纯路径原样
}

} // namespace core
