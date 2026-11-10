// 标签页 Win11 风格改造截图取证 harness（《测试用例-标签页Win11风格改造》
// TC-002/003/004/008 与决策预约验证点 DEC-002/DEC-004 的视觉证据侧执行器，
// 不注册 ctest，由编排脚本 tests\verify-win11tabs.ps1 启动）。
//
// 测试方式：offscreen 平台下实例化真实 Main.qml，按场景构造文档集 /
// 主题 / 窗口宽度 / 悬停 / 键盘焦点后以 QQuickWindow::grabWindow() 截图，
// 每场景保存两份 PNG：
//   <场景名>.png          —— 整窗（默认 1000x640，或场景指定宽度）
//   <场景名>-titlebar.png —— 标题栏区域（高 40）裁剪并 2x 放大（细节审阅）
//
// 场景清单（浅色 light / 深色 dark）：
//   light-single          单标签（DEC-004：菜单按钮与贴底标签垂直错位观感）
//   light-multi-hover     3 标签（1 个未保存）+ 合成悬停于非活动标签
//   light-shrink-1000     6 个长名标签在 1000 宽下等比收缩（省略号 + 右边距）；
//                        8 标签为饱和态（见 light-overflow）
//   light-overflow-800    8 个长名标签在 800 宽下饱和 + 横向滚动条
//   light-focusring       键盘焦点环落在非活动标签
//   dark-single / dark-multi-hover / dark-overflow-800 —— 深色对应场景
//   （DEC-002：dark-multi-hover 验证 #22ffffff 悬停叠加在深色标题栏上可辨）
//
// 「标签页背景加深与分隔线」复检：全部场景标题栏/标签条背景自动取
//   AppTheme.titleBarBackground（本轮已加深：浅 #dde2ec / 深 #171c24）；
//   light/dark-multi-hover（活动=首标签）两对相邻非活动标签间的 1px 竖
//   分隔线（AppTheme.border）入镜，供肉眼审阅加深对比与分隔线观感。
//
// 会话重定向临时目录 + 便携标记不存在 → 无用户会话/草稿恢复干扰，截图
// 文档集完全由本 harness 构造。GUI 红线：强制 QT_QPA_PLATFORM=offscreen，
// 全程无可见窗口。
//
// 输出协议（stdout）：[shot] SAVED <路径> 每张一行；[shot] SCENARIO <名>
// 开始标记；[shot] ALL PASS / [shot] FAIL <原因> 汇总。
// 用法：e2e_tabshot_driver.exe <输出目录>
// 退出码：0 = 全部场景截图成功；非 0 = 失败。

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <cstdio>

#include "document/document.h"
#include "document/documentmanager.h"

namespace {

QTemporaryDir &scratchDir()
{
    static QTemporaryDir dir;
    return dir;
}

void emitMarker(const QByteArray &line)
{
    std::fputs(line.constData(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

bool qmlTypeNameIs(const QObject *obj, const char *name)
{
    const QString cn = QString::fromLatin1(obj->metaObject()->className());
    const int cut = cn.indexOf(u'_');
    return (cut > 0 ? cn.left(cut) : cn) == QLatin1String(name);
}

QQuickItem *findItemByQmlType(QQuickItem *root, const char *name)
{
    if (!root)
        return nullptr;
    if (qmlTypeNameIs(root, name))
        return root;
    const auto kids = root->childItems();
    for (QQuickItem *kid : kids) {
        if (QQuickItem *found = findItemByQmlType(kid, name))
            return found;
    }
    return nullptr;
}

bool classNameStartsWith(const QObject *obj, const char *prefix)
{
    return QString::fromLatin1(obj->metaObject()->className())
        .startsWith(QLatin1String(prefix));
}

QQuickItem *findItemByClassPrefix(QQuickItem *root, const char *prefix)
{
    if (!root)
        return nullptr;
    if (classNameStartsWith(root, prefix))
        return root;
    const auto kids = root->childItems();
    for (QQuickItem *kid : kids) {
        if (QQuickItem *found = findItemByClassPrefix(kid, prefix))
            return found;
    }
    return nullptr;
}

/// 事件泵：让 QML 布局 / hover / 渲染管线安定
void pumpEvents(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
}

/// 每场景独立测试台：真实 Main.qml（DocumentManager 为跨引擎 C++ 单例，
/// 场景开始清空残余文档）
struct ShotBench
{
    QQmlEngine engine;
    QQuickWindow *win = nullptr;
    DocumentManager *dm = nullptr;
    QObject *theme = nullptr;
    QQuickItem *tabs = nullptr;
    QQuickItem *tabList = nullptr;

    bool init()
    {
        QQmlComponent c(&engine);
        c.setData(R"QML(import QtQuick
import qcnote
Main {}
)QML", QUrl(QStringLiteral("bench.qml")));
        if (c.isError()) {
            for (const QQmlError &e : c.errors())
                std::fprintf(stderr, "[shot] bench error: %s\n",
                             qPrintable(e.toString()));
            return false;
        }
        win = qobject_cast<QQuickWindow *>(c.create());
        if (!win)
            return false;
        dm = engine.singletonInstance<DocumentManager *>(u"qcnote", u"DocumentManager");
        theme = engine.singletonInstance<QObject *>(u"qcnote", u"AppTheme");
        tabs = findItemByQmlType(win->contentItem(), "DocumentTabs");
        tabList = tabs ? findItemByClassPrefix(tabs, "QQuickListView") : nullptr;
        if (!dm || !theme || !tabs || !tabList)
            return false;
        const auto docs = dm->documents();
        for (Document *d : docs)
            dm->closeDocument(d);
        return true;
    }

    ~ShotBench() { delete win; }

    QQuickItem *tabAt(int index)
    {
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(tabList, "itemAtIndex",
                                  Q_RETURN_ARG(QQuickItem *, item),
                                  Q_ARG(int, index));
        return item;
    }

    Document *newDoc(const QString &content = QString())
    {
        dm->newDocument();
        pumpEvents(30);
        Document *doc = dm->activeDocument();
        if (doc && !content.isEmpty()) {
            doc->setContent(content);
            pumpEvents(30);
        }
        return doc;
    }

    Document *openLongNameDoc(int seq)
    {
        static const QString kBase =
            QStringLiteral("Win11标签连通式风格改造验收超长文件名文档编号");
        const QString path = scratchDir().filePath(
            QStringLiteral("%1-%2-very-long-file-name-for-elide.txt").arg(kBase).arg(seq));
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
            return nullptr;
        f.write("x\n");
        f.close();
        dm->openUrls({ QUrl::fromLocalFile(path) });
        pumpEvents(30);
        return dm->activeDocument();
    }
};

/// 合成鼠标移动（无按键 MouseMove = hover move，与真实悬停同事件路径）
void sendMouseMove(QQuickWindow *win, const QPointF &scenePos)
{
    QMouseEvent ev(QEvent::MouseMove, scenePos, scenePos, Qt::NoButton,
                   Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(win, &ev);
    pumpEvents(120);
}

/// 截图保存：整窗 + 标题栏（0..40）2x 放大裁剪
bool saveShots(QQuickWindow *win, const QString &dir, const QString &name,
               int titleBarHeight = 40)
{
    win->show();
    pumpEvents(250); // 布局 + 渲染安定
    QImage full = win->grabWindow();
    // 慢机/高负载下首拍可能为空/渲染中途帧：短重试采样（最多再试 3 次）
    for (int retry = 0; full.isNull() && retry < 3; ++retry) {
        pumpEvents(120);
        full = win->grabWindow();
    }
    if (full.isNull()) {
        emitMarker("[shot] FAIL grabWindow null: " + name.toUtf8());
        return false;
    }
    const QString fullPath = QDir(dir).filePath(name + QStringLiteral(".png"));
    if (!full.save(fullPath)) {
        emitMarker("[shot] FAIL save: " + fullPath.toUtf8());
        return false;
    }
    emitMarker("[shot] SAVED " + fullPath.toUtf8());

    QImage bar = full.copy(0, 0, full.width(), titleBarHeight);
    bar = bar.scaled(bar.width() * 2, bar.height() * 2,
                     Qt::KeepAspectRatio, Qt::FastTransformation);
    const QString barPath =
        QDir(dir).filePath(name + QStringLiteral("-titlebar.png"));
    if (!bar.save(barPath)) {
        emitMarker("[shot] FAIL save: " + barPath.toUtf8());
        return false;
    }
    emitMarker("[shot] SAVED " + barPath.toUtf8());
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    // GUI 红线：offscreen，全程无可见窗口
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QCNOTE_TEST_SHELL_NOOP", "1");
    static const char kWinFonts[] = "C:/Windows/Fonts";
    if (QDir(QString::fromLatin1(kWinFonts)).exists())
        qputenv("QT_QPA_FONTDIR", kWinFonts);

    if (argc < 2) {
        std::fputs("用法: e2e_tabshot_driver <输出目录>\n", stderr);
        return 64;
    }
    const QString outDir = QString::fromLocal8Bit(argv[1]);
    if (!QDir().mkpath(outDir)) {
        emitMarker("[shot] FAIL mkpath: " + outDir.toLocal8Bit());
        return 65;
    }

    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    QQuickStyle::setStyle(QStringLiteral("Material"));
    // 会话重定向临时目录：截图文档集不受真实用户会话干扰
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       scratchDir().path());

    int failures = 0;
    const auto beginScenario = [&failures](const char *name) {
        emitMarker(QByteArray("[shot] SCENARIO ") + name);
    };

    // ---- light-single：单标签（DEC-004 错位观感取证）----
    beginScenario("light-single");
    {
        ShotBench b;
        if (!b.init() || !b.newDoc()) {
            emitMarker("[shot] FAIL bench light-single");
            ++failures;
        } else if (!saveShots(b.win, outDir, QStringLiteral("light-single"))) {
            ++failures;
        }
    }

    // ---- light-multi-hover：3 标签（1 未保存）+ 悬停非活动标签 ----
    beginScenario("light-multi-hover");
    {
        ShotBench b;
        if (!b.init() || !b.newDoc() || !b.newDoc(QStringLiteral("dirty"))
                || !b.newDoc()) {
            emitMarker("[shot] FAIL bench light-multi-hover");
            ++failures;
        } else {
            b.dm->setActiveDocument(b.dm->documents().constFirst());
            pumpEvents(80);
            b.win->show();
            pumpEvents(150);
            QQuickItem *del = b.tabAt(2);
            if (del) {
                sendMouseMove(b.win,
                              del->mapToScene(QPointF(del->width() / 2,
                                                      del->height() / 2)));
            } else {
                emitMarker("[shot] FAIL delegate missing light-multi-hover");
                ++failures;
            }
            if (!saveShots(b.win, outDir, QStringLiteral("light-multi-hover")))
                ++failures;
        }
    }

    // ---- light-shrink-1000：6 长名标签 1000 宽未饱和等比收缩（TC-008/TC-011）；
    // 8 个时全部退化为 minTabWidth 并微溢出（属 TC-009 饱和态，非等比收缩），
    // 6 个时 ΣnaturalWidths + gaps > 可用宽（≈812）且缺口 < Σ可收缩量，
    // 各 tab 重度收缩但仍高于 minTabWidth、无溢出 ----
    beginScenario("light-shrink-1000");
    {
        ShotBench b;
        bool ok = b.init();
        for (int i = 0; ok && i < 6; ++i)
            ok = b.openLongNameDoc(i) != nullptr;
        if (!ok) {
            emitMarker("[shot] FAIL bench light-shrink-1000");
            ++failures;
        } else {
            b.win->show();
            b.win->resize(QSize(1000, 640));
            pumpEvents(200);
            // 状态取证：收缩后宽度应全部 > minTabWidth 且 contentWidth ≤ 可用宽
            emitMarker(QByteArray("[shot] shrink-state minTabWidth=")
                       + QByteArray::number(
                           b.tabs->property("minTabWidth").toInt())
                       + QByteArray(" contentWidth=")
                       + QByteArray::number(
                           b.tabList->property("contentWidth").toReal()));
            const QVariantList dist =
                b.tabs->property("distributedWidths").toList();
            QByteArray widths("distributed=");
            for (const QVariant &w : dist)
                widths += QByteArray::number(w.toReal()) + ' ';
            emitMarker(widths);
            if (!saveShots(b.win, outDir, QStringLiteral("light-shrink-1000")))
                ++failures;
        }
    }

    // ---- light-overflow-800：8 长名标签 800 宽饱和 + 滚动条（TC-009）----
    beginScenario("light-overflow-800");
    {
        ShotBench b;
        bool ok = b.init();
        for (int i = 0; ok && i < 8; ++i)
            ok = b.openLongNameDoc(i) != nullptr;
        if (!ok) {
            emitMarker("[shot] FAIL bench light-overflow-800");
            ++failures;
        } else {
            b.win->show();
            b.win->resize(QSize(800, 600));
            pumpEvents(200);
            if (!saveShots(b.win, outDir, QStringLiteral("light-overflow-800")))
                ++failures;
        }
    }

    // ---- light-focusring：键盘焦点环落在非活动标签（TC-010）----
    beginScenario("light-focusring");
    {
        ShotBench b;
        if (!b.init() || !b.newDoc() || !b.newDoc() || !b.newDoc()) {
            emitMarker("[shot] FAIL bench light-focusring");
            ++failures;
        } else {
            b.dm->setActiveDocument(b.dm->documents().constFirst());
            pumpEvents(80);
            b.win->show();
            pumpEvents(150);
            QMetaObject::invokeMethod(b.tabs, "forceActiveFocus");
            pumpEvents(80);
            // 右移一次：焦点环落在第 2 个（非活动）标签
            QKeyEvent right1(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
            QCoreApplication::sendEvent(b.win, &right1);
            QKeyEvent right2(QEvent::KeyRelease, Qt::Key_Right, Qt::NoModifier);
            QCoreApplication::sendEvent(b.win, &right2);
            pumpEvents(150);
            if (!saveShots(b.win, outDir, QStringLiteral("light-focusring")))
                ++failures;
        }
    }

    // ---- dark-single：深色单标签（TC-003）----
    beginScenario("dark-single");
    {
        ShotBench b;
        if (!b.init() || !b.newDoc()) {
            emitMarker("[shot] FAIL bench dark-single");
            ++failures;
        } else if (!b.theme->setProperty("dark", true)) {
            emitMarker("[shot] FAIL set dark theme dark-single");
            ++failures;
        } else {
            pumpEvents(120);
            if (!saveShots(b.win, outDir, QStringLiteral("dark-single")))
                ++failures;
        }
    }

    // ---- dark-multi-hover：深色 3 标签 + 悬停（DEC-002 可辨性取证）----
    beginScenario("dark-multi-hover");
    {
        ShotBench b;
        if (!b.init() || !b.newDoc() || !b.newDoc(QStringLiteral("dirty"))
                || !b.newDoc()) {
            emitMarker("[shot] FAIL bench dark-multi-hover");
            ++failures;
        } else if (!b.theme->setProperty("dark", true)) {
            emitMarker("[shot] FAIL set dark theme dark-multi-hover");
            ++failures;
        } else {
            b.dm->setActiveDocument(b.dm->documents().constFirst());
            pumpEvents(120);
            b.win->show();
            pumpEvents(150);
            QQuickItem *del = b.tabAt(2);
            if (del) {
                sendMouseMove(b.win,
                              del->mapToScene(QPointF(del->width() / 2,
                                                      del->height() / 2)));
            } else {
                emitMarker("[shot] FAIL delegate missing dark-multi-hover");
                ++failures;
            }
            if (!saveShots(b.win, outDir, QStringLiteral("dark-multi-hover")))
                ++failures;
        }
    }

    // ---- dark-overflow-800：深色饱和 + 滚动条 ----
    beginScenario("dark-overflow-800");
    {
        ShotBench b;
        bool ok = b.init();
        for (int i = 0; ok && i < 8; ++i)
            ok = b.openLongNameDoc(i) != nullptr;
        if (!ok) {
            emitMarker("[shot] FAIL bench dark-overflow-800");
            ++failures;
        } else if (!b.theme->setProperty("dark", true)) {
            emitMarker("[shot] FAIL set dark theme dark-overflow-800");
            ++failures;
        } else {
            b.win->show();
            b.win->resize(QSize(800, 600));
            pumpEvents(200);
            if (!saveShots(b.win, outDir, QStringLiteral("dark-overflow-800")))
                ++failures;
        }
    }

    if (failures == 0)
        emitMarker("[shot] ALL PASS");
    else
        emitMarker(QByteArray("[shot] FAIL count=") + QByteArray::number(failures));
    return failures == 0 ? 0 : 1;
}
