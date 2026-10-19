#pragma once
#include <QObject>
#include <QThread>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QVariantMap>
#include <atomic>
#include <memory>
#include <functional>
class BeacnBackend;
class BeacnWorker;
struct BeacnDelivery;

// GUI-thread facade. The worker owns the backend and serializes all native BEACN calls.
class BeacnModule final : public QObject {
    Q_OBJECT
public:
    explicit BeacnModule(QObject *parent = nullptr);
    ~BeacnModule() override;
    // Invalidate earlier work immediately, then queue the new configuration on the worker.
    void configure(bool enabled, const QString &exe, const QString &dll);

    bool busy() const {
        return pending;
    }

    // Send once, rejecting a second command while busy. Lease/epoch changes revoke queued work.
    // Completion runs on this object's thread and is dropped if the module has been destroyed.
    void command(const QJsonObject &args, std::shared_ptr<std::atomic_bool> lease,
                 std::shared_ptr<std::atomic_uint64_t> epoch, quint64 generation,
                 std::function<void(bool, QString)> done);
    static QString validate(const QJsonObject &args, const QVariantList &controls);
signals:
    void snapshot(const QVariantMap &state);

private:
    friend class BeacnWorker;
    friend class ControllerTests;
    QThread *thread;
    BeacnWorker *worker;
    std::shared_ptr<BeacnDelivery> delivery;
    std::shared_ptr<std::atomic_uint64_t> revision = std::make_shared<std::atomic_uint64_t>(0);
    bool pending = false;
    bool enabled = false, stale = false; // Main-thread health watchdog.
    QElapsedTimer lastSnapshot;
};
