#pragma once
#include <QObject>
#include <QColor>
#include <QMenu>
#include <QPointer>
#include <QSettings>
#include <QVariantList>
#include <QVariantMap>
#include <QHash>
#include <QSystemTrayIcon>
#include <QImage>
#include "TaskbarIconManager.h"
#include "IconRenderer.h"
#include "NativeSensorIcon.h"
#include "BrowserModel.h"
#include "core/BackgroundWorker.h"

class SensorTrayManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList entries READ entries NOTIFY configurationChanged)
    Q_PROPERTY(QVariantMap values READ values NOTIFY valuesChanged)
    Q_PROPERTY(QString error MEMBER lastError NOTIFY valuesChanged)
    Q_PROPERTY(bool browsing READ isBrowsing WRITE setBrowsing)
    Q_PROPERTY(QObject *appIcons READ appIcons CONSTANT)
    Q_PROPERTY(QAbstractItemModel *pickerModel READ pickerModel CONSTANT)
    Q_PROPERTY(bool pickerBusy READ pickerBusy NOTIFY pickerChanged)
    Q_PROPERTY(bool trayAvailable READ trayAvailable CONSTANT)
public:
    SensorTrayManager(const QString &settingsFile, bool testing, QObject *parent = nullptr);
    bool browsing = false;

    bool isBrowsing() const {
        return browsing;
    }

    void setBrowsing(bool value) {
        browsing = value;
        taskbar->setBrowsing(value);
        refresh();
    }

    QObject *appIcons() const {
        return taskbar;
    }

    TaskbarIconManager *taskbarIcons() const {
        return taskbar;
    }

    QVariantList entries() const {
        return configuration;
    }

    QVariantMap values() const {
        return displayValues;
    }

    bool trayAvailable() const {
        return QSystemTrayIcon::isSystemTrayAvailable();
    }

    void setContextMenu(QMenu *menu);
    void attachMainTray(QSystemTrayIcon *icon);

    void setMainToolTip(const QString &text) {
        mainToolTip = text;
        refresh();
    }

    Q_INVOKABLE void setSensor(const QString &id, const QString &sensor);
    Q_INVOKABLE void chooseImage(const QString &id);
    Q_INVOKABLE void restoreLogo();
    Q_INVOKABLE void addSelected(const QStringList &ids);
    void setReadings(const QVariantList &rows, bool complete = true);
    Q_INVOKABLE void searchPicker(const QString &query, const QVariantMap &collapsed = {});
    Q_INVOKABLE void closePicker();

    QAbstractItemModel *pickerModel() {
        return &pickerModelData;
    }

    bool pickerBusy() const {
        return pickerActive && (pickerPending || pickerJob.isRunning());
    }

    Q_INVOKABLE void add(const QString &id);
    Q_INVOKABLE void remove(const QString &id);
    Q_INVOKABLE void setColor(const QString &id, bool background, const QColor &colour);
    Q_INVOKABLE void chooseFont(const QString &id);
    Q_INVOKABLE void resetStyle(const QString &id);
    static QString selectImageFile();
    static QVariantMap readImageFile(const QString &path);
    static QString compact(const QVariantMap &row);
    static QString tooltip(const QVariantMap &row);
    static QString moduleName(const QString &source);
    static QImage render(const QString &text, const QVariantMap &style, int size);
signals:
    void configurationChanged();
    void valuesChanged();
    void pickerChanged();
    void openRequested();

private:
    std::unique_ptr<BackgroundWorker> imageJob;
    quint64 imageGeneration = 0;
    void preparePicker();
    BrowserModel pickerModelData;
    BackgroundWorker pickerJob{"Icon sensor picker"};
    QString pickerQuery;
    QVariantMap pickerCollapsed;
    quint64 pickerGeneration = 0;
    bool pickerActive = false, pickerPending = false, readingsComplete = false;
    void save();
    void refresh();
    void style(const QString &id, const QString &key, const QVariant &value);
    QPointer<QMenu> contextMenu;
    QPointer<QSystemTrayIcon> mainTray;
    QString mainToolTip = "WHAT - click to configure", lastError;
    TaskbarIconManager *taskbar = nullptr;
    QSettings settings;
    bool testMode;
    QVariantList configuration;
    QHash<QString, QVariantMap> readings;
    QVariantMap displayValues;
    QHash<QString, NativeSensorIcon *> icons;
    QHash<QString, QString> rendered;
    IconRenderer renderer{
        [](const IconRenderer::Request &r, int size) { return render(r.text, r.style, size); }};
};
