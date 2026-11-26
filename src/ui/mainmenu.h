// 主菜单内容构建（M1-2）：按需求 E1 表格静态定义完整菜单结构，命令路由 stub。
// 状态源（M2 接入）：hasActiveDocument/activeDocumentHasUrl 由文档系统驱动，
// currentEncoding/darkTheme/wordWrap/lineNumbers 由文档/主题/会话驱动。每次打开菜单前按
// 当前状态重建模型（BuildMainMenu 为纯函数，无窗口依赖，可直接单测）。
#pragma once

#include <string_view>

#include "menu.h"

namespace ui {

// 主菜单构建输入状态。M1 取默认值（无文档、UTF-8、浅色、不换行）；
// M2 起 Main 组装层在弹出前刷新。
struct MainMenuState {
    bool hasActiveDocument = false;  // 保存/另存为/关闭标签页/搜索组/通过编码保存组/语言组
    bool activeDocumentHasUrl = false;  // 「通过编码重新加载」组（未命名文档禁用，TC-017）
    enum class Encoding { Utf8, Gbk };
    Encoding currentEncoding = Encoding::Utf8;  // 「通过编码保存」当前编码项打勾
    bool darkTheme = false;  // 视图▸深色主题 勾选态（实现 M6 接，E5）
    bool wordWrap = false;   // 视图▸自动换行 勾选态（实现 M2 接，B2）
    bool lineNumbers = true;  // 视图▸显示行号 勾选态（默认显示，会话驱动）
    bool autoIndent = true;   // 视图▸自动缩进 勾选态（默认开启，会话驱动）
    bool multiCursorActive = false;  // 编辑▸插入编号 使能条件之一（活动编辑器
                                     // 选区数 ≥ 2 的多光标态；组装层在弹出前
                                     // 刷新，列模式填充编号）
    // —— 语言（代码着色 T3）——
    /// 当前生效语言 id（core::LanguageEntry::id，如 "python"；空 = 无语言/
    /// 纯文本渲染）。语言项勾选 = 当前生效语言（TC-006：自动识别结果同样
    /// 显示勾选，与手动指定同态）；取值组装层经 core::effectiveLanguage
    std::string_view effectiveLanguageId;
    /// 是否手动指定语言（false = 自动模式）。「自动」勾选 = false；
    /// 「纯文本」勾选 = true 且 effectiveLanguageId 为空
    bool manualLanguage = false;
};

// 构建主菜单（结构见需求 E1；菜单 id 布局见实现注释）
MenuModel BuildMainMenu(const MainMenuState& state);

}  // namespace ui
