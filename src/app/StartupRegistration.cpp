#include "StartupRegistration.h"
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>

namespace StartupRegistration {
bool supported() {
#ifdef QT_DEBUG
    return false;
#else
    return true;
#endif
}

bool isWhatCommand(const QString &command, const QString &family) {
    const auto args = QProcess::splitCommand(command);
    if (args.isEmpty())
        return false;
    const auto program = QFileInfo(args.first()).fileName();
    if (program.compare("WHAT.exe", Qt::CaseInsensitive) == 0)
        return args.size() == 1;
    return !family.isEmpty() && program.compare("explorer.exe", Qt::CaseInsensitive) == 0 &&
           args.size() == 2 && args[1] == "shell:AppsFolder\\" + family + "!App";
}

State update(QSettings &run, const QString &executable, const QString &family,
             std::optional<bool> enabled) {
    const auto previous = run.value("WHAT").toString();
    const auto command = '"' + QDir::toNativeSeparators(executable) + '"';
    if (!supported()) {
        // Debug builds rely on the development environment to find Qt DLLs.
        // Remove only this executable's old entry, leaving Release startup alone.
        if (previous.compare(command, Qt::CaseInsensitive) == 0) {
            run.remove("WHAT");
            run.sync();
        }
        if (run.status() != QSettings::NoError)
            return {false, "Could not remove this Debug build's Windows startup entry."};
        return {false, enabled.value_or(false)
            ? QString("Windows startup is disabled in Debug builds. Use a Release build.") : QString()};
    }
    const bool legacy = isWhatCommand(previous, family);
    if (family.isEmpty()) {
        if (enabled) {
            if (*enabled)
                run.setValue("WHAT", command);
            else if (legacy)
                run.remove("WHAT");
            run.sync();
        }
        return {run.value("WHAT").toString() == command,
                run.status() == QSettings::NoError ? QString() : "Could not save Windows startup setting"};
    }

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
        using namespace winrt::Windows::ApplicationModel;
        const auto task = StartupTask::GetAsync(L"WHATStartup").get();
        auto state = task.State();
        const bool requestEnable = enabled.value_or(legacy);
        if (requestEnable && state == StartupTaskState::Disabled)
            state = task.RequestEnableAsync().get();
        else if (enabled && !*enabled && state == StartupTaskState::Enabled) {
            task.Disable();
            state = task.State();
        }
        // Once Windows owns startup, remove the old path even when the user has
        // disabled startup in Task Manager. A Run entry must not bypass that choice.
        if (legacy) {
            run.remove("WHAT");
            run.sync();
        }
        const bool active = state == StartupTaskState::Enabled || state == StartupTaskState::EnabledByPolicy;
        QString error;
        if (requestEnable && state == StartupTaskState::DisabledByUser)
            error = "Windows has disabled WHAT at sign-in. Enable it in Windows Settings > Apps > Startup.";
        else if (enabled && active != *enabled)
            error = "Windows policy prevented changing WHAT's startup setting.";
        else if (run.status() != QSettings::NoError)
            error = "Windows startup is configured, but the previous Run entry could not be removed.";
        return {active, error};
    } catch (const winrt::hresult_error &error) {
        return {legacy, "Windows startup could not be configured: " + QString::fromWCharArray(error.message().c_str())};
    }
}
} // namespace StartupRegistration
