#include "core/sessionmanager.h"

#include <shlobj.h>

#include <algorithm>
#include <charconv>
#include <limits>

#include "core/inifile.h"

namespace core {

namespace {

constexpr int kMinWindowWidth = 800;   // 产品最小窗口尺寸（旧版同款）
constexpr int kMinWindowHeight = 500;
constexpr const char* kDocumentsSection = "documents";
constexpr const char* kWindowSection = "window";
constexpr const char* kEditorSection = "editor";

std::optional<long long> parseInt(std::string_view s) {
    s.remove_prefix(std::min(s.size(), s.find_first_not_of(" \t")));
    const std::size_t end = s.find_last_not_of(" \t");
    if (end == std::string_view::npos)
        return std::nullopt;
    s = s.substr(0, end + 1);
    long long value = 0;
    const auto* first = s.data();
    const auto* last = s.data() + s.size();
    // 允许负号（多屏负坐标）；from_chars 不接受前导 '+'（QSettings 也不产生）
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || result.ptr != last)
        return std::nullopt;
    return value;
}

bool asciiIEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    const auto lower = [](char c) {
        return static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    };
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i]))
            return false;
    }
    return true;
}

/// QVariant::toBool 语义：仅 "true"/"1"（大小写不敏感）为真
bool parseBool(std::string_view s) {
    return asciiIEquals(s, "true") || s == "1";
}

/// 百分号解码（旧 QUrl::toString 资产：%XX → 字节，随后按 UTF-8 解释）
std::string percentDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hexVal(s[i + 1]);
            const int lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// 构建与持久化
// ---------------------------------------------------------------------------

SessionData SessionManager::buildSessionData(const DocumentManager& manager) {
    SessionData data;
    int position = 0;
    for (const Document* doc : manager.documents()) {
        if (!doc)
            continue;
        SessionDocumentEntry entry;
        entry.position = position++;
        if (!doc->url().empty()) {
            entry.url = doc->url();
            entry.cursor = doc->cursorPosition();
            entry.encoding = doc->encoding();
        } else {
            entry.uid = doc->uid(); // 未命名：仅记 uid（光标随草稿条目恢复）
        }
        data.documents.push_back(std::move(entry));
    }
    if (const Document* active = manager.activeDocument()) {
        if (!active->url().empty())
            data.activeUrl = active->url();
        else
            data.activeUid = active->uid(); // 与 activeUrl 互斥（整组重写保证）
    }
    return data;
}

bool SessionManager::persistNow(const std::filesystem::path& iniPath, const SessionData& data) {
    IniData ini;
    // [documents]：size + 编号条目 + active/activeUid（互斥；编号即标签位置）
    ini.setValue(kDocumentsSection, "size", std::to_string(data.documents.size()));
    int index = 0;
    for (const auto& entry : data.documents) {
        ++index;
        const std::string prefix = std::to_string(index) + "/";
        if (!entry.url.empty()) {
            ini.setValue(kDocumentsSection, prefix + "url", pathToUrlValue(entry.url));
            ini.setValue(kDocumentsSection, prefix + "cursor", std::to_string(entry.cursor));
            // 编码一律写（含 utf8）：重启后按记忆编码打开，避免自动识别反复误判
            ini.setValue(kDocumentsSection, prefix + "encoding",
                         encodingKey(entry.encoding.value_or(TextEncoding::Utf8)));
        } else if (!entry.uid.empty()) {
            ini.setValue(kDocumentsSection, prefix + "uid", entry.uid);
        }
        // 两键均空的条目不落盘（构建侧不产生；防御）
    }
    if (data.activeUrl && !data.activeUrl->empty())
        ini.setValue(kDocumentsSection, "active", pathToUrlValue(*data.activeUrl));
    else if (data.activeUid && !data.activeUid->empty())
        ini.setValue(kDocumentsSection, "activeUid", *data.activeUid);

    // [window]：持有有效几何才写（无则整节省略，防陈旧残留）
    if (data.window) {
        ini.setValue(kWindowSection, "x", std::to_string(data.window->x));
        ini.setValue(kWindowSection, "y", std::to_string(data.window->y));
        ini.setValue(kWindowSection, "width", std::to_string(data.window->width));
        ini.setValue(kWindowSection, "height", std::to_string(data.window->height));
        ini.setValue(kWindowSection, "maximized", data.window->maximized ? "true" : "false");
    }

    // [editor]：全局设置（自动换行/深色主题）；前向兼容旧格式键位
    if (data.wordWrap)
        ini.setValue(kEditorSection, "wordWrap", *data.wordWrap ? "true" : "false");
    // darkTheme 一律写（显式 false 记录「用户切回浅色」，阻却旧会话残留 true）
    if (data.darkTheme)
        ini.setValue(kEditorSection, "darkTheme", *data.darkTheme ? "true" : "false");

    return IniFile::write(iniPath, ini);
}

SessionData SessionManager::parseSession(const std::filesystem::path& iniPath, FileProbe probe) {
    SessionData data;
    std::optional<IniData> ini = IniFile::read(iniPath);
    if (!ini)
        return data; // 无文件/不可读：空会话（首启行为与现状一致）

    // [documents]
    long long size = 0;
    if (auto raw = ini->value(kDocumentsSection, "size")) {
        if (auto parsed = parseInt(*raw))
            size = *parsed;
    }
    // 上界防御：编号条目至少各占一个键，size 不可能超过节内键数
    //（防手工改坏的 size=999999999 长循环）
    const std::size_t keyCount = ini->entryCount(kDocumentsSection);
    if (size < 0 || static_cast<std::size_t>(size) > keyCount)
        size = static_cast<long long>(keyCount);

    for (long long i = 1; i <= size; ++i) {
        const std::string prefix = std::to_string(i) + "/";
        const auto urlRaw = ini->value(kDocumentsSection, prefix + "url");
        const auto uidRaw = ini->value(kDocumentsSection, prefix + "uid");
        const bool hasUrl = urlRaw && !urlRaw->empty();
        const bool hasUid = uidRaw && !uidRaw->empty();
        if (!hasUrl && !hasUid)
            continue; // 坏条目（两键皆空/编号跳号）：丢弃不阻断

        SessionDocumentEntry entry;
        entry.position = static_cast<int>(i - 1); // 绝对位置（含坏条目空位不回填）
        if (hasUrl) {
            entry.url = urlValueToPath(*urlRaw);
            if (entry.url.empty())
                continue; // 防御：解码失败视为坏条目
            if (probe && !probe(entry.url))
                continue; // 缺失/目录/不可访问：静默跳过（存在性预检）
            if (auto raw = ini->value(kDocumentsSection, prefix + "cursor")) {
                if (auto parsed = parseInt(*raw))
                    entry.cursor = static_cast<int>(*parsed);
            }
            if (entry.cursor < 0)
                entry.cursor = 0; // 负值（损坏数据）钳为 0
            entry.encoding =
                parseEncodingKey(ini->value(kDocumentsSection, prefix + "encoding")
                                     .value_or(""));
            // 编码缺失/非法 → nullopt → 打开时自动识别（旧会话兼容）
        } else {
            entry.uid = *uidRaw; // 未命名占位（旧格式无 uid 的空条目已在上文丢弃）
        }
        data.documents.push_back(std::move(entry));
    }

    // active / activeUid（互斥；active 优先，activeUid 仅在 active 缺失时读）
    if (auto raw = ini->value(kDocumentsSection, "active")) {
        if (!raw->empty())
            data.activeUrl = urlValueToPath(*raw);
        // active 指向的文件即使被预检丢弃也保留记录：恢复侧按「active 丢失
        // 激活最后成功文档」回退（C1）
    } else if (auto uid = ini->value(kDocumentsSection, "activeUid")) {
        if (!uid->empty())
            data.activeUid = *uid;
    }

    // [window]：五键齐全且均为合法整数才有效（对齐旧版 contains 全键判断）
    const auto x = parseInt(ini->value(kWindowSection, "x").value_or(""));
    const auto y = parseInt(ini->value(kWindowSection, "y").value_or(""));
    const auto w = parseInt(ini->value(kWindowSection, "width").value_or(""));
    const auto h = parseInt(ini->value(kWindowSection, "height").value_or(""));
    const auto maximized = ini->value(kWindowSection, "maximized");
    if (x && y && w && h && maximized) {
        data.window = WindowGeometry{static_cast<int>(*x), static_cast<int>(*y),
                                     static_cast<int>(*w), static_cast<int>(*h),
                                     parseBool(*maximized)};
    }

    // [editor]：全局设置（无记录 nullopt 保持默认）
    if (auto raw = ini->value(kEditorSection, "wordWrap"))
        data.wordWrap = parseBool(*raw);
    if (auto raw = ini->value(kEditorSection, "darkTheme"))
        data.darkTheme = parseBool(*raw);

    return data;
}

// ---------------------------------------------------------------------------
// 窗口几何校验
// ---------------------------------------------------------------------------

std::optional<WindowGeometry> SessionManager::validateGeometry(const WindowGeometry& geom,
                                                               const std::vector<RECT>& screens) {
    // 各屏最大可用宽高（多屏取最大，保证窗口能完整放进某一屏）
    LONG maxW = 0, maxH = 0;
    for (const RECT& r : screens) {
        maxW = (std::max)(maxW, r.right - r.left);
        maxH = (std::max)(maxH, r.bottom - r.top);
    }

    // 下限为产品最小窗口尺寸；屏幕过小时下限优先（不破坏最小尺寸保证）
    int width = (std::max)(kMinWindowWidth, geom.width);
    int height = (std::max)(kMinWindowHeight, geom.height);
    if (!screens.empty()) {
        width = (std::min)(width, (std::max)(kMinWindowWidth, static_cast<int>(maxW)));
        height = (std::min)(height, (std::max)(kMinWindowHeight, static_cast<int>(maxH)));
    }

    // 位置校验：最终矩形须与任一屏相交（拔掉副屏等场景），否则回退默认几何
    if (!screens.empty()) {
        const RECT frame{geom.x, geom.y, geom.x + width, geom.y + height};
        bool intersects = false;
        for (const RECT& r : screens) {
            if (frame.left < r.right && frame.right > r.left && frame.top < r.bottom
                && frame.bottom > r.top) {
                intersects = true;
                break;
            }
        }
        if (!intersects)
            return std::nullopt;
    }

    return WindowGeometry{geom.x, geom.y, width, height, geom.maximized};
}

// ---------------------------------------------------------------------------
// 路径解析
// ---------------------------------------------------------------------------

SessionPaths SessionManager::resolvePaths(const std::filesystem::path& exeDir) {
    // %LOCALAPPDATA%\qcnote（SHGetKnownFolderPath；失败回退环境变量；再失败空根）
    std::filesystem::path userBase;
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr,
                                       &localAppData))) {
        userBase = std::filesystem::path(localAppData) / L"qcnote";
    } else if (const wchar_t* env = _wgetenv(L"LOCALAPPDATA")) {
        userBase = std::filesystem::path(env) / L"qcnote";
    }
    if (localAppData)
        CoTaskMemFree(localAppData);
    return resolvePaths(exeDir, userBase);
}

SessionPaths SessionManager::resolvePaths(const std::filesystem::path& exeDir,
                                          const std::filesystem::path& userBaseDir) {
    // 便携判定：exe 旁存在 qcnote.portable 常规文件（仅判断存在，不读内容；
    // 标记是目录/无法判定 → 非便携）。空 exeDir 直接非便携（不以 CWD 为基准）
    std::error_code ec;
    const bool portable =
        !exeDir.empty()
        && std::filesystem::is_regular_file(exeDir / L"qcnote.portable", ec);
    if (portable) {
        return SessionPaths{exeDir / L"qcnote.ini", exeDir / L"drafts", true};
    }
    return SessionPaths{userBaseDir / L"qcnote.ini", userBaseDir / L"drafts", false};
}

// ---------------------------------------------------------------------------
// 会话键值编解码
// ---------------------------------------------------------------------------

std::string SessionManager::encodingKey(TextEncoding e) {
    // 旧格式一致的小写键值（读侧大小写不敏感）
    return e == TextEncoding::Gbk ? "gbk" : "utf8";
}

std::optional<TextEncoding> SessionManager::parseEncodingKey(std::string_view s) {
    if (asciiIEquals(s, "utf8") || asciiIEquals(s, "utf-8"))
        return TextEncoding::Utf8;
    if (asciiIEquals(s, "gbk"))
        return TextEncoding::Gbk;
    return std::nullopt; // 缺失/非法 → 自动识别（不阻断启动）
}

std::filesystem::path SessionManager::urlValueToPath(std::string_view value) {
    if (value.empty())
        return {};
    constexpr std::string_view kFileScheme = "file:";
    if (value.size() > kFileScheme.size()
        && asciiIEquals(value.substr(0, kFileScheme.size()), kFileScheme)) {
        std::string_view rest = value.substr(kFileScheme.size());
        if (rest.starts_with("///")) {
            // 本地路径：file:///C:/...（旧 QUrl::toString 百分号编码）
            return std::filesystem::path(utf8ToWide(percentDecode(rest.substr(3))));
        }
        if (rest.starts_with("//")) {
            // UNC：file://server/share → \\server\share
            std::string decoded = percentDecode(rest.substr(2));
            std::replace(decoded.begin(), decoded.end(), '/', '\\');
            return std::filesystem::path(utf8ToWide("\\\\" + decoded));
        }
        // 罕见形态（file:C:/...）：按剩余部分解码
        return std::filesystem::path(utf8ToWide(percentDecode(rest)));
    }
    // 新格式：泛化分隔符路径（UTF-8）
    return std::filesystem::path(utf8ToWide(std::string(value)));
}

std::string SessionManager::pathToUrlValue(const std::filesystem::path& path) {
    if (path.empty())
        return {};
    return wideToUtf8(path.generic_wstring());
}

} // namespace core
