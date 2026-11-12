#include "core/document.h"

#include <windows.h>
#include <objbase.h> // CoCreateGuid / StringFromGUID2

#include <cwchar>
#include <utility>

namespace core {

namespace {

/// CoCreateGuid → 无大括号 GUID 字符串（旧 QUuid::WithoutBraces 等价）。
/// 生成失败返回空串（理论不可达；uid 稳定性由调用方测试覆盖）
std::string generateUid() {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid)))
        return {};
    wchar_t buffer[64] = {};
    if (StringFromGUID2(guid, buffer, 64) <= 0)
        return {};
    const std::wstring wide(buffer); // 形如 "{B95...-...}"（38 字符）
    std::string uid;
    uid.reserve(36);
    // 剥大括号；GUID 为 ASCII，窄化安全
    for (const wchar_t c : wide) {
        if (c != L'{' && c != L'}')
            uid.push_back(static_cast<char>(c));
    }
    return uid;
}

} // namespace

Document::Document(std::wstring untitledName)
    : m_uid(generateUid())
    , m_untitledName(untitledName.empty() ? L"未命名" : std::move(untitledName)) {
    updateDisplayName();
}

const std::string& Document::uid() const {
    return m_uid; // 不可变：无 setter、不发回调
}

const std::filesystem::path& Document::url() const {
    return m_url;
}

void Document::setUrl(std::filesystem::path url) {
    if (m_url == url)
        return;
    m_url = std::move(url);
    updateDisplayName(); // displayName 随 url 派生
    if (onMetadataChanged)
        onMetadataChanged();
}

const std::wstring& Document::displayName() const {
    return m_displayName;
}

LineEnding Document::lineEnding() const {
    return m_lineEnding;
}

void Document::setLineEnding(LineEnding ending) {
    if (m_lineEnding == ending)
        return;
    m_lineEnding = ending;
    if (onMetadataChanged)
        onMetadataChanged();
}

TextEncoding Document::encoding() const {
    return m_encoding;
}

void Document::setEncoding(TextEncoding encoding) {
    if (m_encoding == encoding)
        return; // 同值不通知（与 setLineEnding 同构）
    m_encoding = encoding;
    if (onMetadataChanged)
        onMetadataChanged();
}

bool Document::hasBom() const {
    return m_hasBom;
}

int Document::cursorPosition() const {
    return m_cursorPosition;
}

bool Document::setCursorPosition(int position) {
    // 值域 [0, +∞) 下界保护：负值（会话记录损坏等异常数据）钳为 0
    if (position < 0)
        position = 0;
    if (m_cursorPosition == position)
        return false; // 同值不通知；返回值供 SCN_UPDATEUI 采样路径消除重复刷新
    m_cursorPosition = position;
    if (onMetadataChanged)
        onMetadataChanged();
    return true;
}

const std::string& Document::content() const {
    return m_content;
}

void Document::setContent(std::string content) {
    if (m_content == content)
        return; // 同值不置脏、不通知（charCount 缓存随内容不变而保持有效）
    const bool wasModified = modified();
    m_content = std::move(content);
    m_charCountValid = false; // 内容变更：码点缓存失效（惰性重算）
    if (onContentChanged)
        onContentChanged();
    // 脏标记按基线比较结果翻转：回到基线自动清脏；脏值之间切换不翻转
    if (modified() != wasModified && onModifiedChanged)
        onModifiedChanged();
}

std::size_t Document::charCount() const {
    // 惰性缓存：状态栏经 SCN_UPDATEUI 每帧采样，全文档 O(N) 扫描只在内容
    // 真正变化后的首次采样发生（选区拖拽逐帧路径零扫描）
    if (!m_charCountValid) {
        m_charCount = utf8CharCount(m_content);
        m_charCountValid = true;
        ++m_charCountComputeCount; // 测试/诊断：观测真实重扫次数
    }
    return m_charCount;
}

bool Document::modified() const {
    return m_content != m_baseline;
}

std::string Document::baselineHash() const {
    // 按内存统一形态（LF、无 BOM）的 UTF-8 字节计算（与磁盘加载归一化后内容直接比较）
    return sha256Hex(m_baseline);
}

void Document::markSaved() {
    if (m_baseline == m_content)
        return; // 已是干净态：基线不重置（内容未变），不通知
    m_baseline = m_content;
    if (onModifiedChanged)
        onModifiedChanged(); // 脏→干净翻转
}

void Document::applyLoadedState(std::filesystem::path url, std::string content, LineEnding ending,
                                TextEncoding encoding, bool hasBom) {
    const bool wasModified = modified();
    setUrl(std::move(url));
    if (m_content != content) {
        m_content = content; // 值语义对齐旧版：内容恰与当前编辑值相同时不发通知
        m_charCountValid = false; // 内容变更：码点缓存失效（与 setContent 同规）
        if (onContentChanged)
            onContentChanged();
    }
    // 基线重置为本次加载内容：磁盘是事实之源，加载后必为干净态
    m_baseline = std::move(content);
    setLineEnding(ending);
    setEncoding(encoding);
    setHasBom(hasBom);
    if (modified() != wasModified && onModifiedChanged)
        onModifiedChanged();
}

void Document::setHasBom(bool hasBom) {
    if (m_hasBom == hasBom)
        return;
    m_hasBom = hasBom;
    if (onMetadataChanged)
        onMetadataChanged();
}

void Document::updateDisplayName() {
    std::wstring name = m_untitledName;
    if (!m_url.empty()) {
        const std::filesystem::path fileName = m_url.filename();
        if (!fileName.empty())
            name = fileName.wstring();
    }
    m_displayName = std::move(name);
}

} // namespace core
