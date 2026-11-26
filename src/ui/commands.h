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

    // —— 编辑（列模式填充编号）——
    InsertNumbering,  // 编辑▸插入编号…（无快捷键，DEC-006）

    // —— 视图（E5/B2）——
    ToggleDarkTheme,  // 深色主题（勾选态）
    ToggleWordWrap,   // 自动换行（勾选态）
    ToggleLineNumbers,  // 显示行号（勾选态，菜单唯一入口，无快捷键）
    ToggleAutoIndent,   // 自动缩进（勾选态，回车复制上一行前导空白；菜单唯一入口）
    ToggleDocList,      // 文档列表（勾选态，左侧文档列表面板显隐；菜单唯一入口，
                        // 无快捷键；显隐与宽度随会话持久化）

    // —— Tab 右键菜单（A3，M1 第三阶段接入；命令 id 先行定义供菜单与
    //     单测共享，快捷键无绑定）——
    CloseCurrentTab,  // 关闭当前文件
    CloseTabsToRight, // 关闭右边所有
    CloseTabsToLeft,  // 关闭左边所有
    CloseOtherTabs,   // 关闭其他文件
    RenameFile,            // 文件重命名…（有路径的标签）
    OpenContainingFolder,  // 打开所在文件夹（有路径的标签）

    // —— 语言（代码着色 T3）——
    // 「自动」「纯文本」+ 21 个语言命令（菜单唯一入口，无快捷键）。
    // 语言命令顺序与 core/syntax.h 注册表（languageAt 下标）一一对应：
    // SelectLanguageC = languageAt(0) … SelectLanguageYaml = languageAt(20)；
    // mainmenu.cpp 构建（displayName→命令）与 main.cpp 路由（命令→注册表
    // 下标）均依赖该对应，新增语言须同步注册表与本组枚举
    SelectLanguageAuto,        // 语言▸自动（清除手动指定，按扩展名识别）
    SelectLanguagePlainText,   // 语言▸纯文本（强制不着色）
    SelectLanguageC,           // 语言▸C
    SelectLanguageCpp,         // 语言▸C++
    SelectLanguageCSharp,      // 语言▸C#
    SelectLanguageJava,        // 语言▸Java
    SelectLanguageJavaScript,  // 语言▸JavaScript
    SelectLanguageTypeScript,  // 语言▸TypeScript
    SelectLanguagePython,      // 语言▸Python
    SelectLanguageGo,          // 语言▸Go
    SelectLanguageRust,        // 语言▸Rust
    SelectLanguageLua,         // 语言▸Lua
    SelectLanguageJson,        // 语言▸JSON
    SelectLanguageMarkdown,    // 语言▸Markdown
    SelectLanguageHtml,        // 语言▸HTML
    SelectLanguageXml,         // 语言▸XML
    SelectLanguageCss,         // 语言▸CSS
    SelectLanguageSql,         // 语言▸SQL
    SelectLanguageBash,        // 语言▸Bash
    SelectLanguagePowerShell,  // 语言▸PowerShell
    SelectLanguageBatch,       // 语言▸Batch
    SelectLanguageIni,         // 语言▸INI
    SelectLanguageYaml,        // 语言▸YAML

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
    case Command::InsertNumbering: return L"InsertNumbering";
    case Command::ToggleDarkTheme: return L"ToggleDarkTheme";
    case Command::ToggleWordWrap: return L"ToggleWordWrap";
    case Command::ToggleLineNumbers: return L"ToggleLineNumbers";
    case Command::ToggleAutoIndent: return L"ToggleAutoIndent";
    case Command::ToggleDocList: return L"ToggleDocList";
    case Command::CloseCurrentTab: return L"CloseCurrentTab";
    case Command::CloseTabsToRight: return L"CloseTabsToRight";
    case Command::CloseTabsToLeft: return L"CloseTabsToLeft";
    case Command::CloseOtherTabs: return L"CloseOtherTabs";
    case Command::RenameFile: return L"RenameFile";
    case Command::OpenContainingFolder: return L"OpenContainingFolder";
    case Command::SelectLanguageAuto: return L"SelectLanguageAuto";
    case Command::SelectLanguagePlainText: return L"SelectLanguagePlainText";
    case Command::SelectLanguageC: return L"SelectLanguageC";
    case Command::SelectLanguageCpp: return L"SelectLanguageCpp";
    case Command::SelectLanguageCSharp: return L"SelectLanguageCSharp";
    case Command::SelectLanguageJava: return L"SelectLanguageJava";
    case Command::SelectLanguageJavaScript: return L"SelectLanguageJavaScript";
    case Command::SelectLanguageTypeScript: return L"SelectLanguageTypeScript";
    case Command::SelectLanguagePython: return L"SelectLanguagePython";
    case Command::SelectLanguageGo: return L"SelectLanguageGo";
    case Command::SelectLanguageRust: return L"SelectLanguageRust";
    case Command::SelectLanguageLua: return L"SelectLanguageLua";
    case Command::SelectLanguageJson: return L"SelectLanguageJson";
    case Command::SelectLanguageMarkdown: return L"SelectLanguageMarkdown";
    case Command::SelectLanguageHtml: return L"SelectLanguageHtml";
    case Command::SelectLanguageXml: return L"SelectLanguageXml";
    case Command::SelectLanguageCss: return L"SelectLanguageCss";
    case Command::SelectLanguageSql: return L"SelectLanguageSql";
    case Command::SelectLanguageBash: return L"SelectLanguageBash";
    case Command::SelectLanguagePowerShell: return L"SelectLanguagePowerShell";
    case Command::SelectLanguageBatch: return L"SelectLanguageBatch";
    case Command::SelectLanguageIni: return L"SelectLanguageIni";
    case Command::SelectLanguageYaml: return L"SelectLanguageYaml";
    case Command::Quit: return L"Quit";
    default: return L"<unknown>";
    }
}

// —— 语言命令辅助（代码着色 T3；main.cpp 路由 / mainmenu.cpp 构建 /
//     menu 单测共用；依赖上方枚举组连续排布的约定）——

/// 是否语言命令组（SelectLanguageAuto..SelectLanguageYaml 共 23 项：
/// 自动 + 纯文本 + 21 语言项）
inline constexpr bool isSelectLanguageCommand(Command c) {
    const int v = static_cast<int>(c);
    const int first = static_cast<int>(Command::SelectLanguageAuto);
    return v >= first && v < first + 23;
}

/// 语言命令 → core 注册表下标（0..20，与 languageAt 对应）；「自动」/
/// 「纯文本」或非语言命令返回 -1
inline constexpr int selectLanguageIndex(Command c) {
    const int v = static_cast<int>(c);
    const int first = static_cast<int>(Command::SelectLanguageC);
    return v >= first && v < first + 21 ? v - first : -1;
}

}  // namespace ui
