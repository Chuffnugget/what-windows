#pragma once
#include <QObject>
#include <QThread>
#include <QMutex>
#include <QMutexLocker>
#include <memory>
#include <functional>
#include <optional>
#include <exception>

// Runs jobs on its own long-lived thread. The work lambda gets copies of what it
// needs (never the Controller), so it's safe to destroy the owner while a job is
// still running: the thread is left to finish rather than being killed.
// Call the public methods from the owner's thread; only the work runs elsewhere.
class BackgroundWorker : public QObject {
public:
    explicit BackgroundWorker(QString name, QObject *parent = nullptr) : QObject(parent) {
        delivery->owner = this;
        thread = new QThread;
        thread->setObjectName(name);
        context = new QObject;
        context->moveToThread(thread);
        connect(thread, &QThread::finished, context, &QObject::deleteLater);
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
    }

    ~BackgroundWorker() override {
        {
            QMutexLocker lock(&delivery->mutex);
            delivery->owner = nullptr;
        }
        thread->requestInterruption();
        // Queue the quit behind any pending jobs, so final writes still happen.
        // Calling quit() directly could drop them if a slow OS call is in progress.
        QMetaObject::invokeMethod(
            context, [workerThread = thread] { workerThread->quit(); }, Qt::QueuedConnection);
        // Don't block shutdown on an OS call we can't cancel. The job keeps its own
        // copies alive until it returns, and its result is simply discarded.
        thread->wait(250);
    }

    // Queue a write in FIFO order. The caller must bound the queue and own all captured state.
    // Unlike submit(), post() is not covered by isRunning() and has no completion callback.
    template <class Work> void post(Work work) {
        QMetaObject::invokeMethod(
            context,
            [work = std::move(work)] {
                try {
                    work();
                } catch (...) {
                    qWarning("Background write failed");
                }
            },
            Qt::QueuedConnection);
    }

    bool isRunning() const {
        return busy;
    }

    std::function<void(QString)> onStatus;

    // Accept one job at a time; a submission while busy is ignored. Work returns a value
    // on the worker thread and transfers ownership to done. Done and onStatus run
    // on this object's thread, with busy cleared.
    // Destruction suppresses delivery, so done may capture the owner but work must not.
    template <class Work, class Done> void submit(Work work, Done done) {
        if (busy)
            return;
        busy = true;
        auto gate = delivery;
        QMetaObject::invokeMethod(
            context,
            [gate, work, done] {
                std::optional<decltype(work())> result;
                QString error;
                try {
                    result.emplace(work());
                } catch (const std::exception &e) {
                    error = QString::fromUtf8(e.what()).left(1000);
                } catch (...) {
                    error = "Unknown worker exception";
                }
                if (!result && error.isEmpty())
                    error = "Worker failed without an error message";
                QMutexLocker lock(&gate->mutex);
                if (auto *owner = gate->owner)
                    QMetaObject::invokeMethod(
                        owner,
                        [owner, result = std::move(result), done, error]() mutable {
                            owner->busy = false;
                            if (owner->onStatus)
                                owner->onStatus(error);
                            if (result)
                                done(std::move(*result));
                        },
                        Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    }

private:
    struct Delivery {
        QMutex mutex;
        BackgroundWorker *owner = nullptr;
    };

    std::shared_ptr<Delivery> delivery = std::make_shared<Delivery>();
    QThread *thread;
    QObject *context;
    bool busy = false;
};
