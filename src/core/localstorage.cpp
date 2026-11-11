#include "core/localstorage.h"

#include <windows.h>

#include <array>
#include <system_error>

namespace core {

namespace {

/// GetLastError → 中文可读消息（系统消息表，回退错误码）
std::wstring lastWin32ErrorMessage(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // 用户默认语言（中文系统为中文消息）
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring message = (len > 0 && buffer) ? std::wstring(buffer, len) : std::wstring();
    if (buffer)
        LocalFree(buffer);
    // 去掉尾部换行/空白（系统消息自带 \r\n）
    while (!message.empty() && iswspace(static_cast<wint_t>(message.back())))
        message.pop_back();
    if (message.empty())
        return L"错误码 " + std::to_wstring(error);
    return message;
}

StorageError makeError(StorageErrorKind kind, const std::wstring& message) {
    return StorageError{kind, message};
}

/// LF 内容 → CRLF 字节（单遍；内容契约为纯 LF，防御性把孤立 CR 也归一为换行）
std::string lfToCrlf(std::string_view content) {
    if (content.find('\n') == std::string_view::npos && content.find('\r') == std::string_view::npos)
        return std::string(content);
    std::string out;
    out.reserve(content.size() + content.size() / 8);
    for (std::size_t i = 0; i < content.size(); ++i) {
        const char c = content[i];
        if (c == '\n') {
            out += "\r\n";
        } else if (c == '\r') {
            if (i + 1 < content.size() && content[i + 1] == '\n')
                ++i; // 防御：CRLF 不重复（契约上不会出现）
            out += "\r\n";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

/// 读取成功后组装 LoadedData：换行探测 + LF 归一
LoadedData assembleLoaded(std::string text, TextEncoding encoding, bool hasBom) {
    LoadedData data;
    data.lineEnding = detectLineEnding(text);
    data.content = normalizeLineEndings(std::move(text));
    data.encoding = encoding;
    data.hasBom = hasBom;
    return data;
}

} // namespace

std::wstring LocalStorage::scheme() const {
    return L"file";
}

bool LocalStorage::supports(const std::filesystem::path& path) const {
    // 与 parseFileUrl 的拒绝口径一致：非 file scheme（http:// 等）与
    // file://host 网络主机形态一律不认领，留给对应 scheme 的存储
    return parseFileUrl(path.wstring()).has_value();
}

std::expected<std::string, StorageError>
LocalStorage::readRawBytes(const std::filesystem::path& path) const {
    if (path.empty())
        return std::unexpected(makeError(StorageErrorKind::InvalidUrl, L"不是有效的本地文件地址"));

    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return std::unexpected(
            makeError(StorageErrorKind::NotFound, L"文件不存在：" + path.wstring()));
    if (std::filesystem::is_directory(path, ec))
        return std::unexpected(
            makeError(StorageErrorKind::IsDirectory, L"目标是目录而不是文件：" + path.wstring()));

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        return std::unexpected(makeError(StorageErrorKind::OpenFailed,
                                         L"无法打开文件 " + path.wstring() + L"："
                                             + lastWin32ErrorMessage(err)));
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0
        || size.QuadPart > static_cast<LONGLONG>(INT32_MAX)) {
        CloseHandle(file);
        return std::unexpected(
            makeError(StorageErrorKind::OpenFailed, L"无法读取文件 " + path.wstring() + L"：文件过大"));
    }

    std::string raw(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset = 0;
    while (offset < raw.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(static_cast<std::size_t>(1) << 30, raw.size() - offset));
        DWORD read = 0;
        if (!ReadFile(file, raw.data() + offset, chunk, &read, nullptr) || read == 0) {
            const DWORD err = GetLastError();
            CloseHandle(file);
            return std::unexpected(makeError(StorageErrorKind::OpenFailed,
                                             L"无法读取文件 " + path.wstring() + L"："
                                                 + lastWin32ErrorMessage(err)));
        }
        offset += read;
    }
    CloseHandle(file);
    return raw;
}

std::expected<LoadedData, StorageError>
LocalStorage::read(const std::filesystem::path& path, std::optional<TextEncoding> forced) const {
    auto raw = readRawBytes(path);
    if (!raw)
        return std::unexpected(raw.error());

    const std::string& bytes = *raw;
    if (!forced) {
        // 自动识别：BOM（权威）→ 严格 UTF-8 校验 → GBK
        const EncodingDetection detected = detectEncoding(bytes);
        if (detected.encoding == TextEncoding::Utf8) {
            // BOM 已由识别判定：显式剥离后解码（BOM 不进内容）
            const std::string_view body =
                detected.hasBom ? std::string_view(bytes).substr(3) : std::string_view(bytes);
            return assembleLoaded(sanitizeUtf8(body), TextEncoding::Utf8, detected.hasBom);
        }
    } else if (*forced == TextEncoding::Utf8) {
        // 强制 UTF-8：磁盘带 BOM 则剥离并记录（内容不含 BOM 字符）
        const bool hasBom = bytes.starts_with(kUtf8Bom);
        const std::string_view body =
            hasBom ? std::string_view(bytes).substr(3) : std::string_view(bytes);
        return assembleLoaded(sanitizeUtf8(body), TextEncoding::Utf8, hasBom);
    }

    // GBK（自动识别回退或强制）：无法解码字节以 U+FFFD 呈现，保证文件可打开
    auto decoded = decodeGbk(bytes);
    if (!decoded)
        return std::unexpected(
            makeError(StorageErrorKind::DecodeFailed,
                      decoded.error().empty() ? L"无法以 GBK 解码文件：" + path.wstring()
                                              : decoded.error()));
    return assembleLoaded(std::move(*decoded), TextEncoding::Gbk, false);
}

std::expected<void, StorageError>
LocalStorage::write(const std::filesystem::path& path, const std::string& content,
                    TextEncoding encoding, bool hasBom, LineEnding lineEnding) const {
    if (path.empty())
        return std::unexpected(makeError(StorageErrorKind::InvalidUrl, L"不是有效的本地文件地址"));

    // —— 字节准备（GBK 不可编码校验先于任何磁盘操作，磁盘不被触碰）——
    std::string bytes;
    if (lineEnding == LineEnding::Crlf) {
        const std::string converted = lfToCrlf(content);
        if (encoding == TextEncoding::Utf8) {
            bytes = converted;
            if (hasBom)
                bytes.insert(bytes.begin(), {'\xEF', '\xBB', '\xBF'}); // BOM 仅元数据
        } else {
            auto encoded = encodeGbk(converted);
            if (!encoded)
                return std::unexpected(makeError(StorageErrorKind::EncodingFailed,
                                                 gbkEncodeFailureMessage(encoded.error())));
            bytes = std::move(*encoded);
        }
    } else if (encoding == TextEncoding::Utf8) {
        bytes = content; // LF 原样（内容契约：内存一律 LF）
        if (hasBom)
            bytes.insert(bytes.begin(), {'\xEF', '\xBB', '\xBF'});
    } else {
        auto encoded = encodeGbk(content);
        if (!encoded)
            return std::unexpected(makeError(StorageErrorKind::EncodingFailed,
                                             gbkEncodeFailureMessage(encoded.error())));
        bytes = std::move(*encoded);
    }

    // —— 原子写：同目录临时文件 → FlushFileBuffers → MoveFileEx 替换 ——
    // 同目录保证与目标同卷（rename 原子性）；任一步失败即删除临时文件，
    // 原文件不被破坏（对齐 QSaveFile 语义）
    std::filesystem::path dir = path.parent_path();
    if (dir.empty())
        dir = L".";
    const std::wstring dirStr = dir.wstring();
    if (dirStr.size() > MAX_PATH - 14)
        return std::unexpected(
            makeError(StorageErrorKind::WriteFailed, L"写入文件失败 " + path.wstring() + L"：路径过长"));

    wchar_t tempPath[MAX_PATH] = {};
    // GetTempFileNameW 原子创建唯一 0 字节临时文件（qcn 前缀 ≤3 字符）
    if (GetTempFileNameW(dirStr.c_str(), L"qcn", 0, tempPath) == 0) {
        const DWORD err = GetLastError();
        return std::unexpected(makeError(StorageErrorKind::WriteFailed,
                                         L"写入文件失败 " + path.wstring() + L"：无法创建临时文件（"
                                             + lastWin32ErrorMessage(err) + L"）"));
    }

    auto failWrite = [&](const std::wstring& detail) -> std::expected<void, StorageError> {
        DeleteFileW(tempPath); // 清理临时文件，不留垃圾
        return std::unexpected(
            makeError(StorageErrorKind::WriteFailed, L"写入文件失败 " + path.wstring() + L"：" + detail));
    };

    HANDLE file = CreateFileW(tempPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        return failWrite(L"无法打开临时文件（" + lastWin32ErrorMessage(err) + L"）");
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(static_cast<std::size_t>(1) << 30, bytes.size() - offset));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr)
            || written != chunk) {
            const DWORD err = GetLastError();
            CloseHandle(file);
            return failWrite(lastWin32ErrorMessage(err));
        }
        offset += written;
    }
    if (!FlushFileBuffers(file)) { // 落盘后再替换，防断电半截文件
        const DWORD err = GetLastError();
        CloseHandle(file);
        return failWrite(lastWin32ErrorMessage(err));
    }
    CloseHandle(file);

    if (!MoveFileExW(tempPath, path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD err = GetLastError();
        return failWrite(L"无法替换原文件（" + lastWin32ErrorMessage(err) + L"）");
    }
    return {};
}

} // namespace core
