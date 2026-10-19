#pragma once
#include "core/BackgroundWorker.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <atomic>

// Native actionable notifications are available in the installed MSIX app.
class RichNotifications : public QObject {
    Q_OBJECT
public:
    explicit RichNotifications(bool testing, QObject *parent = nullptr);
    ~RichNotifications() override;
    static QString validate(const QJsonObject &args);
    void setPermitted(bool enabled);
    void prepareActivation();
    void send(QJsonObject args, QString request, std::shared_ptr<std::atomic_bool> lease);
    QJsonObject state() const;
signals:
    void completed(QString request, bool ok, QString message);
    void changed();
    void openRequested(bool expired);

private:
    struct Mailbox;
    struct Native;
    std::shared_ptr<Mailbox> mailbox;
    std::shared_ptr<Native> native;
    QTimer timer;
    QJsonArray events;
    bool testing;
    bool activationPrepared = false;
    BackgroundWorker worker{"Windows notifications"};
};
