// 主菜单内容（E1）：文件▸（含编码三级级联、语言子菜单）→ 编辑▸ → 搜索▸ →
// 视图▸ → 退出。结构与旧 QML 版 TitleBar.qml 主菜单一一对应（git 历史可查），
// 勾选/置灰语义不变，仅数据载体从 QML Menu 换为 MenuModel：
//   - 保存/另存为…/关闭标签页：有活动文档才可用
//   - 通过编码重新加载（整组）：有活动文档且有磁盘路径（未命名禁用）
//   - 通过编码保存（整组）：有活动文档即可用；当前编码项打勾
//   - 语言（整组，代码着色 T3）：有活动文档才可用；21 语言项 +「自动」
//     「纯文本」勾选态来自 state（生效语言比对/手动模式判定）
//   - 搜索四项：有活动文档才可用（欢迎页置灰）
//   - 深色主题/自动换行/显示行号/自动缩进：勾选态（状态源由文档/主题/会话驱动）
//   - 编辑▸插入编号…：有活动文档且多光标态（选区数 ≥ 2）才可用（列模式
//     填充编号；无快捷键，DEC-006）
// 子菜单叶子（UTF-8/GBK）不单独承载 enabled——父级置灰即整组不可达
// （与 QML 版 Menu.enabled 级联语义一致；语言组同款）。
// 语言子菜单数据源为 core 注册表（ui 层依赖 core 允许；mainmenu 是 ui 中
// 首个取 core 数据的模块，仅 mainmenu.cpp 引 core/syntax.h，头文件不扩散）。
#include "mainmenu.h"

#include "core/syntax.h"  // 代码着色 T3：语言注册表（languageAt/languageCount 构建语言子菜单）

namespace ui {

MenuModel BuildMainMenu(const MainMenuState& state) {
    MenuModel m;
    const int root = m.addMenu();  // 主菜单（id=0）

    // —— 文件 ——
    const int file = m.addMenu(L"文件");
    m.addItem(file, L"新建", L"Ctrl+N", Command::NewDocument);
    m.addItem(file, L"打开…", L"Ctrl+O", Command::OpenDialog);
    m.addItem(file, L"保存", L"Ctrl+S", Command::Save).enabled = state.hasActiveDocument;
    m.addItem(file, L"另存为…", L"Ctrl+Shift+S", Command::SaveAs).enabled =
        state.hasActiveDocument;
    m.addItem(file, L"关闭标签页", L"Ctrl+W", Command::CloseTab).enabled =
        state.hasActiveDocument;
    m.addSeparator(file);

    // 编码三级级联：编码▸（通过编码重新加载▸UTF-8/GBK、通过编码保存▸UTF-8/GBK）
    const int encoding = m.addMenu(L"编码");
    const int reloadEncoding = m.addMenu(L"通过编码重新加载");
    m.addItem(reloadEncoding, L"UTF-8", {}, Command::ReloadWithEncodingUtf8);
    m.addItem(reloadEncoding, L"GBK", {}, Command::ReloadWithEncodingGbk);
    const int saveEncoding = m.addMenu(L"通过编码保存");
    m.addItem(saveEncoding, L"UTF-8", {}, Command::SaveWithEncodingUtf8).checked =
        state.currentEncoding == MainMenuState::Encoding::Utf8;
    m.addItem(saveEncoding, L"GBK", {}, Command::SaveWithEncodingGbk).checked =
        state.currentEncoding == MainMenuState::Encoding::Gbk;
    m.addSubmenuEntry(encoding, L"通过编码重新加载", reloadEncoding).enabled =
        state.activeDocumentHasUrl;
    m.addSubmenuEntry(encoding, L"通过编码保存", saveEncoding).enabled =
        state.hasActiveDocument;
    m.addSubmenuEntry(file, L"编码", encoding);

    // —— 语言子菜单（代码着色 T3）：21 语言项（core 注册表顺序）+ 分隔线 +
    //     「自动」「纯文本」。勾选态：语言项 = 当前生效语言（id 比对，自动
    //     识别结果同样显示勾选，TC-006）；「自动」= 无手动指定；「纯文本」=
    //     手动指定且生效语言为空。整组经父级条目 enabled（有活动文档才可用，
    //     与编码组同款级联置灰，叶子不单独承载 enabled）
    const int language = m.addMenu(L"语言");
    for (std::size_t i = 0; i < core::languageCount(); ++i) {
        const core::LanguageEntry* entry = core::languageAt(i);
        // 命令按下标映射（commands.h 语言命令组与注册表顺序一一对应）；
        // displayName 为 wchar_t 字面量，复制进 MenuItem 无生命周期问题
        m.addItem(language, entry->displayName, {},
                  static_cast<Command>(static_cast<int>(Command::SelectLanguageC) +
                                       static_cast<int>(i)))
            .checked = entry->id == state.effectiveLanguageId;
    }
    m.addSeparator(language);
    m.addItem(language, L"自动", {}, Command::SelectLanguageAuto).checked =
        !state.manualLanguage;
    m.addItem(language, L"纯文本", {}, Command::SelectLanguagePlainText).checked =
        state.manualLanguage && state.effectiveLanguageId.empty();
    m.addSubmenuEntry(file, L"语言", language).enabled = state.hasActiveDocument;

    // —— 编辑（列模式填充编号）——
    // 插入编号…：有活动文档且多光标态（选区数 ≥ 2）才可用；无快捷键（DEC-006）
    const int edit = m.addMenu(L"编辑");
    m.addItem(edit, L"插入编号…", {}, Command::InsertNumbering).enabled =
        state.hasActiveDocument && state.multiCursorActive;

    // —— 搜索（四项均要求活动文档，B5：欢迎页置灰）——
    const int search = m.addMenu(L"搜索");
    m.addItem(search, L"查找…", L"Ctrl+F", Command::FindDialog).enabled =
        state.hasActiveDocument;
    m.addItem(search, L"替换…", L"Ctrl+H", Command::ReplaceDialog).enabled =
        state.hasActiveDocument;
    m.addSeparator(search);
    m.addItem(search, L"下一个", L"F3", Command::FindNext).enabled =
        state.hasActiveDocument;
    m.addItem(search, L"上一个", L"Shift+F3", Command::FindPrevious).enabled =
        state.hasActiveDocument;

    // —— 视图（勾选态 stub）——
    const int view = m.addMenu(L"视图");
    m.addItem(view, L"深色主题", {}, Command::ToggleDarkTheme).checked = state.darkTheme;
    m.addItem(view, L"自动换行", {}, Command::ToggleWordWrap).checked = state.wordWrap;
    m.addItem(view, L"显示行号", {}, Command::ToggleLineNumbers).checked = state.lineNumbers;
    m.addItem(view, L"自动缩进", {}, Command::ToggleAutoIndent).checked = state.autoIndent;

    // —— 主菜单（顶层：文件▸ / 编辑▸ / 搜索▸ / ── / 视图▸ / ── / 退出）——
    m.addSubmenuEntry(root, L"文件", file);
    m.addSubmenuEntry(root, L"编辑", edit);
    m.addSubmenuEntry(root, L"搜索", search);
    m.addSeparator(root);
    m.addSubmenuEntry(root, L"视图", view);
    m.addSeparator(root);
    m.addItem(root, L"退出", {}, Command::Quit);

    return m;
}

}  // namespace ui
