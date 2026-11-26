// qcnote Lexilla 词法器目录（代码着色 T1，DEC-007）：按词法器名创建 ILexer5。
//
// 背景：vendored Lexilla 5.4.5 为精选子集（vendor/lexilla：lexlib 完整 +
// lexers 14 个文件 15 个词法器），不含上游 src/Lexilla.cxx（其 CatalogueModules
// 静态引用全部 ~50 词法器，与「小体积绿色工具」目标冲突）。本文件在项目侧复刻
// CreateLexer 的按名分发：extern 引用各词法器 TU 定义的全局 LexerModule 符号，
// 静态表按名匹配后 module.Create()。
//
// 上游缺失语言的承载决策（DEC-008/009）：
//   ini —— 上游无 "ini" 词法器，由 "props" 词法器承载（properties 与 INI 结构
//          同构：[节]/key=value/;注释，SCE_PROPS_* 槽位匹配）
//   go  —— 上游无 "go" 词法器，由 "cpp" 词法器 + Go 关键词表承载（C 系语法
//          兼容；关键词注入在 core 语言注册表，非本层职责）
//
// 分层：本文件属 editor 层（Scintilla/Lexilla 封装），允许 include Scintilla 头。
#pragma once

#include "ILexer.h"

namespace editor {

// 按词法器名（Lexilla 注册名，如 "cpp"/"python"/"hypertext"）创建词法器实例。
// 命中返回新建 ILexer5（调用方负责 Release）；未知名或 nullptr 入参返回 nullptr。
Scintilla::ILexer5* CreateLexerByName(const char* lexerName);

}  // namespace editor
