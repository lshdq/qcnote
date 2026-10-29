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

void Document::applyLoadedState(const QUrl &url, const QString &content, LineEnding ending)
{
    setUrl(url);
    if (m_content != content) {
        m_content = content;
        emit contentChanged();
    }
    setLineEnding(ending);
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
