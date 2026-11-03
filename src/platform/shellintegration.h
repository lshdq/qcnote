#pragma once

#include <QtQmlIntegration/qqmlintegration.h>
#include <QObject>
#include <QString>
#include <QVariantMap>

class QQmlEngine;
class QJSEngine;

/**
 * @brief 资源管理器右键菜单集成（QML 单例）。
 *
 * 仅负责注册表读写，不做 UI：
 * - 首启判定：标记键下值 contextMenuPrompted 不存在视为首次启动（由 QML 弹框
 *   询问用户是否把“用qcnote打开”加入文件右键菜单）；
 * - 应用用户选择：同意 → 写菜单项 + 标记；拒绝 → 仅写标记。菜单写入失败时
 *   标记仍写入（用户不再被重复询问），并经返回值暴露可读错误信息；菜单
 *   主键写成功而 command 等后续写失败时，尽力回滚删除已写入的菜单主键
 *   子树（回滚失败不掩盖原错误，仅附加到错误信息）。
 *
 * 注册表内容（全部写 HKCU，绝不提权）：
 * - 标记：<标记根>\contextMenuPrompted = 1（REG_DWORD）；
 * - 菜单：<Classes根>\*\shell\qcnote 默认值 = “用qcnote打开”、Icon = 带引号的
 *   exe 绝对路径；...\qcnote\command 默认值 = "<exe绝对路径>" "%1"。
 *   默认值经 QSettings NativeFormat 的 setValue(".") 约定写法落为键的 (Default)
 *   （已用 reg query /ve 实证，而非名为 "." 的普通值）。
 *
 * 注册表实现全部以 #ifdef Q_OS_WIN 包裹，非 Windows 为 no-op（不提示、不写），
 * contextMenuPromptNeeded() 恒 false。
 *
 * 测试隔离钩子（仅自动化测试使用）：环境变量 QCNOTE_TEST_SHELL_NOOP 已设置时，
 * 生产默认构造（未注入测试注册表根）的实例行为等同非 Windows no-op——
 * contextMenuPromptNeeded() 恒 false、applyContextMenuChoice() 不写注册表
 * 直接返回成功。目的：加载完整 Main.qml 的 QML 集成测试在无首启标记的
 * 机器上不会弹出首启提示框（其模态会锁定 uiLocked、拦截窗口关闭路径导致
 * 测试超时）。注入根的测试构造路径不受该环境变量影响，始终读写真实注册表
 * （随机测试子树）。可脱离 QML 引擎在 C++ 测试中直接 new
 * （注入标记根与 Classes 根两个注册表路径，测试用随机子树隔离真机注册表）；
 * QML 侧经 create() 复用同一实例。读写均为无状态的按需构造 QSettings（每实例
 * 直读注册表，无跨实例缓存担忧），不在成员中长期持有配置对象。
 *
 * 本类必须保持不可默认构造（两个注入参数必填，同 SingleInstance/D1 教训）：
 * Qt 6 单例构造模式判定中 std::is_default_constructible 优先级高于 create
 * 工厂，若默认可构造，QML 引擎会绕过 create() 直接 new ShellIntegration()，
 * create() 复用永不发生。create() 内部显式传空参构造（空即生产配置）。
 */
class ShellIntegration : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    /**
     * @param markerRoot 首启标记键的注册表完整路径（NativeFormat 直连该键，
     *                   如 HKEY_CURRENT_USER\Software\qcnote-test-<uuid>\marker）；
     *                   为空时用生产默认 QSettings(NativeFormat, UserScope,
     *                   组织名/应用名均为 qcnote)，即 HKCU\Software\qcnote\qcnote
     * @param classesRoot 右键菜单的 Classes 根；为空时用生产默认
     *                    HKEY_CURRENT_USER\Software\Classes
     * @note 两个注入参数必填（无默认值）：保持类不可默认构造是 QML 单例机制
     *       的前提，见类注释（D1 教训）；create() 与生产/测试代码均显式传参，
     *       空参即生产配置
     */
    explicit ShellIntegration(const QString &markerRoot, const QString &classesRoot,
                              QObject *parent = nullptr);
    ~ShellIntegration() override;

    /// QML 单例工厂：复用同一实例（生命周期归 C++ 管理，QML 引擎不负责销毁）
    static ShellIntegration *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);

    /// C++ 侧单例访问（未经 create() 创建时为 nullptr）
    static ShellIntegration *instance();

    /// 首启判定：Windows 下标记 contextMenuPrompted 不存在 → true；否则 false。
    /// 非 Windows 恒 false。不写任何值；注意 QSettings NativeFormat 的 contains()
    /// 读取不存在的键时会顺带创建空的标记键（无值，键存在与否无语义，判定只看值）
    Q_INVOKABLE bool contextMenuPromptNeeded() const;

    /**
     * @brief 应用用户在首启提示中的选择。
     *
     * accepted=true：写菜单项 + 标记；accepted=false：仅写标记。菜单写入失败时
     * 标记仍写入，且整体返回失败。非 Windows 为 no-op 成功。
     * @return { ok: bool, error: string }——ok 为 false 时 error 为可读中文信息
     *         （含失败键路径），供现有错误对话框展示；成功时 error 为空串
     */
    Q_INVOKABLE QVariantMap applyContextMenuChoice(bool accepted);

private:
    /// 写菜单注册表项（菜单主键默认值/Icon + command 默认值）；失败置 *error
    bool writeContextMenuEntries(QString *error);
    /// 写首启标记 contextMenuPrompted=1；失败置 *error
    bool writeMarker(QString *error);
    /// 菜单主键完整路径：<Classes根>\*\shell\qcnote
    QString menuKeyPath() const;
    /// command 子键完整路径：<菜单主键>\command
    QString commandKeyPath() const;
    /// 标记键完整路径（默认时展开为 HKCU\Software\qcnote\qcnote），用于错误信息
    QString markerKeyPath() const;

    static ShellIntegration *s_instance;

    QString m_markerRoot;  ///< 空 = 生产默认（NativeFormat+UserScope，qcnote/qcnote）
    QString m_classesRoot; ///< 空 = 生产默认（HKEY_CURRENT_USER\Software\Classes）
    bool m_noop;           ///< no-op 模式（非 Windows，或设置了 QCNOTE_TEST_SHELL_NOOP
                           ///< 的生产默认实例）：不提示、不写注册表，见类注释
};
