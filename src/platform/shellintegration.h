// qcnote M5：资源管理器右键菜单集成（D3）——注册表读写（仅 HKCU，绝不提权）。
//
// 键结构（照搬旧版 Qt ShellIntegration，基线 4e313f0；键名/图标/命令结构不变）：
//   HKCU\Software\Classes\*\shell\qcnote
//     (Default) = "用 qcnote 打开"          ← 菜单显示文本（重写需求 D3 文案）
//     Icon       = "<exe 绝对路径>"          ← 带引号 REG_SZ
//   HKCU\Software\Classes\*\shell\qcnote\command
//     (Default) = "<exe 绝对路径>" "%1"      ← %1 = 资源管理器传入的目标文件
//   HKCU\Software\qcnote\qcnote
//     contextMenuPrompted = 1 (REG_DWORD)    ← 首启提示标记（不再重复询问）
// 注：「*」为注册表字面量键名（所有文件的菜单挂载点），非通配符。
//
// 注册时机（照旧版）：**首启提示**而非自动注册——首次启动（标记键不存在）由
// main 弹「添加右键菜单」对话框询问；「添加」→ 写菜单 + 标记，「不添加」/
// Esc → 仅写标记。菜单写入失败时标记仍写入（用户不再被重复询问，错误经返回
// 值交错误框展示）；command 写失败时尽力回滚删除菜单主键子树（半套残留会让
// explorer 显示菜单但点击无效）。
//
// 测试注入（不碰真实键）：Config 三项均可注入——classesRoot 换随机子树
// （如 Software\qcnote-selftest-<rand>）、menuKeyName 换 qcnote-selftest-<rand>
// （e2e 经 exe 钩子 --selftest-shell-* 走真实 Software\Classes 下的临时键名）、
// markerRoot 换随机键。读写均为按需打开注册表键，无跨实例缓存。
#pragma once

#include <filesystem>
#include <string>

namespace platform::shell {

/// 注册表布局配置（生产默认 = 旧版键结构；测试注入随机名隔离）
struct Config {
    std::wstring classesRoot = L"Software\\Classes";  ///< HKCU 相对键根
    std::wstring menuKeyName = L"qcnote";             ///< *\shell\<name> 键名
    std::wstring menuLabel = L"用 qcnote 打开";       ///< 菜单显示文本（键默认值）
    std::wstring markerRoot = L"Software\\qcnote\\qcnote";  ///< 首启标记键（HKCU 相对）
};

class ShellIntegration {
public:
    explicit ShellIntegration(Config config = {});

    /// command 子键默认值存在且非空（探测只读，不创建键）
    bool isRegistered() const;

    /// 写菜单主键（默认值 + Icon）与 command 子键。exe 路径由调用方传入
    ///（生产 = GetModuleFileNameW 运行时解析，便携路径照常写入当时位置）。
    /// 失败置 *error（含键路径），并尽力回滚删除已写入的菜单子键
    bool registerContextMenu(const std::filesystem::path& exePath, std::wstring* error = nullptr);

    /// 删除菜单主键子树（幂等：键不存在视为成功）
    bool unregisterContextMenu(std::wstring* error = nullptr);

    /// 首启判定：标记值不存在 → true（只读探测，不创建任何键）
    bool contextMenuPromptNeeded() const;

    /// 应用首启提示中的选择（旧版 applyContextMenuChoice 语义）：
    /// accepted=true → 写菜单 + 标记；false → 仅写标记（用户不再被询问）。
    /// 菜单写入失败时标记仍写入，整体返回失败
    bool applyContextMenuChoice(bool accepted, const std::filesystem::path& exePath,
                                std::wstring* error = nullptr);

    // —— 键完整路径（错误信息/测试断言用；HKCU 相对）——
    std::wstring menuKeyPath() const;     // <classesRoot>\*\shell\<menuKeyName>
    std::wstring commandKeyPath() const;  // <菜单主键>\command
    std::wstring markerKeyPath() const;   // <markerRoot>

private:
    bool writeContextMenuEntries(const std::filesystem::path& exePath, std::wstring* error);
    bool writeMarker(std::wstring* error);

    Config m_config;
};

}  // namespace platform::shell
