#pragma once
#include <QColor>
#include <QObject>
#include <QSettings>
#include <QStringList>

// One palette per profile, shared by the lighting and icon colour pickers.
class ColourPalette : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList recent READ recent NOTIFY changed)
    Q_PROPERTY(QStringList favourites READ favourites NOTIFY changed)
public:
    explicit ColourPalette(QSettings &settings, QObject *parent = nullptr);
    QStringList recent() const;
    QStringList favourites() const;
    Q_INVOKABLE void remember(const QColor &colour);
    Q_INVOKABLE void setFavourite(const QColor &colour, bool favourite);
    Q_INVOKABLE QColor choose(const QColor &initial, const QString &title, bool alpha = false);

signals:
    void changed();

private:
    QStringList colours(const QString &key, int limit) const;
    QSettings &settings;
};
