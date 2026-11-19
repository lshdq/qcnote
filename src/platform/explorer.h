// 资源管理器定位：打开所在文件夹并选中文件（platform 层，纯 Win32）
#pragma once
#include <filesystem>
namespace platform {
/// 在资源管理器中打开并选中该文件。文件不存在返回 false（调用方负责提示），
/// 不启动 explorer
bool OpenSelectInExplorer(const std::filesystem::path& file);
}
