// 草稿缓存 QML 单例链路回归测试（DEC-005，此前盲区补齐）：
// 《测试用例-退出免提示与草稿缓存单例修复》TC-002/TC-003 的可自动化部分，
// TC-001（QML 单例解析经 create 工厂）隐含在两者中断言——写盘/恢复本身就
// 是「QML 引擎实例」与「DraftCacheManager 工厂链实例」的跨实例交互。
//
// 根因背景：Qt 6.11 qqmlprivate.h 的 singletonConstructionMode() 对 QML
// 单例的构造选择顺序为「默认构造 > create 工厂」。DocumentManager 此前
// 构造带全默认参数（默认可构造），引擎解析单例时直接 new 出独立实例，
// 绕过 create() 工厂；而 DraftCacheManager（无默认构造，走工厂链）连接
// 到工厂实例 → 双实例分裂：文档信号到不了草稿管理器，草稿从不写盘。
// C++ 单测直接 new DocumentManager/DraftCacheManager，绕过 QML 单例解析
// 路径，故上一轮全绿却漏掉此缺陷——本测试用真实 Main.qml + 真实防抖计时
// 堵住该盲区，修复前红、修复后绿，是根因的直接回归锁。
//
// 两个场景（纯 main + 手工断言，退出码 0/1）：
// - tc002 真实防抖写盘：加载 Main.qml → 引擎拿 DocumentManager 单例 →
//   newDocument + setContent → qWait(1500)（真实 1000ms 防抖到期，禁止
//   手动 flush）→ 断言 drafts 目录出现条目且 JSON 字段正确；
// - tc003 未命名草稿恢复激活：预放置合法未命名条目 → 加载 Main.qml
//   （Component.onCompleted → restoreDrafts）→ 断言文档恢复且
//   activeDocument 指向它（DEC-002 恢复激活规则）。
//
// 隔离：会话经 QSettings::setPath 重定向到临时目录（冷启动无文档）；
// 缓存目录按本测试 exe 名派生（%LOCALAPPDATA%\test_draftsingleton\drafts，
// 独立于真实 qcnote），启动前预清理、结束清理（参照 tst_dialogstyle 模式）。
//
// GUI 红线：main 中强制 QT_QPA_PLATFORM=offscreen，全程无可见窗口。

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QTest>
#include <QVariant>

#include "document/document.h"
#include "document/documentmanager.h"
#include "document/sessionmanager.h"

// ==================== 断言与目录辅助 ====================

namespace {

/// 失败计数（main 返回非 0 使 ctest 判红）
int g_failures = 0;

/// 手工断言：通过/失败均打印（[draftsingleton] 前缀），失败累计
void check(bool ok, const QString &what)
{
    if (ok) {
        qInfo().noquote() << "[draftsingleton] PASS:" << what;
    } else {
        ++g_failures;
        qWarning().noquote() << "[draftsingleton] FAIL:" << what;
    }
}

/// 草稿缓存目录：与 DraftCacheManager 构造完全一致的解析逻辑
/// （便携标记 → exe 旁 drafts/；否则 AppLocalDataLocation/drafts，按本
/// 测试 exe 名派生，独立于真实 qcnote 用户数据）
QString draftsDir()
{
    const QString portableIni =
            SessionManager::resolvePortableIniPath(QCoreApplication::applicationDirPath());
    const QString base = !portableIni.isEmpty()
            ? QFileInfo(portableIni).absolutePath()
            : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir(base).filePath(QStringLiteral("drafts"));
}

/// 缓存目录中的条目文件名列表（目录不存在视为空）
QStringList draftEntries()
{
    return QDir(draftsDir()).entryList({ QStringLiteral("*.json") }, QDir::Files);
}

/// 关闭全部文档（documentClosed 时仍 modified → DraftCacheManager 删条目），
/// 场景收尾用：既清空文档列表，也顺带清掉本场景写入的草稿条目
void closeAllDocuments(DocumentManager *dm)
{
    const QList<Document *> docs = dm->documents();
    for (Document *d : docs)
        dm->closeDocument(d);
}

} // namespace

int main(int argc, char *argv[])
{
    // GUI 红线：自动化验证一律 offscreen，无可见窗口；
    // 隔离首启右键菜单提示（避免真实注册表读写与首启弹框干扰）
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QCNOTE_TEST_SHELL_NOOP", "1");
    // offscreen 平台不带字体目录时指定系统字体，保证文本渲染路径正常
    static const char kWinFonts[] = "C:/Windows/Fonts";
    if (QDir(QString::fromLatin1(kWinFonts)).exists())
        qputenv("QT_QPA_FONTDIR", kWinFonts);

    QGuiApplication app(argc, argv);
    // 窗口关闭/销毁不得触发自动退出（本测试不进 exec 事件循环）
    app.setQuitOnLastWindowClosed(false);

    // 会话重定向到临时目录（必须在任何 QSettings 实例创建前 setPath）：
    // 两个场景都需要「会话无文档」的冷启动状态，且不污染真实用户配置
    QTemporaryDir sessionRedirectDir;
    check(sessionRedirectDir.isValid(), "session redirect dir valid");
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, sessionRedirectDir.path());

    // 与主程序一致的控件样式（Main.qml 使用 Material 附加属性）
    QQuickStyle::setStyle(QStringLiteral("Material"));

    // 启动预清理：上一轮残留条目会让本轮恢复出额外文档（参照 tst_dialogstyle）
    QDir(draftsDir()).removeRecursively();

    // ==================== tc002：真实防抖链路写盘（TC-001/TC-002） ====================
    // 修复前红：引擎默认构造实例与 DraftCacheManager 工厂链实例分裂，
    // contentChanged 到不了草稿管理器 → drafts 目录不创建、条目不落盘
    {
        QQmlApplicationEngine engine;
        QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                         [] { QCoreApplication::exit(-2); }, Qt::QueuedConnection);
        engine.loadFromModule(QStringLiteral("qcnote"), QStringLiteral("Main"));
        check(!engine.rootObjects().isEmpty(), "tc002: Main.qml loaded");

        // 经引擎拿 DocumentManager 单例（QML 界面实际使用的实例）
        DocumentManager *dm =
                engine.singletonInstance<DocumentManager *>(u"qcnote", u"DocumentManager");
        check(dm != nullptr, "tc002: DocumentManager singleton resolved");

        // TC-001 根因探针：引擎单例必须与工厂链登记的同一实例同源。注意
        // （修复前实测）：引擎直构实例的构造会自登记 s_instance，故此指针
        // 比对在修复前也可能“空转通过”；真正的跨实例验证是下方写盘行为
        // 本身（DraftCacheManager 连的若是另一实例，条目永远不落盘）
        check(DocumentManager::instance() == dm,
              QStringLiteral("tc002/TC-001: engine singleton == DocumentManager::instance() "
                             "(engine=%1 instance=%2)")
                      .arg(QString::fromLatin1(QByteArray::number(reinterpret_cast<quintptr>(dm),
                                                                  16)),
                           QString::fromLatin1(QByteArray::number(
                                   reinterpret_cast<quintptr>(DocumentManager::instance()), 16))));

        dm->newDocument();
        Document *doc = dm->activeDocument();
        check(doc != nullptr, "tc002: newDocument created document");
        if (doc) {
            const QString uid = doc->uid();
            doc->setContent(QStringLiteral("singleton-regression"));

            // 真实防抖：等待 1500ms > 1000ms 防抖间隔，让计时器自然到期
            // （禁止手动 flush——那会绕过「信号是否到达草稿管理器」这一被测链路）
            QTest::qWait(1500);

            const QStringList entries = draftEntries();
            check(entries.size() == 1,
                  QStringLiteral("tc002: exactly one draft entry after debounce (got %1 in %2)")
                          .arg(QString::number(entries.size()), draftsDir()));

            // 条目字段：content/uid/name/url（未命名）。读取后立即 close：
            // Windows 下句柄未释放会占用条目文件，后续清理（removeEntry →
            // QFile::remove）会失败
            {
                QFile file(QDir(draftsDir()).filePath(uid + QStringLiteral(".json")));
                const bool opened = file.open(QIODevice::ReadOnly);
                check(opened, "tc002: draft entry file openable");
                if (opened) {
                    const QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
                    check(obj.value(QStringLiteral("content")).toString()
                                  == QStringLiteral("singleton-regression"),
                          "tc002: entry content matches");
                    check(obj.value(QStringLiteral("uid")).toString() == uid,
                          "tc002: entry uid matches");
                    check(obj.value(QStringLiteral("name")).toString()
                                  == QStringLiteral("未命名-1"),
                          "tc002: entry name is 未命名-1");
                    check(obj.value(QStringLiteral("url")).toString().isEmpty(),
                          "tc002: entry url empty (untitled)");
                    check(obj.value(QStringLiteral("version")).toInt() == 1,
                          "tc002: entry version is 1");
                    file.close();
                }
            }

            // 收尾：关闭文档（仍脏 → 条目清理），随后销毁引擎与窗口
            closeAllDocuments(dm);
            check(draftEntries().isEmpty(), "tc002: cleanup removed draft entry");
        }
    }

    // ==================== tc003：未命名草稿恢复后激活（TC-003） ====================
    // 新引擎 = 新「进程」语义（单例经 create 工厂跨引擎复用同一 C++ 实例）；
    // 预放置条目必须在 Main.qml 实例化之前落盘（restoreDrafts 在
    // Component.onCompleted 中执行）
    QDir(draftsDir()).removeRecursively();
    {
        check(QDir().mkpath(draftsDir()), "tc003: drafts dir created for pre-placed entry");

        // 合法未命名条目：version=1、url 空串、name=未命名-9、光标 0、
        // baselineHash 任意非空、savedAt 当前秒
        const QString uid = QStringLiteral("11111111-2222-3333-4444-555555555555");
        QJsonObject entry;
        entry.insert(QStringLiteral("version"), 1);
        entry.insert(QStringLiteral("uid"), uid);
        entry.insert(QStringLiteral("url"), QString());
        entry.insert(QStringLiteral("name"), QStringLiteral("未命名-9"));
        entry.insert(QStringLiteral("content"), QStringLiteral("singleton-restore-content"));
        entry.insert(QStringLiteral("cursor"), 0);
        entry.insert(QStringLiteral("lineEnding"), QStringLiteral("lf"));
        entry.insert(QStringLiteral("encoding"), QStringLiteral("utf8"));
        entry.insert(QStringLiteral("hasBom"), false);
        entry.insert(QStringLiteral("baselineHash"), QStringLiteral("any-nonempty-hash"));
        entry.insert(QStringLiteral("savedAt"),
                     static_cast<qint64>(QDateTime::currentSecsSinceEpoch()));
        QFile entryFile(QDir(draftsDir()).filePath(uid + QStringLiteral(".json")));
        const bool written = entryFile.open(QIODevice::WriteOnly | QIODevice::Truncate)
                && entryFile.write(QJsonDocument(entry).toJson()) > 0;
        entryFile.close();
        check(written, "tc003: untitled draft entry pre-placed");

        QQmlApplicationEngine engine;
        QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                         [] { QCoreApplication::exit(-2); }, Qt::QueuedConnection);
        engine.loadFromModule(QStringLiteral("qcnote"), QStringLiteral("Main"));
        check(!engine.rootObjects().isEmpty(), "tc003: Main.qml loaded");

        // 单例跨引擎复用同一 C++ 实例（create 工厂语义），文档应恢复于其中
        DocumentManager *dm =
                engine.singletonInstance<DocumentManager *>(u"qcnote", u"DocumentManager");
        check(dm != nullptr, "tc003: DocumentManager singleton resolved");
        if (dm) {
            check(dm->documents().size() == 1,
                  QStringLiteral("tc003: exactly one restored document (got %1)")
                          .arg(QString::number(dm->documents().size())));
            if (dm->documents().size() == 1) {
                Document *doc = dm->documents().constFirst();
                check(doc->displayName() == QStringLiteral("未命名-9"),
                      "tc003: displayName is 未命名-9");
                check(doc->content() == QStringLiteral("singleton-restore-content"),
                      "tc003: content matches cache");
                check(doc->modified() == true, "tc003: restored as modified (unsaved)");
                // DEC-002 恢复激活规则：会话无活动文档时激活首个恢复文档
                check(dm->activeDocument() == doc,
                      "tc003: activeDocument points to restored document");
            }

            // 收尾：关闭文档（删条目），销毁引擎
            closeAllDocuments(dm);
        }
    }

    // 结束清理：删除本轮测试写入的草稿目录（含目录本身），
    // 不在 %LOCALAPPDATA% 留测试产物
    QDir(draftsDir()).removeRecursively();

    qInfo().noquote() << "[draftsingleton] total failures:" << g_failures;
    return g_failures == 0 ? 0 : 1;
}
