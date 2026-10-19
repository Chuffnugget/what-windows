#pragma once
#include "core/BackgroundWorker.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QVariantList>
#include <atomic>

// Owns WHAT's announcement player. It does not change another application's
// media session or the Windows master volume.
class Announcements : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList voices READ voices NOTIFY changed)
public:
    explicit Announcements(bool testing, QObject *parent = nullptr);
    ~Announcements() override;
    static QString validate(const QString &action, const QJsonObject &args);
    void execute(QString action, QJsonObject args, QString request,
                 std::shared_ptr<std::atomic_bool> connection = {});
    void setPermitted(bool permitted);
    QJsonObject state() const;

    QVariantList voices() const {
        return voiceList.toVariantList();
    }

signals:
    void completed(QString request, bool ok, QString message);
    void changed();

private:
    struct Native;
    std::shared_ptr<Native> native;
    std::shared_ptr<std::atomic_uint64_t> epoch = std::make_shared<std::atomic_uint64_t>(0);
    QTimer timer;
    QJsonObject playback;
    QJsonArray voiceList;
    bool permitted = false, testing;
    BackgroundWorker worker{"Speech and audio"};
};
