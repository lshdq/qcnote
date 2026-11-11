// 主菜单内容（E1）：文件▸（含编码三级级联）→ 搜索▸ → 视图▸ → 退出。
// 结构与旧 QML 版 TitleBar.qml 主菜单一一对应（git 历史可查），勾选/置灰
// 语义不变，仅数据载体从 QML Menu 换为 MenuModel：
//   - 保存/另存为…/关闭标签页：有活动文档才可用
//   - 通过编码重新加载（整组）：有活动文档且有磁盘路径（未命名禁用）
//   - 通过编码保存（整组）：有活动文档即可用；当前编码项打勾
//   - 搜索四项：有活动文档才可用（欢迎页置灰）
//   - 深色主题/自动换行：勾选态（stub，状态源 M2/M6 接）
// 子菜单叶子（UTF-8/GBK）不单独承载 enabled——父级置灰即整组不可达
// （与 QML 版 Menu.enabled 级联语义一致）。
#include "mainmenu.h"

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

    // —— 主菜单（顶层：文件▸ / 搜索▸ / ── / 视图▸ / ── / 退出）——
    m.addSubmenuEntry(root, L"文件", file);
    m.addSubmenuEntry(root, L"搜索", search);
    m.addSeparator(root);
    m.addSubmenuEntry(root, L"视图", view);
    m.addSeparator(root);
    m.addItem(root, L"退出", {}, Command::Quit);

    return m;
}

}  // namespace ui
