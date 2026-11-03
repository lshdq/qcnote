#include "platform/shellintegration.h"

#include <QCoreApplication>
#include <QDir>
#include <QJSEngine>
#include <QQmlEngine>
#include <QSettings>
#include <QStringList>

#include <memory>

#ifdef Q_OS_WIN
namespace {
// 按注入参数构造标记存储：注入路径直连该注册表键；为空时用生产默认
// （NativeFormat + UserScope，组织名/应用名均为 qcnote，即 HKCU\Software\qcnote\qcnote）。
// 每次调用新建实例，保证读到的就是注册表现值（QSettings 仅在实例内部缓存）
std::unique_ptr<QSettings> markerSettingsFor(const QString &markerRoot)
{
    if (markerRoot.isEmpty())
        return std::make_unique<QSettings>(QSettings::NativeFormat, QSettings::UserScope,
                                           QStringLiteral("qcnote"), QStringLiteral("qcnote"));
    return std::make_unique<QSettings>(markerRoot, QSettings::NativeFormat);
}
} // namespace
#endif // Q_OS_WIN

ShellIntegration *ShellIntegration::s_instance = nullptr;

ShellIntegration::ShellIntegration(const QString &markerRoot, const QString &classesRoot,
                                   QObject *parent)
    : QObject(parent)
    , m_markerRoot(markerRoot)
    , m_classesRoot(classesRoot)
    // no-op 模式判定（构造时读一次环境变量）：非 Windows 恒 no-op；Windows 下
    // 仅“生产默认构造（未注入任何测试根）且设置了 QCNOTE_TEST_SHELL_NOOP”
    // 才 no-op——该变量仅供自动化测试隔离首启提示框，注入根的测试构造路径
    // 不受影响，始终走真实注册表
#ifdef Q_OS_WIN
    , m_noop(markerRoot.isEmpty() && classesRoot.isEmpty()
             && qEnvironmentVariableIsSet("QCNOTE_TEST_SHELL_NOOP"))
#else
    , m_noop(true)
#endif
{
}

ShellIntegration::~ShellIntegration()
{
    if (s_instance == this)
        s_instance = nullptr;
}

ShellIntegration *ShellIntegration::create(QQmlEngine *qmlEngine, QJSEngine *jsEngine)
{
    // 与 SessionManager::create 一致：实例无 QObject 父对象，由 C++ 侧持有直至
    // 进程退出；所有权固定 CppOwnership（QML 引擎不负责销毁）。无前置依赖单例。
    // 显式传空参 = 生产配置（空即生产，见构造函数 @param）；类不可默认构造，
    // QML 引擎只能经本工厂创建并复用（D1 机制，见类注释）
    if (!s_instance)
        s_instance = new ShellIntegration(QString(), QString());
    QJSEngine::setObjectOwnership(s_instance, QQmlEngine::CppOwnership);
    return s_instance;
}

ShellIntegration *ShellIntegration::instance()
{
    return s_instance;
}

bool ShellIntegration::contextMenuPromptNeeded() const
{
#ifdef Q_OS_WIN
    if (!m_noop) {
        // 标记存在（无论上次选择同意或拒绝）即不再提示；只读探测不创建任何键
        const std::unique_ptr<QSettings> settings = markerSettingsFor(m_markerRoot);
        return !settings->contains(QStringLiteral("contextMenuPrompted"));
    }
#endif
    // no-op：非 Windows，或测试隔离（QCNOTE_TEST_SHELL_NOOP）下的生产默认实例
    return false;
}

QVariantMap ShellIntegration::applyContextMenuChoice(bool accepted)
{
    QVariantMap result;
#ifdef Q_OS_WIN
    if (!m_noop) {
        // 逐项独立收集错误：菜单失败时标记仍要写（需求：用户不再被重复询问），
        // 末尾汇总成败；多处同时失败时以换行合并错误信息
        QStringList errors;
        if (accepted) {
            QString menuError;
            if (!writeContextMenuEntries(&menuError))
                errors.append(menuError);
        }
        {
            QString markerError;
            if (!writeMarker(&markerError))
                errors.append(markerError);
        }
        result.insert(QStringLiteral("ok"), errors.isEmpty());
        result.insert(QStringLiteral("error"), errors.join(QChar(u'\n')));
        return result;
    }
#endif
    // no-op：非 Windows，或测试隔离下的生产默认实例——不提示、不写注册表，
    // 被调用时按成功处理，不产生任何注册表/文件副作用
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("error"), QString());
    return result;
}

bool ShellIntegration::writeContextMenuEntries(QString *error)
{
#ifdef Q_OS_WIN
    // 命令与图标用当前 exe 的绝对路径（原生分隔符、带引号）；便携模式照常写入
    // 当时路径（exe 移动后菜单失效由用户自行承担，已确认为接受行为）
    const QString exePath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    const QString quotedExe = QStringLiteral("\"%1\"").arg(exePath);

    // 菜单主键：默认值 = 用qcnote打开（"." 为 QSettings 写注册表默认值的约定，
    // 落为键的 (Default)，已用 reg query /ve 实证）+ Icon = 带引号 exe 路径
    {
        QSettings menu(menuKeyPath(), QSettings::NativeFormat);
        menu.setValue(QStringLiteral("."), QStringLiteral("用qcnote打开"));
        menu.setValue(QStringLiteral("Icon"), quotedExe);
        menu.sync();
        if (menu.status() != QSettings::NoError) {
            *error = QStringLiteral("写入右键菜单注册表键失败：%1").arg(menuKeyPath());
            return false;
        }
    }
    // command 子键：默认值 = "<exe绝对路径>" "%1"（%1 为资源管理器传入的右键目标
    // 文件；双参一次替换，避免路径文本中被误展开）
    {
        QSettings command(commandKeyPath(), QSettings::NativeFormat);
        command.setValue(QStringLiteral("."),
                         QStringLiteral("\"%1\" \"%2\"").arg(exePath, QStringLiteral("%1")));
        command.sync();
        if (command.status() != QSettings::NoError) {
            // 尽力回滚：command 写失败时菜单主键已成半套残留（explorer 显示
            // 菜单但点击无效），删除整个菜单主键子树。经父键 remove("qcnote")
            // 整树删除（父键因主键已写成功而必然存在，构造父键不产生新键）。
            // 回滚失败不掩盖原错误，仅附加到错误信息
            QString rollbackNote;
            {
                const QString menuKey = menuKeyPath();
                QSettings parent(menuKey.left(menuKey.lastIndexOf(u'\\')), QSettings::NativeFormat);
                parent.remove(QStringLiteral("qcnote"));
                parent.sync();
                if (parent.status() != QSettings::NoError)
                    rollbackNote =
                        QStringLiteral("（回滚清理菜单键亦失败，可能残留：%1）").arg(menuKey);
            }
            *error = QStringLiteral("写入右键菜单命令注册表键失败：%1%2")
                         .arg(commandKeyPath(), rollbackNote);
            return false;
        }
    }
    return true;
#else
    Q_UNUSED(error)
    return true;
#endif
}

bool ShellIntegration::writeMarker(QString *error)
{
#ifdef Q_OS_WIN
    const std::unique_ptr<QSettings> settings = markerSettingsFor(m_markerRoot);
    settings->setValue(QStringLiteral("contextMenuPrompted"), 1); // int 落为 REG_DWORD
    settings->sync();
    if (settings->status() != QSettings::NoError) {
        *error = QStringLiteral("写入首启标记注册表键失败：%1").arg(markerKeyPath());
        return false;
    }
    return true;
#else
    Q_UNUSED(error)
    return true;
#endif
}

QString ShellIntegration::menuKeyPath() const
{
    const QString root = m_classesRoot.isEmpty()
                             ? QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes")
                             : m_classesRoot;
    // “*” 为注册表字面量键名（对所有文件生效的菜单挂载点），非通配符
    return root + QStringLiteral("\\*\\shell\\qcnote");
}

QString ShellIntegration::commandKeyPath() const
{
    return menuKeyPath() + QStringLiteral("\\command");
}

QString ShellIntegration::markerKeyPath() const
{
    // 默认组织/应用名（qcnote/qcnote）在 NativeFormat + UserScope 下的实际落点
    return m_markerRoot.isEmpty()
               ? QStringLiteral("HKEY_CURRENT_USER\\Software\\qcnote\\qcnote")
               : m_markerRoot;
}
