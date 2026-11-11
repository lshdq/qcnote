// qcnote M5：资源管理器右键菜单集成实现（D3）。键结构与注册时机决策见头注释。
#include "platform/shellintegration.h"

#include <windows.h>

namespace platform::shell {
namespace {

/// 打开/创建 HKCU 相对键（samDesired 最小权限：读 KEY_QUERY_VALUE / 读值 KEY_READ
/// 由调用方按需传；写路径 KEY_WRITE 隐含可建子键）。失败返回 nullptr
HKEY OpenKey(const std::wstring& subKey, REGSAM sam, bool create) {
    HKEY key = nullptr;
    const LONG rc = create
                        ? RegCreateKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, nullptr,
                                          REG_OPTION_NON_VOLATILE, sam, nullptr, &key, nullptr)
                        : RegOpenKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, sam, &key);
    if (rc != ERROR_SUCCESS) return nullptr;
    return key;
}

/// 读键默认值（REG_SZ）。不存在/类型不符/打不开键 → 空串
std::wstring ReadDefaultValue(const std::wstring& subKey) {
    HKEY key = OpenKey(subKey, KEY_QUERY_VALUE, /*create=*/false);
    if (!key) return {};
    wchar_t buffer[1024] = {};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    std::wstring value;
    if (RegQueryValueExW(key, nullptr, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS &&
        type == REG_SZ && size >= sizeof(wchar_t)) {
        value.assign(buffer, size / sizeof(wchar_t));
        while (!value.empty() && value.back() == L'\0') value.pop_back();
    }
    RegCloseKey(key);
    return value;
}

/// 写键默认值（REG_SZ）
bool WriteDefaultValue(HKEY key, const std::wstring& value) {
    const DWORD size = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(key, nullptr, 0, REG_SZ,
                          reinterpret_cast<const BYTE*>(value.c_str()), size) == ERROR_SUCCESS;
}

/// 删除 HKCU 相对键整棵子树（不存在视为成功——幂等语义）
bool DeleteKeyTree(const std::wstring& subKey) {
    const LONG rc = RegDeleteTreeW(HKEY_CURRENT_USER, subKey.c_str());
    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

/// 值是否存在（不读内容；只读探测不创建键）
bool ValueExists(const std::wstring& subKey, const wchar_t* valueName) {
    HKEY key = OpenKey(subKey, KEY_QUERY_VALUE, /*create=*/false);
    if (!key) return false;
    const LONG rc = RegQueryValueExW(key, valueName, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

std::wstring Quote(const std::wstring& path) { return L"\"" + path + L"\""; }

}  // namespace

ShellIntegration::ShellIntegration(Config config)
    : m_config(std::move(config)) {}

std::wstring ShellIntegration::menuKeyPath() const {
    // 「*」为注册表字面量键名（所有文件的菜单挂载点），非通配符
    return m_config.classesRoot + L"\\*\\shell\\" + m_config.menuKeyName;
}

std::wstring ShellIntegration::commandKeyPath() const {
    return menuKeyPath() + L"\\command";
}

std::wstring ShellIntegration::markerKeyPath() const { return m_config.markerRoot; }

bool ShellIntegration::isRegistered() const {
    // 探测依据 = command 子键默认值（ explorer 实际调用的命令串）非空
    return !ReadDefaultValue(commandKeyPath()).empty();
}

bool ShellIntegration::writeContextMenuEntries(const std::filesystem::path& exePath,
                                               std::wstring* error) {
    const std::wstring nativeExe = exePath.wstring();
    const std::wstring quotedExe = Quote(nativeExe);

    // 菜单主键：默认值 = 显示文本；Icon = 带引号 exe 路径
    {
        HKEY key = OpenKey(menuKeyPath(), KEY_SET_VALUE, /*create=*/true);
        if (!key) {
            if (error) *error = L"写入右键菜单注册表键失败：HKCU\\" + menuKeyPath();
            return false;
        }
        const bool ok = WriteDefaultValue(key, m_config.menuLabel) &&
                        RegSetValueExW(key, L"Icon", 0, REG_SZ,
                                       reinterpret_cast<const BYTE*>(quotedExe.c_str()),
                                       static_cast<DWORD>((quotedExe.size() + 1) *
                                                          sizeof(wchar_t))) == ERROR_SUCCESS;
        RegCloseKey(key);
        if (!ok) {
            if (error) *error = L"写入右键菜单注册表键失败：HKCU\\" + menuKeyPath();
            return false;
        }
    }
    // command 子键：默认值 = "<exe 绝对路径>" "%1"（%1 为资源管理器传入的右键
    // 目标文件，注册表不展开——由 explorer 启动时替换）
    {
        HKEY key = OpenKey(commandKeyPath(), KEY_SET_VALUE, /*create=*/true);
        if (!key) {
            // 回滚：command 写失败时菜单主键已成半套残留（explorer 显示菜单但
            // 点击无效），删除整个菜单主键子树；回滚失败不掩盖原错误仅追加
            std::wstring message = L"写入右键菜单命令注册表键失败：HKCU\\" + commandKeyPath();
            if (!DeleteKeyTree(menuKeyPath())) {
                message += L"（回滚清理菜单键亦失败，可能残留：HKCU\\" + menuKeyPath() + L"）";
            }
            if (error) *error = message;
            return false;
        }
        const bool ok = WriteDefaultValue(key, Quote(nativeExe) + L" \"%1\"");
        RegCloseKey(key);
        if (!ok) {
            std::wstring message = L"写入右键菜单命令注册表键失败：HKCU\\" + commandKeyPath();
            if (!DeleteKeyTree(menuKeyPath())) {
                message += L"（回滚清理菜单键亦失败，可能残留：HKCU\\" + menuKeyPath() + L"）";
            }
            if (error) *error = message;
            return false;
        }
    }
    return true;
}

bool ShellIntegration::writeMarker(std::wstring* error) {
    HKEY key = OpenKey(markerKeyPath(), KEY_SET_VALUE, /*create=*/true);
    if (!key) {
        if (error) *error = L"写入首启标记注册表键失败：HKCU\\" + markerKeyPath();
        return false;
    }
    const DWORD one = 1;
    const bool ok = RegSetValueExW(key, L"contextMenuPrompted", 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&one), sizeof(one)) ==
                    ERROR_SUCCESS;
    RegCloseKey(key);
    if (!ok && error) *error = L"写入首启标记注册表键失败：HKCU\\" + markerKeyPath();
    return ok;
}

bool ShellIntegration::registerContextMenu(const std::filesystem::path& exePath,
                                           std::wstring* error) {
    return writeContextMenuEntries(exePath, error);
}

bool ShellIntegration::unregisterContextMenu(std::wstring* error) {
    if (!DeleteKeyTree(menuKeyPath())) {
        if (error) *error = L"删除右键菜单注册表键失败：HKCU\\" + menuKeyPath();
        return false;
    }
    return true;
}

bool ShellIntegration::contextMenuPromptNeeded() const {
    // 标记存在（上次无论同意或拒绝）即不再提示；只读探测不创建键
    return !ValueExists(markerKeyPath(), L"contextMenuPrompted");
}

bool ShellIntegration::applyContextMenuChoice(bool accepted,
                                              const std::filesystem::path& exePath,
                                              std::wstring* error) {
    // 逐项独立收集错误：菜单失败时标记仍要写（需求：用户不再被重复询问），
    // 末尾汇总成败（与旧版 QStringList errors 汇聚语义一致）
    std::wstring menuError;
    if (accepted && !writeContextMenuEntries(exePath, &menuError)) {
        if (error) *error = menuError;
    }
    std::wstring markerError;
    const bool markerOk = writeMarker(&markerError);
    if (!markerOk && error) {
        if (!error->empty()) *error += L"\n";
        *error += markerError;
    }
    return menuError.empty() && markerOk;
}

}  // namespace platform::shell
