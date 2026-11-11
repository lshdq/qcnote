#include "core/inifile.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <system_error>

namespace core {

namespace {

// —— ASCII 工具 ——
bool asciiIEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i]))
            != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

std::string_view trimAscii(std::string_view s) {
    while (!s.empty()
           && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
        s.remove_prefix(1);
    while (!s.empty()
           && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

bool isHexDigit(char c) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

int hexDigitValue(char c) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (c >= '0' && c <= '9')
        return c - '0';
    return c - 'a' + 10;
}

char hexUpper(int v) { return "0123456789ABCDEF"[v & 0xF]; }

/// 码点 → UTF-8 追加
void appendUtf8(std::string& out, unsigned cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

/// 宽容 UTF-8 解码：非法字节按单字节码点（≤U+00FF）处理——转义只关心
/// 「码点是否 ≤0xFF/≤0x1F」，宽容语义下不丢失可逆性
unsigned decodeUtf8(std::string_view s, std::size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    const auto cont = [&](std::size_t k) -> bool {
        return i + k < s.size()
            && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
    };
    if (c < 0x80) {
        ++i;
        return c;
    }
    unsigned cp = 0;
    if ((c & 0xE0) == 0xC0 && cont(1)) {
        cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
        i += 2;
    } else if ((c & 0xF0) == 0xE0 && cont(1) && cont(2)) {
        cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6)
            | (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
        i += 3;
    } else if ((c & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
        cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12)
            | ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6)
            | (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
        i += 4;
    } else {
        // 非法/截断序列：按单字节宽容处理
        ++i;
        return c;
    }
    return cp;
}

bool isKeySafeChar(unsigned cp) {
    if (cp >= 'a' && cp <= 'z') return true;
    if (cp >= 'A' && cp <= 'Z') return true;
    if (cp >= '0' && cp <= '9') return true;
    return cp == '_' || cp == '-' || cp == '.';
}

std::string_view trimSpacesTabs(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// IniData
// ---------------------------------------------------------------------------

const IniSection* IniData::findSection(std::string_view name) const {
    return const_cast<IniData*>(this)->findSectionMutable(name);
}

IniSection* IniData::findSectionMutable(std::string_view name) {
    for (auto& section : m_sections) {
        if (asciiIEquals(section.name, name))
            return &section;
    }
    return nullptr;
}

std::optional<std::string> IniData::value(std::string_view section, std::string_view key) const {
    const IniSection* sec = findSection(section);
    if (!sec)
        return std::nullopt;
    for (const auto& entry : sec->entries) {
        if (asciiIEquals(entry.key, key))
            return entry.value;
    }
    return std::nullopt;
}

void IniData::setValue(std::string_view section, std::string_view key, std::string_view value) {
    IniSection* sec = findSectionMutable(section);
    if (!sec) {
        m_sections.push_back(IniSection{std::string(section), {}});
        sec = &m_sections.back();
    }
    for (auto& entry : sec->entries) {
        if (asciiIEquals(entry.key, key)) {
            entry.value = std::string(value); // 重复键：后者覆盖，保留首现位置
            return;
        }
    }
    sec->entries.push_back(IniEntry{std::string(key), std::string(value)});
}

std::size_t IniData::entryCount(std::string_view section) const {
    const IniSection* sec = findSection(section);
    return sec ? sec->entries.size() : 0;
}

// ---------------------------------------------------------------------------
// 转义纯函数（QSettings IniFormat 对齐，见文件头说明）
// ---------------------------------------------------------------------------

std::string IniFile::escapeKey(std::string_view utf8Key) {
    std::string out;
    out.reserve(utf8Key.size() * 2);
    std::size_t i = 0;
    while (i < utf8Key.size()) {
        const unsigned cp = decodeUtf8(utf8Key, i);
        if (cp == '/') {
            out.push_back('\\');
        } else if (isKeySafeChar(cp)) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0xFF) {
            out.push_back('%');
            out.push_back(hexUpper(cp >> 4));
            out.push_back(hexUpper(cp & 0xF));
        } else {
            out += "%U";
            out.push_back(hexUpper((cp >> 12) & 0xF));
            out.push_back(hexUpper((cp >> 8) & 0xF));
            out.push_back(hexUpper((cp >> 4) & 0xF));
            out.push_back(hexUpper(cp & 0xF));
        }
    }
    return out;
}

std::string IniFile::unescapeKey(std::string_view fileKey) {
    std::string out;
    out.reserve(fileKey.size());
    std::size_t i = 0;
    while (i < fileKey.size()) {
        const char c = fileKey[i];
        if (c == '\\') {
            out.push_back('/'); // 文件中的 '\' 还原为键分隔符 '/'
            ++i;
            continue;
        }
        if (c == '%' && i + 1 < fileKey.size()) {
            std::size_t digits = 2;
            std::size_t first = i + 1;
            if (fileKey[first] == 'U' || fileKey[first] == 'u') {
                digits = 4;
                ++first;
            }
            if (first + digits <= fileKey.size()) {
                unsigned cp = 0;
                bool ok = true;
                for (std::size_t k = 0; k < digits; ++k) {
                    const char h = fileKey[first + k];
                    if (!isHexDigit(h)) {
                        ok = false;
                        break;
                    }
                    cp = (cp << 4) | static_cast<unsigned>(hexDigitValue(h));
                }
                if (ok) {
                    appendUtf8(out, cp);
                    i = first + digits;
                    continue;
                }
            }
            out.push_back('%'); // 非法转义：'%' 按字面
            ++i;
            continue;
        }
        out.push_back(c); // 原样字节（含 UTF-8 多字节序列的各字节）
        ++i;
    }
    return out;
}

std::string IniFile::escapeValue(std::string_view utf8Value) {
    std::string out;
    out.reserve(utf8Value.size() + 8);
    bool needsQuotes = false;
    bool escapeNextIfDigit = false;
    for (const char raw : utf8Value) {
        const unsigned char ch = static_cast<unsigned char>(raw);
        if (ch == ';' || ch == ',' || ch == '=')
            needsQuotes = true;

        // \xHH 后跟十六进制字符：强制再转义（读侧 \x 吞全部十六进制数字的消歧）
        if (escapeNextIfDigit && isHexDigit(raw)) {
            out += "\\x";
            // QByteArray::number(ch, 16)：小写、不补零
            if (ch >> 4)
                out.push_back("0123456789abcdef"[ch >> 4]);
            out.push_back("0123456789abcdef"[ch & 0xF]);
            continue;
        }
        escapeNextIfDigit = false;

        switch (ch) {
        case '\0': out += "\\0"; escapeNextIfDigit = true; break;
        case '\a': out += "\\a"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\v': out += "\\v"; break;
        case '"':
        case '\\':
            out.push_back('\\');
            out.push_back(static_cast<char>(ch));
            break;
        default:
            if (ch <= 0x1F) {
                // QByteArray::number(ch, 16)：小写、不补零（Qt 兼容：0x01 → "\x1"）
                out += "\\x";
                if (ch >> 4)
                    out.push_back("0123456789abcdef"[ch >> 4]);
                out.push_back("0123456789abcdef"[ch & 0xF]);
                escapeNextIfDigit = true;
            } else {
                out.push_back(static_cast<char>(ch)); // 含 UTF-8 多字节原样
            }
            break;
        }
    }
    // 首尾空格或含 ;,= → 引号包裹
    const bool edgeSpace =
        !out.empty() && (out.front() == ' ' || out.back() == ' ');
    if (needsQuotes || edgeSpace) {
        out.insert(out.begin(), '"');
        out.push_back('"');
    }
    return out;
}

std::string IniFile::unescapeValue(std::string_view fileValue) {
    std::string out;
    out.reserve(fileValue.size());
    std::size_t i = 0;
    // 引导空白不属于值（QSettings StSkipSpaces）
    while (i < fileValue.size() && (fileValue[i] == ' ' || fileValue[i] == '\t'))
        ++i;
    bool inQuotes = false;
    std::size_t solidEnd = 0; // 截断保护界：转义/闭引号之后的内容不被尾部截断
    while (i < fileValue.size()) {
        const char c = fileValue[i];
        if (c == '\\') {
            ++i;
            if (i >= fileValue.size())
                break; // 末尾孤立反斜杠：丢弃
            const char e = fileValue[i++];
            switch (e) {
            case 'a': out.push_back('\a'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'v': out.push_back('\v'); break;
            case '"': out.push_back('"'); break;
            case '?': out.push_back('?'); break;
            case '\'': out.push_back('\''); break;
            case '\\': out.push_back('\\'); break;
            case 'x': {
                // 吞全部连续十六进制数字（写侧 escapeNextIfDigit 消歧的读侧）
                unsigned cp = 0;
                int digits = 0;
                while (i < fileValue.size() && isHexDigit(fileValue[i]) && digits < 8) {
                    cp = (cp << 4) | static_cast<unsigned>(hexDigitValue(fileValue[i]));
                    ++i;
                    ++digits;
                }
                if (digits == 0)
                    out.push_back('x'); // "\x" 无数字：'x' 按字面（防御）
                else
                    appendUtf8(out, cp & 0xFFFF);
                break;
            }
            default:
                if (e >= '0' && e <= '7') {
                    // 八进制转义（最多 3 位）
                    unsigned cp = static_cast<unsigned>(e - '0');
                    int digits = 1;
                    while (i < fileValue.size() && fileValue[i] >= '0' && fileValue[i] <= '7'
                           && digits < 3) {
                        cp = (cp << 3) | static_cast<unsigned>(fileValue[i] - '0');
                        ++i;
                        ++digits;
                    }
                    appendUtf8(out, cp & 0xFF);
                } else if (e == '\n' || e == '\r') {
                    // 续行：吞掉配对换行（\r\n / \n\r）
                    if (i < fileValue.size()
                        && (fileValue[i] == '\n' || fileValue[i] == '\r') && fileValue[i] != e)
                        ++i;
                } else {
                    out.push_back(e); // 未知转义：丢反斜杠、保字符（QSettings 语义）
                }
                break;
            }
            solidEnd = out.size();
            continue;
        }
        if (c == '"') {
            ++i;
            inQuotes = !inQuotes;
            if (!inQuotes) {
                // 闭引号后紧跟的空白不属于值（StSkipSpaces），其后内容受保护
                while (i < fileValue.size() && (fileValue[i] == ' ' || fileValue[i] == '\t'))
                    ++i;
                solidEnd = out.size();
            }
            continue;
        }
        out.push_back(c);
        ++i;
    }
    // 未以引号段收尾的尾部空白截断（截到保护界为止）
    if (!inQuotes) {
        while (out.size() > solidEnd && (out.back() == ' ' || out.back() == '\t'))
            out.pop_back();
    }
    return out;
}

// ---------------------------------------------------------------------------
// 读写
// ---------------------------------------------------------------------------

std::optional<IniData> IniFile::read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return std::nullopt; // 不存在/不可读：与会话「无文件」语义区分
    std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // UTF-8 BOM 容忍（读侧跳过；写侧永不产生）
    if (data.starts_with("\xEF\xBB\xBF"))
        data.erase(0, 3);

    IniData ini;
    std::string currentSection; // [General] 特例 → 空串

    std::size_t i = 0;
    const std::size_t n = data.size();
    while (true) {
        // 行间空白
        while (i < n && (data[i] == ' ' || data[i] == '\t' || data[i] == '\r' || data[i] == '\n'))
            ++i;
        if (i >= n)
            break;
        if (data[i] == ';') {
            // 注释行：跳到行尾
            while (i < n && data[i] != '\n' && data[i] != '\r')
                ++i;
            continue;
        }

        // 逻辑行扫描（引号内不结束；反斜杠转义/续行；行内 ';' 注释截断）
        const std::size_t lineStart = i;
        std::size_t equalsPos = std::string_view::npos;
        bool inQuotes = false;
        while (i < n) {
            const char c = data[i];
            if (c == '\\' && i + 1 < n) {
                const char next = data[i + 1];
                i += 2;
                if ((next == '\n' || next == '\r')) {
                    // 续行：吞掉配对换行后继续扫描本逻辑行
                    if (i < n && (data[i] == '\n' || data[i] == '\r') && data[i] != next)
                        ++i;
                }
                continue; // 被转义字符不参与 =/;/" 判定
            }
            if (c == '"') {
                inQuotes = !inQuotes;
                ++i;
                continue;
            }
            if (!inQuotes) {
                if (c == '=' && equalsPos == std::string_view::npos) {
                    equalsPos = i;
                    ++i;
                    continue;
                }
                if (c == '\n' || c == '\r' || c == ';')
                    break; // 行结束 / 行内注释起点
            }
            ++i;
        }
        const std::size_t lineEnd = i;
        const std::string_view line(data.data() + lineStart, lineEnd - lineStart);

        if (!line.empty() && line.front() == '[') {
            // 节头：首个 ']' 前（无 ']' 容忍到行尾）
            const std::size_t close = line.find(']');
            const std::string_view raw =
                trimAscii(close == std::string_view::npos
                              ? line.substr(1)
                              : line.substr(1, close - 1));
            if (asciiIEquals(raw, "general")) {
                currentSection.clear(); // [General] → 空节名
            } else if (!raw.empty() && raw.front() == '%') {
                currentSection = unescapeKey(raw.substr(1)); // [%General] 字面 general 节
            } else {
                currentSection = unescapeKey(raw);
            }
            continue; // 空节不实体化（首个键到达时按序创建）
        }
        if (equalsPos != std::string_view::npos) {
            const std::string_view keyPart = trimAscii(
                std::string_view(data.data() + lineStart, equalsPos - lineStart));
            const std::string_view valuePart(data.data() + equalsPos + 1,
                                             lineEnd - equalsPos - 1);
            ini.setValue(currentSection, unescapeKey(keyPart), unescapeValue(valuePart));
        }
        // 无 '=' 的非注释行：忽略（QSettings 记 FormatError；尽力恢复语义）
    }
    return ini;
}

bool IniFile::write(const std::filesystem::path& path, const IniData& data) {
    std::string out;
    for (std::size_t s = 0; s < data.sections().size(); ++s) {
        const IniSection& section = data.sections()[s];
        if (s != 0)
            out += "\r\n"; // 节间空行（QSettings 写侧节前 eol）
        std::string header = escapeKey(section.name);
        if (header.empty()) {
            out += "[General]\r\n"; // 空节名特例（本应用不产生，防御完整性）
        } else if (asciiIEquals(header, "general")) {
            out += "[%General]\r\n"; // 字面 general 节特例
        } else {
            out += "[" + header + "]\r\n";
        }
        for (const auto& entry : section.entries) {
            out += escapeKey(entry.key);
            out += '=';
            out += escapeValue(entry.value);
            out += "\r\n";
        }
    }
    return writeFileAtomically(path, out, /*createParentDirs=*/true);
}

// ---------------------------------------------------------------------------
// 原子写
// ---------------------------------------------------------------------------

bool writeFileAtomically(const std::filesystem::path& path, std::string_view bytes,
                         bool createParentDirs) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = path.parent_path();
    if (dir.empty())
        dir = L".";
    if (!fs::is_directory(dir, ec)) {
        if (!createParentDirs || !fs::create_directories(dir, ec) || !fs::is_directory(dir, ec))
            return false;
    }
    const std::wstring dirStr = dir.wstring();
    if (dirStr.size() > MAX_PATH - 14)
        return false;

    wchar_t tempPath[MAX_PATH] = {};
    // GetTempFileNameW 原子创建唯一 0 字节临时文件（qcn 前缀 ≤3 字符）
    if (GetTempFileNameW(dirStr.c_str(), L"qcn", 0, tempPath) == 0)
        return false;

    auto fail = [&]() -> bool {
        DeleteFileW(tempPath); // 清理临时文件，不留垃圾
        return false;
    };

    HANDLE file = CreateFileW(tempPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return fail();

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk =
            static_cast<DWORD>((std::min)(static_cast<std::size_t>(1) << 30, bytes.size() - offset));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) || written != chunk) {
            CloseHandle(file);
            return fail();
        }
        offset += written;
    }
    if (!FlushFileBuffers(file)) { // 落盘后再替换，防断电半截文件
        CloseHandle(file);
        return fail();
    }
    CloseHandle(file);

    if (!MoveFileExW(tempPath, path.c_str(), MOVEFILE_REPLACE_EXISTING))
        return fail();
    return true;
}

} // namespace core
