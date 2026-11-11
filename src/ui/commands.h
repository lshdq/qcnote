// qcnote 命令 id 集中定义（M1-2）。
//
// 菜单条目与后续快捷键系统（M3）的双入口统一路由到本枚举：UI 层只产生
// Command，业务层（Main 组装）只消费 Command，两侧互不感知入口来源。
// 扩展约定：新增命令一律追加在对应分组内，不复用已删除语义的值。
#pragma once

namespace ui {

enum class Command : int {
    None = 0,  // 未触发（菜单关闭/Escape/点击外部）

    // —— 文件（E1/A1）——
    NewDocument,           // 新建 Ctrl+N
    OpenDialog,            // 打开… Ctrl+O
    Save,                  // 保存 Ctrl+S
    SaveAs,                // 另存为… Ctrl+Shift+S
    CloseTab,              // 关闭标签页 Ctrl+W
    ReloadWithEncodingUtf8,  // 文件▸编码▸通过编码重新加载▸UTF-8（三级级联）
    ReloadWithEncodingGbk,   // 文件▸编码▸通过编码重新加载▸GBK
    SaveWithEncodingUtf8,    // 文件▸编码▸通过编码保存▸UTF-8（当前编码项打勾）
    SaveWithEncodingGbk,     // 文件▸编码▸通过编码保存▸GBK

    // —— 搜索（B5）——
    FindDialog,     // 查找… Ctrl+F
    ReplaceDialog,  // 替换… Ctrl+H
    FindNext,       // 下一个 F3
    FindPrevious,   // 上一个 Shift+F3

    // —— 视图（E5/B2）——
    ToggleDarkTheme,  // 深色主题（勾选态）
    ToggleWordWrap,   // 自动换行（勾选态）

    // —— Tab 右键菜单（A3，M1 第三阶段接入；命令 id 先行定义供菜单与
    //     单测共享，快捷键无绑定）——
    CloseCurrentTab,  // 关闭当前文件
    CloseTabsToRight, // 关闭右边所有
    CloseTabsToLeft,  // 关闭左边所有
    CloseOtherTabs,   // 关闭其他文件

    // —— 应用 ——
    Quit,  // 退出（C4 热退出，M2 接草稿 flush）
};

// 命令名称（调试日志/自动化验证输出用；非用户可见字符串）
inline const wchar_t* CommandName(Command c) {
    switch (c) {
    case Command::None: return L"None";
    case Command::NewDocument: return L"NewDocument";
    case Command::OpenDialog: return L"OpenDialog";
    case Command::Save: return L"Save";
    case Command::SaveAs: return L"SaveAs";
    case Command::CloseTab: return L"CloseTab";
    case Command::ReloadWithEncodingUtf8: return L"ReloadWithEncodingUtf8";
    case Command::ReloadWithEncodingGbk: return L"ReloadWithEncodingGbk";
    case Command::SaveWithEncodingUtf8: return L"SaveWithEncodingUtf8";
    case Command::SaveWithEncodingGbk: return L"SaveWithEncodingGbk";
    case Command::FindDialog: return L"FindDialog";
    case Command::ReplaceDialog: return L"ReplaceDialog";
    case Command::FindNext: return L"FindNext";
    case Command::FindPrevious: return L"FindPrevious";
    case Command::ToggleDarkTheme: return L"ToggleDarkTheme";
    case Command::ToggleWordWrap: return L"ToggleWordWrap";
    case Command::CloseCurrentTab: return L"CloseCurrentTab";
    case Command::CloseTabsToRight: return L"CloseTabsToRight";
    case Command::CloseTabsToLeft: return L"CloseTabsToLeft";
    case Command::CloseOtherTabs: return L"CloseOtherTabs";
    case Command::Quit: return L"Quit";
    default: return L"<unknown>";
    }
}

}  // namespace ui
