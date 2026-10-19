#pragma once
#include <QAbstractListModel>
#include <QVariantList>
#include <QSet>

// Update values in place, preserving scroll position and delegates on every poll.
class BrowserModel : public QAbstractListModel {
public:
    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() ? 0 : int(rows.size());
    }

    enum Role { Row = Qt::UserRole + 1, Value, Available, Reason, Collapsed };

    struct Change {
        int index;
        QList<int> roles;
    };

    struct Range {
        int first, last;
    };

    struct Prepared {
        QVariantList next;
        QList<Change> changes;
        bool sameStructure = true;
        quint64 revision = 0;
        QList<Range> removals, insertions;
        QVariantList reordered;
        QList<int> oldToNew;
    };

    struct Category {
        QString name;
        QStringList ancestors;
        int first = 0, count = 0;
    };

    struct Categories {
        QVariantList rows;
        QList<Category> groups;
    };

    // Prepare once on the browser worker. Folding then needs only category spans
    // and integer row positions, without regrouping or comparing sensor maps.
    static Categories prepareCategories(QVariantList rows) {
        Categories result{std::move(rows), {}};
        QHash<QString, QString> parents;
        for (int i = 0; i < result.rows.size(); ++i) {
            const auto row = result.rows[i].toMap();
            if (!row.value("header").toBool())
                continue;
            if (!result.groups.isEmpty())
                result.groups.last().count = i - result.groups.last().first;
            const auto name = row.value("category").toString();
            parents.insert(name, row.value("parentCategory").toString());
            result.groups.append({name, {}, i, int(result.rows.size()) - i});
        }
        for (auto &group : result.groups) {
            auto parent = parents.value(group.name);
            while (!parent.isEmpty() && !group.ancestors.contains(parent)) {
                group.ancestors.append(parent);
                parent = parents.value(parent);
            }
        }
        return result;
    }

    static QList<int> visibleCategoryRows(const Categories &categories, const QSet<QString> &closed) {
        QList<int> result;
        result.reserve(categories.rows.size());
        for (const auto &group : categories.groups) {
            bool hidden = false;
            for (const auto &ancestor : group.ancestors)
                hidden |= closed.contains(ancestor);
            if (hidden)
                continue;
            const int count = closed.contains(group.name) ? 1 : group.count;
            for (int n = 0; n < count; ++n)
                result.append(group.first + n);
        }
        return result;
    }

    static QVariantList categorySnapshot(const Categories &categories, const QList<int> &visible,
                                         const QSet<QString> &closed) {
        QVariantList result;
        result.reserve(visible.size());
        int group = 0;
        for (const int source : visible) {
            while (group + 1 < categories.groups.size() && categories.groups[group + 1].first <= source)
                ++group;
            if (source == categories.groups[group].first) {
                auto header = categories.rows[source].toMap();
                header["collapsed"] = closed.contains(categories.groups[group].name);
                result.append(header);
            } else
                result.append(categories.rows[source]);
        }
        return result;
    }

    // Exchange ownership so the caller can release the old catalogue off-thread.
    void swapCategories(Categories &next, QList<int> &visible, const QSet<QString> &closed) {
        std::swap(categories, next);
        visibleSources.swap(visible);
        closedCategories = closed;
        categoriesReady = true;
    }

    bool setCollapsed(const QSet<QString> &closed) {
        if (!categoriesReady)
            return false;
        if (closed == closedCategories)
            return true;
        const auto nextSources = visibleCategoryRows(categories, closed);
        const auto next = categorySnapshot(categories, nextSources, closed);
        // Both index lists are ordered positions in the same cached catalogue.
        // Remove/insert contiguous runs, retaining surviving delegates and indexes.
        int position = 0;
        while (position < visibleSources.size() || position < nextSources.size()) {
            if (position < visibleSources.size() && position < nextSources.size() &&
                visibleSources[position] == nextSources[position]) {
                ++position;
                continue;
            }
            if (position < visibleSources.size() &&
                (position == nextSources.size() || visibleSources[position] < nextSources[position])) {
                int end = position + 1;
                while (end < visibleSources.size() &&
                       (position == nextSources.size() || visibleSources[end] < nextSources[position]))
                    ++end;
                beginRemoveRows({}, position, end - 1);
                rows.remove(position, end - position);
                visibleSources.remove(position, end - position);
                endRemoveRows();
            } else {
                int end = position + 1;
                while (end < nextSources.size() &&
                       (position == visibleSources.size() || nextSources[end] < visibleSources[position]))
                    ++end;
                beginInsertRows({}, position, end - 1);
                rows.insert(position, end - position, QVariant{});
                visibleSources.insert(position, end - position, 0);
                for (int i = position; i < end; ++i) {
                    rows[i] = next[i];
                    visibleSources[i] = nextSources[i];
                }
                endInsertRows();
                position = end;
            }
        }
        int visible = 0;
        for (const auto &group : categories.groups) {
            while (visible < nextSources.size() && nextSources[visible] < group.first)
                ++visible;
            if (visible < nextSources.size() && nextSources[visible] == group.first &&
                rows[visible] != next[visible]) {
                rows[visible] = next[visible];
                emit dataChanged(index(visible), index(visible), {Collapsed});
            }
        }
        closedCategories = closed;
        ++revision;
        return true;
    }

    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rows.size())
            return {};
        if (role == Row)
            return rows[index.row()];
        const auto row = rows[index.row()].toMap();
        if (role == Value)
            return row.value("value");
        if (role == Available)
            return row.value("available", false);
        if (role == Reason)
            return row.value("unavailable_reason", QString{});
        if (role == Collapsed)
            return row.value("collapsed", false);
        return {};
    }

    QHash<int, QByteArray> roleNames() const override {
        return {{Row, "modelData"},
                {Value, "readingValue"},
                {Available, "readingAvailable"},
                {Reason, "readingReason"},
                {Collapsed, "categoryCollapsed"}};
    }

    QVariantList snapshot() const {
        return rows;
    }

    quint64 generation() const {
        return revision;
    }

    static QString key(const QVariant &value) {
        const auto row = value.toMap();
        return row.value("header").toBool() ? "category:" + row.value("category").toString()
                                            : "sensor:" + row.value("id").toString();
    }

    static Prepared prepare(const QVariantList &before, const QVariantList &next, quint64 revision) {
        Prepared result{next, {}, before.size() == next.size(), revision};
        if (!result.sameStructure)
            return prepareStructure(before, next, revision);
        for (int i = 0; i < next.size(); ++i) {
            if (key(before[i]) != key(next[i])) {
                return prepareStructure(before, next, revision);
            }
            if (before[i] == next[i])
                continue;
            auto old = before[i].toMap(), row = next[i].toMap();
            QList<int> roles;
            if (old.value("value") != row.value("value"))
                roles.append(Value);
            if (old.value("available") != row.value("available"))
                roles.append(Available);
            if (old.value("unavailable_reason") != row.value("unavailable_reason"))
                roles.append(Reason);
            if (old.value("collapsed") != row.value("collapsed"))
                roles.append(Collapsed);
            if (row.value("header").toBool() || row.contains("control"))
                roles.append(Row);
            else {
                for (auto name : {"value", "available", "unavailable_reason"}) {
                    old.remove(name);
                    row.remove(name);
                }
                if (old != row)
                    roles.append(Row);
            }
            result.changes.append({i, roles});
        }
        return result;
    }

    void apply(const Prepared &change) {
        categoriesReady = false;
        if (change.revision != revision) {
            update(change.next);
            return;
        }
        ++revision;
        if (!change.sameStructure) {
            for (const auto &range : change.removals) {
                beginRemoveRows({}, range.first, range.last);
                rows.remove(range.first, range.last - range.first + 1);
                endRemoveRows();
            }
            if (!change.oldToNew.isEmpty()) {
                emit layoutAboutToBeChanged();
                const auto oldIndexes = persistentIndexList();
                QModelIndexList newIndexes;
                for (const auto &item : oldIndexes)
                    newIndexes.append(index(change.oldToNew[item.row()], item.column()));
                rows = change.reordered;
                changePersistentIndexList(oldIndexes, newIndexes);
                emit layoutChanged();
            }
            for (const auto &range : change.insertions) {
                beginInsertRows({}, range.first, range.last);
                rows.insert(range.first, range.last - range.first + 1, QVariant{});
                for (int n = range.first; n <= range.last; ++n)
                    rows[n] = change.next[n];
                endInsertRows();
            }
        }
        for (int n = 0; n < change.changes.size();) {
            const int first = change.changes[n].index;
            const auto roles = change.changes[n].roles;
            int last = first;
            rows[first] = change.next[first];
            ++n;
            while (n < change.changes.size() && change.changes[n].index == last + 1 &&
                   change.changes[n].roles == roles) {
                last = change.changes[n++].index;
                rows[last] = change.next[last];
            }
            emit dataChanged(index(first), index(last), roles);
        }
    }

    void update(const QVariantList &next) {
        apply(prepare(rows, next, revision));
    }

    void replace(const QVariantList &next) {
        categoriesReady = false;
        beginResetModel();
        rows = next;
        ++revision;
        endResetModel();
    }

private:
    // Prepare catalogue comparisons and index maps on the worker. apply() only
    // performs the model notifications and row edits that must run on the GUI.
    static Prepared prepareStructure(const QVariantList &before, const QVariantList &next, quint64 revision) {
        Prepared result{next, {}, false, revision};
        if (before.isEmpty()) {
            if (!next.isEmpty())
                result.insertions.append({0, int(next.size()) - 1});
            return result;
        }
        if (next.isEmpty()) {
            result.removals.append({0, int(before.size()) - 1});
            return result;
        }

        // Read each identity once. Search results usually keep the same ordering,
        // so no temporary catalogue edits or repeated map lookups are needed.
        QStringList oldKeys, newKeys;
        QHash<QString, int> positions;
        QSet<QString> wanted;
        for (int i = 0; i < before.size(); ++i) {
            const auto id = key(before[i]);
            oldKeys.append(id);
            positions.insert(id, i);
        }
        for (const auto &row : next) {
            const auto id = key(row);
            newKeys.append(id);
            wanted.insert(id);
        }
        for (int i = int(before.size()) - 1; i >= 0;) {
            if (wanted.contains(oldKeys[i])) {
                --i;
                continue;
            }
            const int last = i;
            while (i >= 0 && !wanted.contains(oldKeys[i]))
                --i;
            result.removals.append({i + 1, last});
        }
        QList<int> keptPosition(before.size(), -1);
        int keptCount = 0;
        for (int i = 0; i < before.size(); ++i)
            if (wanted.contains(oldKeys[i]))
                keptPosition[i] = keptCount++;
        result.oldToNew.resize(keptCount);
        bool reordered = false;
        int retained = 0;
        for (int i = 0; i < next.size();) {
            const auto found = positions.constFind(newKeys[i]);
            if (found == positions.cend()) {
                const int first = i++;
                while (i < next.size() && !positions.contains(newKeys[i]))
                    ++i;
                result.insertions.append({first, i - 1});
                continue;
            }
            const int old = found.value();
            const int kept = keptPosition[old];
            reordered |= kept != retained;
            result.oldToNew[kept] = retained++;
            result.reordered.append(before[old]);
            if (before[old] != next[i])
                result.changes.append({i, {Row, Value, Available, Reason, Collapsed}});
            ++i;
        }
        if (!reordered) {
            result.oldToNew.clear();
            result.reordered.clear();
        }
        return result;
    }

    Categories categories;
    QList<int> visibleSources;
    QSet<QString> closedCategories;
    bool categoriesReady = false;
    QVariantList rows;
    quint64 revision = 0;
};
