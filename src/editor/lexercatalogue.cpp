// qcnote Lexilla 词法器目录实现（代码着色 T1，DEC-007）。
// 模块符号与词法器名的对照逐一取自 vendored 词法器源文件中的
// LexerModule 注册定义（extern const LexerModule lmXXX(...)），不凭记忆书写。
#include "lexercatalogue.h"

#include <cstring>

#include "ILexer.h"        // vendor/scintilla/include（Sci_Position/SCI_METHOD，须先于 LexerModule.h）
#include "LexerModule.h"   // vendor/lexilla/lexlib（Lexilla::LexerModule）

// 各词法器 TU 在全局作用域定义的 extern const 模块符号（词法器文件内
// `using namespace Lexilla;` + 全局定义，故此处同为全局作用域声明，
// 符号修饰名与上游 src/Lexilla.cxx 的 catalogue 引用完全一致）。
extern const Lexilla::LexerModule lmCPP;         // LexCPP.cxx   "cpp"
extern const Lexilla::LexerModule lmPython;      // LexPython.cxx "python"
extern const Lexilla::LexerModule lmJSON;        // LexJSON.cxx  "json"
extern const Lexilla::LexerModule lmMarkdown;    // LexMarkdown.cxx "markdown"
extern const Lexilla::LexerModule lmHTML;        // LexHTML.cxx  "hypertext"
extern const Lexilla::LexerModule lmXML;         // LexHTML.cxx  "xml"（与 hypertext 同文件）
extern const Lexilla::LexerModule lmCss;         // LexCSS.cxx   "css"
extern const Lexilla::LexerModule lmSQL;         // LexSQL.cxx   "sql"
extern const Lexilla::LexerModule lmBash;        // LexBash.cxx  "bash"
extern const Lexilla::LexerModule lmPowerShell;  // LexPowerShell.cxx "powershell"
extern const Lexilla::LexerModule lmYAML;        // LexYAML.cxx  "yaml"
extern const Lexilla::LexerModule lmLua;         // LexLua.cxx   "lua"
extern const Lexilla::LexerModule lmRust;        // LexRust.cxx  "rust"
extern const Lexilla::LexerModule lmBatch;       // LexBatch.cxx "batch"
extern const Lexilla::LexerModule lmProps;       // LexProps.cxx "props"（properties
                                                 // 词法器，承载 INI 语言，DEC-008）

namespace {

// 静态表：{模块符号引用, 词法器注册名}。名字以各词法器注册定义为准。
struct LexerEntry {
    const Lexilla::LexerModule* module;
    const char* lexerName;
};

constexpr LexerEntry kLexers[] = {
    {&lmCPP, "cpp"},
    {&lmPython, "python"},
    {&lmJSON, "json"},
    {&lmMarkdown, "markdown"},
    {&lmHTML, "hypertext"},
    {&lmXML, "xml"},
    {&lmCss, "css"},
    {&lmSQL, "sql"},
    {&lmBash, "bash"},
    {&lmPowerShell, "powershell"},
    {&lmYAML, "yaml"},
    {&lmLua, "lua"},
    {&lmRust, "rust"},
    {&lmBatch, "batch"},
    {&lmProps, "props"},
};

}  // namespace

namespace editor {

Scintilla::ILexer5* CreateLexerByName(const char* lexerName) {
    if (lexerName == nullptr) {
        return nullptr;
    }
    for (const LexerEntry& entry : kLexers) {
        if (std::strcmp(entry.lexerName, lexerName) == 0) {
            return entry.module->Create();
        }
    }
    return nullptr;
}

}  // namespace editor
