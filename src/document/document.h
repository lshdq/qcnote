#pragma once

#include <QtQmlIntegration/qqmlintegration.h>
#include <QObject>
#include <QString>
#include <QUrl>

/**
 * @brief 打开的文本文档。
 *
 * 内容在内存中一律为 LF 换行、无 BOM；lineEnding 记录原文件换行风格，
 * 保存时由存储层据此写回。Document 只能由 DocumentManager 创建
 * （QML_UNCREATABLE），QML 侧通过 DocumentManager 的属性访问。
 */
class Document : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Document 只能由 DocumentManager 创建")
    Q_PROPERTY(QUrl url READ url WRITE setUrl NOTIFY urlChanged)
    Q_PROPERTY(QString content READ content WRITE setContent NOTIFY contentChanged)
    Q_PROPERTY(bool modified READ modified WRITE setModified NOTIFY modifiedChanged)
    Q_PROPERTY(QString displayName READ displayName NOTIFY displayNameChanged)
    Q_PROPERTY(QString filePath READ filePath NOTIFY filePathChanged)
    Q_PROPERTY(LineEnding lineEnding READ lineEnding WRITE setLineEnding NOTIFY lineEndingChanged)
    Q_PROPERTY(int cursorPosition READ cursorPosition WRITE setCursorPosition NOTIFY cursorPositionChanged)

public:
    enum class LineEnding
    {
        Lf,
        Crlf
    };
    Q_ENUM(LineEnding)

    /**
     * @param untitledName 未保存文档的显示名（如 "未命名-1"）；为空时默认 "未命名"
     */
    explicit Document(const QString &untitledName = QString(), QObject *parent = nullptr);
    ~Document() override;

    QUrl url() const;
    QString content() const;
    bool modified() const;
    QString displayName() const;
    QString filePath() const;
    LineEnding lineEnding() const;
    int cursorPosition() const;

    void setUrl(const QUrl &url);
    void setContent(const QString &content);
    void setModified(bool modified);
    void setLineEnding(LineEnding ending);
    void setCursorPosition(int position);

    /**
     * @brief 加载路径专用：写入 url/内容/换行风格且不置脏。
     *
     * 与 setContent 不同（不同值会置 modified=true），供 DocumentManager
     * 在从存储读取文件后初始化文档状态。
     * 不改动 cursorPosition：另存覆盖重载场景只同步内容等状态，不动光标。
     */
    void applyLoadedState(const QUrl &url, const QString &content, LineEnding ending);

signals:
    void urlChanged();
    void contentChanged();
    void modifiedChanged();
    void displayNameChanged();
    void filePathChanged();
    void lineEndingChanged();
    void cursorPositionChanged();

private:
    void updateDisplayName();

    QUrl m_url;
    QString m_content;
    bool m_modified = false;
    QString m_displayName;
    QString m_untitledName;
    LineEnding m_lineEnding = LineEnding::Lf;
    int m_cursorPosition = 0;
};
