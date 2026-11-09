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
 * modified 是与基线（上次加载/保存的内容，见 m_savedContent）比较的计算属性：
 * 仅反映内容差异，编码/换行/BOM 元数据变化不置脏；内容回到基线（如撤消到
 * 打开时的样子）即自动清脏。改用基线比较而非粘性标志位，是为修复“撤消回到
 * 原始内容后仍提示未保存”的缺陷（回写内容相对脏内容是“变化”，曾导致误置脏）。
 * uid 为构造时生成的不可变 UUID，作草稿缓存条目主键（未命名文档无 URL、
 * 另存为后 URL 变化，uid 全生命周期稳定，DEC-002）；savedContentHash 为基线
 * 内容的 SHA-256 指纹，供草稿恢复时检测磁盘是否被外部修改。
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
    Q_PROPERTY(bool modified READ modified NOTIFY modifiedChanged)
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
    /// 构造时生成的不可变 UUID（无大括号形式）：草稿缓存条目主键（DEC-002），
    /// 供 DraftCacheManager 以 <uid>.json 定位条目；无 setter、不发信号
    QString uid() const;
    QString content() const;
    bool modified() const;
    /// 基线（m_savedContent，上次加载/保存内容）的 SHA-256 指纹（hex）：供草稿
    /// 恢复时检测磁盘是否被外部修改。内存内容一律 LF、无 BOM，指纹按 UTF-8
    /// 字节计算，与磁盘加载归一化后的内容可直接比较
    QString savedContentHash() const;
    QString displayName() const;
    QString filePath() const;
    LineEnding lineEnding() const;
    Encoding encoding() const;
    bool hasBom() const;
    int cursorPosition() const;

    void setUrl(const QUrl &url);
    void setContent(const QString &content);
    void setLineEnding(LineEnding ending);
    void setEncoding(Encoding encoding);
    void setCursorPosition(int position);

    /**
     * @brief 保存成功专用：把基线重置为当前内容（此后 modified 必为 false）。
     *
     * 仅供 DocumentManager 在写盘成功后调用；保存失败/编码回滚路径不得调用
     * （脏标记须保留）。已是干净态时基线不变、不发信号。
     */
    void markSaved();

    /**
     * @brief 加载路径专用：写入 url/内容/换行风格/编码/BOM 状态并重置内容基线。
     *
     * 与 setContent 不同（setContent 以基线为参照按比较结果决定脏标记，
     * applyLoadedState 则把基线重置为本次加载内容，加载后必为干净态），
     * 供 DocumentManager 在从存储读取文件后初始化文档状态
     * （打开/按编码重载/另存覆盖重载共用）。
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

    QString m_uid; ///< 构造时生成的不可变 UUID（草稿缓存条目主键，DEC-002）
    QUrl m_url;
    QString m_content;
    /// 基线：上次加载/保存的内容（内存统一 LF、无 BOM 形态）。modified = m_content != m_savedContent
    QString m_savedContent;
    QString m_displayName;
    QString m_untitledName;
    LineEnding m_lineEnding = LineEnding::Lf;
    Encoding m_encoding = Encoding::Utf8; ///< 新建文档默认 UTF-8（需求约定）
    bool m_hasBom = false;                ///< 上次以 UTF-8 读/写时是否带 BOM（GBK 保存不清除）
    int m_cursorPosition = 0;
};
