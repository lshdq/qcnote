#pragma once

#include <QtQmlIntegration/qqmlintegration.h>
#include <QAbstractListModel>
#include <QList>

#include "document/document.h"

/**
 * @brief 文档列表的行级增量模型（供 QML Repeater/ListView 使用）。
 *
 * 解决 QVariantList 作 model 时增删文档导致 delegate 全量销毁重建
 * （撤销历史/光标丢失）的问题：增删只发 rowsInserted/rowsRemoved，
 * 视图按行增量更新。由 DocumentManager 持有并维护，QML 侧只读。
 */
class DocumentListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("DocumentListModel 只能由 DocumentManager 创建")
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    explicit DocumentListModel(QObject *parent = nullptr);

    enum Roles
    {
        DocumentRole = Qt::UserRole + 1 ///< 文档对象（Document*，供 delegate 绑定）
    };

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const;

    /// 边界安全：越界返回 nullptr（QML 侧可直接判空）
    Q_INVOKABLE Document *at(int index) const;
    /// 不在其中返回 -1
    Q_INVOKABLE int indexOf(Document *document) const;

    /// 供 DocumentManager 调用：在 index 处插入（行增量通知）
    void insert(int index, Document *document);
    /// 供 DocumentManager 调用：移除 index 行（行增量通知）；越界忽略
    void removeAt(int index);

signals:
    void countChanged();

private:
    QList<Document *> m_documents;
};
