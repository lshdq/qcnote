// qcnote 纯逻辑核心层：语言注册表数据与查询实现（代码着色 T2）。
//
// 样式槽数值来源：vendor/lexilla/include/SciLexer.h 的 SCE_* 常量（逐一
// 核对誊抄，每条注释保留 SCE 常量名）；槽位实际使用依据各词法器
// vendor/lexilla/lexers/Lex*.cxx 的 Colourise 实现（见各表注释）。
// 关键词表内容按公开语言规范常见集合整理（每语言 30~80 词，追求常用
// 覆盖而非穷举）。
//
// 设计说明（映射取舍）：
// - 映射表只收录有语义着色的槽位；词法器 DEFAULT/LINE_BEGIN/PRECHAR 等
//   「无着色」槽位不入表（编辑器默认文字色渲染），次要槽位归并到相近
//   token（如 Markdown STRIKEOUT→Em、HTML 各嵌入脚本注释统一 Comment）；
// - HTML/XML 不注入关键词组 0：LexHTML 的 classifyTagHTML/classifyAttribHTML
//   在「无关键词」时把一切标签/属性按已知（TAG/ATTRIBUTE）着色——注入
//   有限词表反而会使词表外的正常标签/属性落入 *UNKNOWN 错误槽（红色噪音）；
// - cpp 系 7 语言共享同一份槽位映射表（同一 "cpp" 词法器，槽位语义一致；
//   Go 按 DEC-009 亦按 C 系槽位）；差异只在关键词分组与扩展名。
#include "core/syntax.h"

namespace core {
namespace {

// ===========================================================================
// 样式槽映射表（词法器 → 语义 token；槽位值核对自 SciLexer.h）
// ===========================================================================

/// "cpp" 词法器（C/C++/C#/Java/JavaScript/TypeScript/Go 共用；DEC-009）
constexpr StyleMapEntry kCppStyleMap[] = {
    {1, SyntaxToken::Comment},        // SCE_C_COMMENT（块注释）
    {2, SyntaxToken::Comment},        // SCE_C_COMMENTLINE（行注释）
    {3, SyntaxToken::CommentDoc},     // SCE_C_COMMENTDOC
    {4, SyntaxToken::Number},         // SCE_C_NUMBER
    {5, SyntaxToken::Keyword},        // SCE_C_WORD（组 0 主关键字）
    {6, SyntaxToken::String},         // SCE_C_STRING
    {7, SyntaxToken::String},         // SCE_C_CHARACTER
    {8, SyntaxToken::Number},         // SCE_C_UUID（归并数字）
    {9, SyntaxToken::Preprocessor},   // SCE_C_PREPROCESSOR
    {10, SyntaxToken::Operator},      // SCE_C_OPERATOR
    {11, SyntaxToken::Identifier},    // SCE_C_IDENTIFIER
    {12, SyntaxToken::Error},         // SCE_C_STRINGEOL（未终止字符串）
    {13, SyntaxToken::String},        // SCE_C_VERBATIM（@"..."）
    {14, SyntaxToken::Regex},         // SCE_C_REGEX
    {15, SyntaxToken::CommentDoc},    // SCE_C_COMMENTLINEDOC（///）
    {16, SyntaxToken::Keyword2},      // SCE_C_WORD2（组 1 次级关键字）
    {17, SyntaxToken::CommentDoc},    // SCE_C_COMMENTDOCKEYWORD
    {18, SyntaxToken::Error},         // SCE_C_COMMENTDOCKEYWORDERROR
    {19, SyntaxToken::Type},          // SCE_C_GLOBALCLASS（组 3 全局类/类型）
    {20, SyntaxToken::String2},       // SCE_C_STRINGRAW（R"(...)"）
    {21, SyntaxToken::String2},       // SCE_C_TRIPLEVERBATIM
    {22, SyntaxToken::String},        // SCE_C_HASHQUOTEDSTRING
    {23, SyntaxToken::Comment},       // SCE_C_PREPROCESSORCOMMENT（块注释归并）
    {24, SyntaxToken::CommentDoc},    // SCE_C_PREPROCESSORCOMMENTDOC
    {25, SyntaxToken::String},        // SCE_C_USERLITERAL（数字分隔符类字面量）
    {26, SyntaxToken::Comment},       // SCE_C_TASKMARKER（TODO 等归并注释）
    {27, SyntaxToken::Escape},        // SCE_C_ESCAPESEQUENCE
    {-1, SyntaxToken::None},          // 哨兵
};

/// "python" 词法器
constexpr StyleMapEntry kPythonStyleMap[] = {
    {1, SyntaxToken::Comment},      // SCE_P_COMMENTLINE
    {2, SyntaxToken::Number},       // SCE_P_NUMBER
    {3, SyntaxToken::String},       // SCE_P_STRING
    {4, SyntaxToken::String},       // SCE_P_CHARACTER
    {5, SyntaxToken::Keyword},      // SCE_P_WORD（组 0）
    {6, SyntaxToken::String2},      // SCE_P_TRIPLE（'''...'''）
    {7, SyntaxToken::String2},      // SCE_P_TRIPLEDOUBLE（"""..."""）
    {8, SyntaxToken::Type},         // SCE_P_CLASSNAME（class 后类名）
    {9, SyntaxToken::Function},     // SCE_P_DEFNAME（def 后函数名）
    {10, SyntaxToken::Operator},    // SCE_P_OPERATOR
    {11, SyntaxToken::Identifier},  // SCE_P_IDENTIFIER
    {12, SyntaxToken::Comment},     // SCE_P_COMMENTBLOCK（块注释归并行注释）
    {13, SyntaxToken::Error},       // SCE_P_STRINGEOL
    {14, SyntaxToken::Keyword2},    // SCE_P_WORD2（组 1 高亮标识符/内建）
    {15, SyntaxToken::Attribute},   // SCE_P_DECORATOR（@decorator）
    {16, SyntaxToken::String},      // SCE_P_FSTRING
    {17, SyntaxToken::String},      // SCE_P_FCHARACTER
    {18, SyntaxToken::String2},     // SCE_P_FTRIPLE
    {19, SyntaxToken::String2},     // SCE_P_FTRIPLEDOUBLE
    {20, SyntaxToken::Identifier},  // SCE_P_ATTRIBUTE（obj.attr 的 attr）
    {-1, SyntaxToken::None},        // 哨兵
};

/// "json" 词法器
constexpr StyleMapEntry kJsonStyleMap[] = {
    {1, SyntaxToken::Number},      // SCE_JSON_NUMBER
    {2, SyntaxToken::String},      // SCE_JSON_STRING
    {3, SyntaxToken::Error},       // SCE_JSON_STRINGEOL
    {4, SyntaxToken::Key},         // SCE_JSON_PROPERTYNAME（"key":）
    {5, SyntaxToken::Escape},      // SCE_JSON_ESCAPESEQUENCE
    {6, SyntaxToken::Comment},     // SCE_JSON_LINECOMMENT
    {7, SyntaxToken::Comment},     // SCE_JSON_BLOCKCOMMENT
    {8, SyntaxToken::Operator},    // SCE_JSON_OPERATOR
    {9, SyntaxToken::String2},     // SCE_JSON_URI（URL 字面量归并次要字符串）
    {10, SyntaxToken::Identifier}, // SCE_JSON_COMPACTIRI
    {11, SyntaxToken::Keyword},    // SCE_JSON_KEYWORD（true/false/null，组 0）
    {12, SyntaxToken::Keyword2},   // SCE_JSON_LDKEYWORD（JSON-LD @词，组 1）
    {13, SyntaxToken::Error},      // SCE_JSON_ERROR
    {-1, SyntaxToken::None},       // 哨兵
};

/// "markdown" 词法器
constexpr StyleMapEntry kMarkdownStyleMap[] = {
    {2, SyntaxToken::Strong},     // SCE_MARKDOWN_STRONG1（**x**）
    {3, SyntaxToken::Strong},     // SCE_MARKDOWN_STRONG2（__x__）
    {4, SyntaxToken::Em},         // SCE_MARKDOWN_EM1（*x*）
    {5, SyntaxToken::Em},         // SCE_MARKDOWN_EM2（_x_）
    {6, SyntaxToken::Header},     // SCE_MARKDOWN_HEADER1
    {7, SyntaxToken::Header},     // SCE_MARKDOWN_HEADER2
    {8, SyntaxToken::Header},     // SCE_MARKDOWN_HEADER3
    {9, SyntaxToken::Header},     // SCE_MARKDOWN_HEADER4
    {10, SyntaxToken::Header},    // SCE_MARKDOWN_HEADER5
    {11, SyntaxToken::Header},    // SCE_MARKDOWN_HEADER6
    {13, SyntaxToken::Operator},  // SCE_MARKDOWN_ULIST_ITEM（- / * / +）
    {14, SyntaxToken::Operator},  // SCE_MARKDOWN_OLIST_ITEM（1.）
    {15, SyntaxToken::Comment},   // SCE_MARKDOWN_BLOCKQUOTE（> 引用归并注释色）
    {16, SyntaxToken::Em},        // SCE_MARKDOWN_STRIKEOUT（~~x~~ 归并弱强调）
    {17, SyntaxToken::Operator},  // SCE_MARKDOWN_HRULE（---）
    {18, SyntaxToken::Link},      // SCE_MARKDOWN_LINK
    {19, SyntaxToken::String2},   // SCE_MARKDOWN_CODE（`x` 行内代码）
    {20, SyntaxToken::String2},   // SCE_MARKDOWN_CODE2（``x``）
    {21, SyntaxToken::String2},   // SCE_MARKDOWN_CODEBK（``` 代码块）
    {-1, SyntaxToken::None},      // 哨兵
};

/// "hypertext" 词法器（HTML：基础 0..31 + 嵌入脚本 HJ/HJA/HB/HBA/HP/HPA/HPHP）
constexpr StyleMapEntry kHtmlStyleMap[] = {
    {1, SyntaxToken::Tag},           // SCE_H_TAG
    {2, SyntaxToken::Error},         // SCE_H_TAGUNKNOWN
    {3, SyntaxToken::Attribute},     // SCE_H_ATTRIBUTE
    {4, SyntaxToken::Error},         // SCE_H_ATTRIBUTEUNKNOWN
    {5, SyntaxToken::Number},        // SCE_H_NUMBER
    {6, SyntaxToken::String},        // SCE_H_DOUBLESTRING
    {7, SyntaxToken::String},        // SCE_H_SINGLESTRING
    {8, SyntaxToken::Operator},      // SCE_H_OTHER（标签内 = / 空白等）
    {9, SyntaxToken::Comment},       // SCE_H_COMMENT
    {10, SyntaxToken::Escape},       // SCE_H_ENTITY（&amp; 实体）
    {11, SyntaxToken::Tag},          // SCE_H_TAGEND（/>）
    {12, SyntaxToken::Preprocessor}, // SCE_H_XMLSTART（<?xml）
    {13, SyntaxToken::Preprocessor}, // SCE_H_XMLEND（?>）
    {14, SyntaxToken::Tag},          // SCE_H_SCRIPT（<script> 过渡段）
    {15, SyntaxToken::Preprocessor}, // SCE_H_ASP
    {16, SyntaxToken::Preprocessor}, // SCE_H_ASPAT
    {17, SyntaxToken::String2},      // SCE_H_CDATA
    {18, SyntaxToken::Preprocessor}, // SCE_H_QUESTION（<?）
    {19, SyntaxToken::String},       // SCE_H_VALUE（无引号属性值）
    {20, SyntaxToken::Comment},      // SCE_H_XCCOMMENT
    {22, SyntaxToken::Preprocessor}, // SCE_H_SGML_COMMAND（<!DOCTYPE）
    {23, SyntaxToken::Identifier},   // SCE_H_SGML_1ST_PARAM
    {24, SyntaxToken::String},       // SCE_H_SGML_DOUBLESTRING
    {25, SyntaxToken::String},       // SCE_H_SGML_SIMPLESTRING
    {26, SyntaxToken::Error},        // SCE_H_SGML_ERROR
    {27, SyntaxToken::Preprocessor}, // SCE_H_SGML_SPECIAL
    {28, SyntaxToken::Escape},       // SCE_H_SGML_ENTITY
    {29, SyntaxToken::Comment},      // SCE_H_SGML_COMMENT
    {30, SyntaxToken::Comment},      // SCE_H_SGML_1ST_PARAM_COMMENT
    // 嵌入 JavaScript（<script>：无 type 或 type=text/javascript）
    {42, SyntaxToken::Comment},     // SCE_HJ_COMMENT
    {43, SyntaxToken::Comment},     // SCE_HJ_COMMENTLINE
    {44, SyntaxToken::CommentDoc},  // SCE_HJ_COMMENTDOC
    {45, SyntaxToken::Number},      // SCE_HJ_NUMBER
    {46, SyntaxToken::Identifier},  // SCE_HJ_WORD
    {47, SyntaxToken::Keyword},     // SCE_HJ_KEYWORD（组 1 JS 关键字）
    {48, SyntaxToken::String},      // SCE_HJ_DOUBLESTRING
    {49, SyntaxToken::String},      // SCE_HJ_SINGLESTRING
    {50, SyntaxToken::Operator},    // SCE_HJ_SYMBOLS
    {51, SyntaxToken::Error},       // SCE_HJ_STRINGEOL
    {52, SyntaxToken::Regex},       // SCE_HJ_REGEX
    {53, SyntaxToken::String2},     // SCE_HJ_TEMPLATELITERAL（`...`）
    // 嵌入 ASP JavaScript
    {57, SyntaxToken::Comment},     // SCE_HJA_COMMENT
    {58, SyntaxToken::Comment},     // SCE_HJA_COMMENTLINE
    {59, SyntaxToken::CommentDoc},  // SCE_HJA_COMMENTDOC
    {60, SyntaxToken::Number},      // SCE_HJA_NUMBER
    {61, SyntaxToken::Identifier},  // SCE_HJA_WORD
    {62, SyntaxToken::Keyword},     // SCE_HJA_KEYWORD
    {63, SyntaxToken::String},      // SCE_HJA_DOUBLESTRING
    {64, SyntaxToken::String},      // SCE_HJA_SINGLESTRING
    {65, SyntaxToken::Operator},    // SCE_HJA_SYMBOLS
    {66, SyntaxToken::Error},       // SCE_HJA_STRINGEOL
    {67, SyntaxToken::Regex},       // SCE_HJA_REGEX
    {68, SyntaxToken::String2},     // SCE_HJA_TEMPLATELITERAL
    // 嵌入 VBScript（<script language="vbscript">）
    {72, SyntaxToken::Comment},     // SCE_HB_COMMENTLINE
    {73, SyntaxToken::Number},      // SCE_HB_NUMBER
    {74, SyntaxToken::Identifier},  // SCE_HB_WORD
    {75, SyntaxToken::String},      // SCE_HB_STRING
    {76, SyntaxToken::Identifier},  // SCE_HB_IDENTIFIER
    {77, SyntaxToken::Error},       // SCE_HB_STRINGEOL
    // 嵌入 ASP VBScript
    {82, SyntaxToken::Comment},     // SCE_HBA_COMMENTLINE
    {83, SyntaxToken::Number},      // SCE_HBA_NUMBER
    {84, SyntaxToken::Identifier},  // SCE_HBA_WORD
    {85, SyntaxToken::String},      // SCE_HBA_STRING
    {86, SyntaxToken::Identifier},  // SCE_HBA_IDENTIFIER
    {87, SyntaxToken::Error},       // SCE_HBA_STRINGEOL
    // 嵌入 Python（<script type="text/x-python">）
    {92, SyntaxToken::Comment},     // SCE_HP_COMMENTLINE
    {93, SyntaxToken::Number},      // SCE_HP_NUMBER
    {94, SyntaxToken::String},      // SCE_HP_STRING
    {95, SyntaxToken::String},      // SCE_HP_CHARACTER
    {96, SyntaxToken::Keyword},     // SCE_HP_WORD
    {97, SyntaxToken::String2},     // SCE_HP_TRIPLE
    {98, SyntaxToken::String2},     // SCE_HP_TRIPLEDOUBLE
    {99, SyntaxToken::Type},        // SCE_HP_CLASSNAME
    {100, SyntaxToken::Function},   // SCE_HP_DEFNAME
    {101, SyntaxToken::Operator},   // SCE_HP_OPERATOR
    {102, SyntaxToken::Identifier}, // SCE_HP_IDENTIFIER
    // 嵌入 ASP Python
    {107, SyntaxToken::Comment},     // SCE_HPA_COMMENTLINE
    {108, SyntaxToken::Number},      // SCE_HPA_NUMBER
    {109, SyntaxToken::String},      // SCE_HPA_STRING
    {110, SyntaxToken::String},      // SCE_HPA_CHARACTER
    {111, SyntaxToken::Keyword},     // SCE_HPA_WORD
    {112, SyntaxToken::String2},     // SCE_HPA_TRIPLE
    {113, SyntaxToken::String2},     // SCE_HPA_TRIPLEDOUBLE
    {114, SyntaxToken::Type},        // SCE_HPA_CLASSNAME
    {115, SyntaxToken::Function},    // SCE_HPA_DEFNAME
    {116, SyntaxToken::Operator},    // SCE_HPA_OPERATOR
    {117, SyntaxToken::Identifier},  // SCE_HPA_IDENTIFIER
    // 嵌入 PHP
    {119, SyntaxToken::String2},   // SCE_HPHP_HSTRING（heredoc）
    {120, SyntaxToken::String},    // SCE_HPHP_SIMPLESTRING
    {121, SyntaxToken::Keyword},   // SCE_HPHP_WORD
    {122, SyntaxToken::Number},    // SCE_HPHP_NUMBER
    {123, SyntaxToken::Variable},  // SCE_HPHP_VARIABLE（$var）
    {124, SyntaxToken::Comment},   // SCE_HPHP_COMMENT
    {125, SyntaxToken::Comment},   // SCE_HPHP_COMMENTLINE
    {126, SyntaxToken::Variable},  // SCE_HPHP_HSTRING_VARIABLE
    {127, SyntaxToken::Operator},  // SCE_HPHP_OPERATOR
    {-1, SyntaxToken::None},       // 哨兵
};

/// "xml" 词法器（LexHTML 的 XML 模式：复用 SCE_H_ 槽位，无嵌入脚本态）
constexpr StyleMapEntry kXmlStyleMap[] = {
    {1, SyntaxToken::Tag},           // SCE_H_TAG
    {2, SyntaxToken::Error},         // SCE_H_TAGUNKNOWN
    {3, SyntaxToken::Attribute},     // SCE_H_ATTRIBUTE
    {4, SyntaxToken::Error},         // SCE_H_ATTRIBUTEUNKNOWN
    {5, SyntaxToken::Number},        // SCE_H_NUMBER
    {6, SyntaxToken::String},        // SCE_H_DOUBLESTRING
    {7, SyntaxToken::String},        // SCE_H_SINGLESTRING
    {8, SyntaxToken::Operator},      // SCE_H_OTHER
    {9, SyntaxToken::Comment},       // SCE_H_COMMENT
    {10, SyntaxToken::Escape},       // SCE_H_ENTITY
    {11, SyntaxToken::Tag},          // SCE_H_TAGEND
    {12, SyntaxToken::Preprocessor}, // SCE_H_XMLSTART
    {13, SyntaxToken::Preprocessor}, // SCE_H_XMLEND
    {17, SyntaxToken::String2},      // SCE_H_CDATA
    {18, SyntaxToken::Preprocessor}, // SCE_H_QUESTION
    {19, SyntaxToken::String},       // SCE_H_VALUE
    {22, SyntaxToken::Preprocessor}, // SCE_H_SGML_COMMAND
    {23, SyntaxToken::Identifier},   // SCE_H_SGML_1ST_PARAM
    {24, SyntaxToken::String},       // SCE_H_SGML_DOUBLESTRING
    {25, SyntaxToken::String},       // SCE_H_SGML_SIMPLESTRING
    {26, SyntaxToken::Error},        // SCE_H_SGML_ERROR
    {27, SyntaxToken::Preprocessor}, // SCE_H_SGML_SPECIAL
    {28, SyntaxToken::Escape},       // SCE_H_SGML_ENTITY
    {29, SyntaxToken::Comment},      // SCE_H_SGML_COMMENT
    {30, SyntaxToken::Comment},      // SCE_H_SGML_1ST_PARAM_COMMENT
    {-1, SyntaxToken::None},         // 哨兵
};

/// "css" 词法器
constexpr StyleMapEntry kCssStyleMap[] = {
    {1, SyntaxToken::Tag},        // SCE_CSS_TAG（元素选择器）
    {2, SyntaxToken::Type},       // SCE_CSS_CLASS（.class 选择器）
    {3, SyntaxToken::Keyword2},   // SCE_CSS_PSEUDOCLASS（:hover）
    {4, SyntaxToken::Keyword2},   // SCE_CSS_UNKNOWN_PSEUDOCLASS（未知伪类归并）
    {5, SyntaxToken::Operator},   // SCE_CSS_OPERATOR
    {6, SyntaxToken::Identifier}, // SCE_CSS_IDENTIFIER（CSS1 属性名，组 0）
    {7, SyntaxToken::Identifier}, // SCE_CSS_UNKNOWN_IDENTIFIER（未知属性归并）
    {8, SyntaxToken::Value},      // SCE_CSS_VALUE（属性取值）
    {9, SyntaxToken::Comment},    // SCE_CSS_COMMENT
    {10, SyntaxToken::Type},      // SCE_CSS_ID（#id 选择器）
    {11, SyntaxToken::Keyword},   // SCE_CSS_IMPORTANT（!important）
    {12, SyntaxToken::Preprocessor}, // SCE_CSS_DIRECTIVE（@media）
    {13, SyntaxToken::String},    // SCE_CSS_DOUBLESTRING
    {14, SyntaxToken::String},    // SCE_CSS_SINGLESTRING
    {15, SyntaxToken::Identifier}, // SCE_CSS_IDENTIFIER2（CSS2 属性，组 2）
    {16, SyntaxToken::Attribute}, // SCE_CSS_ATTRIBUTE（[attr] 选择器）
    {17, SyntaxToken::Identifier}, // SCE_CSS_IDENTIFIER3（CSS3 属性，组 3）
    {18, SyntaxToken::Keyword2},  // SCE_CSS_PSEUDOELEMENT（::before）
    {19, SyntaxToken::Identifier}, // SCE_CSS_EXTENDED_IDENTIFIER
    {20, SyntaxToken::Keyword2},  // SCE_CSS_EXTENDED_PSEUDOCLASS
    {21, SyntaxToken::Keyword2},  // SCE_CSS_EXTENDED_PSEUDOELEMENT
    {22, SyntaxToken::Preprocessor}, // SCE_CSS_GROUP_RULE（@media 组规则体）
    {23, SyntaxToken::Variable},  // SCE_CSS_VARIABLE（SCSS/Less 变量）
    {-1, SyntaxToken::None},      // 哨兵
};

/// "sql" 词法器
constexpr StyleMapEntry kSqlStyleMap[] = {
    {1, SyntaxToken::Comment},      // SCE_SQL_COMMENT
    {2, SyntaxToken::Comment},      // SCE_SQL_COMMENTLINE（-- 行注释）
    {3, SyntaxToken::CommentDoc},   // SCE_SQL_COMMENTDOC
    {4, SyntaxToken::Number},       // SCE_SQL_NUMBER
    {5, SyntaxToken::Keyword},      // SCE_SQL_WORD（组 0 关键字）
    {6, SyntaxToken::String},       // SCE_SQL_STRING
    {7, SyntaxToken::String},       // SCE_SQL_CHARACTER
    {8, SyntaxToken::Preprocessor}, // SCE_SQL_SQLPLUS（SQL*Plus 指令）
    {9, SyntaxToken::Operator},     // SCE_SQL_SQLPLUS_PROMPT
    {10, SyntaxToken::Operator},    // SCE_SQL_OPERATOR
    {11, SyntaxToken::Identifier},  // SCE_SQL_IDENTIFIER
    {13, SyntaxToken::Comment},     // SCE_SQL_SQLPLUS_COMMENT
    {15, SyntaxToken::CommentDoc},  // SCE_SQL_COMMENTLINEDOC
    {16, SyntaxToken::Keyword2},    // SCE_SQL_WORD2（组 1 数据类型等）
    {17, SyntaxToken::CommentDoc},  // SCE_SQL_COMMENTDOCKEYWORD
    {18, SyntaxToken::Error},       // SCE_SQL_COMMENTDOCKEYWORDERROR
    {23, SyntaxToken::Identifier},  // SCE_SQL_QUOTEDIDENTIFIER（"ident"）
    {24, SyntaxToken::Operator},    // SCE_SQL_QOPERATOR（@变量操作符）
    {-1, SyntaxToken::None},        // 哨兵
};

/// "bash" 词法器
constexpr StyleMapEntry kBashStyleMap[] = {
    {1, SyntaxToken::Error},     // SCE_SH_ERROR
    {2, SyntaxToken::Comment},   // SCE_SH_COMMENTLINE（#）
    {3, SyntaxToken::Number},    // SCE_SH_NUMBER
    {4, SyntaxToken::Keyword},   // SCE_SH_WORD（组 0 关键字）
    {5, SyntaxToken::String},    // SCE_SH_STRING
    {6, SyntaxToken::String},    // SCE_SH_CHARACTER
    {7, SyntaxToken::Operator},  // SCE_SH_OPERATOR
    {8, SyntaxToken::Identifier}, // SCE_SH_IDENTIFIER
    {9, SyntaxToken::Variable},  // SCE_SH_SCALAR（$var）
    {10, SyntaxToken::Variable}, // SCE_SH_PARAM（$1 ${x}）
    {11, SyntaxToken::String2},  // SCE_SH_BACKTICKS（`cmd`）
    {12, SyntaxToken::Keyword2}, // SCE_SH_HERE_DELIM（<<EOF 记号）
    {13, SyntaxToken::String2},  // SCE_SH_HERE_Q（here-doc 内容）
    {-1, SyntaxToken::None},     // 哨兵
};

/// "powershell" 词法器
constexpr StyleMapEntry kPowerShellStyleMap[] = {
    {1, SyntaxToken::Comment},     // SCE_POWERSHELL_COMMENT（# 行注释）
    {2, SyntaxToken::String},      // SCE_POWERSHELL_STRING
    {3, SyntaxToken::String},      // SCE_POWERSHELL_CHARACTER
    {4, SyntaxToken::Number},      // SCE_POWERSHELL_NUMBER
    {5, SyntaxToken::Variable},    // SCE_POWERSHELL_VARIABLE（$var）
    {6, SyntaxToken::Operator},    // SCE_POWERSHELL_OPERATOR
    {7, SyntaxToken::Identifier},  // SCE_POWERSHELL_IDENTIFIER
    {8, SyntaxToken::Keyword},     // SCE_POWERSHELL_KEYWORD（组 0 命令）
    {9, SyntaxToken::Function},    // SCE_POWERSHELL_CMDLET（组 1 cmdlet）
    {10, SyntaxToken::Keyword2},   // SCE_POWERSHELL_ALIAS（组 2 别名）
    {11, SyntaxToken::Function},   // SCE_POWERSHELL_FUNCTION（组 3 函数）
    {13, SyntaxToken::Comment},    // SCE_POWERSHELL_COMMENTSTREAM（<# #>）
    {14, SyntaxToken::String2},    // SCE_POWERSHELL_HERE_STRING（@" "@）
    {15, SyntaxToken::String2},    // SCE_POWERSHELL_HERE_CHARACTER（@' '@）
    {16, SyntaxToken::CommentDoc}, // SCE_POWERSHELL_COMMENTDOCKEYWORD
    {-1, SyntaxToken::None},       // 哨兵
};

/// "yaml" 词法器
constexpr StyleMapEntry kYamlStyleMap[] = {
    {1, SyntaxToken::Comment},     // SCE_YAML_COMMENT（#）
    {2, SyntaxToken::Key},         // SCE_YAML_IDENTIFIER（key:）
    {3, SyntaxToken::Keyword},     // SCE_YAML_KEYWORD（true/false/null，组 0）
    {4, SyntaxToken::Number},      // SCE_YAML_NUMBER
    {5, SyntaxToken::Variable},    // SCE_YAML_REFERENCE（*ref / &anchor）
    {6, SyntaxToken::Preprocessor}, // SCE_YAML_DOCUMENT（--- / ...）
    {7, SyntaxToken::Value},       // SCE_YAML_TEXT（无引号文本值）
    {8, SyntaxToken::Error},       // SCE_YAML_ERROR
    {9, SyntaxToken::Operator},    // SCE_YAML_OPERATOR
    {-1, SyntaxToken::None},       // 哨兵
};

/// "lua" 词法器
constexpr StyleMapEntry kLuaStyleMap[] = {
    {1, SyntaxToken::Comment},      // SCE_LUA_COMMENT（--[[ ]]）
    {2, SyntaxToken::Comment},      // SCE_LUA_COMMENTLINE（--）
    {3, SyntaxToken::CommentDoc},   // SCE_LUA_COMMENTDOC（---）
    {4, SyntaxToken::Number},       // SCE_LUA_NUMBER
    {5, SyntaxToken::Keyword},      // SCE_LUA_WORD（组 0 关键字）
    {6, SyntaxToken::String},       // SCE_LUA_STRING
    {7, SyntaxToken::String},       // SCE_LUA_CHARACTER
    {8, SyntaxToken::String2},      // SCE_LUA_LITERALSTRING（[[...]]）
    {9, SyntaxToken::Preprocessor}, // SCE_LUA_PREPROCESSOR（#! 归并指令）
    {10, SyntaxToken::Operator},    // SCE_LUA_OPERATOR
    {11, SyntaxToken::Identifier},  // SCE_LUA_IDENTIFIER
    {12, SyntaxToken::Error},       // SCE_LUA_STRINGEOL
    {13, SyntaxToken::Keyword2},    // SCE_LUA_WORD2（组 1 基础函数）
    {20, SyntaxToken::Label},       // SCE_LUA_LABEL（::label::）
    {-1, SyntaxToken::None},        // 哨兵
};

/// "rust" 词法器
constexpr StyleMapEntry kRustStyleMap[] = {
    {1, SyntaxToken::Comment},    // SCE_RUST_COMMENTBLOCK
    {2, SyntaxToken::Comment},    // SCE_RUST_COMMENTLINE
    {3, SyntaxToken::CommentDoc}, // SCE_RUST_COMMENTBLOCKDOC
    {4, SyntaxToken::CommentDoc}, // SCE_RUST_COMMENTLINEDOC
    {5, SyntaxToken::Number},     // SCE_RUST_NUMBER
    {6, SyntaxToken::Keyword},    // SCE_RUST_WORD（组 0 主关键字）
    {7, SyntaxToken::Type},       // SCE_RUST_WORD2（组 1 内建类型）
    {8, SyntaxToken::Keyword2},   // SCE_RUST_WORD3（组 2 保留/次要关键字）
    {13, SyntaxToken::String},    // SCE_RUST_STRING
    {14, SyntaxToken::String2},   // SCE_RUST_STRINGR（r"..."）
    {15, SyntaxToken::String},    // SCE_RUST_CHARACTER
    {16, SyntaxToken::Operator},  // SCE_RUST_OPERATOR
    {17, SyntaxToken::Identifier}, // SCE_RUST_IDENTIFIER
    {18, SyntaxToken::Variable},  // SCE_RUST_LIFETIME（'a）
    {19, SyntaxToken::Function},  // SCE_RUST_MACRO（println! 等）
    {20, SyntaxToken::Error},     // SCE_RUST_LEXERROR
    {21, SyntaxToken::String},    // SCE_RUST_BYTESTRING（b"..."）
    {22, SyntaxToken::String2},   // SCE_RUST_BYTESTRINGR
    {23, SyntaxToken::String},    // SCE_RUST_BYTECHARACTER（b'x'）
    {24, SyntaxToken::String},    // SCE_RUST_CSTRING（c"..."）
    {25, SyntaxToken::String2},   // SCE_RUST_CSTRINGR
    {-1, SyntaxToken::None},      // 哨兵
};

/// "batch" 词法器
constexpr StyleMapEntry kBatchStyleMap[] = {
    {1, SyntaxToken::Comment},     // SCE_BAT_COMMENT（rem / ::）
    {2, SyntaxToken::Keyword},     // SCE_BAT_WORD（组 0 内部命令）
    {3, SyntaxToken::Label},       // SCE_BAT_LABEL（:label）
    {4, SyntaxToken::Comment},     // SCE_BAT_HIDE（@echo 隐藏标记归并注释）
    {5, SyntaxToken::Keyword2},    // SCE_BAT_COMMAND（组 1 外部命令）
    {6, SyntaxToken::Identifier},  // SCE_BAT_IDENTIFIER
    {7, SyntaxToken::Operator},    // SCE_BAT_OPERATOR
    {8, SyntaxToken::Identifier},  // SCE_BAT_AFTER_LABEL（标签后行归并标识符）
    {-1, SyntaxToken::None},       // 哨兵
};

/// "props" 词法器（INI 承载，DEC-008）
constexpr StyleMapEntry kPropsStyleMap[] = {
    {1, SyntaxToken::Comment},   // SCE_PROPS_COMMENT（# / ; 注释）
    {2, SyntaxToken::Section},   // SCE_PROPS_SECTION（[节]）
    {3, SyntaxToken::Operator},  // SCE_PROPS_ASSIGNMENT（=）
    {4, SyntaxToken::Value},     // SCE_PROPS_DEFVAL（?/: 默认值）
    {5, SyntaxToken::Key},       // SCE_PROPS_KEY
    {-1, SyntaxToken::None},     // 哨兵
};

// ===========================================================================
// 关键词表（SCI_SETKEYWORDS 空格分隔串；每语言 30~80 词，常用覆盖）
// ===========================================================================

constexpr const char* kCKeywords =
    "auto break case char const continue default do double else enum extern "
    "float for goto if inline int long register restrict return short signed "
    "sizeof static struct switch typedef union unsigned void volatile "
    "_Bool _Complex _Imaginary";

constexpr const char* kCKeywords2 =
    "NULL true false size_t ssize_t ptrdiff_t int8_t int16_t int32_t int64_t "
    "uint8_t uint16_t uint32_t uint64_t intptr_t uintptr_t bool FILE";

constexpr const char* kCppKeywords =
    "alignas alignof and asm auto bool break case catch char char8_t char16_t "
    "char32_t class co_await co_return co_yield concept const const_cast "
    "consteval constexpr constinit continue decltype default delete do double "
    "dynamic_cast else enum explicit export extern false float for friend goto "
    "if inline int long mutable namespace new noexcept not not_eq nullptr "
    "operator or private protected public register reinterpret_cast requires "
    "return short signed sizeof static static_assert static_cast struct switch "
    "template this thread_local throw true try typedef typeid typename union "
    "unsigned using virtual void volatile wchar_t while xor";

constexpr const char* kCppKeywords2 =
    "NULL size_t ptrdiff_t int8_t int16_t int32_t int64_t uint8_t uint16_t "
    "uint32_t uint64_t intptr_t uintptr_t string wstring vector deque list map "
    "set unordered_map unordered_set array unique_ptr shared_ptr make_unique "
    "make_shared function optional variant tuple pair span";

constexpr const char* kCSharpKeywords =
    "abstract as async await base bool break byte case catch char checked "
    "class const continue decimal default delegate do double dynamic else enum "
    "event explicit extern false finally fixed float for foreach goto if "
    "implicit in int interface internal is lock long namespace new null object "
    "operator out override params private protected public readonly ref return "
    "sbyte sealed short sizeof stackalloc static string struct switch this "
    "throw true try typeof uint ulong unchecked unsafe ushort using var virtual "
    "void volatile while yield partial";

constexpr const char* kCSharpKeywords2 =
    "string object Console Convert Math DateTime TimeSpan Guid Task List "
    "Dictionary HashSet IEnumerable Action Func IntPtr String Boolean Int32 "
    "Int64 Double Single Byte";

constexpr const char* kJavaKeywords =
    "abstract assert boolean break byte case catch char class const continue "
    "default do double else enum extends final finally float for goto if "
    "implements import instanceof int interface long native new package private "
    "protected public record return sealed short static strictfp super switch "
    "synchronized this throw throws transient try var void volatile while "
    "permits yield";

constexpr const char* kJavaKeywords2 =
    "true false null String System Integer Long Double Boolean Character Float "
    "Short Byte Object List ArrayList LinkedList Map HashMap TreeMap Set "
    "HashSet Optional Exception RuntimeException Error Thread Math Objects "
    "Arrays Collections StringBuilder StringBuffer CharSequence Iterable";

constexpr const char* kJsKeywords =
    "async await break case catch class const continue debugger default delete "
    "do else export extends false finally for from function get if import in "
    "instanceof let new null of return set static super switch this throw true "
    "try typeof var void while with yield";

constexpr const char* kJsKeywords2 =
    "undefined NaN Infinity globalThis console window document Math JSON Date "
    "RegExp Promise Array Object String Number Boolean Map Set WeakMap WeakSet "
    "Symbol Proxy Reflect parseInt parseFloat isNaN isFinite eval setTimeout "
    "setInterval clearTimeout clearInterval fetch";

constexpr const char* kTsKeywords =
    "abstract any as asserts async await bigint boolean break case catch class "
    "const constructor continue declare default delete do else enum export "
    "extends false finally for from function get if implements import in "
    "infer instanceof interface is keyof let module namespace never new null "
    "of number object operator out override package private protected public "
    "readonly require return satisfies set static string super switch symbol "
    "this throw true try type typeof undefined union unknown var void while "
    "with yield";

constexpr const char* kTsKeywords2 =
    "undefined NaN Infinity globalThis console window document Math JSON Date "
    "RegExp Promise Array Object String Number Boolean Map Set WeakMap WeakSet "
    "Symbol Proxy Reflect parseInt parseFloat isNaN isFinite";

constexpr const char* kGoKeywords =
    "break case chan const continue default defer else fallthrough for func go "
    "goto if import interface map package range return select struct switch "
    "type var";

constexpr const char* kGoKeywords2 =
    "append bool byte cap close complex complex64 complex128 copy delete error "
    "false float32 float64 imag int int8 int16 int32 int64 iota len make new "
    "nil panic print println real recover rune string true uint uint8 uint16 "
    "uint32 uint64 uintptr";

constexpr const char* kPythonKeywords =
    "False None True and as assert async await break class continue def del "
    "elif else except finally for from global if import in is lambda nonlocal "
    "not or pass raise return try while with yield";

constexpr const char* kPythonKeywords2 =
    "self cls print len range int str float bool bytes list dict set tuple "
    "frozenset bytearray object type abs min max sum pow round divmod enumerate "
    "zip map filter sorted reversed any all open input isinstance issubclass "
    "super staticmethod classmethod property repr id hash iter next";

constexpr const char* kJsonKeywords = "true false null";

constexpr const char* kYamlKeywords = "true false null yes no on off";

constexpr const char* kLuaKeywords =
    "and break do else elseif end false for function goto if in local nil not "
    "or repeat return then true until while";

constexpr const char* kLuaKeywords2 =
    "print pairs ipairs type tostring tonumber error assert pcall xpcall "
    "select require setmetatable getmetatable rawget rawset rawequal rawlen "
    "load dofile loadstring unpack next";

constexpr const char* kSqlKeywords =
    "ADD ALL ALTER AND ANY AS ASC BEGIN BETWEEN BY CASE CHECK COLUMN COMMIT "
    "CONSTRAINT CREATE CROSS CURSOR DATABASE DECLARE DEFAULT DELETE DESC "
    "DISTINCT DROP ELSE END EXISTS FOREIGN FROM FULL FUNCTION GRANT GROUP "
    "HAVING IF IN INDEX INNER INSERT INTO IS JOIN KEY LEFT LIKE LIMIT NOT NULL "
    "OFFSET ON OR ORDER OUTER PRIMARY PROCEDURE REFERENCES REVOKE RIGHT "
    "ROLLBACK SELECT SET TABLE THEN TRANSACTION TRIGGER TRUNCATE UNION UNIQUE "
    "UPDATE VALUES VIEW WHEN WHERE WHILE WITH";

constexpr const char* kBashKeywords =
    "if then else elif fi for while until do done case esac function in select "
    "time coproc break continue return exit export local readonly declare "
    "typeset unset shift eval exec source alias unalias trap set";

constexpr const char* kPowerShellKeywords =
    "begin break catch class continue data define do dynamicparam else elseif "
    "end enum exit filter finally for foreach from function hidden if in param "
    "process return static switch throw trap try until using var while workflow";

constexpr const char* kPowerShellCmdlets =
    "Get-ChildItem Get-Content Get-Item Get-ItemProperty Get-Member "
    "Get-Process Get-Service Set-Content Set-Item Set-Location New-Item "
    "New-Object New-Variable ForEach-Object Where-Object Select-Object "
    "Sort-Object Measure-Object Group-Object Write-Host Write-Output "
    "Write-Error Write-Warning Write-Verbose Write-Progress Out-Host "
    "Out-String Out-File Test-Path Split-Path Join-Path ConvertTo-Json "
    "ConvertFrom-Json Invoke-Command Invoke-Item Invoke-Expression "
    "Start-Process Stop-Process Stop-Service Start-Service Copy-Item "
    "Move-Item Remove-Item";

constexpr const char* kPowerShellAliases =
    "ls dir cat gc sc echo cd sl cls clear man help diff compare sleep date";

constexpr const char* kRustKeywords =
    "as async await break const continue crate dyn else enum extern false fn "
    "for if impl in let loop match mod move mut pub ref return self Self static "
    "struct super trait true type unsafe use where while";

constexpr const char* kRustKeywords2 =
    "bool char str u8 u16 u32 u64 u128 usize i8 i16 i32 i64 i128 isize f32 f64";

constexpr const char* kRustKeywords3 =
    "abstract become box do final macro override priv typeof union unsized "
    "virtual yield try";

constexpr const char* kBatchKeywords =
    "assoc attrib call cd chcp cls copy del dir dpath echo endlocal erase exit "
    "for ftype goto if md mkdir mklink move pause popd pushd prompt rd rem ren "
    "rename rmdir set setlocal shift start title type ver verify vol xcopy";

// ===========================================================================
// 扩展名表（小写、不带点）
// ===========================================================================

constexpr const wchar_t* kExtC[] = {L"c", L"h", nullptr};
constexpr const wchar_t* kExtCpp[] = {L"cpp", L"cc", L"cxx", L"hpp", L"hh",
                                      L"hxx", L"inl", nullptr};
constexpr const wchar_t* kExtCSharp[] = {L"cs", nullptr};
constexpr const wchar_t* kExtJava[] = {L"java", nullptr};
constexpr const wchar_t* kExtJavaScript[] = {L"js", L"mjs", L"cjs", nullptr};
constexpr const wchar_t* kExtTypeScript[] = {L"ts", L"tsx", nullptr};
constexpr const wchar_t* kExtPython[] = {L"py", L"pyw", nullptr};
constexpr const wchar_t* kExtGo[] = {L"go", nullptr};
constexpr const wchar_t* kExtRust[] = {L"rs", nullptr};
constexpr const wchar_t* kExtLua[] = {L"lua", nullptr};
constexpr const wchar_t* kExtJson[] = {L"json", nullptr};
constexpr const wchar_t* kExtMarkdown[] = {L"md", L"markdown", nullptr};
constexpr const wchar_t* kExtHtml[] = {L"html", L"htm", nullptr};
constexpr const wchar_t* kExtXml[] = {L"xml", L"xsl", L"xsd", L"svg", nullptr};
constexpr const wchar_t* kExtCss[] = {L"css", nullptr};
constexpr const wchar_t* kExtSql[] = {L"sql", nullptr};
constexpr const wchar_t* kExtBash[] = {L"sh", L"bash", nullptr};
constexpr const wchar_t* kExtPowerShell[] = {L"ps1", L"psm1", nullptr};
constexpr const wchar_t* kExtBatch[] = {L"bat", L"cmd", nullptr};
constexpr const wchar_t* kExtIni[] = {L"ini", L"cfg", L"conf", L"properties",
                                      nullptr};
constexpr const wchar_t* kExtYaml[] = {L"yaml", L"yml", nullptr};

// ===========================================================================
// 语言注册表（21 条目；顺序即「语言」子菜单条目顺序）
// ===========================================================================

/// cpp 系 7 语言共享槽位映射（同一词法器）+ 各自关键词分组
constexpr LanguageEntry kLanguages[] = {
    {.id = "c",
     .displayName = L"C",
     .extensions = kExtC,
     .lexerName = "cpp",
     .keywords = {kCKeywords, kCKeywords2, nullptr, nullptr, nullptr, nullptr,
                  nullptr, nullptr, nullptr},
     .styleMap = kCppStyleMap},
    {.id = "cpp",
     .displayName = L"C++",
     .extensions = kExtCpp,
     .lexerName = "cpp",
     .keywords = {kCppKeywords, kCppKeywords2, nullptr, nullptr, nullptr,
                  nullptr, nullptr, nullptr, nullptr},
     .styleMap = kCppStyleMap},
    {.id = "csharp",
     .displayName = L"C#",
     .extensions = kExtCSharp,
     .lexerName = "cpp",
     .keywords = {kCSharpKeywords, kCSharpKeywords2, nullptr, nullptr, nullptr,
                  nullptr, nullptr, nullptr, nullptr},
     .styleMap = kCppStyleMap},
    {.id = "java",
     .displayName = L"Java",
     .extensions = kExtJava,
     .lexerName = "cpp",
     .keywords = {kJavaKeywords, kJavaKeywords2, nullptr, nullptr, nullptr,
                  nullptr, nullptr, nullptr, nullptr},
     .styleMap = kCppStyleMap},
    {.id = "javascript",
     .displayName = L"JavaScript",
     .extensions = kExtJavaScript,
     .lexerName = "cpp",
     .keywords = {kJsKeywords, kJsKeywords2, nullptr, nullptr, nullptr, nullptr,
                  nullptr, nullptr, nullptr},
     .styleMap = kCppStyleMap},
    {.id = "typescript",
     .displayName = L"TypeScript",
     .extensions = kExtTypeScript,
     .lexerName = "cpp",
     .keywords = {kTsKeywords, kTsKeywords2, nullptr, nullptr, nullptr, nullptr,
                  nullptr, nullptr, nullptr},
     .styleMap = kCppStyleMap},
    {.id = "python",
     .displayName = L"Python",
     .extensions = kExtPython,
     .lexerName = "python",
     .keywords = {kPythonKeywords, kPythonKeywords2, nullptr, nullptr, nullptr,
                  nullptr, nullptr, nullptr, nullptr},
     .styleMap = kPythonStyleMap},
    {.id = "go",
     .displayName = L"Go",
     .extensions = kExtGo,
     .lexerName = "cpp", // DEC-009：无独立 go 词法器，cpp + Go 关键词表
     .keywords = {kGoKeywords, kGoKeywords2, nullptr, nullptr, nullptr, nullptr,
                  nullptr, nullptr, nullptr},
     .styleMap = kCppStyleMap},
    {.id = "rust",
     .displayName = L"Rust",
     .extensions = kExtRust,
     .lexerName = "rust",
     .keywords = {kRustKeywords, kRustKeywords2, kRustKeywords3, nullptr,
                  nullptr, nullptr, nullptr, nullptr, nullptr},
     .styleMap = kRustStyleMap},
    {.id = "lua",
     .displayName = L"Lua",
     .extensions = kExtLua,
     .lexerName = "lua",
     .keywords = {kLuaKeywords, kLuaKeywords2, nullptr, nullptr, nullptr,
                  nullptr, nullptr, nullptr, nullptr},
     .styleMap = kLuaStyleMap},
    {.id = "json",
     .displayName = L"JSON",
     .extensions = kExtJson,
     .lexerName = "json",
     .keywords = {kJsonKeywords, nullptr, nullptr, nullptr, nullptr, nullptr,
                  nullptr, nullptr, nullptr},
     .styleMap = kJsonStyleMap},
    {.id = "markdown",
     .displayName = L"Markdown",
     .extensions = kExtMarkdown,
     .lexerName = "markdown",
     .keywords = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                  nullptr, nullptr},
     .styleMap = kMarkdownStyleMap},
    {.id = "html",
     .displayName = L"HTML",
     .extensions = kExtHtml,
     .lexerName = "hypertext",
     // 组 0（HTML 标签/属性表）有意留空：LexHTML 无关键词时把一切标签/
     // 属性按已知着色，注入有限词表反而使词表外正常标签落 *UNKNOWN 错误槽；
     // 组 1 = JavaScript 关键字（<script> 内高亮，htmlWordListDesc 约定）
     .keywords = {nullptr, kJsKeywords, nullptr, nullptr, nullptr, nullptr,
                  nullptr, nullptr, nullptr},
     .styleMap = kHtmlStyleMap},
    {.id = "xml",
     .displayName = L"XML",
     .extensions = kExtXml,
     .lexerName = "xml",
     .keywords = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                  nullptr, nullptr},
     .styleMap = kXmlStyleMap},
    {.id = "css",
     .displayName = L"CSS",
     .extensions = kExtCss,
     .lexerName = "css",
     // 关键词组全空：CSS 词法器对已知/未知属性与伪类仅细分槽位，本映射表
     // 已归并到相同 token（Identifier/Keyword2），注入词表不改变着色效果
     .keywords = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                  nullptr, nullptr},
     .styleMap = kCssStyleMap},
    {.id = "sql",
     .displayName = L"SQL",
     .extensions = kExtSql,
     .lexerName = "sql",
     .keywords = {kSqlKeywords, nullptr, nullptr, nullptr, nullptr, nullptr,
                  nullptr, nullptr, nullptr},
     .styleMap = kSqlStyleMap},
    {.id = "bash",
     .displayName = L"Bash",
     .extensions = kExtBash,
     .lexerName = "bash",
     .keywords = {kBashKeywords, nullptr, nullptr, nullptr, nullptr, nullptr,
                  nullptr, nullptr, nullptr},
     .styleMap = kBashStyleMap},
    {.id = "powershell",
     .displayName = L"PowerShell",
     .extensions = kExtPowerShell,
     .lexerName = "powershell",
     // 组 0 命令 / 组 1 cmdlet / 组 2 别名（powershellWordLists 约定）
     .keywords = {kPowerShellKeywords, kPowerShellCmdlets,
                  kPowerShellAliases, nullptr, nullptr, nullptr, nullptr,
                  nullptr, nullptr},
     .styleMap = kPowerShellStyleMap},
    {.id = "batch",
     .displayName = L"Batch",
     .extensions = kExtBatch,
     .lexerName = "batch",
     // 组 0 内部命令 / 组 1 外部命令（batchWordListDesc 约定）
     .keywords = {kBatchKeywords, nullptr, nullptr, nullptr, nullptr, nullptr,
                  nullptr, nullptr, nullptr},
     .styleMap = kBatchStyleMap},
    {.id = "ini",
     .displayName = L"INI",
     .extensions = kExtIni,
     .lexerName = "props", // DEC-008：INI 由 props 词法器承载
     .keywords = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                  nullptr, nullptr},
     .styleMap = kPropsStyleMap},
    {.id = "yaml",
     .displayName = L"YAML",
     .extensions = kExtYaml,
     .lexerName = "yaml",
     .keywords = {kYamlKeywords, nullptr, nullptr, nullptr, nullptr, nullptr,
                  nullptr, nullptr, nullptr},
     .styleMap = kYamlStyleMap},
};

static_assert(std::size(kLanguages) == 21, "语言注册表条目数（需求 21 语言）");

/// ASCII 小写（扩展名/文件名比较用；非字母原样）
constexpr wchar_t asciiLower(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? c - L'A' + L'a' : c;
}

} // namespace

// ---------------------------------------------------------------------------
// 查询接口
// ---------------------------------------------------------------------------

const LanguageEntry* languageById(std::string_view id) {
    for (const LanguageEntry& entry : kLanguages) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr; // 未知 id：纯文本/丢弃语义由调用方定义
}

const LanguageEntry* languageForFileName(const std::wstring& fileName) {
    // 扩展名 = 最后一个 '.' 之后（无点/点在末尾 → 无扩展 → 纯文本）
    const std::size_t dot = fileName.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 == fileName.size())
        return nullptr;
    std::wstring extension;
    extension.reserve(fileName.size() - dot - 1);
    for (std::size_t i = dot + 1; i < fileName.size(); ++i)
        extension.push_back(asciiLower(fileName[i])); // 大小写不敏感
    for (const LanguageEntry& entry : kLanguages) {
        for (const wchar_t* const* ext = entry.extensions; *ext; ++ext) {
            if (extension == *ext)
                return &entry;
        }
    }
    return nullptr; // 未知扩展名：纯文本
}

const LanguageEntry* languageAt(std::size_t index) {
    return index < std::size(kLanguages) ? &kLanguages[index] : nullptr;
}

bool isValidLanguageId(std::string_view id) {
    return id == kPlainTextLanguageId || languageById(id) != nullptr;
}

// ---------------------------------------------------------------------------
// 文档生效语言计算
// ---------------------------------------------------------------------------

const LanguageEntry* effectiveLanguage(const Document& document) {
    // 手动指定优先（含 "plaintext" 强制纯文本；未知 id 容错回退自动识别）
    if (const std::optional<std::string>& manual = document.manualLanguage()) {
        if (*manual == kPlainTextLanguageId)
            return nullptr; // 手动纯文本
        if (const LanguageEntry* entry = languageById(*manual))
            return entry; // 合法手动语言
        // 未知 id（数据异常）：回退自动识别（与编码非法回退语义一致）
    }
    // 自动模式：按 displayName（url 文件名派生）扩展名识别；未命名文档
    // displayName 为「未命名-N」形态，无扩展名 → nullptr（纯文本）
    return languageForFileName(document.displayName());
}

const LanguageEntry* effectiveLanguageForColoring(const Document& document) {
    // DEC-005：超过 16MB 跳过着色（纯文本渲染；语言身份判定不受影响）
    if (document.content().size() > kSyntaxColoringMaxBytes)
        return nullptr;
    return effectiveLanguage(document);
}

} // namespace core
