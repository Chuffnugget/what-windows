#include "AppRuntime.h"
#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <windows.h>
#include <psapi.h>
#include <limits>
#include <iterator>

double AppUsage::cpuPercent(quint64 cpuTicks, qint64 elapsedNs, unsigned processors) {
    if (elapsedNs <= 0 || processors == 0)
        return 0;
    // GetProcessTimes counts 100 ns units across all cores. Match Task Manager's 0-100% scale.
    return qBound(0.0, double(cpuTicks) * 10000.0 / double(elapsedNs) / processors, 100.0);
}

QJsonObject AppUsage::sample() {
    QJsonObject result{{"cpu_percent", QJsonValue::Null}, {"memory_bytes", QJsonValue::Null}};
    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        auto ticks = [](FILETIME value) {
            return (quint64(value.dwHighDateTime) << 32) | value.dwLowDateTime;
        };
        const quint64 total = ticks(kernel) + ticks(user);
        if (clock.isValid() && total >= previousCpu)
            result["cpu_percent"] = cpuPercent(total - previousCpu, clock.nsecsElapsed(),
                                               GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
        previousCpu = total;
        clock.start();
    } else {
        clock.invalidate();
    }
    PROCESS_MEMORY_COUNTERS memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
        result["memory_bytes"] = double(memory.WorkingSetSize);
    return result;
}

QString AppRestart::launch(const QString &executable, QStringList arguments,
                           const QString &workingDirectory) {
    // Do not carry the previous handoff PID through successive restarts.
    for (qsizetype i = 0; i < arguments.size();) {
        if (arguments[i] == "--restart-after") {
            arguments.removeAt(i);
            if (i < arguments.size())
                arguments.removeAt(i);
        } else if (arguments[i].startsWith("--restart-after=")) {
            arguments.removeAt(i);
        } else {
            ++i;
        }
    }
    arguments << "--restart-after" << QString::number(QCoreApplication::applicationPid());
    return QProcess::startDetached(executable, arguments, workingDirectory)
               ? QString()
               : QStringLiteral("Windows could not start the replacement WHAT process");
}

bool AppRestart::waitForPrevious(qint64 processId, unsigned timeoutMs) {
    if (processId <= 0 || processId > std::numeric_limits<DWORD>::max())
        return false;
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(processId));
    if (!process)
        return GetLastError() == ERROR_INVALID_PARAMETER; // The original already exited.
    wchar_t path[32768];
    DWORD length = DWORD(std::size(path));
    const bool sameProgram =
        QueryFullProcessImageNameW(process, 0, path, &length) &&
        QDir::fromNativeSeparators(QString::fromWCharArray(path, int(length)))
                .compare(QCoreApplication::applicationFilePath(), Qt::CaseInsensitive) == 0;
    const bool finished = sameProgram && WaitForSingleObject(process, timeoutMs) == WAIT_OBJECT_0;
    CloseHandle(process);
    return finished;
}
