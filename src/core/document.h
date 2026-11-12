// qcnote 纯逻辑核心层：文档对象（M2-A，旧 src/document/document.cpp 语义移植）。
//
// 内容契约（DEC-010）：UTF-8 std::string、LF、无 BOM；Document 持有一份内容供
// 纯逻辑操作（保存/编码转换/草稿），M2-B 的 Scintilla 同步策略由接线层决定
// （Scintilla SC_CP_UTF8 下字节流与本层内容零转换直通）。
// modified 是与基线（上次加载/保存内容）比较的计算属性：仅反映内容差异，
// 编码/换行/BOM 元数据变化不置脏；内容回到基线自动清脏（修复撤回到原样仍提示
// 未保存的缺陷——旧版基线比较语义，保持）。
// uid 为构造时生成的不可变 GUID 字符串（CoCreateGuid，无大括号形式），作草稿
// 缓存条目主键（旧 DEC-002：无 URL 未命名文档、另存为后 URL 变化，uid 稳定）。
// 变更通知：无 Qt 信号，用单订阅 std::function 回调（DocumentManager 接线后
// 转发为多播事件，见 documentmanager.h）。
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

#include "core/encoding.h"
#include "core/textutil.h"

namespace core {

class Document {
public:
    /// @param untitledName 未保存文档的显示名（如 L"未命名-1"）；空时默认 L"未命名"
    explicit Document(std::wstring untitledName = L"");

    // —— 元数据 ——
    const std::string& uid() const;                 ///< 构造生成，不可变（无 setter）
    const std::filesystem::path& url() const;       ///< 空 = 未命名
    void setUrl(std::filesystem::path url);
    const std::wstring& displayName() const;        ///< url 文件名；无 url 时未命名名
    LineEnding lineEnding() const;
    void setLineEnding(LineEnding ending);
    TextEncoding encoding() const;
    void setEncoding(TextEncoding encoding);        ///< 新建文档默认 Utf8
    bool hasBom() const;                            ///< 上次以 UTF-8 读/写是否带 BOM
    /// 光标位置：Scintilla 字节偏移（SC_CP_UTF8 下与本文档 UTF-8 内容字节流一致；
    /// 状态栏字符列显示经 textutil 字节↔字符换算）。负值钳为 0；同值不通知。
    /// 返回 true = 位置实际变化（已通知），false = 同值短路——SCN_UPDATEUI 每
    /// 帧回写采样依赖此返回值消除重复刷新
    int cursorPosition() const;
    bool setCursorPosition(int position);

    // —— 内容（UTF-8、LF、无 BOM）——
    const std::string& content() const;
    void setContent(std::string content);
    bool modified() const;                          ///< content != 基线（计算属性）
    /// 全文 UTF-8 码点数（惰性缓存；仅内容变更时失效）。状态栏总字符数经
    /// SCN_UPDATEUI 每帧采样，此前每次全文档 O(N) 扫描是选区拖拽逐帧路径的
    /// 主要成本项——缓存后只有真正的内容编辑才重新计数。计数口径与
    /// core::utf8CharCount 一致（无效字节按每字节 1 计）
    std::size_t charCount() const;
    /// charCount() 实际重算次数（仅测试/诊断用途）：验证内容未变时不重扫全文
    int charCountComputeCount() const { return m_charCountComputeCount; }
    /// 基线（上次加载/保存内容）的 SHA-256 指纹（hex 小写）：草稿恢复时检测
    /// 磁盘是否被外部修改。内存内容一律 LF、无 BOM，与磁盘加载归一化后的
    /// 内容可直接比较
    std::string baselineHash() const;

    /// 保存成功专用：把基线重置为当前内容（此后 modified 必为 false）。
    /// 仅在写盘成功后调用（DocumentManager 负责）；已是干净态时不通知。
    void markSaved();

    /// 加载路径专用：写入 url/内容/换行/编码/BOM 并重置内容基线（加载后必为
    /// 干净态）。打开/按编码重载/另存覆盖重载共用。不动 cursorPosition
    /// （编码重载后的光标钳制由 DocumentManager 负责——旧版语义）。
    void applyLoadedState(std::filesystem::path url, std::string content, LineEnding ending,
                          TextEncoding encoding, bool hasBom);

    // —— 变更回调（单订阅；由持有者 DocumentManager 接线转发，见 documentmanager.h）——
    std::function<void()> onContentChanged;    ///< 内容值变化
    std::function<void()> onModifiedChanged;   ///< 脏↔干净翻转（对齐旧 modifiedChanged）
    std::function<void()> onMetadataChanged;   ///< url/displayName/lineEnding/encoding/hasBom/cursorPosition 变化

private:
    /// 仅加载路径（applyLoadedState）可写：hasBom 是磁盘事实记录，外部不直接改
    void setHasBom(bool hasBom);
    void updateDisplayName();

    std::string m_uid;                       ///< CoCreateGuid 字符串（不可变）
    std::filesystem::path m_url;
    std::string m_content;                   ///< UTF-8、LF
    std::string m_baseline;                  ///< 基线：上次加载/保存的内容
    std::wstring m_displayName;
    std::wstring m_untitledName;
    LineEnding m_lineEnding = LineEnding::Lf;
    TextEncoding m_encoding = TextEncoding::Utf8;
    bool m_hasBom = false;                   ///< 上次 UTF-8 读/写是否带 BOM（GBK 保存不清除）
    int m_cursorPosition = 0;                ///< 字节偏移
    mutable std::size_t m_charCount = 0;     ///< charCount() 惰性缓存（码点数）
    mutable bool m_charCountValid = false;   ///< 缓存有效标志（内容变更即失效）
    mutable int m_charCountComputeCount = 0; ///< charCount() 实际重算次数（仅测试/诊断用途）
};

} // namespace core
