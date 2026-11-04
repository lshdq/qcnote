#include "document/document.h"

Document::Document(const QString &untitledName, QObject *parent)
    : QObject(parent)
    , m_untitledName(untitledName.isEmpty() ? QStringLiteral("未命名") : untitledName)
{
    updateDisplayName();
}

Document::~Document() = default;

QUrl Document::url() const
{
    return m_url;
}

QString Document::content() const
{
    return m_content;
}

bool Document::modified() const
{
    return m_modified;
}

QString Document::displayName() const
{
    return m_displayName;
}

QString Document::filePath() const
{
    return m_url.isLocalFile() ? m_url.toLocalFile() : QString();
}

Document::LineEnding Document::lineEnding() const
{
    return m_lineEnding;
}

Document::Encoding Document::encoding() const
{
    return m_encoding;
}

bool Document::hasBom() const
{
    return m_hasBom;
}

int Document::cursorPosition() const
{
    return m_cursorPosition;
}

void Document::setUrl(const QUrl &url)
{
    if (m_url == url)
        return;
    m_url = url;
    emit urlChanged();
    emit filePathChanged(); // filePath 由 url 派生
    updateDisplayName();
}

void Document::setContent(const QString &content)
{
    if (m_content == content)
        return; // 同值不置脏
    m_content = content;
    emit contentChanged();
    setModified(true);
}

void Document::setModified(bool modified)
{
    if (m_modified == modified)
        return;
    m_modified = modified;
    emit modifiedChanged();
}

void Document::setLineEnding(LineEnding ending)
{
    if (m_lineEnding == ending)
        return;
    m_lineEnding = ending;
    emit lineEndingChanged();
}

void Document::setEncoding(Encoding encoding)
{
    if (m_encoding == encoding)
        return; // 同值不发信号（与 setLineEnding 同构）
    m_encoding = encoding;
    emit encodingChanged();
}

void Document::setHasBom(bool hasBom)
{
    if (m_hasBom == hasBom)
        return;
    m_hasBom = hasBom;
    emit hasBomChanged();
}

void Document::setCursorPosition(int position)
{
    // 值域 [0, +∞) 下界保护：负值（会话记录损坏等异常数据）钳为 0；
    // 钳制后再比较，同值无信号语义不变
    position = qMax(0, position);
    if (m_cursorPosition == position)
        return; // 同值直接返回，不发射信号（防 QML 光标位置回写造成的绑定环）
    m_cursorPosition = position;
    emit cursorPositionChanged();
}

void Document::applyLoadedState(const QUrl &url, const QString &content, LineEnding ending,
                                Encoding encoding, bool hasBom)
{
    setUrl(url);
    if (m_content != content) {
        m_content = content;
        emit contentChanged();
    }
    setLineEnding(ending);
    setEncoding(encoding);
    setHasBom(hasBom);
    setModified(false);
}

void Document::updateDisplayName()
{
    QString name = m_untitledName;
    if (!m_url.isEmpty() && !m_url.fileName().isEmpty())
        name = m_url.fileName();
    if (name == m_displayName)
        return;
    m_displayName = name;
    emit displayNameChanged();
}
