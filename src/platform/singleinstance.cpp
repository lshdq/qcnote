// qcnote M5：单实例转发实现（D1）。设计与旧版语义对照见 singleinstance.h 头注释。
#include "platform/singleinstance.h"

#include <cwctype>
#include <cstring>

namespace platform::singleinstance {
namespace {

/// 载荷尺寸上限（1 MiB）：命令行实际远小于此；防御异常对端（旧版 4 MiB 帧缓冲
/// 上限的同目的收敛——WM_COPYDATA 由内核按 cbData 拷贝，超限直接拒收）
constexpr std::size_t kMaxPayloadBytes = 1 << 20;

/// 首实例窗口就绪重试窗口：互斥量已建而主窗口未建的启动竞态（约几十毫秒级）
/// 在此窗口内轮询等待；超时仍无窗口 → 降级独立运行（D1，与旧版 1s 连接超时同级）
constexpr ULONGLONG kWindowReadyTimeoutMs = 1000;
constexpr DWORD kWindowReadyPollMs = 100;

/// WM_COPYDATA 发送超时：SMTO_ABORTIFHUNG 防对端挂死（旧版 connect/write
/// 各 1000ms 的收敛上限）
constexpr UINT kCopyDataSendTimeoutMs = 5000;

/// FNV-1a 64（域标签/互斥量名共用；确定性：同 userDir 跨运行稳定）
ULONG_PTR Fnv1a64(const std::wstring& text) {
    std::uint64_t hash = 0xcbf29ce484222325ull;  // FNV 偏移基 = kBaseDomainTag
    for (const wchar_t ch : text) {
        hash ^= static_cast<std::uint16_t>(ch);  // wchar_t 截到 16 位（UTF-16 码元）
        hash *= 1099511628211ull;                // FNV 素数
    }
    return static_cast<ULONG_PTR>(hash);
}

/// 小端 u32 读取（memcpy——lpData 不保证对齐）
std::uint32_t ReadU32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
    return v;
}

void AppendU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

}  // namespace

const wchar_t* const kBaseMutexName = L"Local\\qcnote.singleinstance";
const wchar_t* const kDomainWindowProp = L"QcnoteSI.Domain";

ULONG_PTR DomainTag(const std::wstring& userDir) {
    std::wstring lower;
    lower.reserve(userDir.size());
    for (const wchar_t ch : userDir) lower.push_back(static_cast<wchar_t>(std::towlower(ch)));
    return Fnv1a64(lower);
}

std::wstring MutexName(const std::wstring& userDir) {
    const ULONG_PTR tag = DomainTag(userDir);
    if (tag == kBaseDomainTag) return kBaseMutexName;
    wchar_t hex[17] = {};
    swprintf_s(hex, 17, L"%016llx", static_cast<unsigned long long>(tag));
    return std::wstring(L"Local\\qcnote.si.") + hex;
}

std::vector<std::uint8_t> EncodeForwardPayload(const std::vector<std::filesystem::path>& paths) {
    std::vector<std::uint8_t> out;
    out.reserve(4 + paths.size() * 12);
    AppendU32(out, static_cast<std::uint32_t>(paths.size()));
    for (const auto& path : paths) {
        const std::wstring wide = path.wstring();
        AppendU32(out, static_cast<std::uint32_t>(wide.size()));
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(wide.data());
        out.insert(out.end(), bytes, bytes + wide.size() * sizeof(wchar_t));
    }
    return out;
}

std::optional<std::vector<std::filesystem::path>>
DecodeForwardPayload(const void* data, std::size_t bytes) {
    if (data == nullptr || bytes < 4 || bytes > kMaxPayloadBytes) return std::nullopt;
    const auto* p = static_cast<const std::uint8_t*>(data);
    const std::size_t count = ReadU32(p);
    p += 4;
    std::size_t remaining = bytes - 4;
    // 数量上限：每条目至少 4 字节长度字段（结构性防御，防 count 值爆炸）
    if (count > remaining / 4) return std::nullopt;

    std::vector<std::filesystem::path> paths;
    paths.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (remaining < 4) return std::nullopt;
        const std::size_t len = ReadU32(p);
        p += 4;
        remaining -= 4;
        if (len > remaining / sizeof(wchar_t)) return std::nullopt;
        const std::size_t byteLen = len * sizeof(wchar_t);
        if (len > 0) {
            std::wstring wide(len, L'\0');
            memcpy(wide.data(), p, byteLen);  // lpData 不保证对齐，逐字节拷入 wstring
            paths.emplace_back(std::move(wide));
        }  // 空串路径跳过（旧版语义：不影响整帧成功）
        p += byteLen;
        remaining -= byteLen;
    }
    if (remaining != 0) return std::nullopt;  // 尾部余量 = 帧不完整/被篡改
    return paths;
}

bool IsForwardPayload(const COPYDATASTRUCT* cds) {
    return cds != nullptr && cds->dwData == kForwardMagic && cds->lpData != nullptr &&
           cds->cbData >= 4 && cds->cbData <= kMaxPayloadBytes;
}

std::vector<HWND> EnumTopLevelWindowsOfClass(const wchar_t* className) {
    struct EnumCtx {
        const wchar_t* className = nullptr;
        std::vector<HWND> found;
    } ctx{className};

    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* context = reinterpret_cast<EnumCtx*>(lp);
            wchar_t name[64] = {};
            const int n = GetClassNameW(hwnd, name, 64);
            if (n > 0 && wcscmp(name, context->className) == 0) {
                context->found.push_back(hwnd);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return std::move(ctx.found);
}

HWND SelectPrimaryWindow(const std::vector<HWND>& candidates, ULONG_PTR domainTag,
                         const std::function<ULONG_PTR(HWND)>& propReader) {
    if (!propReader) return nullptr;
    for (HWND hwnd : candidates) {  // EnumWindows 序 = Z 序自顶向下：取首个匹配
        if (propReader(hwnd) == domainTag) return hwnd;
    }
    return nullptr;
}

std::filesystem::path AbsolutizeForOpen(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) return path;
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec || absolute.empty()) return path;
    return absolute;
}

bool TryForwardToPrimary(const std::vector<std::filesystem::path>& paths,
                         ULONG_PTR domainTag, const wchar_t* windowClass) {
    // 窗口枚举（带就绪重试）：互斥量存在但主窗口未建（首实例慢启动竞态）时，
    // 在 ~1s 窗口内轮询；超时无窗口 → 降级
    HWND target = nullptr;
    const auto propReader = [](HWND hwnd) {
        return reinterpret_cast<ULONG_PTR>(GetPropW(hwnd, kDomainWindowProp));
    };
    const ULONGLONG deadline = GetTickCount64() + kWindowReadyTimeoutMs;
    for (;;) {
        target = SelectPrimaryWindow(EnumTopLevelWindowsOfClass(windowClass), domainTag,
                                     propReader);
        if (target || GetTickCount64() >= deadline) break;
        Sleep(kWindowReadyPollMs);
    }
    if (!target) return false;

    // WM_COPYDATA（同步）：首实例 WM_COPYDATA 处理内只入队 + 投递消息并立即返回
    // TRUE，实际打开在消息循环消化——本调用快速返回，不会等文件打开
    const std::vector<std::uint8_t> payload = EncodeForwardPayload(paths);
    COPYDATASTRUCT cds{};
    cds.dwData = kForwardMagic;
    cds.cbData = static_cast<DWORD>(payload.size());
    cds.lpData = const_cast<std::uint8_t*>(payload.data());
    DWORD_PTR sendResult = 0;
    const LRESULT sent = SendMessageTimeoutW(
        target, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds),
        SMTO_NORMAL | SMTO_ABORTIFHUNG, kCopyDataSendTimeoutMs, &sendResult);
    return sent != 0 && sendResult != 0;
}

void AttachDomainTag(HWND mainWindow, ULONG_PTR domainTag) {
    if (mainWindow) SetPropW(mainWindow, kDomainWindowProp, reinterpret_cast<HANDLE>(domainTag));
}

void ActivateMainWindow(HWND mainWindow) {
    if (!mainWindow) return;
    if (IsIconic(mainWindow)) ShowWindow(mainWindow, SW_RESTORE);
    SetForegroundWindow(mainWindow);
}

}  // namespace platform::singleinstance
