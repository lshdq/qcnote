// 主菜单内容构建（M1-2）：按需求 E1 表格静态定义完整菜单结构，命令路由 stub。
// 状态源（M2 接入）：hasActiveDocument/activeDocumentHasUrl 由文档系统驱动，
// currentEncoding/darkTheme/wordWrap 由文档/主题/会话驱动。每次打开菜单前按
// 当前状态重建模型（BuildMainMenu 为纯函数，无窗口依赖，可直接单测）。
#pragma once

#include "menu.h"

namespace ui {

// 主菜单构建输入状态。M1 取默认值（无文档、UTF-8、浅色、不换行）；
// M2 起 Main 组装层在弹出前刷新。
struct MainMenuState {
    bool hasActiveDocument = false;  // 保存/另存为/关闭标签页/搜索组/通过编码保存组
    bool activeDocumentHasUrl = false;  // 「通过编码重新加载」组（未命名文档禁用，TC-017）
    enum class Encoding { Utf8, Gbk };
    Encoding currentEncoding = Encoding::Utf8;  // 「通过编码保存」当前编码项打勾
    bool darkTheme = false;  // 视图▸深色主题 勾选态（实现 M6 接，E5）
    bool wordWrap = false;   // 视图▸自动换行 勾选态（实现 M2 接，B2）
};

// 构建主菜单（结构见需求 E1；菜单 id 布局见实现注释）
MenuModel BuildMainMenu(const MainMenuState& state);

}  // namespace ui
