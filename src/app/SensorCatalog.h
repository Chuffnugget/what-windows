#pragma once
#include <QJsonObject>
#include <QMap>
#include <array>

// Snapshots share small pages, rather than one tree containing every module.
// A native event changing one reading only detaches that reading's page.
class SensorCatalog {
    static constexpr size_t PageCount = 256;
    using Page = QMap<QString, QJsonObject>;
    std::array<Page, PageCount> pages;
    qsizetype count = 0;

    static size_t pageFor(const QString &id) {
        return qHash(id, 0) % PageCount;
    }

public:
    class const_iterator {
        friend class SensorCatalog;
        const SensorCatalog *owner = nullptr;
        size_t page = PageCount;
        Page::const_iterator entry;

        const_iterator(const SensorCatalog *c, size_t p, Page::const_iterator i = {})
            : owner(c), page(p), entry(i) {
            advance();
        }

        void advance() {
            while (page < PageCount && entry == owner->pages[page].cend()) {
                if (++page < PageCount)
                    entry = owner->pages[page].cbegin();
            }
        }

    public:
        const_iterator() = default;

        const QString &key() const {
            return entry.key();
        }

        const QJsonObject &value() const {
            return entry.value();
        }

        const QJsonObject &operator*() const {
            return value();
        }

        const_iterator &operator++() {
            ++entry;
            advance();
            return *this;
        }

        bool operator==(const const_iterator &other) const {
            return owner == other.owner && page == other.page && (page == PageCount || entry == other.entry);
        }

        bool operator!=(const const_iterator &other) const {
            return !(*this == other);
        }
    };

    qsizetype size() const {
        return count;
    }

    bool isEmpty() const {
        return count == 0;
    }

    void clear() {
        for (auto &p : pages)
            p.clear();
        count = 0;
    }

    bool contains(const QString &id) const {
        return pages[pageFor(id)].contains(id);
    }

    QJsonObject value(const QString &id) const {
        return pages[pageFor(id)].value(id);
    }

    QJsonObject &operator[](const QString &id) {
        auto &p = pages[pageFor(id)];
        if (!p.contains(id))
            ++count;
        return p[id];
    }

    void insert(const QString &id, const QJsonObject &value) {
        (*this)[id] = value;
    }

    // Most pages can be adopted directly from a worker. If a native event or
    // another module changed a page meanwhile, preserve its unrelated readings.
    bool applyChanges(const SensorCatalog &before, const SensorCatalog &after, qsizetype limit) {
        bool complete = true;
        for (size_t p = 0; p < PageCount; ++p) {
            const auto &original = before.pages[p];
            const auto &updated = after.pages[p];
            auto &current = pages[p];
            if (updated.isSharedWith(original))
                continue;
            const auto growth = updated.size() - current.size();
            if (current.isSharedWith(original) && count + growth <= limit) {
                count += growth;
                current = updated;
                continue;
            }
            for (auto it = updated.cbegin(); it != updated.cend(); ++it) {
                if (original.value(it.key()) == it.value())
                    continue;
                if (count < limit || contains(it.key()))
                    insert(it.key(), it.value());
                else
                    complete = false;
            }
        }
        return complete;
    }

    const_iterator cbegin() const {
        return {this, 0, pages[0].cbegin()};
    }

    const_iterator cend() const {
        return {this, PageCount};
    }

    const_iterator begin() const {
        return cbegin();
    }

    const_iterator end() const {
        return cend();
    }

    const_iterator constFind(const QString &id) const {
        const auto p = pageFor(id);
        const auto it = pages[p].constFind(id);
        return it == pages[p].cend() ? cend() : const_iterator(this, p, it);
    }
};
