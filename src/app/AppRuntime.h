#pragma once
#include <QElapsedTimer>
#include <QJsonObject>
#include <QStringList>

// Used only by the app diagnostics worker. Samples the current process, not its helpers.
class AppUsage {
public:
    QJsonObject sample();
    static double cpuPercent(quint64 cpuTicks, qint64 elapsedNs, unsigned processors);

private:
    QElapsedTimer clock;
    quint64 previousCpu = 0;
};

namespace AppRestart {
QString launch(const QString &executable, QStringList arguments, const QString &workingDirectory);
// The replacement waits before opening the profile or taking its single-instance lock.
bool waitForPrevious(qint64 processId, unsigned timeoutMs = 30000);
} // namespace AppRestart
