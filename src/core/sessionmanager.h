// qcnote 纯逻辑核心层：会话管理器（M4-A，旧 src/document/sessionmanager 语义移植）。
//
// 职责（纯逻辑、无窗口依赖；计时器/屏幕/文件探测均注入）：
// - 会话数据构建与持久化：[documents] size=N + N\url / N\cursor / N\encoding
//   （utf8/gbk）+ 未命名 N\uid（uid 即标签位置记录，DEC-001）+ active（URL）
//   或 activeUid（两键互斥，整组重写）+ [window] x/y/width/height/maximized
//  （[editor] wordWrap 前向兼容读写）；
// - 恢复查询：parseSession 容错解析（坏条目丢弃、编号跳号容忍、url 与 uid
//   均空丢弃、旧格式无 uid 未命名条目跳过【DEC-005】、编码非法回退自动识别、
//   文件存在性预检注入【缺失/目录静默跳过】）；
// - 窗口几何校验：宽高钳 [800×500, 屏幕可用区]，位置与某屏相交否则 nullopt
//  （显示器变更回退默认几何）；screens 由调用方注入 vector<RECT>（可 mock）；
// - 路径解析：便携模式（exe 旁 qcnote.portable 标记文件）→ ini 与 drafts/ 落
//   exe 旁；否则 %LOCALAPPDATA%\qcnote\qcnote.ini 与 ...\drafts（D5）。
//   注：旧 Qt 版 QSettings IniFormat UserScope 实际落 %APPDATA%\qcnote\
//   （Roaming），本版按重写需求 D5 统一为 LOCALAPPDATA 单层，旧资产自然失联
//   不迁移（与便携切换同一语义）；
// - 防抖：纯逻辑 Debouncer（时钟注入），会话 500ms / 草稿 1000ms 共用。
//   本层不持有定时器——B 层驱动 trigger/flushDue（UI timer 或消息循环）。
#pragma once

#include <windows.h> // RECT（几何校验的屏幕可用区注入）

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/documentmanager.h"
#include "core/encoding.h"

namespace core {

/// 会话文档条目：url 非空 = 文件条目（记光标/编码）；uid 非空 = 未命名占位
///（内容随草稿缓存恢复，位置由 position 记录——关闭现场还原 DEC-001/002）
struct SessionDocumentEntry {
    std::filesystem::path url;                    ///< 空 = 未命名占位条目
    std::string uid;                              ///< 未命名条目主键（与草稿同源）
    int cursor = 0;                               ///< 字节偏移（文件条目）
    std::optional<TextEncoding> encoding;         ///< 缺失/非法 → 打开时自动识别
    int position = 0;                             ///< 标签栏绝对位置（0 基，读自编号-1）

    bool operator==(const SessionDocumentEntry&) const = default;
};

/// 窗口 normal 几何 + 最大化标志
struct WindowGeometry {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool maximized = false;

    bool operator==(const WindowGeometry&) const = default;
};

/// 会话数据（持久化载体与恢复查询结果）
struct SessionData {
    /// 有序文档条目（含未命名占位；position 保留原始编号的绝对位置——
    /// 坏条目被丢弃后形成空位时后续条目 position 不回填）
    std::vector<SessionDocumentEntry> documents;
    std::optional<std::filesystem::path> activeUrl; ///< 活动标签为文件（与 activeUid 互斥）
    std::optional<std::string> activeUid;           ///< 活动标签为未命名
    std::optional<WindowGeometry> window;           ///< 五键齐全才有值
    std::optional<bool> wordWrap;                   ///< [editor] 全局设置（前向兼容）
    std::optional<bool> darkTheme;                  ///< [editor] 深色主题（E5，M6）

    bool operator==(const SessionData&) const = default;
};

/// 防抖器（纯逻辑；时钟由调用方注入，无定时器/线程）。
/// B 层接线形态：变化处 trigger(now)；消息循环周期性或定时器回调里
/// flushDue(now)，返回 true 时执行落盘
class Debouncer {
public:
    explicit Debouncer(long long intervalMs)
        : m_interval(intervalMs) {}

    /// （重）启动防抖窗口（restart 语义，聚合高频变化）
    void trigger(long long now) {
        m_active = true;
        m_dueAt = now + m_interval;
    }

    bool pending() const { return m_active; }

    /// 挂起时的到期时刻（未挂起返回 nullopt）
    std::optional<long long> dueAt() const {
        return m_active ? std::optional<long long>(m_dueAt) : std::nullopt;
    }

    /// now ≥ 到期时刻 → 返回 true 并清除挂起（调用方执行 flush）；否则 false
    bool flushDue(long long now) {
        if (m_active && now >= m_dueAt) {
            m_active = false;
            return true;
        }
        return false;
    }

    void cancel() { m_active = false; }

private:
    long long m_interval;
    bool m_active = false;
    long long m_dueAt = 0;
};

/// 会话 ini 与草稿目录解析结果
struct SessionPaths {
    std::filesystem::path iniPath;    ///< 会话文件
    std::filesystem::path draftsDir;  ///< 草稿缓存目录（与 ini 同根）
    bool portable = false;            ///< exe 旁 qcnote.portable 标记命中
};

class SessionManager {
public:
    /// 文件状态预检注入（parseSession 可选）：返回 url 是否存在且为常规文件。
    /// 提供时缺失/目录条目按坏条目静默跳过（C1 文件存在性预检）
    using FileProbe = std::function<bool(const std::filesystem::path&)>;

    // —— 构建（从 DocumentManager 快照；只记有 URL 的文档 + 未命名 uid 位置）——
    static SessionData buildSessionData(const DocumentManager& manager);

    // —— 持久化（整组重写 [documents]，active/activeUid 互斥）——
    static bool persistNow(const std::filesystem::path& iniPath, const SessionData& data);

    /// 解析会话文件（容错；文件不存在返回空 SessionData）
    static SessionData parseSession(const std::filesystem::path& iniPath, FileProbe probe = nullptr);

    // —— 窗口几何校验（纯函数，screens 可 mock）——
    /// 宽钳 [800, 各屏最大可用宽]、高钳 [500, 各屏最大可用高]（多屏取最大，
    /// 屏幕过小时下限优先）；最终矩形须与任一屏相交，否则 nullopt（回退默认
    /// 几何）。位置不调整（仅校验）。screens 为空：仅下限钳制、不校验位置
    static std::optional<WindowGeometry> validateGeometry(const WindowGeometry& geom,
                                                          const std::vector<RECT>& screens);

    // —— 路径解析（纯函数 + 环境兜底）——
    /// 便携检测 + %LOCALAPPDATA% 兜底（生产路径）
    static SessionPaths resolvePaths(const std::filesystem::path& exeDir);
    /// 同上，用户根目录注入（测试不碰真实用户目录）
    static SessionPaths resolvePaths(const std::filesystem::path& exeDir,
                                     const std::filesystem::path& userBaseDir);

    // —— 会话键值编解码（编码字符串 / URL 值转换；草稿层复用）——
    static std::string encodingKey(TextEncoding e);              ///< "utf8"/"gbk"（旧格式一致）
    static std::optional<TextEncoding> parseEncodingKey(std::string_view s); ///< 宽容（大小写不敏感）
    /// ini/JSON 中的 url 值 → 路径：file:/// 前缀（旧 QUrl 资产）→ 百分号解码
    /// 本地路径；其余按泛化分隔符路径原样
    static std::filesystem::path urlValueToPath(std::string_view value);
    /// 路径 → url 值：泛化分隔符（正斜杠）UTF-8（新格式；读侧两者兼容）
    static std::string pathToUrlValue(const std::filesystem::path& path);
};

} // namespace core
