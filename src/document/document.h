#pragma once

#include <QtQmlIntegration/qqmlintegration.h>
#include <QObject>
#include <QString>
#include <QUrl>

/**
 * @brief 打开的文本文档。
 *
 * 内容在内存中一律为 LF 换行、无 BOM；lineEnding 记录原文件换行风格，
 * encoding/hasBom 记录编码元数据（保存时由存储层据此写回）。
 * hasBom 语义为“该文档上次以 UTF-8 读/写时是否带 BOM”：GBK 保存不清除记录、
 * 写 GBK 忽略它（UTF-8+BOM 文件转 GBK 再转回 UTF-8 仍带 BOM）。
 * Document 只能由 DocumentManager 创建
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
    Q_PROPERTY(Encoding encoding READ encoding WRITE setEncoding NOTIFY encodingChanged)
    Q_PROPERTY(bool hasBom READ hasBom NOTIFY hasBomChanged)
    Q_PROPERTY(int cursorPosition READ cursorPosition WRITE setCursorPosition NOTIFY cursorPositionChanged)

public:
    enum class LineEnding
    {
        Lf,
        Crlf
    };
    Q_ENUM(LineEnding)

    /// 文档编码（QML 以 Document.Encoding.Gbk 使用）；新建文档默认 Utf8
    enum class Encoding
    {
        Utf8,
        Gbk
    };
    Q_ENUM(Encoding)

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
    Encoding encoding() const;
    bool hasBom() const;
    int cursorPosition() const;

    void setUrl(const QUrl &url);
    void setContent(const QString &content);
    void setModified(bool modified);
    void setLineEnding(LineEnding ending);
    void setEncoding(Encoding encoding);
    void setCursorPosition(int position);

    /**
     * @brief 加载路径专用：写入 url/内容/换行风格/编码/BOM 状态且不置脏。
     *
     * 与 setContent 不同（不同值会置 modified=true），供 DocumentManager
     * 在从存储读取文件后初始化文档状态（打开/按编码重载/另存覆盖重载共用）。
     * 不改动 cursorPosition：另存覆盖重载场景只同步内容等状态，不动光标
     * （按编码重载后的光标钳制由 DocumentManager 负责）。
     */
    void applyLoadedState(const QUrl &url, const QString &content, LineEnding ending,
                          Encoding encoding, bool hasBom);

signals:
    void urlChanged();
    void contentChanged();
    void modifiedChanged();
    void displayNameChanged();
    void filePathChanged();
    void lineEndingChanged();
    void encodingChanged();
    void hasBomChanged();
    void cursorPositionChanged();

private:
    /// 仅加载路径（applyLoadedState）可写：hasBom 是磁盘事实的记录，QML 不直接改
    void setHasBom(bool hasBom);

    void updateDisplayName();

    QUrl m_url;
    QString m_content;
    bool m_modified = false;
    QString m_displayName;
    QString m_untitledName;
    LineEnding m_lineEnding = LineEnding::Lf;
    Encoding m_encoding = Encoding::Utf8; ///< 新建文档默认 UTF-8（需求约定）
    bool m_hasBom = false;                ///< 上次以 UTF-8 读/写时是否带 BOM（GBK 保存不清除）
    int m_cursorPosition = 0;
};
