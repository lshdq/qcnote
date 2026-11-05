#pragma once

#include <QtQmlIntegration/qqmlintegration.h>
#include <QObject>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QString>
#include <QVariantList>

#include "editor/searchengine.h"

// moc 生成的元对象要求指针属性指向完整类型，QQuickItem/QQuickTextDocument
// 须完整包含；QTextDocument 仅在实现中使用，前置声明即可
class QTextDocument;

namespace qcnote {

/**
 * @brief 查找替换 QML 控制器：在 SearchEngine 纯逻辑之上对接 QML TextArea。
 *
 * 职责边界（复刻 MultiCursorController 的三层模式）：
 * - 引擎（searchengine）只做纯文本坐标计算（模式编译、全部匹配、环绕导航、
 *   替换编辑批次）；本控制器持有查找参数与匹配状态，把替换编辑应用到
 *   QTextDocument（QQuickTextDocument），并以属性形式暴露给 QML
 *   （高亮层逐匹配绘制、计数显示、当前匹配选区联动）。
 * - pattern/replacement/三个选项为可写属性：查找参数变更即重编译重算并把
 *   当前匹配归零；replacement 不影响匹配集合，变更仅通知。
 * - 文档 contentsChange（非自身编辑）时重算匹配并钳制当前索引。
 * - 替换编辑全部在单 QTextCursor::editBlock 内从后往前应用：「替换」一步
 *   一次撤销，「全部替换」整体单次撤销。
 *
 * QML 侧典型接入（后续任务接线）：
 * - textDocument 绑定 editor.textDocument；target 绑定对应 TextArea；
 * - 查找输入框 text 绑定 pattern，选项开关绑定三个选项属性；
 * - matchRanges/overflowed 驱动高亮层与计数；Enter/Shift+Enter 调
 *   findNext()/findPrev()；「替换」「全部替换」调 replaceCurrent()/replaceAll()；
 * - 当前匹配选区可直接绑定 currentRange，或依赖控制器经 target 的
 *   select(start,end) 调用自动设置原生选区（DEC-007）。
 *
 * 所有属性共用同一个 stateChanged 通知信号（与 MultiCursorController 一致）：
 * 任何状态变更后统一发射，QML 绑定按需重新拉取最新值。
 */
class SearchController : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QString pattern READ pattern WRITE setPattern NOTIFY stateChanged)
    Q_PROPERTY(QString replacement READ replacement WRITE setReplacement NOTIFY stateChanged)
    Q_PROPERTY(bool caseSensitive READ caseSensitive WRITE setCaseSensitive NOTIFY stateChanged)
    Q_PROPERTY(bool wholeWord READ wholeWord WRITE setWholeWord NOTIFY stateChanged)
    Q_PROPERTY(bool useRegex READ useRegex WRITE setUseRegex NOTIFY stateChanged)
    Q_PROPERTY(int matchCount READ matchCount NOTIFY stateChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY stateChanged)
    Q_PROPERTY(QVariantList matchRanges READ matchRanges NOTIFY stateChanged)
    Q_PROPERTY(QVariantList currentRange READ currentRange NOTIFY stateChanged)
    Q_PROPERTY(bool overflowed READ overflowed NOTIFY stateChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY stateChanged)
    Q_PROPERTY(QQuickItem *target READ target WRITE setTarget NOTIFY stateChanged)
    Q_PROPERTY(QQuickTextDocument *textDocument READ textDocument WRITE setTextDocument NOTIFY stateChanged)

public:
    explicit SearchController(QObject *parent = nullptr);
    ~SearchController() override;

    QString pattern() const;
    void setPattern(const QString &pattern);

    QString replacement() const;
    void setReplacement(const QString &replacement);

    bool caseSensitive() const;
    void setCaseSensitive(bool on);

    bool wholeWord() const;
    void setWholeWord(bool on);

    bool useRegex() const;
    void setUseRegex(bool on);

    /// 匹配数（overflowed 为真时真实匹配数更多，计数显示「>10000」）
    int matchCount() const;
    /// 当前匹配下标；-1 = 无当前匹配（无匹配或尚未定位）
    int currentIndex() const;
    /// 全部匹配区间，每项内嵌 [start,end]（QVariantList），按 start 升序
    QVariantList matchRanges() const;
    /// 当前匹配区间 [start,end]；无当前匹配时为空列表（QML 选区联动用）
    QVariantList currentRange() const;
    /// 匹配数是否超过 10000 上限（超出部分未计算，高亮层应停止绘制）
    bool overflowed() const;
    /// 无效正则等错误信息（空串 = 无错误；QML 直接显示文本，非仅颜色线索）
    QString errorMessage() const;

    QQuickItem *target() const;
    void setTarget(QQuickItem *target);

    QQuickTextDocument *textDocument() const;
    void setTextDocument(QQuickTextDocument *textDocument);

    /// 下一个匹配（环绕）；无匹配无操作。当前匹配经 target 设置为原生选区
    Q_INVOKABLE void findNext();
    /// 上一个匹配（环绕）；无匹配无操作
    Q_INVOKABLE void findPrev();
    /// 替换当前匹配：单 editBlock 应用（一次撤销），重算匹配后指向下一个
    /// 匹配（从“替换文本之后”的坐标起环绕查找），并把光标设置到替换文本之后
    Q_INVOKABLE void replaceCurrent();
    /// 全部替换：全部编辑单 editBlock 应用（整体单次撤销），返回替换处数。
    /// 按原文本坐标一次性计算，替换产物不参与本轮；溢出时仅替换已计算的
    /// 10000 处（DEC-004：导航与替换仍可用）
    Q_INVOKABLE int replaceAll();
    /// 清空匹配状态（关闭对话框/关闭文档时 QML 调用）。查找串/替换串/选项
    /// 保留——需求要求三者会话内保留（内存，不写盘），下次打开沿用
    Q_INVOKABLE void reset();

signals:
    /// 全部属性共用的变更通知
    void stateChanged();

private:
    /// 重算全部匹配并刷新 matchCount/matchRanges/overflowed 缓存（不发信号）
    void recomputeMatches();

    /// 重算并把当前匹配归零到首个匹配：查找参数（pattern/选项）变更后的
    /// 全新查找会话语义
    void restartSearch();

    /// 把编辑批次应用到文档：单 editBlock、按 start 降序从后往前（一次
    /// 替换操作 = 单次撤销）。应用前先静默清空对外匹配状态（见实现），调用
    /// 方在应用后显式重算并定位，最后统一发 stateChanged
    void applyEdits(const QList<SearchEdit> &edits);

    /// 文档内容变化（contentsChange，非本控制器编辑）时重算匹配并钳制索引
    void onDocumentContentsChanged();

    /// 经 target（TextArea）的 select(start,end) 设置原生选区/光标；target
    /// 未设置或不可调用时静默忽略（QML 侧可改用 currentRange 属性联动）
    void selectInTarget(int start, int end);

    /// 把当前匹配经 selectInTarget 设置为 target 原生选区（DEC-007）
    void selectCurrentInTarget();

    SearchEngine m_engine;          // 编译后的模式 + 选项 + 错误信息
    QString m_replacement;          // 替换串（不影响匹配集合，独立持有）
    QList<SearchMatch> m_matches;   // 当前匹配列表（findAll 结果缓存）
    QVariantList m_matchRanges;     // matchRanges 属性缓存（避免每次取值重建）
    int m_currentIndex = -1;
    bool m_overflowed = false;

    QQuickItem *m_target = nullptr;
    QQuickTextDocument *m_textDocument = nullptr;
    QTextDocument *m_doc = nullptr;

    /// applyEdits 编辑文档期间为真：此时匹配状态已静默清空，contentsChange
    /// 重算跳过（编辑完成后由调用方显式重算并定位），防重入
    bool m_applyingEdits = false;
};

} // namespace qcnote
