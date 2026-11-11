// qcnote M5 平台层单测（D1 单实例 / D2 命令行归一 / D3 shell 集成）。
//
// 覆盖（测试用例文档 TC-I04 降级路径的可测注入 + TC-I11 shell 注册的单元级）：
// - 单实例判定：互斥量名派生（基础域全局唯一 / --userdir 注入域隔离 / 大小写
//   归一 / 确定性）、命名互斥量 OS 机制实证（二次 CreateMutex → ALREADY_EXISTS）；
// - 命令行序列化：WM_COPYDATA 载荷编解码往返（空列表/多路径/空格/中文/反斜杠）、
//   坏帧拒绝（截断/数量溢出/尾部余量/超限/魔数不符）；
// - 降级函数（注入）：SelectPrimaryWindow 域标签匹配（首命中/无匹配/空候选/
//   空 reader）；TryForwardToPrimary 真实调用无在域窗口 → 降级 false（D1 契约）；
// - 位置参数归一：相对 → 绝对（当前目录基准）、绝对/空不变；
// - shell 集成（注入配置，全程不碰真实键：HKCU 下随机 qcnote-selftest-<sfx>
//   子树，RAII 清理）：register → 注册表值精确断言（命令串/图标/显示文本）、
//   isRegistered、unregister 清除与幂等、首启标记语义（applyChoice false 仅写
//   标记 / true 写菜单+标记）。
#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "platform/shellintegration.h"
#include "platform/singleinstance.h"

using namespace platform::singleinstance;

namespace {

/// 十六进制字符判定（互斥量名后缀格式校验）
bool IsHexDigit(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
}

/// 读 HKCU 相对键的具名 REG_SZ 值（测试对注册表内容的直接断言，不经被测代码）
std::wstring ReadNamedValue(const std::wstring& subKey, const wchar_t* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return {};
    }
    wchar_t buffer[1024] = {};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    std::wstring value;
    if (RegQueryValueExW(key, valueName, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buffer), &size) == ERROR_SUCCESS &&
        type == REG_SZ && size >= sizeof(wchar_t)) {
        value.assign(buffer, size / sizeof(wchar_t));
        while (!value.empty() && value.back() == L'\0') value.pop_back();
    }
    RegCloseKey(key);
    return value;
}

bool KeyExists(const std::wstring& subKey) {
    HKEY key = nullptr;
    const bool exists = RegOpenKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0,
                                      KEY_QUERY_VALUE, &key) == ERROR_SUCCESS;
    if (exists) RegCloseKey(key);
    return exists;
}

/// 读 HKCU 相对键的具名 REG_DWORD 值（不存在/类型不符 → nullopt）
std::optional<std::uint32_t> ReadNamedDword(const std::wstring& subKey,
                                            const wchar_t* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return std::nullopt;
    }
    DWORD value = 0, size = sizeof(value), type = 0;
    const bool ok = RegQueryValueExW(key, valueName, nullptr, &type,
                                     reinterpret_cast<LPBYTE>(&value), &size) ==
                        ERROR_SUCCESS &&
                    type == REG_DWORD;
    RegCloseKey(key);
    if (!ok) return std::nullopt;
    return value;
}

/// 测试注册表子树 RAII 清理（HKCU\Software\qcnote-selftest-<sfx>）
struct RegistryTestScope {
    std::wstring classesRoot;  // 测试菜单键根（含 *\shell\<name> 整树）
    std::wstring markerRoot;   // 测试标记键
    unsigned counter = 0;

    RegistryTestScope() {
        const std::wstring sfx = std::to_wstring(GetCurrentProcessId()) + L"-" +
                                 std::to_wstring(GetTickCount64());
        classesRoot = L"Software\\qcnote-selftest-cls-" + sfx;
        markerRoot = L"Software\\qcnote-selftest-mk-" + sfx;
    }
    ~RegistryTestScope() {
        RegDeleteTreeW(HKEY_CURRENT_USER, classesRoot.c_str());
        RegDeleteTreeW(HKEY_CURRENT_USER, markerRoot.c_str());
    }
    RegistryTestScope(const RegistryTestScope&) = delete;
    RegistryTestScope& operator=(const RegistryTestScope&) = delete;
};

}  // namespace

// ---------------------------------------------------------------------------
// 单实例判定：互斥量名派生（D1）
// ---------------------------------------------------------------------------

TEST_CASE("单实例互斥量名：基础域全局唯一（不含 exe 路径成分）", "[platform][si]") {
    // 旧版服务名 qcnote-<USERNAME> 全局唯一、无路径成分 → 本版对齐为固定名
    REQUIRE(MutexName(L"") == kBaseMutexName);
    REQUIRE(kBaseMutexName == std::wstring(L"Local\\qcnote.singleinstance"));
    REQUIRE(MutexName(L"").find(L".si.") == std::wstring::npos);  // 无域后缀
}

TEST_CASE("单实例互斥量名：userdir 注入域派生（隔离 + 大小写归一 + 确定性）",
          "[platform][si]") {
    const std::wstring a = MutexName(L"D:\\temp\\user-a");
    const std::wstring b = MutexName(L"D:\\temp\\user-b");
    REQUIRE(a != kBaseMutexName);
    REQUIRE(a != b);
    REQUIRE(a.rfind(L"Local\\qcnote.si.", 0) == 0);
    REQUIRE(a.size() == 16 + 16);  // 前缀 16 字符 + 16 位十六进制标签
    const std::wstring suffix = a.substr(16);
    REQUIRE(suffix.find_first_not_of(L"0123456789abcdef") == std::wstring::npos);
    // 大小写归一：路径大小写不敏感（Windows 文件系统语义），同域
    REQUIRE(MutexName(L"d:\\TEMP\\user-a") == a);
    // 确定性：同输入跨调用稳定
    REQUIRE(MutexName(L"D:\\temp\\user-a") == a);
    // 域标签：空 userDir = 基础域常量；非空 = 派生且互异
    REQUIRE(DomainTag(L"") == kBaseDomainTag);
    REQUIRE(DomainTag(L"D:\\temp\\user-a") != kBaseDomainTag);
    REQUIRE(DomainTag(L"D:\\temp\\user-a") != DomainTag(L"D:\\temp\\user-b"));
}

TEST_CASE("命名互斥量 OS 机制：同进程二次创建报告 ALREADY_EXISTS", "[platform][si]") {
    // 实证「互斥量存在性探测」依赖的内核语义（首实例判定基础）
    const std::wstring unique = L"Local\\qcnote.test." +
                                std::to_wstring(GetCurrentProcessId()) + L"-" +
                                std::to_wstring(GetTickCount64());
    SetLastError(ERROR_SUCCESS);
    const HANDLE first = CreateMutexW(nullptr, FALSE, unique.c_str());
    REQUIRE(first != nullptr);
    REQUIRE(GetLastError() != ERROR_ALREADY_EXISTS);  // 首次创建
    SetLastError(ERROR_SUCCESS);
    const HANDLE second = CreateMutexW(nullptr, FALSE, unique.c_str());
    REQUIRE(second != nullptr);
    REQUIRE(GetLastError() == ERROR_ALREADY_EXISTS);  // 二次创建 → 已存在
    CloseHandle(second);
    CloseHandle(first);
}

// ---------------------------------------------------------------------------
// 命令行序列化：WM_COPYDATA 载荷（D1/D2）
// ---------------------------------------------------------------------------

TEST_CASE("转发载荷：空列表编码为 4 字节头并往返（仅激活语义）", "[platform][si]") {
    const std::vector<std::uint8_t> payload = EncodeForwardPayload({});
    REQUIRE(payload.size() == 4);
    const auto decoded = DecodeForwardPayload(payload.data(), payload.size());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->empty());
    COPYDATASTRUCT cds{};
    cds.dwData = kForwardMagic;
    cds.cbData = static_cast<DWORD>(payload.size());
    cds.lpData = const_cast<std::uint8_t*>(payload.data());
    REQUIRE(IsForwardPayload(&cds));
}

TEST_CASE("转发载荷：多路径往返（空格/中文/反斜杠/盘符）", "[platform][si]") {
    const std::vector<std::filesystem::path> paths = {
        L"D:\\notes\\hello.txt",
        L"D:\\temp\\带 空 格 的 文件.md",
        L"E:\\数据\\中文目录\\笔记 001.txt",
        L"C:\\a\\b\\c\\deep path with spaces.txt",
    };
    const std::vector<std::uint8_t> payload = EncodeForwardPayload(paths);
    const auto decoded = DecodeForwardPayload(payload.data(), payload.size());
    REQUIRE(decoded.has_value());
    REQUIRE(*decoded == paths);
}

TEST_CASE("转发载荷：空串路径跳过（旧版 decodeFrame 语义）", "[platform][si]") {
    const std::filesystem::path real = L"D:\\x\\y.txt";
    const std::vector<std::uint8_t> payload = EncodeForwardPayload({L"", real});
    const auto decoded = DecodeForwardPayload(payload.data(), payload.size());
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->size() == 1);
    REQUIRE((*decoded)[0] == real);
}

TEST_CASE("转发载荷：坏帧拒绝（截断/数量溢出/尾部余量/超限/魔数）", "[platform][si]") {
    const std::vector<std::uint8_t> good = EncodeForwardPayload({L"D:\\a b\\中文.txt"});
    // 头部截断
    REQUIRE_FALSE(DecodeForwardPayload(good.data(), 2).has_value());
    REQUIRE_FALSE(DecodeForwardPayload(good.data(), 0).has_value());
    REQUIRE_FALSE(DecodeForwardPayload(nullptr, 4).has_value());
    // 路径段截断（头 + 长度字段 + 半个字符）
    REQUIRE_FALSE(DecodeForwardPayload(good.data(), 4 + 4 + 1).has_value());
    // 尾部多余字节
    {
        std::vector<std::uint8_t> padded = good;
        padded.push_back(0);
        REQUIRE_FALSE(DecodeForwardPayload(padded.data(), padded.size()).has_value());
    }
    // 数量字段溢出（count=0xFFFFFFF、体仅 0 字节路径）
    {
        std::vector<std::uint8_t> evil{0xFF, 0xFF, 0xFF, 0x0F, 0x00, 0x00, 0x00, 0x00};
        REQUIRE_FALSE(DecodeForwardPayload(evil.data(), evil.size()).has_value());
    }
    // 超尺寸上限 → IsForwardPayload 拒收
    {
        COPYDATASTRUCT cds{};
        cds.dwData = kForwardMagic;
        cds.cbData = (1u << 20) + 1;
        cds.lpData = const_cast<std::uint8_t*>(good.data());
        REQUIRE_FALSE(IsForwardPayload(&cds));
        REQUIRE_FALSE(DecodeForwardPayload(good.data(), cds.cbData).has_value());
    }
    // 魔数不符 / 尺寸过小 / 空指针
    {
        COPYDATASTRUCT cds{};
        cds.dwData = kForwardMagic + 1;
        cds.cbData = static_cast<DWORD>(good.size());
        cds.lpData = const_cast<std::uint8_t*>(good.data());
        REQUIRE_FALSE(IsForwardPayload(&cds));
        cds.dwData = kForwardMagic;
        cds.cbData = 2;
        REQUIRE_FALSE(IsForwardPayload(&cds));
        REQUIRE_FALSE(IsForwardPayload(nullptr));
    }
}

// ---------------------------------------------------------------------------
// 降级函数（注入）：主窗口选择与转发失败路径（D1）
// ---------------------------------------------------------------------------

TEST_CASE("主窗口选择：域标签首命中 / 无匹配降级 / 空候选 / 空 reader", "[platform][si]") {
    static int dummy1 = 0, dummy2 = 0, dummy3 = 0;
    const HWND h1 = reinterpret_cast<HWND>(&dummy1);
    const HWND h2 = reinterpret_cast<HWND>(&dummy2);
    const HWND h3 = reinterpret_cast<HWND>(&dummy3);
    const ULONG_PTR wrongTag = 0x1111, rightTag = 0x2222;
    const std::map<HWND, ULONG_PTR> props{{h1, wrongTag}, {h2, rightTag}, {h3, rightTag}};

    const auto reader = [&props](HWND hwnd) -> ULONG_PTR {
        const auto it = props.find(hwnd);
        return it == props.end() ? 0 : it->second;
    };
    // 候选序（= 生产 EnumWindows 的 Z 序自顶向下）：取第一个域匹配者 h2
    REQUIRE(SelectPrimaryWindow({h1, h2, h3}, rightTag, reader) == h2);
    REQUIRE(SelectPrimaryWindow({h1, h2, h3}, wrongTag, reader) == h1);
    // 无匹配 / 空候选 / 空 reader → nullptr（调用方降级独立运行）
    REQUIRE(SelectPrimaryWindow({h1, h2, h3}, 0x3333, reader) == nullptr);
    REQUIRE(SelectPrimaryWindow({}, rightTag, reader) == nullptr);
    REQUIRE(SelectPrimaryWindow({h1, h2}, rightTag, nullptr) == nullptr);
    // 无属性（生产 GetPropW 对未挂属性窗口返回 0）→ 不命中基础域标签
    const auto zeroReader = [](HWND) -> ULONG_PTR { return 0; };
    REQUIRE(SelectPrimaryWindow({h1, h2}, rightTag, zeroReader) == nullptr);
}

TEST_CASE("转发降级：无在域窗口时 TryForwardToPrimary 返回 false（D1 不阻断契约）",
          "[platform][si]") {
    // 真实调用（非 mock）：用极不可能有窗口持有的域标签枚举全系统顶层窗口，
    // ~1s 就绪窗口内无匹配 → 降级 false（该返回值即 main 侧「独立启动」依据）
    const ULONG_PTR unlikelyTag = DomainTag(
        L"Q:\\qcnote-selftest-domain-that-never-exists-7f3a91c");
    const auto started = GetTickCount64();
    REQUIRE_FALSE(TryForwardToPrimary({L"D:\\some file.txt"}, unlikelyTag,
                                      L"QcnoteSelftestNoSuchWindowClass"));
    const auto elapsed = GetTickCount64() - started;
    REQUIRE(elapsed >= 900);   // 覆盖就绪重试窗口（慢启动竞态兜底）
    REQUIRE(elapsed <= 10000); // 但有界（CI 慢机余量）
}

// ---------------------------------------------------------------------------
// 位置参数归一（D2 健壮性）
// ---------------------------------------------------------------------------

TEST_CASE("位置参数归一：相对绝对化 / 绝对与空不变", "[platform][args]") {
    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    REQUIRE_FALSE(ec);

    REQUIRE(AbsolutizeForOpen(L"").empty());
    const std::filesystem::path absolute = L"D:\\dir with space\\文件.txt";
    REQUIRE(AbsolutizeForOpen(absolute) == absolute);
    // 相对路径 → 当前目录基准绝对化（与旧版 QUrl::fromUserInput 行为对齐）
    const std::filesystem::path made = AbsolutizeForOpen(L"sub\\rel 文件.txt");
    REQUIRE(made.is_absolute());
    REQUIRE((cwd / L"sub\\rel 文件.txt").lexically_normal() == made.lexically_normal());
}

// ---------------------------------------------------------------------------
// shell 集成（注入配置；全程不碰真实键，RAII 清理测试子树）
// ---------------------------------------------------------------------------

TEST_CASE("shell 集成：register 写入精确键值 / unregister 清除且幂等", "[platform][shell]") {
    RegistryTestScope scope;
    platform::shell::Config cfg;
    cfg.classesRoot = scope.classesRoot;
    cfg.markerRoot = scope.markerRoot;
    platform::shell::ShellIntegration shell(cfg);

    const std::filesystem::path exe = L"C:\\Program Dir\\qcnote.exe";
    const std::wstring menuKey = shell.menuKeyPath();
    const std::wstring commandKey = shell.commandKeyPath();

    // 初始：未注册、未标记（首启）
    REQUIRE_FALSE(shell.isRegistered());
    REQUIRE(shell.contextMenuPromptNeeded());
    REQUIRE_FALSE(KeyExists(menuKey));

    // 注册 → 注册表值精确断言（键结构照搬旧版）
    std::wstring error;
    REQUIRE(shell.registerContextMenu(exe, &error));
    REQUIRE(error.empty());
    REQUIRE(shell.isRegistered());
    REQUIRE(ReadNamedValue(menuKey, nullptr) == L"用 qcnote 打开");           // 默认值
    REQUIRE(ReadNamedValue(menuKey, L"Icon") == L"\"C:\\Program Dir\\qcnote.exe\"");
    REQUIRE(ReadNamedValue(commandKey, nullptr) ==
            L"\"C:\\Program Dir\\qcnote.exe\" \"%1\"");

    // 反注册 → 整树清除（含 command 子键）+ 幂等
    REQUIRE(shell.unregisterContextMenu(&error));
    REQUIRE_FALSE(shell.isRegistered());
    REQUIRE_FALSE(KeyExists(menuKey));
    REQUIRE(shell.unregisterContextMenu(&error));  // 键不存在视为成功
}

TEST_CASE("shell 集成：首启选择语义（拒绝仅写标记 / 同意写菜单+标记）",
          "[platform][shell]") {
    RegistryTestScope scope;
    platform::shell::Config cfg;
    cfg.classesRoot = scope.classesRoot;
    cfg.markerRoot = scope.markerRoot;
    platform::shell::ShellIntegration shell(cfg);
    const std::filesystem::path exe = L"D:\\绿 色 目 录\\qcnote.exe";

    // 拒绝：仅写标记（不再询问），不写菜单
    std::wstring error;
    REQUIRE(shell.applyContextMenuChoice(false, exe, &error));
    REQUIRE_FALSE(shell.isRegistered());
    REQUIRE_FALSE(shell.contextMenuPromptNeeded());
    REQUIRE(KeyExists(shell.markerKeyPath()));
    REQUIRE(ReadNamedDword(shell.markerKeyPath(), L"contextMenuPrompted") == std::uint32_t{1});

    // 同意：写菜单 + 标记（标记已存在，幂等重写）
    REQUIRE(shell.applyContextMenuChoice(true, exe, &error));
    REQUIRE(shell.isRegistered());
    REQUIRE_FALSE(shell.contextMenuPromptNeeded());
    REQUIRE(ReadNamedValue(shell.commandKeyPath(), nullptr) ==
            L"\"D:\\绿 色 目 录\\qcnote.exe\" \"%1\"");
}

TEST_CASE("shell 集成：注入键名与生产默认键路径布局", "[platform][shell]") {
    platform::shell::ShellIntegration production;  // 默认配置 = 旧版键结构
    REQUIRE(production.menuKeyPath() == L"Software\\Classes\\*\\shell\\qcnote");
    REQUIRE(production.commandKeyPath() == L"Software\\Classes\\*\\shell\\qcnote\\command");
    REQUIRE(production.markerKeyPath() == L"Software\\qcnote\\qcnote");
    // 只读探测不创建键：生产键不存在时 isRegistered/false 不落任何写入
    //（本机可能已有真实注册——不断言生产键状态，仅断言探测无副作用）
    REQUIRE(production.menuKeyPath().find(L"qcnote-selftest") == std::wstring::npos);
}
