#pragma once
#include <QThread>
#include <memory>
#include <utility>

// Final disposal starts only after every job/completion has released its shared
// reference. A slow vendor destructor owns its state until it returns, without
// blocking the GUI or terminating a thread during a native call.
template <class T, class... Args> std::shared_ptr<T> makeAsyncDisposed(Args &&...args) {
    return std::shared_ptr<T>(new T{std::forward<Args>(args)...}, [](T *value) {
        auto *thread = QThread::create([state = std::unique_ptr<T>(value)]() mutable { state.reset(); });
        thread->setObjectName("Native resource disposal");
        QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
    });
}
