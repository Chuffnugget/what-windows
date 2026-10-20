#pragma once
#include <QObject>
#include <QColor>
#include <QSettings>
#include <QVariantList>
#include <QVariantMap>
#include <QPointer>
#include <QWindow>
#include <QWidget>
#include <QIcon>
#include <QSet>
#include "IconRenderer.h"
#include "core/BackgroundWorker.h"

class TaskbarIconManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList entries READ entries NOTIFY configurationChanged)
    Q_PROPERTY(QVariantMap values READ values NOTIFY valuesChanged)
    Q_PROPERTY(QString error MEMBER lastError NOTIFY valuesChanged)
public:
    TaskbarIconManager(const QString &file, bool testing, QObject *parent = nullptr);
    ~TaskbarIconManager() override;

    QVariantList entries() const {
        return configuration;
    }

    QVariantMap values() const {
        return displayValues;
    }

    void setReadings(const QVariantList &rows);
    void attachMainWindow(QWindow *window);

    void setBrowsing(bool value) {
        browsing = value;
        refresh();
    }

    Q_INVOKABLE void addSelected(const QStringList &sensors);
    Q_INVOKABLE void setSensor(const QString &id, const QString &sensor);
    Q_INVOKABLE void remove(const QString &id);
    Q_INVOKABLE void rename(const QString &id, const QString &name);
    Q_INVOKABLE void setColor(const QString &id, bool background, const QColor &colour);
    Q_INVOKABLE void chooseFont(const QString &id);
    Q_INVOKABLE void chooseImage(const QString &id);
    Q_INVOKABLE void addCustom();
    Q_INVOKABLE void resetStyle(const QString &id);
    Q_INVOKABLE void restoreLogo();
    Q_INVOKABLE void openPanel(const QString &id);
signals:
    void configurationChanged();
    void valuesChanged();
    void openRequested();

private:
    void save();
    void refresh();
    void change(const QString &id, const QString &key, const QVariant &value);
    void importImage(const QString &id, bool adding);
    std::unique_ptr<BackgroundWorker> imageJob;
    quint64 imageGeneration = 0;
    QString lastError;
    QSettings settings;
    bool testMode;
    QVariantList configuration;
    QHash<QString, QVariantMap> readings;
    QVariantMap displayValues;
    QHash<QString, QWidget *> panels;
    QHash<QString, QIcon> cachedIcons;
    QHash<QString, QString> rendered;
    QPointer<QWindow> mainWindow;
    bool browsing = false;
    bool refreshing = false, refreshPending = false, refreshQueued = false;
    IconRenderer renderer;
};
