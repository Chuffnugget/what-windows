#pragma once
#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QVariantMap>
#include <QSet>
#include <QTimer>
#include <QElapsedTimer>
#include <functional>
#include <memory>
#include <map>
#include <QStringList>

namespace OptionalWindows {
QStringList keys();
bool reportedInUse(quint64 start, quint64 stop);
QString featureForId(const QString &id);
int scopeForId(const QString &id);
bool permitted(const QString &id, const QJsonObject &options);

struct Result {
    QJsonArray rows;
    QVariantMap meters;
    QString error;
};

// Construct, collect and destroy on the same worker. Owns COM and WinRT handles.
class Backend {
public:
    Backend();
    ~Backend();
    Result collect(const QString &feature, const QJsonObject &options);

private:
    struct State;
    std::unique_ptr<State> state;
};
} // namespace OptionalWindows

// Optional sources create workers only after explicit opt-in. Each slow API has
// its own queue so a printer or Windows Update query cannot hold up audio meters.
class OptionalWindowsSources : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap meters READ meters NOTIFY metersChanged)
    Q_PROPERTY(QVariantMap issues READ issues NOTIFY issuesChanged)
public:
    explicit OptionalWindowsSources(QObject *parent = nullptr);
    ~OptionalWindowsSources() override;
    void configure(const QJsonObject &options, bool enabled, bool testMode, int publishMs);

    QVariantMap meters() const {
        return meterValues;
    }

    QVariantMap issues() const {
        return issueValues;
    }

    Q_INVOKABLE void watchMeter(const QString &id, bool visible);
    Q_INVOKABLE void requestLocationAccess();
signals:
    void readingsReady(const QString &feature, const QJsonArray &rows);
    void metersChanged();
    void issuesChanged();

private:
    friend class ControllerTests;
    struct Job;
    std::map<QString, std::unique_ptr<Job>> jobs;
    QJsonObject options;
    QVariantMap meterValues, issueValues;
    QSet<QString> watched;
    QTimer timer;
    QElapsedTimer clock;
    quint64 generation = 0;
    int publishMs = 500;
    qint64 lastAudioPublish = -1;
    bool testMode = false;
    // Tests inject data without enabling real microphones, location or radios.
    std::function<OptionalWindows::Result(const QString &, const QJsonObject &)> testCollector;
    void tick();
    void refreshLocation();
    void updateTimer();
    void setIssue(const QString &feature, const QString &message);
};
