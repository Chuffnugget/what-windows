#pragma once
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <functional>

class DynamicLighting : public QObject {
    Q_OBJECT
public:
    explicit DynamicLighting(QObject *parent = nullptr);
    ~DynamicLighting() override;
    void setEnabled(bool enabled, bool testing = false);
    void refresh();
    void apply(QJsonObject command, std::function<void(bool, QString)> done);

    QJsonObject state() const {
        return data;
    }

    static QString validate(const QJsonObject &command);
signals:
    void changed();

private:
    friend class ControllerTests;
    void send(QJsonObject request, std::function<void(bool, QString)> done = {});
    void finish(bool ok, const QString &message);
    void stop(bool shuttingDown = false);
    QProcess *process = nullptr;
    QTimer poll, deadline;
    QByteArray buffer;
    QJsonObject data{{"devices", QJsonArray{}}, {"status", "Disabled"}};
    QString pending;
    bool enabled = false;
    std::function<void(bool, QString)> completion;
};
