// 资源管理器定位实现（platform 层）：explorer.exe /select,"<完整路径>" 打开
// 所在文件夹并选中文件。仅宽字符 Win32 API，无窗口/消息依赖，可在任意线程
// 调用（当前仅主线程 UI 命令路径）。
#include "explorer.h"

#include <windows.h>
#include <shellapi.h>  // ShellExecuteW

namespace platform {

bool OpenSelectInExplorer(const std::filesystem::path& file) {
    // 存在性检查：无效属性（含不存在/不可访问）不启动 explorer，交调用方提示
    if (GetFileAttributesW(file.wstring().c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    // 参数字符串须在 ShellExecuteW 调用期间存活：先构造临时 std::wstring 持有
    const std::wstring args = L"/select,\"" + file.wstring() + L"\"";
    // 返回值 > 32 视为成功（ShellExecuteW 历史契约：<=32 为错误码）
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(),
                                      nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

}  // namespace platform
