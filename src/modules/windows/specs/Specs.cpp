#include "app/Controller.h"
#include "modules/windows/specs/SpecsFallback.h"
#include "modules/windows/specs/SpecsDisplays.h"
#include "modules/windows/specs/SpecsHardware.h"
#include "modules/windows/specs/SpecsIssues.h"
#include "modules/windows/displays/Displays.h"
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QLocale>
#include <QProcess>
#include <windows.h>

void Controller::copySpecs() {
    QApplication::clipboard()->setText(specsText);
}

void Controller::refreshSpecs() {
    if (specsBusy)
        return;
    specsLoaded = true;
    specsBusy = true;
    specsCollectionNotice.clear();
    specsCollectionIssues.clear();
    emit specsChanged(); // Keep the last report readable while refreshing.

    // PowerShell owns the CIM work. Native driver queries and report preparation
    // also run off the GUI thread, including when CIM fails or times out.
    const auto collectNative = [this](QString output, QString failure) {
        if (output.isEmpty() && !failure.isEmpty() && !specsRefreshTime.isEmpty()) {
            specsBusy = false;
            specsCollectionNotice = failure + " The previous report is still displayed.";
            specsCollectionIssues = {failure};
            log(specsCollectionNotice);
            emit specsChanged();
            return;
        }
        using Report = QPair<QString, QStringList>;
        auto *watcher = new QFutureWatcher<Report>(this);
        connect(watcher, &QFutureWatcher<Report>::finished, this, [this, watcher, failure] {
            const auto result = watcher->result();
            specsText = result.first;
            specsCollectionIssues = result.second;
            if (!failure.isEmpty())
                specsCollectionIssues.prepend(failure);
            specsBusy = false;
            specsRefreshTime = QLocale().toString(QDateTime::currentDateTime(), QLocale::ShortFormat);
            specsCollectionNotice = failure;
            if (specsCollectionNotice.isEmpty() && !specsCollectionIssues.isEmpty())
                specsCollectionNotice =
                    "Some details are unavailable";
            log(specsCollectionNotice.isEmpty()
                    ? "Computer specification collection finished"
                    : "Computer specifications collected with unavailable details");
            emit specsChanged();
            watcher->deleteLater();
        });
        watcher->setFuture(QtConcurrent::run([output] {
            QString report = output;
            try {
                if (report.isEmpty())
                    report = nativeSpecifications() + "\n\nSome Windows inventory details were unavailable.";
                report += "\n\n" + graphicsSpecifications();
                report += "\n\n" + displaySpecifications(Displays::query(true));
                report += "\n\n" + firmwareSpecifications();
            } catch (const std::exception &) {
                report += "\n\nFurther native hardware details unavailable.";
            }
            return Report{report, specificationIssues(report)};
        }));
    };

    QFile inventory(":/src/modules/windows/specs/SpecsInventory.ps1");
    if (!inventory.open(QIODevice::ReadOnly)) {
        collectNative({}, "The Windows inventory script could not be loaded.");
        return;
    }
    const auto script = QString::fromUtf8(inventory.readAll());
    auto *process = new QProcess(this);
    process->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) { args->flags |= CREATE_NO_WINDOW; });
    auto *timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, [process] {
        process->setProperty("timedOut", true);
        process->kill();
    });
    connect(process, &QProcess::errorOccurred, this, [process, collectNative](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        collectNative({}, "Windows specification collection could not start.");
        process->deleteLater();
    });
    connect(process, &QProcess::finished, this,
            [process, timeout, collectNative](int code, QProcess::ExitStatus status) {
                timeout->stop();
                QString failure;
                if (process->property("timedOut").toBool())
                    failure = "Windows specification collection timed out. Refresh to retry.";
                else if (status != QProcess::NormalExit || code != 0)
                    failure = "Windows could not finish collecting specifications. Refresh to retry.";
                const auto output = QString::fromUtf8(process->readAllStandardOutput()).trimmed();
                if (output.isEmpty() && failure.isEmpty())
                    failure = "Windows returned no specification data. Refresh to retry.";
                collectNative(output, failure);
                process->deleteLater();
            });
    const QString executable =
        qEnvironmentVariable("SystemRoot", "C:/Windows") + "/System32/WindowsPowerShell/v1.0/powershell.exe";
    process->start(executable, {"-NoLogo", "-NoProfile", "-NonInteractive", "-Command", script});
    timeout->start(25000);
}
