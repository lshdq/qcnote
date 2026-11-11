// qcnote 纯逻辑核心层：极简 INI 读写（M4-A，会话持久化基建；DEC-013 会话用 INI）。
//
// 转义语义对齐 Qt QSettings::IniFormat（旧 qcnote.ini 资产兼容；规则取自
// qtbase src/corelib/io/qsettings.cpp）：
// - 文件形态：节 [section]、键=值、UTF-8 无 BOM、CRLF 行尾；
// - 键转义（写侧）：'/'→'\'；[A-Za-z0-9_.-] 原样；其余 ≤U+00FF → %XX（大写
//   hex）、>U+00FF → %UXXXX（iniEscapedKey 同款——旧版 [documents] 1/url 键
//   落盘即 "1\url"）。读侧 '\'→'/' 还原（对未转义的 '/' 键同样兼容）；
// - 值转义（写侧）：含 ';'/','/'=' 或首尾空格 → 双引号包裹；\0\a\b\f\n\r\t\v
//   → 反斜杠助记转义；'"'→\"、'\'→\\；其余 ≤U+001F 控制字符 → \xHH（小写、
//   不补零）；\xHH 后跟十六进制字符时该字符亦强制转义（读侧 \x 吞全部十六
//   进制数字的消歧，iniEscapedString escapeNextIfDigit 同款）；非 ASCII 按
//   UTF-8 原样（值中 '%' 不转义——QSettings 仅键用百分号转义）；
// - 读侧宽容（尽力恢复语义）：BOM 跳过；';' 注释行与行内注释；行尾反斜杠
//   续行；引号段（引号内 ';'/= 不结束行）；未加引号值的尾部空格/tab 截断；
//   无 '=' 的行忽略；[General]/[%General] 特例；重复键后者覆盖（保留首现
//   位置）；节名/键名 ASCII 大小写不敏感（QSettings IniCaseSensitivity）。
//   有意差异：值中未加引号的 ',' 按字面处理（QSettings 会拆字符串列表；
//   本应用从不写入列表值，写侧含 ',' 必加引号，无实际分歧）。
// 写入原子性：同目录临时文件 + FlushFileBuffers + MoveFileEx REPLACE_EXISTING
//（复用 LocalStorage 模式）；父目录不存在时自动创建（用户目录首启场景）。
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace core {

/// 有序 INI 数据：节 →（键, 值）列表；键值均为解码后的逻辑 UTF-8 字符串
///（文件中的转义形式由 IniFile 统一处理，本结构不感知）
struct IniEntry {
    std::string key;
    std::string value;
};

struct IniSection {
    std::string name;        ///< 逻辑节名（[General] 特例为空串）
    std::vector<IniEntry> entries;
};

class IniData {
public:
    /// 节查找（ASCII 大小写不敏感）；不存在返回 nullptr
    const IniSection* findSection(std::string_view name) const;
    /// 取值（节、键均 ASCII 大小写不敏感）；不存在返回 nullopt
    std::optional<std::string> value(std::string_view section, std::string_view key) const;
    /// 设值：节/键不存在则按调用顺序追加（保序）；已存在则原位覆盖（保首现位置）
    void setValue(std::string_view section, std::string_view key, std::string_view value);
    /// 节内条目数（0 = 节不存在或为空）
    std::size_t entryCount(std::string_view section) const;

    const std::vector<IniSection>& sections() const { return m_sections; }

private:
    IniSection* findSectionMutable(std::string_view name);

    std::vector<IniSection> m_sections;
};

class IniFile {
public:
    /// 解析 INI 文件（宽容尽力恢复：坏行忽略，不整体失败）。
    /// 文件不存在/无法打开返回 nullopt（与空文件区分——空文件返回空 IniData）
    std::optional<IniData> static read(const std::filesystem::path& path);
    /// 原子写入（UTF-8 无 BOM、CRLF、QSettings 风格转义；父目录自动创建）。
    /// 失败返回 false 不抛（调用方静默容错语义）
    static bool write(const std::filesystem::path& path, const IniData& data);

    // —— 转义纯函数（单测直测）——
    static std::string escapeKey(std::string_view utf8Key);      ///< "1/url" → "1\url"
    static std::string unescapeKey(std::string_view fileKey);    ///< "1\url" → "1/url"
    static std::string escapeValue(std::string_view utf8Value);  ///< 含 ;,= / 首尾空格 → 引号包裹
    static std::string unescapeValue(std::string_view fileValue);
};

/// 原子写文件（同目录临时文件 + Flush + MoveFileEx 替换；LocalStorage 同款）。
/// createParentDirs：父目录不存在时先建（会话/草稿目录首启场景）。
/// 会话与草稿共用的落盘基建；失败返回 false 不抛
bool writeFileAtomically(const std::filesystem::path& path, std::string_view bytes,
                         bool createParentDirs);

} // namespace core
