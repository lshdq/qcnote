// qcnote M5：单实例转发（D1）——Win32 命名互斥量 + 主窗口域标签 + WM_COPYDATA。
//
// 旧版（Qt QLocalServer，基线 4e313f0）语义对照与本版决策：
// - 旧服务名 qcnote-<USERNAME>：机器级命名管道 + 用户名后缀隔离 → **全局唯一，
//   不含 exe 路径成分**（便携复制到不同目录也并入同一单实例域）。本版对齐：
//   命名互斥量固定 Local\qcnote.singleinstance（Local\ 前缀天然按登录会话隔离，
//   无需再拼用户名）——不按 exe 路径派生（决策记录见 M5 报告）。
// - 二次启动：参数转发既有实例后以退出码 0 结束（空列表也发送：对方仅前置
//   窗口不开新标签）；转发失败（窗口枚举不到 / WM_COPYDATA 超时或返回失败）
//   → **降级为独立运行，不阻断启动**（D1「IPC 不可用时降级」）。
// - 主实例侧：转发到达 → 参数入待打开队列（WM_COPYDATA 处理内只入队 + 投递
//   消息，立即返回 TRUE 解除对端阻塞）→ 消息循环消化时经「打开文件」语义打开
//   （去重激活已有标签、失败弹错误框，非会话恢复的静默跳过）→ 前置主窗口
//   （最小化先还原；旧版 requestActivate 语义）。
//
// 域标签（窗口属性）机制：主实例在主窗口挂 SetProp 域标签；转发方枚举候选
// 窗口后按域匹配。目的：同机多域并存时（--userdir 测试隔离域 vs 真实实例的
// 基础域，或降级产生的多实例）不把参数错投给别的域的窗口。
//
// 测试隔离：互斥量域可由 --userdir（仅测试钩子）派生——不同 userdir = 不同
// 互斥量名 + 不同域标签，e2e 场景互不干扰、绝不触碰真实实例的基础域；
// SelectPrimaryWindow 的属性读取经函数注入，单测可 fake（tests/test_platform.cpp）。
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace platform::singleinstance {

/// WM_COPYDATA.dwData 魔数（'Q''C''N''1' 小端 = 0x314E4351）：首实例侧帧识别
constexpr ULONG_PTR kForwardMagic = 0x314E4351;

/// 基础域互斥量名（全局唯一；Local\ = 登录会话命名空间）
extern const wchar_t* const kBaseMutexName;

/// 主窗口域标签的窗口属性名（SetPropW/GetPropW）
extern const wchar_t* const kDomainWindowProp;

/// 基础域标签（= 空 userDir 的 FNV-1a 64 偏移基，见 DomainTag）
constexpr ULONG_PTR kBaseDomainTag = 0xcbf29ce484222325ull;

/// 域标签：userDir 为空 → 基础域固定值；非空 → 路径小写化后 FNV-1a 64。
/// 互斥量名（MutexName）与主窗口属性（AttachDomainTag）共用同一派生
ULONG_PTR DomainTag(const std::wstring& userDir);

/// 互斥量名：基础域 → kBaseMutexName；注入域 → Local\qcnote.si.<16hex 标签>
std::wstring MutexName(const std::wstring& userDir);

// —— WM_COPYDATA 载荷编解码（纯函数；UTF-16 路径数组，空列表合法 = 仅激活）——
/// 帧结构（小端）：u32 路径数 + 逐路径 { u32 wchar 长度 + UTF-16 内容（无 NUL）}
std::vector<std::uint8_t> EncodeForwardPayload(const std::vector<std::filesystem::path>& paths);

/// 解码一帧；结构不合法（短读 / 超限 / 尾部余量 / 长度越界）返回 nullopt。
/// 空串路径跳过（防御，不影响整帧成功，与旧版 decodeFrame 语义一致）
std::optional<std::vector<std::filesystem::path>>
DecodeForwardPayload(const void* data, std::size_t bytes);

/// WM_COPYDATA 处理入口判定：魔数 + 尺寸上下限校验（下限 4 = count 字段）
bool IsForwardPayload(const COPYDATASTRUCT* cds);

// —— 主窗口选择（枚举注入可测）——
/// 枚举全部顶层 <className> 窗口（不过滤可见性：headless 主窗口永不显示，
/// 但仍应被转发命中；EnumWindows 自顶向下 Z 序）
std::vector<HWND> EnumTopLevelWindowsOfClass(const wchar_t* className);

/// 从候选中选首实例主窗口：第一个域标签匹配者（Z 序最高）；无匹配 → nullptr
///（调用方降级为独立运行）。propReader 注入：生产 = GetPropW 包装，测试 = fake
HWND SelectPrimaryWindow(const std::vector<HWND>& candidates, ULONG_PTR domainTag,
                         const std::function<ULONG_PTR(HWND)>& propReader);

/// 位置参数归一（D2 健壮性）：相对路径 → 按当前工作目录绝对化（保留原样语义，
/// 不解析符号链接/不校验存在——不存在文件的报错路径要展示用户给的路径）；
/// 绝对化失败（如当前目录不可得）原样返回
std::filesystem::path AbsolutizeForOpen(const std::filesystem::path& path);

// —— 进程级流程（main 接线）——
/// 二次启动转发：枚举（含 ~1s 首实例窗口就绪重试，覆盖慢启动竞态）→
/// SendMessageTimeout（5s，SMTO_ABORTIFHUNG）发 WM_COPYDATA。
/// @return true = 转发成功（调用方应立即以退出码 0 结束）；
///         false = 窗口枚举不到 / 发送失败（降级独立启动，D1 不阻断）
bool TryForwardToPrimary(const std::vector<std::filesystem::path>& paths,
                         ULONG_PTR domainTag, const wchar_t* windowClass);

/// 主实例：在主窗口挂域标签（转发方匹配依据；窗口销毁时系统自动回收属性）
void AttachDomainTag(HWND mainWindow, ULONG_PTR domainTag);

/// 转发到达后的激活（旧版 requestActivate 语义）：最小化先还原 + 前置。
/// --headless 等「不可见窗口」场景由调用方跳过（无激活语义，防自动化扰动）
void ActivateMainWindow(HWND mainWindow);

}  // namespace platform::singleinstance
