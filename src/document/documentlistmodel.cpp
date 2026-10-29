#include "document/documentlistmodel.h"

DocumentListModel::DocumentListModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

int DocumentListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0; // 列表模型无子层级
    return m_documents.size();
}

QVariant DocumentListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_documents.size())
        return QVariant();
    if (role != DocumentRole)
        return QVariant();
    return QVariant::fromValue(m_documents.at(index.row()));
}

QHash<int, QByteArray> DocumentListModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles.insert(DocumentRole, QByteArrayLiteral("document"));
    return roles;
}

int DocumentListModel::count() const
{
    return m_documents.size();
}

Document *DocumentListModel::at(int index) const
{
    if (index < 0 || index >= m_documents.size())
        return nullptr; // 越界防御：不崩溃，调用方判空
    return m_documents.at(index);
}

int DocumentListModel::indexOf(Document *document) const
{
    return m_documents.indexOf(document);
}

void DocumentListModel::insert(int index, Document *document)
{
    if (!document)
        return; // 空指针不入列表
    if (index < 0 || index > m_documents.size())
        index = m_documents.size(); // 越界钳制到末尾追加
    beginInsertRows(QModelIndex(), index, index);
    m_documents.insert(index, document);
    endInsertRows();
    emit countChanged();
}

void DocumentListModel::removeAt(int index)
{
    if (index < 0 || index >= m_documents.size())
        return; // 越界忽略
    beginRemoveRows(QModelIndex(), index, index);
    m_documents.removeAt(index);
    endRemoveRows();
    emit countChanged();
}
