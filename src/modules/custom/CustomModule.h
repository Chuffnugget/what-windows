#pragma once
#include "core/BackgroundWorker.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <QElapsedTimer>
#include <QVariantList>
#include <atomic>

class CustomModule : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList commands READ commands NOTIFY commandsChanged)
    Q_PROPERTY(QVariantList sensors READ sensors NOTIFY configurationChanged)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(bool controlEnabled READ controlEnabled WRITE setControlEnabled NOTIFY changed)
    Q_PROPERTY(bool cloudEnabled READ cloudEnabled WRITE setCloudEnabled NOTIFY changed)
    Q_PROPERTY(bool testing READ testing NOTIFY previewChanged)
    Q_PROPERTY(QVariantMap preview READ preview NOTIFY previewChanged)
public:
    explicit CustomModule(bool testMode, QObject *parent = nullptr);
    ~CustomModule() override;
    void restore(QJsonArray definitions, bool enabled, bool control, bool cloud);

    QJsonArray definitions() const {
        return sources;
    }

    QVariantList commands() const;
    QVariantList sensors() const;
    QStringList issues() const;
    QJsonArray readings() const;
    std::function<QJsonArray()> captureReadings() const;
    QJsonObject shared(bool forHomeAssistant = false) const;

    bool enabled() const {
        return active;
    }

    bool controlEnabled() const {
        return controls;
    }

    bool cloudEnabled() const {
        return cloud;
    }

    bool testing() const {
        return previewJob && previewJob->isRunning();
    }

    QVariantMap preview() const {
        return lastPreview.toVariantMap();
    }

    void setEnabled(bool enabled);
    void setControlEnabled(bool enabled);
    void setCloudEnabled(bool enabled);
    Q_INVOKABLE QString newId() const;
    Q_INVOKABLE QVariantMap newDefinition(const QString &mode) const;
    Q_INVOKABLE QVariantMap definitionForReading(const QString &id) const;
    Q_INVOKABLE QVariantMap save(const QVariantMap &definition);
    Q_INVOKABLE void remove(const QString &id);
    Q_INVOKABLE void test(const QVariantMap &definition);
    Q_INVOKABLE void cancelTest();
    // Remote calls supply their session lease; local calls use the module's lifetime.
    void runAction(const QString &id, const QString &request, std::shared_ptr<std::atomic_bool> lease = {},
                   bool viaCloud = false);
signals:
    void changed();
    void commandsChanged();
    void previewChanged();
    void configurationChanged();
    void readingsChanged();
    void actionFinished(QString request, bool ok, QString message);
    void diagnostic(QString message);

private:
    struct State {
        QJsonObject result;
        QString lastSuccess;
        qint64 next = 0;
        bool running = false;
        int failures = 0;
        std::shared_ptr<std::atomic_bool> lease = std::make_shared<std::atomic_bool>(true);
    };

    void schedule();
    QVariantList entriesForMode(const QString &mode) const;
    void invalidate(const QString &id);
    void notifyConfiguration();
    QJsonArray sources;
    QHash<QString, State> states;
    std::unique_ptr<BackgroundWorker> readerA, readerB, actions, previewJob;
    std::shared_ptr<std::atomic_bool> previewLease;
    QJsonObject lastPreview;
    QTimer timer;
    QElapsedTimer pollClock;
    qsizetype nextSource = 0;
    bool active = false, controls = false, cloud = false, testMode = false;
};
