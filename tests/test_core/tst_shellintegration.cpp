#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QRegularExpression>
#include <QStringList>
#include <QTest>
#include <QUuid>

#include <type_traits>

#include "platform/shellintegration.h"

// 编译期钉住机制（同 tst_singleinstance 的 D1 断言）：若 ShellIntegration 变回
// 可默认构造，QML 引擎将绕过 create 工厂直接 new，create 复用永不发生
static_assert(!std::is_default_constructible_v<ShellIntegration>,
              "ShellIntegration 必须不可默认构造：否则 QML 引擎绕过 create 工厂（D1 机制）");

#ifdef Q_OS_WIN

// RAII 注册表随机子树：全部用例的注册表读写仅发生在
// HKEY_CURRENT_USER\Software\qcnote-test-<uuid> 子树内（标记根/Classes 根均为
// 其后代），析构时递归删除，绝不触碰真实 HKCU\Software\Classes 与
// HKCU\Software\qcnote。QVERIFY 失败仅 return 出测试函数，析构照常执行清理。
class RegistrySandbox
{
public:
    RegistrySandbox()
        : m_root(QStringLiteral("HKEY_CURRENT_USER\\Software\\qcnote-test-")
                 + QUuid::createUuid().toString(QUuid::Id128))
    {
    }

    ~RegistrySandbox()
    {
        if (!removeTree()) {
            qWarning("清理注册表测试子树失败，可能残留：%ls",
                     reinterpret_cast<const wchar_t *>(m_root.utf16()));
        }
    }

    QString root() const { return m_root; }
    /// 标记键注入路径
    QString markerRoot() const { return m_root + QStringLiteral("\\marker"); }
    /// Classes 根注入路径（菜单键为其后代）
    QString classesRoot() const { return m_root + QStringLiteral("\\classes"); }
    /// 菜单主键 <Classes根>\*\shell\qcnote（与 ShellIntegration::menuKeyPath 同构）
    QString menuKey() const
    {
        return classesRoot() + QStringLiteral("\\*\\shell\\qcnote");
    }
    QString commandKey() const { return menuKey() + QStringLiteral("\\command"); }

    /// 递归删除子树；从未写入（只读用例）视为已干净，不算失败
    bool removeTree()
    {
        if (runReg({ QStringLiteral("query"), m_root }) != 0)
            return true; // 本就不存在
        return runReg({ QStringLiteral("delete"), m_root, QStringLiteral("/f") }) == 0;
    }

    /// 运行 reg.exe（无 shell 参与，参数原样传递）；返回退出码，output 收取
    /// stdout+stderr 解码结果。0=成功，1=键/值不存在，负值=启动/超时异常
    static int runReg(const QStringList &args, QString *output = nullptr)
    {
        QProcess reg;
        reg.start(QStringLiteral("reg.exe"), args);
        if (!reg.waitForStarted(5000)) {
            if (output)
                *output = QStringLiteral("reg.exe 启动失败");
            return -1;
        }
        if (!reg.waitForFinished(10000)) {
            reg.kill();
            reg.waitForFinished(2000);
            if (output)
                *output = QStringLiteral("reg.exe 执行超时");
            return -2;
        }
        if (output)
            *output = decodeRegOutput(reg.readAllStandardOutput() + reg.readAllStandardError());
        return reg.exitCode();
    }

    // reg.exe 输出编码跟随控制台代码页（zh-CN 管道输出为 GBK；若控制台已切
    // 65001 则为 UTF-8）：先按 UTF-8 严格解码，出现替换符再回退本地代码页
    static QString decodeRegOutput(const QByteArray &raw)
    {
        QString text = QString::fromUtf8(raw);
        if (text.contains(QChar(0xFFFD)))
            text = QString::fromLocal8Bit(raw);
        return text;
    }

private:
    QString m_root;
};

// —— reg query 便捷封装（退出码非 0 即键/值不存在）——
static int regQueryKey(const QString &key, QString *out = nullptr)
{
    return RegistrySandbox::runReg({ QStringLiteral("query"), key }, out);
}

static int regQueryValue(const QString &key, const QString &name, QString *out = nullptr)
{
    return RegistrySandbox::runReg(
        { QStringLiteral("query"), key, QStringLiteral("/v"), name }, out);
}

static int regQueryDefault(const QString &key, QString *out = nullptr)
{
    return RegistrySandbox::runReg({ QStringLiteral("query"), key, QStringLiteral("/ve") }, out);
}

#endif // Q_OS_WIN

// 资源管理器右键菜单注册表行为：首启判定 → 同意（默认值/Icon/command/标记 +
// reg query /ve 实证 (Default)）→ 判定变 false；拒绝仅标记无菜单键；写入失败
// 返回中文错误且标记仍写入、不崩溃。均为 C++ 直接 new（注入随机测试子树），
// 不经 QML 单例工厂。
class TestShellIntegration : public QObject
{
    Q_OBJECT

private slots:
    void firstStartNeedsPrompt();
    void existingMarkerSuppressesPrompt();
    void acceptWritesMenuAndMarker();
    void declineWritesMarkerOnly();
    void writeFailureReportsErrorAndKeepsMarker();
    void testNoopEnvMakesProductionInstanceNoop();
};

// 首启判定：标记值不存在 → true。判定只看值（键可能因 QSettings contains()
// 读取被顺带创建为空键，无值即视为未提示过——与需求“标记不存在视为首启”一致）
void TestShellIntegration::firstStartNeedsPrompt()
{
#ifndef Q_OS_WIN
    QSKIP("注册表行为仅 Windows，非 Windows 恒 false");
#else
    RegistrySandbox sandbox;
    ShellIntegration si(sandbox.markerRoot(), sandbox.classesRoot());

    QVERIFY(si.contextMenuPromptNeeded());
    // 只读探测不写标记值（键可能被创建为空键，无语义；此处锁定值级零副作用）
    QVERIFY(regQueryValue(sandbox.markerRoot(), QStringLiteral("contextMenuPrompted")) != 0);
#endif
}

// 标记已存在（上次已选择过，无论同意/拒绝）→ 不再提示
void TestShellIntegration::existingMarkerSuppressesPrompt()
{
#ifndef Q_OS_WIN
    QSKIP("注册表行为仅 Windows，非 Windows 恒 false");
#else
    RegistrySandbox sandbox;
    // 预置标记：reg add 直接写（独立于被测代码的写路径，避免自证）
    QCOMPARE(RegistrySandbox::runReg({ QStringLiteral("add"), sandbox.markerRoot(),
                                       QStringLiteral("/v"), QStringLiteral("contextMenuPrompted"),
                                       QStringLiteral("/t"), QStringLiteral("REG_DWORD"),
                                       QStringLiteral("/d"), QStringLiteral("1"),
                                       QStringLiteral("/f") }),
             0);

    ShellIntegration si(sandbox.markerRoot(), sandbox.classesRoot());
    QVERIFY(!si.contextMenuPromptNeeded());
#endif
}

// 同意路径全量验证：首启 true → applyContextMenuChoice(true) 成功 →
// 菜单主键默认值=(Default)“用qcnote打开”（reg query /ve 实证，且值清单无名为
// “.”的普通值）、Icon=带引号 exe 路径、command 默认值=“"<exe>" "%1"”、
// 标记 REG_DWORD 0x1 → 判定变 false（当前实例 + 新实例双验证）
void TestShellIntegration::acceptWritesMenuAndMarker()
{
#ifndef Q_OS_WIN
    QSKIP("注册表行为仅 Windows，非 Windows 为 no-op");
#else
    RegistrySandbox sandbox;
    const QString exePath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());

    ShellIntegration si(sandbox.markerRoot(), sandbox.classesRoot());
    QVERIFY(si.contextMenuPromptNeeded()); // 前置：首启判定为 true

    const QVariantMap result = si.applyContextMenuChoice(true);
    QCOMPARE(result.value(QStringLiteral("ok")).toBool(), true);
    QVERIFY(result.value(QStringLiteral("error")).toString().isEmpty());

    // 默认值实证：/ve 显示 (默认)=用qcnote打开，而非名为 “.” 的普通值
    QString menuDefault;
    QCOMPARE(regQueryDefault(sandbox.menuKey(), &menuDefault), 0);
    QVERIFY(menuDefault.contains(QStringLiteral("REG_SZ")));
    QVERIFY(menuDefault.contains(QStringLiteral("用qcnote打开")));

    // 值清单中不存在名为 “.” 的普通值（默认值写法正确的反向证明）
    const QRegularExpression dotValue(QStringLiteral("^\\s+\\.\\s+REG_SZ"),
                                      QRegularExpression::MultilineOption);
    QString menuAll;
    QCOMPARE(regQueryKey(sandbox.menuKey(), &menuAll), 0);
    QVERIFY(!dotValue.match(menuAll).hasMatch());

    // Icon = 带引号的当前进程 exe 绝对路径
    QString icon;
    QCOMPARE(regQueryValue(sandbox.menuKey(), QStringLiteral("Icon"), &icon), 0);
    QVERIFY(icon.contains(QStringLiteral("REG_SZ")));
    QVERIFY(icon.contains(QStringLiteral("\"%1\"").arg(exePath)));

    // command 默认值 = "<exe绝对路径>" "%1"
    QString commandDefault;
    QCOMPARE(regQueryDefault(sandbox.commandKey(), &commandDefault), 0);
    QVERIFY(commandDefault.contains(QStringLiteral("REG_SZ")));
    QVERIFY(commandDefault.contains(
        QStringLiteral("\"%1\" \"%2\"").arg(exePath, QStringLiteral("%1"))));
    QString commandAll;
    QCOMPARE(regQueryKey(sandbox.commandKey(), &commandAll), 0);
    QVERIFY(!dotValue.match(commandAll).hasMatch());

    // 标记 contextMenuPrompted = REG_DWORD 0x1
    QString marker;
    QCOMPARE(regQueryValue(sandbox.markerRoot(), QStringLiteral("contextMenuPrompted"), &marker), 0);
    QVERIFY(marker.contains(QStringLiteral("REG_DWORD")));
    QVERIFY(marker.contains(QStringLiteral("0x1")));

    // 判定变 false：当前实例与新建实例（排除实例内缓存巧合）一致
    QVERIFY(!si.contextMenuPromptNeeded());
    ShellIntegration fresh(sandbox.markerRoot(), sandbox.classesRoot());
    QVERIFY(!fresh.contextMenuPromptNeeded());
#endif
}

// 拒绝路径：仅写标记，Classes 子树无任何菜单键；不再提示
void TestShellIntegration::declineWritesMarkerOnly()
{
#ifndef Q_OS_WIN
    QSKIP("注册表行为仅 Windows，非 Windows 为 no-op");
#else
    RegistrySandbox sandbox;
    ShellIntegration si(sandbox.markerRoot(), sandbox.classesRoot());
    QVERIFY(si.contextMenuPromptNeeded());

    const QVariantMap result = si.applyContextMenuChoice(false);
    QCOMPARE(result.value(QStringLiteral("ok")).toBool(), true);
    QVERIFY(result.value(QStringLiteral("error")).toString().isEmpty());

    // 标记已写
    QString marker;
    QCOMPARE(regQueryValue(sandbox.markerRoot(), QStringLiteral("contextMenuPrompted"), &marker), 0);
    QVERIFY(marker.contains(QStringLiteral("REG_DWORD")));

    // 无任何菜单键：Classes 根与菜单主键均不存在（reg query 退出码非 0）
    QVERIFY(regQueryKey(sandbox.classesRoot()) != 0);
    QVERIFY(regQueryKey(sandbox.menuKey()) != 0);

    QVERIFY(!si.contextMenuPromptNeeded());
#endif
}

// 写入失败路径：注入必然无法创建的 Classes 根（键名组件超过注册表 255 字符
// 上限，RegCreateKeyExW 必败、QSettings 检出 AccessError——探针实证）。要求：
// ok=false 且 error 为非空中文信息、进程不崩溃、菜单键无残留、标记仍写入
// （用户不再被重复询问）。注：任务书原提示“在 Software 下建值再以其为父级
// 构造键”经本机实证无效——注册表键/值命名空间独立、同名可共存，无法造成
// 创建失败，故改用超长键名注入。
void TestShellIntegration::writeFailureReportsErrorAndKeepsMarker()
{
#ifndef Q_OS_WIN
    QSKIP("注册表行为仅 Windows，非 Windows 为 no-op");
#else
    RegistrySandbox sandbox;
    const QString badClasses =
        sandbox.root() + QStringLiteral("\\") + QString(300, QChar(u'k'));

    ShellIntegration si(sandbox.markerRoot(), badClasses);
    const QVariantMap result = si.applyContextMenuChoice(true); // 不崩溃即通过本行

    QCOMPARE(result.value(QStringLiteral("ok")).toBool(), false);
    const QString error = result.value(QStringLiteral("error")).toString();
    QVERIFY(!error.isEmpty());
    QVERIFY(error.contains(QStringLiteral("失败")));
    QVERIFY(error.contains(badClasses)); // 错误信息含失败键路径，便于排查

    // 标记仍写入（菜单失败不阻断标记）
    QString marker;
    QCOMPARE(regQueryValue(sandbox.markerRoot(), QStringLiteral("contextMenuPrompted"), &marker), 0);
    QVERIFY(marker.contains(QStringLiteral("REG_DWORD")));
    QVERIFY(!si.contextMenuPromptNeeded());

    // 失败路径无菜单键残留
    QVERIFY(regQueryKey(badClasses + QStringLiteral("\\*\\shell\\qcnote")) != 0);
#endif
}

// 测试隔离钩子 QCNOTE_TEST_SHELL_NOOP：生产默认构造（空参注入）的实例在
// 变量已设置时行为等同非 Windows no-op——promptNeeded 恒 false、
// applyContextMenuChoice 成功且对真实注册表（生产标记键与 Classes 菜单键）
// 零写入。零写入以快照对照断言：调用前后两键的 reg query 退出码与完整输出
// 完全一致（机器已用过功能、键本就存在时，“键不存在”式断言会误报；快照
// 不变才是零写入语义）。RAII 末尾还原环境变量，避免污染同进程后续用例。
// 另钉死隔离红线：变量存续期间注入随机沙箱根的实例仍走真实注册表并真实
// 落地——no-op 判定必须含“两根均为空”条件，环境变量只影响生产默认构造
// 路径、不影响注入根（防未来重构丢掉该条件）。
void TestShellIntegration::testNoopEnvMakesProductionInstanceNoop()
{
#ifndef Q_OS_WIN
    QSKIP("no-op 环境变量钩子仅 Windows 注册表实现有意义，非 Windows 本就 no-op");
#else
    qputenv("QCNOTE_TEST_SHELL_NOOP", "1");
    struct EnvGuard {
        ~EnvGuard() { qunsetenv("QCNOTE_TEST_SHELL_NOOP"); }
    } envGuard;

    const QString prodMarkerKey =
        QStringLiteral("HKEY_CURRENT_USER\\Software\\qcnote\\qcnote");
    const QString prodMenuKey =
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\*\\shell\\qcnote");

    // 调用前快照：两键的 reg query 退出码 + 完整输出
    QString markerBefore, menuBefore;
    const int markerExitBefore =
        regQueryValue(prodMarkerKey, QStringLiteral("contextMenuPrompted"), &markerBefore);
    const int menuExitBefore = regQueryKey(prodMenuKey, &menuBefore);

    ShellIntegration si{QString(), QString()}; // 空参 = 生产默认配置（花括号避开函数声明的歧义解析）
    QVERIFY(!si.contextMenuPromptNeeded());

    const QVariantMap result = si.applyContextMenuChoice(true);
    QCOMPARE(result.value(QStringLiteral("ok")).toBool(), true);
    QVERIFY(result.value(QStringLiteral("error")).toString().isEmpty());

    // 零写入断言：两键快照（退出码 + 输出）与调用前完全一致
    QString markerAfter, menuAfter;
    QCOMPARE(regQueryValue(prodMarkerKey, QStringLiteral("contextMenuPrompted"), &markerAfter),
             markerExitBefore);
    QCOMPARE(markerAfter, markerBefore);
    QCOMPARE(regQueryKey(prodMenuKey, &menuAfter), menuExitBefore);
    QCOMPARE(menuAfter, menuBefore);

    // 隔离红线：环境变量存续期间，注入随机沙箱根的实例不受 no-op 影响，
    // 添加操作在沙箱内真实落地（沙箱 RAII 随既有模式清理）
    {
        RegistrySandbox sandbox;
        ShellIntegration injected(sandbox.markerRoot(), sandbox.classesRoot());
        QVERIFY(injected.contextMenuPromptNeeded()); // 注入实例未被 no-op 化

        const QVariantMap injectedResult = injected.applyContextMenuChoice(true);
        QCOMPARE(injectedResult.value(QStringLiteral("ok")).toBool(), true);

        QString menuDefault;
        QCOMPARE(regQueryDefault(sandbox.menuKey(), &menuDefault), 0);
        QVERIFY(menuDefault.contains(QStringLiteral("用qcnote打开")));
        QString marker;
        QCOMPARE(regQueryValue(sandbox.markerRoot(), QStringLiteral("contextMenuPrompted"),
                               &marker),
                 0);
        QVERIFY(marker.contains(QStringLiteral("REG_DWORD")));
        QVERIFY(!injected.contextMenuPromptNeeded());
    }
#endif
}

int runShellIntegrationTests(int argc, char *argv[])
{
    // QProcess（reg.exe）与 QSettings 均不依赖 GUI；注册表用例仅 Windows 生效，
    // 非 Windows 由 QSKIP 逐用例跳过
    QCoreApplication app(argc, argv);
    TestShellIntegration t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_shellintegration.moc"
