#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickStyle>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("qcnote"));
    QGuiApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QQuickStyle::setStyle(QStringLiteral("Material"));

    // 运行时窗口图标：Windows 下与 exe 内嵌图标一致，兼顾未来跨平台
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/qt/qml/qcnote/icons/qcnote.svg")));

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     &app, [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("qcnote"), QStringLiteral("Main"));

    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}
