// 端到端草稿链路驱动 harness（《测试用例-退出免提示与草稿缓存单例修复》
// TC-010/TC-011/TC-012 的进程侧执行器，不注册 ctest，由编排脚本
// tests/verify-hotexit-draft-e2e.ps1 启动）：
// 加载真实 Main.qml（与 qcnote.exe 完全同一 QML 启动链路），经 QML 动作
// 入口与活动编辑器等价驱动用户操作，弥补真实进程无法合成键盘输入的盲区。
//
// 四种模式（第一参数；输入内容经环境变量 QCNOTE_E2E_CONTENT 传入——
// Windows 环境变量全程 Unicode，避免命令行参数本地编码转换）：
// - hotexit：新建标签输入 → 等真实防抖（1000ms）到期 → doQuit()（QML
//   退出入口，与 Ctrl+Q/菜单退出同链路）→ onClosing 放行 → 最后窗口
//   关闭 → app.exec() 自然返回（aboutToQuit flush 兜底）——正常热退出；
// - kill：新建标签输入 → 打印 WROTE 标记 → 挂事件循环等待外部
//   Stop-Process -Force 强杀（任务管理器等价；防抖在输入后 1s 自然到期）；
// - blurkill：输入后 200ms（防抖远未到期）经 setApplicationState(Inactive)
//   发真实 applicationStateChanged 信号（失焦即写链路）→ 打印 BLURRED
//   标记 → 挂事件循环等待外部强杀——验证失焦 flush 抢在防抖之前落盘；
// - restore：加载后（Component.onCompleted → restoreDrafts 已执行）直接
//   断言 DocumentManager 状态：恰一文档、未命名-1、内容匹配、恢复为
//   未保存（modified）、activeDocument 指向它（DEC-002 恢复激活）。
//
// 关闭现场还原（TC-011）扩展两模式（文件经环境变量 QCNOTE_E2E_FILE_A /
// QCNOTE_E2E_FILE_B 传入）：
// - mixexit：构造 [文件A, 未命名1(键入内容), 文件B] 且活动=未命名1 的关闭
//   现场 → 等防抖落盘 → doQuit() 热退出（会话记录 uid 条目 + activeUid）；
// - restoremix：加载后直接断言恢复现场：3 文档顺序 [A, 未命名1(未保存), B]
//   且 activeDocument 指向未命名1（DEC-002 保序 + DEC-003 一级激活）。
//
// 关闭现场还原验收抽查（本轮测试扩展，同一套环境变量；U2 内容经
// QCNOTE_E2E_CONTENT2 传入）：
// - mixexit2：构造 [未命名1(键入), 文件A, 未命名2(键入)] 且活动=未命名2 的
//   关闭现场（验收标准 2：未命名分居头尾）→ 热退出；
// - restoremix2：加载后直接断言：3 文档顺序 [未命名1, A, 未命名2] 且
//   activeDocument 指向未命名2；
// - mixfile：构造 [文件A, 未命名1(键入), 文件B] 且活动=文件B（验收标准 3：
//   活动为文件标签现状不变）→ 热退出（会话记 active=B url，非 activeUid）；
// - mixempty：构造 [文件A, 未命名1(无内容), 文件B] 且活动=文件B（验收标准
//   4：无内容未命名不恢复）→ 热退出（会话记录空 uid 条目，无草稿）。
//   mixfile/mixempty 的恢复侧由编排脚本以真实 qcnote.exe 文件级断言验证。
//
// 输入驱动（真实 QML 输入链路，不用 Document::setContent 直写）：
//   1) QMetaObject 调 Main.qml doNewDocument()（新建标签页的 QML 动作入口）；
//   2) 定位活动编辑器 TextArea（QML 短名匹配，Qt 6 中 TextArea 是 Controls
//      的 QML 合成类型）调 append()——与用户在文末键入等价：TextArea.text
//      变化 → EditorArea onTextChanged → doc.content 回写 →
//      Document::contentChanged → DraftCacheManager 防抖挂起（qputenv 前置
//      环境与 test_draftsingleton 一致）。
//
// 输出协议（stdout，行缓冲 + fflush，供外部脚本即时轮询）：
//   [e2e] WROTE uid=<uid> name=<name>      —— 输入完成（content 见环境变量）
//   [e2e] WROTE uid1=<uid1> uid2=<uid2>    —— mixexit2 两个未命名输入完成
//   [e2e] BLURRED                          —— 失焦即写已触发（blurkill）
//   [e2e] EXITED                           —— hotexit 正常退出路径完成
//   [e2e] RESTORE PASS / FAIL              —— restore 断言汇总
//   [e2e] RESTOREMIX-CHECK PASS/FAIL <what> / RESTOREMIX PASS / FAIL
//                                          —— restoremix 断言（TC-011）
//   [e2e] RESTOREMIX2-CHECK PASS/FAIL <what> / RESTOREMIX2 PASS / FAIL
//                                          —— restoremix2 断言（验收标准 2）
//   [e2e] ERROR <what>                     —— 驱动自身失败
// 退出码：0 成功；非 0 失败（RESTORE/RESTOREMIX FAIL 返回 1）。
//
// 缓存目录：随便携标记与 qcnote.exe 同目录解析（本 exe 输出到 build 根，
// 由编排脚本放置 build\qcnote.portable → 缓存 build\drafts），不污染用户数据。
// GUI 红线：内部强制 QT_QPA_PLATFORM=offscreen，全程无可见窗口。

#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QMetaObject>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QString>
#include <QStringLiteral>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include <cstdio>

#include "document/document.h"
#include "document/documentmanager.h"

namespace {

/// 标记行输出（stdout 行缓冲 + 立即 flush，供外部脚本轮询）
void emitMarker(const QByteArray &line)
{
    std::fputs(line.constData(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

/// QML 类型短名精确匹配（与 tst_windowclose 的 qmlTypeNameIs 同模式）：
/// Qt 6 的 TextArea 是 QtQuick.Controls 的 QML 合成类型（TextArea.qml 继承
/// TextEdit），实例 metaObject 类名形如 "TextArea_QMLTYPE_13"，取首个
/// 下划线前段与目标比较
bool qmlTypeNameIs(const QObject *obj, const char *name)
{
    const QString cn = QString::fromLatin1(obj->metaObject()->className());
    const int cut = cn.indexOf(u'_');
    return (cut > 0 ? cn.left(cut) : cn) == QLatin1String(name);
}

/// 可视树深搜：按 QML 类型短名定位（Main.qml 可视树内的编辑器 TextArea
/// 唯一——单文档场景 EditorArea Repeater 只实例化一个 delegate）
QQuickItem *findByQmlType(QQuickItem *root, const char *name)
{
    if (!root)
        return nullptr;
    if (qmlTypeNameIs(root, name))
        return root;
    const auto kids = root->childItems();
    for (QQuickItem *kid : kids) {
        if (QQuickItem *found = findByQmlType(kid, name))
            return found;
    }
    return nullptr;
}

} // namespace

int main(int argc, char *argv[])
{
    // GUI 红线 + 首启提示隔离 + offscreen 字体目录（与 test_draftsingleton 一致）
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QCNOTE_TEST_SHELL_NOOP", "1");
    static const char kWinFonts[] = "C:/Windows/Fonts";
    if (QDir(QString::fromLatin1(kWinFonts)).exists())
        qputenv("QT_QPA_FONTDIR", kWinFonts);

    if (argc < 2) {
        std::fputs("用法: e2e_draft_driver <hotexit|kill|blurkill|restore|mixexit|restoremix|mixexit2|restoremix2|mixfile|mixempty>\n"
                   "输入内容经环境变量 QCNOTE_E2E_CONTENT 传入\n"
                   "mixexit*/restoremix* 另需 QCNOTE_E2E_FILE_A / QCNOTE_E2E_FILE_B\n"
                   "mixexit2/restoremix2 另需 QCNOTE_E2E_CONTENT2（第二个未命名的键入内容）\n",
                   stderr);
        return 64;
    }
    const QString mode = QString::fromLatin1(argv[1]);
    // 环境变量全程 Unicode，规避命令行参数本地编码歧义
    const QString content = qEnvironmentVariable("QCNOTE_E2E_CONTENT");
    if (content.isEmpty()) {
        std::fputs("环境变量 QCNOTE_E2E_CONTENT 未设置\n", stderr);
        return 64;
    }

    QGuiApplication app(argc, argv);
    // quitOnLastWindowClosed 保持默认 true：hotexit 的 close() 放行后最后
    // 窗口关闭 → exec() 返回，与真实 qcnote.exe（main 的 app.exec()）同路径
    QQuickStyle::setStyle(QStringLiteral("Material"));

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(-2); }, Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("qcnote"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        emitMarker("[e2e] ERROR main-load-failed");
        return -2;
    }
    QQuickWindow *win = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (!win) {
        emitMarker("[e2e] ERROR root-not-window");
        return -3;
    }

    // ==================== restore：恢复状态直接断言（restoreDrafts 已跑） ====================
    if (mode == QLatin1String("restore")) {
        DocumentManager *dm =
                engine.singletonInstance<DocumentManager *>(u"qcnote", u"DocumentManager");
        int failures = 0;
        auto check = [&failures](bool ok, const char *what) {
            emitMarker(QByteArray("[e2e] RESTORE-CHECK ") + (ok ? "PASS " : "FAIL ") + what);
            if (!ok)
                ++failures;
        };
        check(dm != nullptr, "DocumentManager singleton resolved");
        if (dm) {
            check(dm->documents().size() == 1, "exactly one restored document");
            if (dm->documents().size() == 1) {
                Document *doc = dm->documents().constFirst();
                check(doc->displayName() == QStringLiteral("未命名-1"),
                      "displayName is 未命名-1");
                check(doc->content() == content, "content matches typed input");
                check(doc->modified() == true, "restored as modified (unsaved)");
                check(dm->activeDocument() == doc,
                      "activeDocument points to restored document");
            }
        }
        emitMarker(failures == 0 ? "[e2e] RESTORE PASS" : "[e2e] RESTORE FAIL");
        // 不进事件循环直接返回：恢复文档的防抖挂起不落盘、单例不析构，
        // drafts 目录保持空（条目已被 restoreDrafts 消费），场景间无残留
        return failures == 0 ? 0 : 1;
    }

    // ==================== mixexit：混排关闭现场 → 热退出（TC-011 写入侧） ====================
    // 构造 [文件A, 未命名1(键入内容), 文件B] 且活动=未命名1：文件打开走
    // DocumentManager 生产链路 openUrls（QML 文件入口为原生 FileDialog 无法
    // 自动化；SingleInstance 启动参数打开内部同样走 openUrls），未命名键入
    // 与退出走既有真实 QML 链路（doNewDocument + TextArea append + doQuit）
    if (mode == QLatin1String("mixexit")) {
        const QString fileA = qEnvironmentVariable("QCNOTE_E2E_FILE_A");
        const QString fileB = qEnvironmentVariable("QCNOTE_E2E_FILE_B");
        if (fileA.isEmpty() || fileB.isEmpty()) {
            std::fputs("环境变量 QCNOTE_E2E_FILE_A/QCNOTE_E2E_FILE_B 未设置\n", stderr);
            return 64;
        }
        DocumentManager *dm =
                engine.singletonInstance<DocumentManager *>(u"qcnote", u"DocumentManager");
        if (!dm) {
            emitMarker("[e2e] ERROR dm-resolve-failed");
            return -20;
        }
        // t=300ms 构造现场：打开 A → 新建未命名并真实键入 → 打开 B → 活动切回
        // 未命名（标签序 [A, 未命名1, B]、活动=未命名1）
        QTimer::singleShot(300, &app, [&]() {
            dm->openUrls({ QUrl::fromLocalFile(fileA) });
            if (!QMetaObject::invokeMethod(win, "doNewDocument")) {
                emitMarker("[e2e] ERROR doNewDocument-invoke-failed");
                QCoreApplication::exit(-10);
                return;
            }
            Document *doc = dm->activeDocument();
            // 多文档场景下可视树中有多个 TextArea（每文档一个、仅活动项可见），
            // 盲搜第一个会命中非活动编辑器——改读 EditorArea.activeEditor 属性
            // （由活动 delegate 的 onIsActiveChanged 维护）精确定位活动编辑器
            QQuickItem *editorArea = findByQmlType(win->contentItem(), "EditorArea");
            QQuickItem *editor =
                    editorArea ? editorArea->property("activeEditor").value<QQuickItem *>() : nullptr;
            if (!doc || !editor) {
                emitMarker("[e2e] ERROR mix-scene-setup-failed");
                QCoreApplication::exit(-12);
                return;
            }
            if (!QMetaObject::invokeMethod(editor, "append", Q_ARG(QString, content))) {
                emitMarker("[e2e] ERROR append-invoke-failed");
                QCoreApplication::exit(-13);
                return;
            }
            if (doc->content() != content) {
                emitMarker("[e2e] ERROR content-writeback-mismatch");
                QCoreApplication::exit(-14);
                return;
            }
            dm->openUrls({ QUrl::fromLocalFile(fileB) }); // B 追加在未命名之后
            dm->setActiveDocument(doc);                    // 活动标签 = 未命名（关闭现场）
            emitMarker("[e2e] WROTE uid=" + doc->uid().toUtf8()
                       + " name=" + doc->displayName().toUtf8());
        });
        // t=3200ms（键入后 ≥2.5s：草稿防抖 1000ms 与会话防抖 500ms 均已自然
        // 到期落盘）经 QML 退出入口热退出（与 hotexit 同路径；aboutToQuit
        // flush 再兜底一次）
        QTimer::singleShot(3200, &app, [&]() {
            if (!QMetaObject::invokeMethod(win, "doQuit")) {
                emitMarker("[e2e] ERROR doQuit-invoke-failed");
                QCoreApplication::exit(-15);
            }
        });
        const int rc = app.exec();
        emitMarker("[e2e] EXITED");
        return rc;
    }

    // ==================== mixexit2：双未命名夹文件现场 → 热退出（验收标准 2 写入侧） ====================
    // 构造 [未命名1(键入), 文件A, 未命名2(键入)] 且活动=未命名2：未命名分居
    // 头尾、文件居中。U2 内容经环境变量 QCNOTE_E2E_CONTENT2 传入；WROTE 标记
    // 同时携带两个未命名的 uid，供脚本做会话 [uid1, A, uid2] + activeUid 互斥
    // 断言
    if (mode == QLatin1String("mixexit2")) {
        const QString fileA = qEnvironmentVariable("QCNOTE_E2E_FILE_A");
        const QString content2 = qEnvironmentVariable("QCNOTE_E2E_CONTENT2");
        if (fileA.isEmpty() || content2.isEmpty()) {
            std::fputs("环境变量 QCNOTE_E2E_FILE_A/QCNOTE_E2E_CONTENT2 未设置\n", stderr);
            return 64;
        }
        DocumentManager *dm =
                engine.singletonInstance<DocumentManager *>(u"qcnote", u"DocumentManager");
        if (!dm) {
            emitMarker("[e2e] ERROR dm-resolve-failed");
            return -20;
        }
        // t=300ms 构造现场：新建 U1 键入 → 打开 A（居中）→ 新建 U2 键入 →
        // 活动切 U2（标签序 [U1, A, U2]、活动=未命名2）
        QTimer::singleShot(300, &app, [&]() {
            if (!QMetaObject::invokeMethod(win, "doNewDocument")) {
                emitMarker("[e2e] ERROR doNewDocument-invoke-failed");
                QCoreApplication::exit(-10);
                return;
            }
            Document *docU1 = dm->activeDocument();
            QQuickItem *editorArea = findByQmlType(win->contentItem(), "EditorArea");
            QQuickItem *editor =
                    editorArea ? editorArea->property("activeEditor").value<QQuickItem *>() : nullptr;
            if (!docU1 || !editor) {
                emitMarker("[e2e] ERROR mix2-scene-setup-failed");
                QCoreApplication::exit(-22);
                return;
            }
            if (!QMetaObject::invokeMethod(editor, "append", Q_ARG(QString, content))) {
                emitMarker("[e2e] ERROR append-invoke-failed");
                QCoreApplication::exit(-13);
                return;
            }
            if (docU1->content() != content) {
                emitMarker("[e2e] ERROR content-writeback-mismatch");
                QCoreApplication::exit(-14);
                return;
            }
            dm->openUrls({ QUrl::fromLocalFile(fileA) }); // A 居中（激活 A）
            if (!QMetaObject::invokeMethod(win, "doNewDocument")) {
                emitMarker("[e2e] ERROR doNewDocument-invoke-failed");
                QCoreApplication::exit(-10);
                return;
            }
            Document *docU2 = dm->activeDocument();
            editor = editorArea ? editorArea->property("activeEditor").value<QQuickItem *>()
                                : nullptr; // 活动编辑器已切换到 U2
            if (!docU2 || !editor) {
                emitMarker("[e2e] ERROR mix2-scene-setup-failed");
                QCoreApplication::exit(-23);
                return;
            }
            if (!QMetaObject::invokeMethod(editor, "append", Q_ARG(QString, content2))) {
                emitMarker("[e2e] ERROR append-invoke-failed");
                QCoreApplication::exit(-13);
                return;
            }
            if (docU2->content() != content2) {
                emitMarker("[e2e] ERROR content-writeback-mismatch");
                QCoreApplication::exit(-14);
                return;
            }
            dm->setActiveDocument(docU2); // 活动标签 = 未命名2（关闭现场）
            emitMarker("[e2e] WROTE uid1=" + docU1->uid().toUtf8()
                       + " uid2=" + docU2->uid().toUtf8());
        });
        // t=3200ms（两次键入后 ≥2.5s：防抖均已自然到期落盘）热退出
        QTimer::singleShot(3200, &app, [&]() {
            if (!QMetaObject::invokeMethod(win, "doQuit")) {
                emitMarker("[e2e] ERROR doQuit-invoke-failed");
                QCoreApplication::exit(-15);
            }
        });
        const int rc = app.exec();
        emitMarker("[e2e] EXITED");
        return rc;
    }

    // ==================== mixfile：文件活动现场 → 热退出（验收标准 3 写入侧） ====================
    // 构造 [文件A, 未命名1(键入), 文件B] 且活动=文件B：与 mixexit 唯一差异是
    // 关闭现场的活动标签为文件（openUrls(B) 自动激活，不切回未命名）——会话
    // 应按现状记 active=B 的 URL（非 activeUid），重启后行为与现状一致
    if (mode == QLatin1String("mixfile")) {
        const QString fileA = qEnvironmentVariable("QCNOTE_E2E_FILE_A");
        const QString fileB = qEnvironmentVariable("QCNOTE_E2E_FILE_B");
        if (fileA.isEmpty() || fileB.isEmpty()) {
            std::fputs("环境变量 QCNOTE_E2E_FILE_A/QCNOTE_E2E_FILE_B 未设置\n", stderr);
            return 64;
        }
        DocumentManager *dm =
                engine.singletonInstance<DocumentManager *>(u"qcnote", u"DocumentManager");
        if (!dm) {
            emitMarker("[e2e] ERROR dm-resolve-failed");
            return -20;
        }
        QTimer::singleShot(300, &app, [&]() {
            dm->openUrls({ QUrl::fromLocalFile(fileA) });
            if (!QMetaObject::invokeMethod(win, "doNewDocument")) {
                emitMarker("[e2e] ERROR doNewDocument-invoke-failed");
                QCoreApplication::exit(-10);
                return;
            }
            Document *doc = dm->activeDocument();
            QQuickItem *editorArea = findByQmlType(win->contentItem(), "EditorArea");
            QQuickItem *editor =
                    editorArea ? editorArea->property("activeEditor").value<QQuickItem *>() : nullptr;
            if (!doc || !editor) {
                emitMarker("[e2e] ERROR mix-scene-setup-failed");
                QCoreApplication::exit(-12);
                return;
            }
            if (!QMetaObject::invokeMethod(editor, "append", Q_ARG(QString, content))) {
                emitMarker("[e2e] ERROR append-invoke-failed");
                QCoreApplication::exit(-13);
                return;
            }
            if (doc->content() != content) {
                emitMarker("[e2e] ERROR content-writeback-mismatch");
                QCoreApplication::exit(-14);
                return;
            }
            dm->openUrls({ QUrl::fromLocalFile(fileB) }); // B 追加且自动激活（活动=文件B）
            emitMarker("[e2e] WROTE uid=" + doc->uid().toUtf8()
                       + " name=" + doc->displayName().toUtf8());
        });
        QTimer::singleShot(3200, &app, [&]() {
            if (!QMetaObject::invokeMethod(win, "doQuit")) {
                emitMarker("[e2e] ERROR doQuit-invoke-failed");
                QCoreApplication::exit(-15);
            }
        });
        const int rc = app.exec();
        emitMarker("[e2e] EXITED");
        return rc;
    }

    // ==================== mixempty：空未命名现场 → 热退出（验收标准 4 写入侧） ====================
    // 构造 [文件A, 未命名1(无内容), 文件B] 且活动=文件B：未命名标签无键入、
    // 无草稿条目，但会话仍记录其 uid 位置（完整标签序列）。重启后该空位自然
    // 消除（不恢复），其余标签顺序不变——恢复侧由脚本文件级断言验证
    if (mode == QLatin1String("mixempty")) {
        const QString fileA = qEnvironmentVariable("QCNOTE_E2E_FILE_A");
        const QString fileB = qEnvironmentVariable("QCNOTE_E2E_FILE_B");
        if (fileA.isEmpty() || fileB.isEmpty()) {
            std::fputs("环境变量 QCNOTE_E2E_FILE_A/QCNOTE_E2E_FILE_B 未设置\n", stderr);
            return 64;
        }
        DocumentManager *dm =
                engine.singletonInstance<DocumentManager *>(u"qcnote", u"DocumentManager");
        if (!dm) {
            emitMarker("[e2e] ERROR dm-resolve-failed");
            return -20;
        }
        QTimer::singleShot(300, &app, [&]() {
            dm->openUrls({ QUrl::fromLocalFile(fileA) });
            if (!QMetaObject::invokeMethod(win, "doNewDocument")) {
                emitMarker("[e2e] ERROR doNewDocument-invoke-failed");
                QCoreApplication::exit(-10);
                return;
            }
            Document *doc = dm->activeDocument(); // 空未命名（不键入 → 无草稿）
            if (!doc) {
                emitMarker("[e2e] ERROR mix-scene-setup-failed");
                QCoreApplication::exit(-12);
                return;
            }
            dm->openUrls({ QUrl::fromLocalFile(fileB) }); // B 自动激活（活动=文件B）
            emitMarker("[e2e] WROTE uid=" + doc->uid().toUtf8()
                       + " name=" + doc->displayName().toUtf8());
        });
        QTimer::singleShot(3200, &app, [&]() {
            if (!QMetaObject::invokeMethod(win, "doQuit")) {
                emitMarker("[e2e] ERROR doQuit-invoke-failed");
                QCoreApplication::exit(-15);
            }
        });
        const int rc = app.exec();
        emitMarker("[e2e] EXITED");
        return rc;
    }

    // ==================== restoremix：混排恢复现场直断言（TC-011 恢复侧） ====================
    // 加载即断言（Component.onCompleted → restoreDocuments + restoreDrafts 已
    // 执行）：3 文档顺序 [文件A, 未命名1(未保存), 文件B] 且活动=未命名1
    if (mode == QLatin1String("restoremix")) {
        const QString fileA = qEnvironmentVariable("QCNOTE_E2E_FILE_A");
        const QString fileB = qEnvironmentVariable("QCNOTE_E2E_FILE_B");
        if (fileA.isEmpty() || fileB.isEmpty()) {
            std::fputs("环境变量 QCNOTE_E2E_FILE_A/QCNOTE_E2E_FILE_B 未设置\n", stderr);
            return 64;
        }
        DocumentManager *dm =
                engine.singletonInstance<DocumentManager *>(u"qcnote", u"DocumentManager");
        int failures = 0;
        auto check = [&failures](bool ok, const char *what) {
            emitMarker(QByteArray("[e2e] RESTOREMIX-CHECK ") + (ok ? "PASS " : "FAIL ") + what);
            if (!ok)
                ++failures;
        };
        check(dm != nullptr, "DocumentManager singleton resolved");
        if (dm) {
            check(dm->documents().size() == 3, "three documents restored in tab order");
            if (dm->documents().size() == 3) {
                Document *docA = dm->documents().at(0);
                Document *docU = dm->documents().at(1);
                Document *docB = dm->documents().at(2);
                check(docA->url() == QUrl::fromLocalFile(fileA), "tab 0 is file A");
                check(docU->url() == QUrl(), "tab 1 is unnamed (no url)");
                check(docU->displayName() == QStringLiteral("未命名-1"),
                      "tab 1 displayName is 未命名-1");
                check(docU->content() == content, "tab 1 content matches typed input");
                check(docU->modified() == true, "tab 1 restored as modified (unsaved)");
                check(docB->url() == QUrl::fromLocalFile(fileB), "tab 2 is file B");
                check(dm->activeDocument() == docU,
                      "active tab is the restored unnamed document (DEC-003)");
            }
        }
        emitMarker(failures == 0 ? "[e2e] RESTOREMIX PASS" : "[e2e] RESTOREMIX FAIL");
        // 不进事件循环直接返回：恢复文档的防抖挂起不落盘（条目已被
        // restoreDrafts 消费），场景间无残留（同 restore 模式）
        return failures == 0 ? 0 : 1;
    }

    // ==================== restoremix2：双未命名恢复现场直断言（验收标准 2 恢复侧） ====================
    // 加载即断言：3 文档顺序 [未命名1(未保存), 文件A, 未命名2(未保存)] 且
    // 活动=未命名2（头/尾按位插入 + 一级激活命中尾部未命名）。U1 内容比对
    // QCNOTE_E2E_CONTENT、U2 内容比对 QCNOTE_E2E_CONTENT2
    if (mode == QLatin1String("restoremix2")) {
        const QString fileA = qEnvironmentVariable("QCNOTE_E2E_FILE_A");
        const QString content2 = qEnvironmentVariable("QCNOTE_E2E_CONTENT2");
        if (fileA.isEmpty() || content2.isEmpty()) {
            std::fputs("环境变量 QCNOTE_E2E_FILE_A/QCNOTE_E2E_CONTENT2 未设置\n", stderr);
            return 64;
        }
        DocumentManager *dm =
                engine.singletonInstance<DocumentManager *>(u"qcnote", u"DocumentManager");
        int failures = 0;
        auto check = [&failures](bool ok, const char *what) {
            emitMarker(QByteArray("[e2e] RESTOREMIX2-CHECK ") + (ok ? "PASS " : "FAIL ") + what);
            if (!ok)
                ++failures;
        };
        check(dm != nullptr, "DocumentManager singleton resolved");
        if (dm) {
            check(dm->documents().size() == 3, "three documents restored in tab order");
            if (dm->documents().size() == 3) {
                Document *docU1 = dm->documents().at(0);
                Document *docA = dm->documents().at(1);
                Document *docU2 = dm->documents().at(2);
                check(docU1->url() == QUrl(), "tab 0 is unnamed (no url)");
                check(docU1->displayName() == QStringLiteral("未命名-1"),
                      "tab 0 displayName is 未命名-1");
                check(docU1->content() == content, "tab 0 content matches typed input");
                check(docU1->modified() == true, "tab 0 restored as modified (unsaved)");
                check(docA->url() == QUrl::fromLocalFile(fileA), "tab 1 is file A");
                check(docU2->url() == QUrl(), "tab 2 is unnamed (no url)");
                check(docU2->displayName() == QStringLiteral("未命名-2"),
                      "tab 2 displayName is 未命名-2");
                check(docU2->content() == content2, "tab 2 content matches typed input 2");
                check(docU2->modified() == true, "tab 2 restored as modified (unsaved)");
                check(dm->activeDocument() == docU2,
                      "active tab is the restored unnamed-2 document");
            }
        }
        emitMarker(failures == 0 ? "[e2e] RESTOREMIX2 PASS" : "[e2e] RESTOREMIX2 FAIL");
        // 不进事件循环直接返回（同 restoremix：场景间无残留）
        return failures == 0 ? 0 : 1;
    }

    // ==================== 写入类模式（hotexit / kill / blurkill） ====================
    // 输入驱动：t=300ms 经 QML 动作入口新建标签 + 编辑器 append 键入
    QTimer::singleShot(300, &app, [&]() {
        if (!QMetaObject::invokeMethod(win, "doNewDocument")) {
            emitMarker("[e2e] ERROR doNewDocument-invoke-failed");
            QCoreApplication::exit(-10);
            return;
        }
        DocumentManager *dm =
                engine.singletonInstance<DocumentManager *>(u"qcnote", u"DocumentManager");
        Document *doc = dm ? dm->activeDocument() : nullptr;
        if (!doc) {
            emitMarker("[e2e] ERROR no-active-document");
            QCoreApplication::exit(-11);
            return;
        }
        QQuickItem *editor = findByQmlType(win->contentItem(), "TextArea");
        if (!editor) {
            emitMarker("[e2e] ERROR textarea-not-found");
            QCoreApplication::exit(-12);
            return;
        }
        if (!QMetaObject::invokeMethod(editor, "append", Q_ARG(QString, content))) {
            emitMarker("[e2e] ERROR append-invoke-failed");
            QCoreApplication::exit(-13);
            return;
        }
        // 回写链路断言：TextArea 变化必须已同步回 Document.content
        // （append → onTextChanged → doc.content 赋值，同步完成）
        if (doc->content() != content) {
            emitMarker("[e2e] ERROR content-writeback-mismatch");
            QCoreApplication::exit(-14);
            return;
        }
        emitMarker("[e2e] WROTE uid=" + doc->uid().toUtf8()
                   + " name=" + doc->displayName().toUtf8());
    });

    if (mode == QLatin1String("hotexit")) {
        // t=2800ms（输入后 2.5s，真实防抖 1000ms 早已自然到期落盘）经 QML
        // 退出入口触发热退出：doQuit → root.close() → onClosing 放行
        QTimer::singleShot(2800, &app, [&]() {
            if (!QMetaObject::invokeMethod(win, "doQuit")) {
                emitMarker("[e2e] ERROR doQuit-invoke-failed");
                QCoreApplication::exit(-15);
            }
        });
        const int rc = app.exec();
        emitMarker("[e2e] EXITED");
        return rc;
    }

    if (mode == QLatin1String("kill")) {
        // 挂住事件循环等待外部强杀（输入后 1000ms 防抖自然到期落盘）
        return app.exec();
    }

    if (mode == QLatin1String("blurkill")) {
        // t=500ms（输入后 200ms，防抖 1000ms 远未到期）失焦：QGuiApplication
        // 的状态 setter 为私有 API（QGuiApplicationPrivate），offscreen 下
        // 以 QMetaObject::invokeMethod 触发真实 applicationStateChanged 信号
        // 等价驱动（信号 → DraftCacheManager 构造期连接的同一连接 →
        // onApplicationStateChanged(Inactive) → flushPending 立即落盘，
        // DEC-003 失焦即写唯一落盘路径）
        QTimer::singleShot(500, &app, [&]() {
            QMetaObject::invokeMethod(&app, "applicationStateChanged",
                                      Q_ARG(Qt::ApplicationState, Qt::ApplicationInactive));
            emitMarker("[e2e] BLURRED");
        });
        return app.exec();
    }

    std::fputs("未知模式（应为 hotexit|kill|blurkill|restore|mixexit|restoremix|mixexit2|restoremix2|mixfile|mixempty）\n", stderr);
    return 64;
}
