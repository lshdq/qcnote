#include "document/sessionmanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJSEngine>
#include <QQmlEngine>
#include <QScreen>
#include <QSettings>

#include <optional>

#include "document/document.h"
#include "document/documentmanager.h"

SessionManager *SessionManager::s_instance = nullptr;

SessionManager::SessionManager(DocumentManager *manager, const QString &filePath, QObject *parent)
    : QObject(parent)
    , m_manager(manager)
{
    // 会话文件：测试注入显式路径；缺省优先便携模式（exe 旁存在 qcnote.portable
    // 标记时会话 ini 落在 exe 同目录），无标记时为用户目录 qcnote.ini
    if (filePath.isEmpty()) {
        const QString portableIni = resolvePortableIniPath(QCoreApplication::applicationDirPath());
        if (!portableIni.isEmpty())
            m_settings = std::make_unique<QSettings>(portableIni, QSettings::IniFormat);
        else
            m_settings = std::make_unique<QSettings>(QSettings::IniFormat, QSettings::UserScope,
                                                     QStringLiteral("qcnote"), QStringLiteral("qcnote"));
    } else
        m_settings = std::make_unique<QSettings>(filePath, QSettings::IniFormat);

    // 防抖：500ms 单触发聚合高频变化（光标移动、窗口拖动），CoarseTimer 精度足够
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(500);
    m_debounce.setTimerType(Qt::CoarseTimer);
    connect(&m_debounce, &QTimer::timeout, this, &SessionManager::saveNow);

    if (m_manager) {
        // documentsChanged 承担两件事：防抖保存 + 为新文档补连 url/光标信号
        // （newDocument 不发 documentOpened，统一经 documentsChanged 遍历对比集合）
        connect(m_manager, &DocumentManager::documentsChanged, this, [this]() {
            connectDocumentSignals();
            scheduleSave();
        });
        connect(m_manager, &DocumentManager::activeDocumentChanged, this, &SessionManager::scheduleSave);
        connectDocumentSignals(); // 构造时管理器可能已有文档（复用既有实例的场景）
    }

    // 预读 [window]：让本实例的 saveNow 能原样保留上次退出的几何——
    // 启动 restoreDocuments 末尾的对齐写盘不会清掉 QML 尚未应用的窗口状态
    if (m_settings->contains(QStringLiteral("window/x"))
        && m_settings->contains(QStringLiteral("window/y"))
        && m_settings->contains(QStringLiteral("window/width"))
        && m_settings->contains(QStringLiteral("window/height"))
        && m_settings->contains(QStringLiteral("window/maximized"))) {
        m_windowX = m_settings->value(QStringLiteral("window/x")).toInt();
        m_windowY = m_settings->value(QStringLiteral("window/y")).toInt();
        m_windowWidth = m_settings->value(QStringLiteral("window/width")).toInt();
        m_windowHeight = m_settings->value(QStringLiteral("window/height")).toInt();
        m_windowMaximized = m_settings->value(QStringLiteral("window/maximized")).toBool();
        m_windowValid = true;
    }

    // 预读 [editor]：编辑器全局设置（自动换行），无记录保持默认 false、不触发写盘
    if (m_settings->contains(QStringLiteral("editor/wordWrap")))
        m_wordWrap = m_settings->value(QStringLiteral("editor/wordWrap")).toBool();

    // 退出兜底：防抖挂起状态在退出前落盘
    connect(qApp, &QCoreApplication::aboutToQuit, this, &SessionManager::flush);
}

SessionManager::~SessionManager()
{
    // 兜底落盘（QSettings 随成员析构自动 sync；这里显式 flush 覆盖防抖挂起场景）
    flush();
    if (s_instance == this)
        s_instance = nullptr;
}

SessionManager *SessionManager::create(QQmlEngine *qmlEngine, QJSEngine *jsEngine)
{
    // 单例生命周期说明：与 DocumentManager 一致，实例无 QObject 父对象，
    // 由 C++ 侧持有直至进程退出；所有权固定为 CppOwnership（QML 引擎不负责销毁）。
    // 经 DocumentManager::create 保证管理器单例先于本单例存在（幂等：已有实例
    // 直接复用，空时创建并登记，传本工厂收到的引擎即可），消除对 QML 侧引用
    // 顺序的隐式依赖——此前依赖 "Main.qml title 绑定先创建 DocumentManager"
    // 的时序，时序颠倒时会拿到空管理器静默降级
    if (!s_instance)
        s_instance = new SessionManager(DocumentManager::create(qmlEngine, jsEngine));
    QJSEngine::setObjectOwnership(s_instance, QQmlEngine::CppOwnership);
    return s_instance;
}

void SessionManager::restoreDocuments()
{
    if (!m_manager)
        return; // 无管理器（create 早于 DocumentManager 的异常时序）：不动作也不写盘

    // 恢复期间屏蔽连锁 scheduleSave（openUrls/设光标/激活都会触发信号），完成后统一写一次
    m_restoring = true;

    const int size = m_settings->value(QStringLiteral("documents/size"), 0).toInt();
    if (size <= 0) {
        // 空会话/首次启动：不动作也不写盘，保持首启无副作用（欢迎页行为与现状一致）
        m_restoring = false;
        return;
    }

    struct SessionEntry
    {
        QUrl url;
        int cursor = 0;
        /// 会话记录的编码（手动指定或当时的自动识别结果）；缺失/非法值为空（回退自动识别）
        std::optional<Document::Encoding> forcedEncoding;
    };
    QList<SessionEntry> entries;
    for (int i = 1; i <= size; ++i) {
        const QString prefix = QStringLiteral("documents/") + QString::number(i);
        const QUrl url(m_settings->value(prefix + QStringLiteral("/url")).toString());
        const int cursor = m_settings->value(prefix + QStringLiteral("/cursor"), 0).toInt();
        if (url.isEmpty() || !url.isValid())
            continue; // 空/损坏条目：丢弃，不阻断启动
        if (url.isLocalFile() && !QFileInfo(url.toLocalFile()).isFile())
            continue; // 本地文件缺失或路径指向目录：与 LocalStorage::read 的"存在且是
                      // 常规文件"语义对齐，按坏条目静默跳过（不发 openFailed、不弹错、不打断启动）
        if (!m_manager->storageForUrl(url))
            continue; // 无可用存储（损坏/未知 scheme）：静默跳过，避免启动路径弹错

        // 编码记忆：仅接受已知值；非法/缺失（旧会话文件）静默回退自动识别，不阻断启动
        std::optional<Document::Encoding> forcedEncoding;
        const QString encoding = m_settings->value(prefix + QStringLiteral("/encoding")).toString();
        if (encoding == QStringLiteral("utf8"))
            forcedEncoding = Document::Encoding::Utf8;
        else if (encoding == QStringLiteral("gbk"))
            forcedEncoding = Document::Encoding::Gbk;

        entries.append({ url, cursor, forcedEncoding });
    }

    // 按原顺序打开（Tab 顺序 = 上次顺序；最后成功打开者被自动激活）。
    // 逐条打开：有编码记忆走 openUrlWithEncoding（按记忆编码解码），否则自动识别
    for (const SessionEntry &entry : entries) {
        if (entry.forcedEncoding)
            m_manager->openUrlWithEncoding(entry.url, *entry.forcedEncoding);
        else
            m_manager->openUrls({ entry.url });
    }

    // 逐文档恢复光标：钳到 [0, 内容长度]（文件被外部改短/记录损坏时防越界）
    for (const SessionEntry &entry : entries) {
        Document *doc = m_manager->documentForUrl(entry.url);
        if (!doc)
            continue; // openUrls 实际读取失败的条目无文档可恢复
        const int contentLength = static_cast<int>(doc->content().length());
        doc->setCursorPosition(qBound(0, entry.cursor, contentLength));
    }

    // active 命中则激活；未命中（active 文件已缺失）保持 openUrls 后的状态（最后成功打开者）
    const QUrl activeUrl(m_settings->value(QStringLiteral("documents/active")).toString());
    if (Document *active = m_manager->documentForUrl(activeUrl))
        m_manager->setActiveDocument(active);

    m_restoring = false;
    saveNow(); // 会话与实际状态对齐：剔除缺失文件、编号重排为连续
}

QVariantMap SessionManager::windowState() const
{
    if (!m_windowValid)
        return {}; // 无记录：QML 使用默认几何
    QList<QRect> screenGeometries;
    const QList<QScreen *> screens = QGuiApplication::screens();
    screenGeometries.reserve(screens.size());
    for (const QScreen *screen : screens)
        screenGeometries.append(screen->availableGeometry());
    return validateWindowState(m_windowX, m_windowY, m_windowWidth, m_windowHeight,
                               m_windowMaximized, screenGeometries);
}

void SessionManager::saveWindowState(int x, int y, int width, int height, bool maximized)
{
    m_windowX = x;
    m_windowY = y;
    m_windowWidth = width;
    m_windowHeight = height;
    m_windowMaximized = maximized;
    m_windowValid = true;
    scheduleSave();
}

bool SessionManager::wordWrap() const
{
    return m_wordWrap;
}

void SessionManager::setWordWrap(bool wrap)
{
    if (m_wordWrap == wrap)
        return; // 同值早退：不发信号、不触发写盘
    m_wordWrap = wrap;
    emit wordWrapChanged();
    scheduleSave(); // 复用既有防抖写盘（m_restoring 屏蔽、退出兜底）
}

void SessionManager::flush()
{
    if (m_debounce.isActive())
        saveNow(); // 防抖挂起：立即落盘（saveNow 内部停表）
    m_settings->sync();
}

QVariantMap SessionManager::validateWindowState(int x, int y, int width, int height, bool maximized,
                                                const QList<QRect> &screenGeometries)
{
    // 所有屏幕的最大可用宽高（多屏取各屏最大值，保证窗口能完整放进某一屏）
    int maxScreenWidth = 0;
    int maxScreenHeight = 0;
    for (const QRect &geometry : screenGeometries) {
        maxScreenWidth = qMax(maxScreenWidth, geometry.width());
        maxScreenHeight = qMax(maxScreenHeight, geometry.height());
    }

    // 下限为产品最小窗口尺寸 800×500；屏幕过小时下限优先（宁可超出小屏也不破坏最小尺寸保证）
    int clampedWidth = qMax(800, width);
    int clampedHeight = qMax(500, height);
    if (!screenGeometries.isEmpty()) {
        clampedWidth = qBound(800, clampedWidth, qMax(800, maxScreenWidth));
        clampedHeight = qBound(500, clampedHeight, qMax(500, maxScreenHeight));
    }

    // 位置校验：最终矩形须与任一屏幕相交（拔掉副屏等场景），否则由 QML 回退默认几何
    if (!screenGeometries.isEmpty()) {
        const QRect frame(x, y, clampedWidth, clampedHeight);
        bool intersectsAny = false;
        for (const QRect &geometry : screenGeometries) {
            if (frame.intersects(geometry)) {
                intersectsAny = true;
                break;
            }
        }
        if (!intersectsAny)
            return {};
    }

    return {
        { QStringLiteral("x"), x },
        { QStringLiteral("y"), y },
        { QStringLiteral("width"), clampedWidth },
        { QStringLiteral("height"), clampedHeight },
        { QStringLiteral("maximized"), maximized },
    };
}

QString SessionManager::resolvePortableIniPath(const QString &appDir)
{
    // 空 appDir 防御：QDir("") 会以进程 CWD 为基准判断，违背"appDir 下"契约，直接返回空
    if (appDir.isEmpty())
        return QString();
    // 仅判断标记文件存在（不读写内容）；appDir 不存在或标记是目录等情形均返回空
    const QFileInfo marker(QDir(appDir).filePath(QStringLiteral("qcnote.portable")));
    if (!marker.isFile())
        return QString();
    // QDir 拼接保证分隔符正确；返回绝对路径
    return QDir(appDir).absoluteFilePath(QStringLiteral("qcnote.ini"));
}

void SessionManager::scheduleSave()
{
    if (m_restoring)
        return; // 恢复期间的连锁信号不触发写盘（恢复完成由 restoreDocuments 统一写）
    m_debounce.start(); // 单触发计时器：start 具备 restart 语义，聚合高频变化
}

void SessionManager::saveNow()
{
    m_debounce.stop();

    // [documents]：先清残留键再按当前状态重写（编号重排、剔除已关闭/未命名文档）
    m_settings->remove(QStringLiteral("documents"));
    m_settings->beginGroup(QStringLiteral("documents"));
    int index = 0;
    if (m_manager) {
        for (Document *doc : m_manager->documents()) {
            const QUrl url = doc->url();
            if (url.isEmpty())
                continue; // 未命名文档（无路径）不记录
            ++index;
            const QString prefix = QString::number(index);
            m_settings->setValue(prefix + QStringLiteral("/url"), url.toString());
            m_settings->setValue(prefix + QStringLiteral("/cursor"), doc->cursorPosition());
            // 编码一律写（含 utf8）：重启后按记忆编码打开，避免自动识别误判反复发生
            m_settings->setValue(prefix + QStringLiteral("/encoding"),
                                 doc->encoding() == Document::Encoding::Gbk
                                     ? QStringLiteral("gbk")
                                     : QStringLiteral("utf8"));
        }
    }
    m_settings->setValue(QStringLiteral("size"), index);
    if (m_manager) {
        if (Document *active = m_manager->activeDocument()) {
            const QUrl activeUrl = active->url();
            if (!activeUrl.isEmpty())
                m_settings->setValue(QStringLiteral("active"), activeUrl.toString());
        }
    }
    m_settings->endGroup();

    // [window]：本实例持有有效几何才写，否则移除（防止陈旧几何残留）
    if (m_windowValid) {
        m_settings->beginGroup(QStringLiteral("window"));
        m_settings->setValue(QStringLiteral("x"), m_windowX);
        m_settings->setValue(QStringLiteral("y"), m_windowY);
        m_settings->setValue(QStringLiteral("width"), m_windowWidth);
        m_settings->setValue(QStringLiteral("height"), m_windowHeight);
        m_settings->setValue(QStringLiteral("maximized"), m_windowMaximized);
        m_settings->endGroup();
    } else {
        m_settings->remove(QStringLiteral("window"));
    }

    // [editor]：编辑器全局设置（自动换行）；键分组语义与 "documents/size" 一致
    m_settings->setValue(QStringLiteral("editor/wordWrap"), m_wordWrap);

    m_settings->sync(); // 立即落盘：崩溃最多丢最后 500ms 内的变化，文件列表不丢
}

void SessionManager::connectDocumentSignals()
{
    if (!m_manager)
        return;
    const QList<Document *> documents = m_manager->documents();
    // 收缩记录集合：离开列表的文档被对比剔除（QObject 析构已自动断连信号）
    m_connectedDocuments.intersect(QSet<Document *>(documents.cbegin(), documents.cend()));
    for (Document *doc : documents) {
        if (m_connectedDocuments.contains(doc))
            continue;
        connect(doc, &Document::urlChanged, this, &SessionManager::scheduleSave);
        connect(doc, &Document::cursorPositionChanged, this, &SessionManager::scheduleSave);
        connect(doc, &Document::encodingChanged, this, &SessionManager::scheduleSave);
        m_connectedDocuments.insert(doc);
    }
}
