#include <QCommandLineParser>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QIcon>
#include <QList>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QStringList>
#include <QUrl>

#include "platform/singleinstance.h"

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("qcnote"));
    QGuiApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    // 组织名与 SessionManager 会话文件定位一致（QSettings IniFormat + UserScope
    // 组织名/应用名均为 qcnote），保证应用属性与会话路径互不漂移
    QGuiApplication::setOrganizationName(QStringLiteral("qcnote"));

    QQuickStyle::setStyle(QStringLiteral("Material"));

    // 运行时窗口图标：Windows 下与 exe 内嵌图标一致，兼顾未来跨平台
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/qt/qml/qcnote/icons/qcnote.svg")));

    // 位置参数为待打开文件路径列表（资源管理器右键“用qcnote打开”经
    // "<exe>" "%1" 启动；可多个、可含空格/中文）。用 parse 而非 process：
    // 不因未知参数/帮助选项退出，无参数启动行为与现状完全一致
    QCommandLineParser parser;
    parser.addPositionalArgument(QStringLiteral("files"),
                                 QStringLiteral("启动时打开的文件（可多个，可含空格/中文）"));
    parser.parse(QCoreApplication::arguments());
    QList<QUrl> urlArgs;
    const QStringList positional = parser.positionalArguments();
    urlArgs.reserve(positional.size());
    for (const QString &path : positional)
        urlArgs.append(QUrl::fromUserInput(path));

    // 单实例初始化（引擎加载前）：已有实例在运行 → 参数转发给它后本进程以
    // 退出码 0 结束；本实例成为主实例（IPC 不可用时降级，启动不中断），参数
    // 进入待打开队列，QML 侧就绪后经 SingleInstance.openPendingUrls() 刷出
    SingleInstance *singleInstance = SingleInstance::create(nullptr, nullptr);
    if (singleInstance->initialize(urlArgs) == SingleInstance::StartupRole::Forwarded)
        return 0;

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("qcnote"), QStringLiteral("Main"));

    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}
