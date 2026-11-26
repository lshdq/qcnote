// qcnote 纯逻辑核心层：语言注册表（代码着色 T2，DEC-002/003）。
//
// 职责：内置 21 种语言的静态只读注册表（id/显示名/扩展名/Lexilla 词法器
// 名/关键词分组/样式槽→语义 token 映射表）+ 纯查询函数 + 文档生效语言计算。
// 纯数据与纯函数：零 Win32/UI/Scintilla 运行时依赖；样式槽数值以带注释的
// 整数常量直录（不 include SciLexer.h——保持 qcnote_core 零 vendor 头依赖，
// 槽位值从 vendor/lexilla/include/SciLexer.h 的 SCE_* 常量逐一核对誊抄，
// 词法器行为依据 vendor/lexilla/lexers/Lex*.cxx 的 Colourise 实际槽位使用）。
//
// 分层契约（与 T3 的对接面）：
// - editor 装配层：lexerName → editor::CreateLexerByName；keywords[i] →
//   SCI_SETKEYWORDS(i)（NUL 结尾字符串可直接传参）；styleMap → 按槽位设色；
// - ui 主题层：SyntaxToken → CurrentPalette() 语义色 token（本枚举即契约，
//   theme 按 token 加语法色板）；
// - 菜单：languageAt/languageCount 迭代构建「语言」子菜单；
// - Document/会话：手动语言 id 与 N\language 键（sessionmanager）。
//
// 语言与词法器承载：15 个 Lexilla 词法器承载 21 语言；C/C++/C#/Java/
// JavaScript/TypeScript/Go 复用 "cpp" 词法器按各自关键词分组区分（DEC-003
// SciTE 传统做法；Go 无独立词法器【DEC-009】）；INI 由 "props" 词法器承载
// 【DEC-008】（properties 与 INI 的 [节]/key=value/;注释 结构同构）。
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "core/document.h"

namespace core {

/// Scintilla SCI_SETKEYWORDS 的关键词分组数（0..8）
constexpr int kKeywordGroupCount = 9;

/// 语义色 token：词法器样式槽 → 主题色板语义 token 的映射终点（与 T3 的
/// 契约：theme 为每个 token 提供浅色/深色取值；着色即 token→色）。
/// 枚举值语义：
enum class SyntaxToken {
    None,          ///< 无着色（默认文字色渲染；纯文本/词法器 DEFAULT 类槽位）
    Comment,       ///< 行注释/块注释（// # /* */ -- ; '''）
    CommentDoc,    ///< 文档注释（/** */、//!、--[[--、///）
    Keyword,       ///< 语言主关键字（if/for/class/def...）
    Keyword2,      ///< 次级关键字/字面量/内建（true/false/null、内建函数、上下文关键字）
    String,        ///< 普通字符串（单/双引号）
    String2,       ///< 原始/多行/模板字符串（三引号、raw、here-doc、模板字面量、CDATA）
    Number,        ///< 数字字面量（含词法器归类为数字的 UUID 等）
    Operator,      ///< 操作符/标点（含 = 赋值号、列表标记、URL 等次要记号归并）
    Preprocessor,  ///< 预处理/指令（#include、@media、<?xml、YAML --- 文档分隔）
    Identifier,    ///< 高亮标识符（非关键字单词；未知属性名、SGML 参数等）
    Type,          ///< 类型/类名（Python class 名、内建类型、CSS 类选择器、Rust 内建类型）
    Function,      ///< 函数/定义名（Python def 名、Rust 宏、cmdlet、CSS 标签）
    Tag,           ///< 标签/结构标记（HTML/XML 标签名、Markdown 标题记号、<script> 过渡）
    Attribute,     ///< 属性名（HTML/XML 属性、CSS 属性选择器、Python 装饰器）
    Variable,      ///< 变量/引用（Shell $var、PowerShell $var、Rust 生命周期 'a、YAML 锚引用）
    Escape,        ///< 转义序列（\n、\u1234、HTML 实体 &amp;）
    Regex,         ///< 正则字面量（/re/、r"..."）
    Section,       ///< 节标记（INI/properties [section]）
    Key,           ///< 键（JSON 属性名、INI/properties key、YAML key）
    Value,         ///< 值（CSS 值、YAML 文本值、properties 默认值）
    Label,         ///< 标签（Batch :label、Lua ::label::）
    Error,         ///< 词法错误（未终止字符串 STRINGEOL、未知标签、语法错误槽）
    // —— Markdown 语义（需求映射质量要求：标题/强调/代码/链接四类区分）——
    Header,        ///< Markdown 标题（# ~ ###### 行，全部级别归并）
    Strong,        ///< Markdown 粗体（**x** / __x__）
    Em,            ///< Markdown 斜体（*x* / _x_；删除线 ~~x~~ 归并）
    Link,          ///< Markdown 链接（[text](url)）
};

/// 样式槽 → 语义 token 映射条目（词法器输出的样式槽号到主题 token）
struct StyleMapEntry {
    int slot;          ///< Scintilla 样式槽（SCE_* 常量数值；0..127）
    SyntaxToken token; ///< 语义色 token（None 仅出现在表尾哨兵条目）
};

/// 语言条目（静态只读数据；全部指针指向程序内静态存储期字面量）
struct LanguageEntry {
    std::string_view id;    ///< 稳定 ASCII 小写 id（如 "python"；会话 N\language 键值）
    const wchar_t* displayName;      ///< 用户可见显示名（如 L"Python"；菜单/状态栏）
    const wchar_t* const* extensions; ///< 小写扩展名表（不带点），nullptr 结尾
    /// Lexilla 词法器注册名（NUL 结尾字面量；editor 层直接传
    /// CreateLexerByName 与 SCI_SETILEXER 链路）
    const char* lexerName;
    /// SCI_SETKEYWORDS 各组关键词（空格分隔 ASCII 单词串；nullptr/空串 =
    /// 组空不注入）。NUL 结尾字面量——editor 层可直接作为 LPCSTR 传参
    const char* const keywords[kKeywordGroupCount];
    /// 样式槽映射表：只收录有语义着色的槽位（词法器 DEFAULT/LINE_BEGIN 等
    /// 无着色槽位不入表，编辑器默认样式渲染），表尾 {-1, None} 哨兵
    const StyleMapEntry* styleMap;
};

/// 手动「纯文本」保留语言 id（语言注册表之外的合法手动指定值）：
/// Document::setLanguage(kPlainTextLanguageId) = 强制不着色；会话按此值
/// 落盘恢复。区别于「自动」模式（clearLanguage 后按扩展名识别）
inline constexpr std::string_view kPlainTextLanguageId = "plaintext";

/// DEC-005：着色内容阈值 16 MiB。内容字节数超过的文档跳过词法装配按纯
/// 文本渲染（Scintilla 增量着色仅触达可见行，16MB 内开销可控；阈值是
/// 极端大文件兜底保险，无全局开关）
constexpr std::size_t kSyntaxColoringMaxBytes = 16ull * 1024 * 1024;

// —— 查询接口（纯函数）——

/// 按 id 查语言条目（精确匹配，ASCII 小写约定）；未知 id 返回 nullptr
const LanguageEntry* languageById(std::string_view id);

/// 按文件名扩展名识别语言（大小写不敏感；扩展名 = 最后一个 '.' 之后的
/// 部分，无点/空扩展不命中）。未命中返回 nullptr（纯文本语义）
const LanguageEntry* languageForFileName(const std::wstring& fileName);

/// 语言条目数（现 21；静态编译期常量）
constexpr std::size_t languageCount() { return 21; }

/// 按下标取语言条目（「语言」子菜单构建迭代用）；越界返回 nullptr
const LanguageEntry* languageAt(std::size_t index);

/// 合法手动语言 id：注册表 id 或 "plaintext" 保留值（会话 language 键
/// 校验用；未知/非法 id 由调用方丢弃回退自动识别）
bool isValidLanguageId(std::string_view id);

// —— 文档生效语言计算（纯函数）——

/// 文档生效语言（身份判定；菜单勾选态/状态栏语言名用）：
/// - 手动指定优先：kPlainTextLanguageId → nullptr（强制纯文本）；未知 id
///   （会话数据损坏等）容错回退自动识别（与 encoding 非法回退语义一致）；
/// - 自动模式：按 displayName（url 文件名派生）扩展名识别；无 url 的
///   未命名文档 → nullptr（纯文本）。
/// 不判 16MB 阈值——语言身份与着色决策分离（阈值只影响装配，见
/// effectiveLanguageForColoring；另存为/重命名换扩展名后 url 变化触发
/// metadata 通知，调用方重新计算即自动重识别，无需本层主动动作）
const LanguageEntry* effectiveLanguage(const Document& document);

/// 着色装配用生效语言：effectiveLanguage 语义 + DEC-005 16MB 阈值——
/// 内容字节数超过 kSyntaxColoringMaxBytes 时返回 nullptr（跳过着色按纯
/// 文本渲染）。editor 装配层（打开/装载/重载/主题重着色）调用本变体
const LanguageEntry* effectiveLanguageForColoring(const Document& document);

} // namespace core
