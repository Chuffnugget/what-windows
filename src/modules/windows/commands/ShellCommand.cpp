#include "modules/windows/commands/ShellCommand.h"
#include <QStringDecoder>
#include <QDir>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QFile>
#include <QElapsedTimer>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <windows.h>
#include <cmath>
#include <vector>

namespace {
struct Handle {
    HANDLE value = nullptr;

    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};

QString quote(QString value) {
    QString result = "\"";
    int slashes = 0;
    for (QChar ch : value) {
        if (ch == '\\') {
            ++slashes;
            continue;
        }
        result += QString(slashes * (ch == '\"' ? 2 : 1), '\\');
        slashes = 0;
        if (ch == '\"')
            result += '\\';
        result += ch;
    }
    return result + QString(slashes * 2, '\\') + '\"';
}

QJsonObject error(QString message) {
    return {{"error", message}};
}

QString winError(const char *operation) {
    return QString("%1 failed (Windows error %2)").arg(operation).arg(GetLastError());
}

void drain(HANDLE pipe, QByteArray &output, bool &truncated, const std::function<void(QByteArray)> &stream) {
    // Bound each drain pass so a noisy child cannot postpone cancellation.
    for (int pass = 0; pass < 16; ++pass) {
        DWORD available = 0, read = 0;
        char buffer[4096];
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) || !available)
            break;
        if (!ReadFile(pipe, buffer, qMin<DWORD>(available, sizeof(buffer)), &read, nullptr) || !read)
            break;
        const auto keep = qMin<qsizetype>(read, 65536 - output.size());
        output.append(buffer, keep);
        if (keep && stream)
            stream(QByteArray(buffer, keep));
        if (keep < read)
            truncated = true;
    }
}
} // namespace

QString ShellCommand::validate(const QJsonObject &args) {
    const QSet<QString> keys{"shell", "command", "timeout", "distribution", "leave_running"};
    for (auto it = args.begin(); it != args.end(); ++it)
        if (!keys.contains(it.key()))
            return "Unexpected shell argument";
    const auto shell = args["shell"].toString();
    if (!QStringList{"powershell", "pwsh", "cmd", "wsl"}.contains(shell))
        return "Choose powershell, pwsh, cmd or wsl";
    if (args.contains("leave_running") && !args["leave_running"].isBool())
        return "Leave running must be true or false";
    if (shell == "wsl" && args["leave_running"].toBool())
        return "Leave running is supported by Windows shells, not WSL";
    const auto command = args["command"].toString();
    if (command.trimmed().isEmpty() || command.size() > 2048 || command.contains(QChar(0)))
        return "Command must contain 1–2048 characters without NUL";
    const auto timeout = args["timeout"];
    if (!timeout.isDouble() || !std::isfinite(timeout.toDouble()) ||
        timeout.toDouble() != std::floor(timeout.toDouble()) || timeout.toDouble() < 1 ||
        timeout.toDouble() > 300)
        return "Timeout must be 1–300 whole seconds";
    if (args.contains("distribution")) {
        const auto distro = args["distribution"].toString();
        if (shell != "wsl" || distro.isEmpty() || distro.size() > 128 || distro.startsWith('-'))
            return "Distribution is only valid for WSL";
        for (auto ch : distro)
            if (ch.unicode() < 32 || ch.unicode() == 127)
                return "Invalid distribution name";
    }
    return {};
}

QJsonObject ShellCommand::run(const QJsonObject &args, const std::function<bool()> &cancelled,
                              const std::function<void(bool, const QString &)> &stream) {
    const auto invalid = validate(args);
    if (!invalid.isEmpty())
        return error(invalid);
    if (cancelled())
        return error("Command cancelled before launch");
    const auto shell = args["shell"].toString(), command = args["command"].toString();
    const int timeout = args["timeout"].toInt();
    const QString system = qEnvironmentVariable("SystemRoot", "C:/Windows") + "/System32/";
    QString executable, line;
    QTemporaryDir temporary;
    if (!temporary.isValid())
        return error("Cannot create temporary command directory");
    if (shell == "cmd") {
        executable = system + "cmd.exe";
        QFile script(temporary.filePath("command.cmd"));
        if (!script.open(QIODevice::WriteOnly))
            return error("Cannot create temporary CMD script");
        const QByteArray content = "@echo off\r\nchcp 65001 >nul\r\n" + command.toUtf8() + "\r\n";
        if (script.write(content) != content.size())
            return error("Cannot write temporary CMD script");
        script.close();
        line = quote(QDir::toNativeSeparators(executable)) + " /d /s /c \"" +
               quote(QDir::toNativeSeparators(script.fileName())) + "\"";
    } else {
        QStringList arguments;
        if (shell == "wsl") {
            executable = system + "wsl.exe";
            if (args.contains("distribution"))
                arguments << "--distribution" << args["distribution"].toString();
            // Guest-side deadline remains active even if the Windows client disconnects.
            arguments << "--exec" << "timeout" << "--signal=TERM" << "--kill-after=2s"
                      << QString::number(timeout) + "s" << "sh" << "-lc" << command;
        } else {
            executable = shell == "pwsh" ? QStandardPaths::findExecutable("pwsh.exe")
                                         : system + "WindowsPowerShell/v1.0/powershell.exe";
            const QString script =
                "[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false); $OutputEncoding = [Console]::OutputEncoding;\n" +
                command;
            const QByteArray encoded(reinterpret_cast<const char *>(script.utf16()), script.size() * 2);
            arguments << "-NoLogo" << "-NoProfile" << "-NonInteractive" << "-EncodedCommand"
                      << QString::fromLatin1(encoded.toBase64());
        }
        line = quote(executable);
        for (const auto &argument : arguments)
            line += ' ' + quote(argument);
    }
    if (executable.isEmpty() || !QFile::exists(executable))
        return error("Selected shell is not installed or could not be found");
    Handle outRead, outWrite, errRead, errWrite, input, job, process, thread;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    if (!CreatePipe(&outRead.value, &outWrite.value, &security, 0) ||
        !CreatePipe(&errRead.value, &errWrite.value, &security, 0))
        return error(winError("Create output pipes"));
    SetHandleInformation(outRead.value, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead.value, HANDLE_FLAG_INHERIT, 0);
    input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                              OPEN_EXISTING, 0, nullptr);
    if (input.value == INVALID_HANDLE_VALUE)
        return error(winError("Open null input"));
    job.value = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.value ||
        !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return error(winError("Create command job"));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdOutput = outWrite.value;
    startup.StartupInfo.hStdError = errWrite.value;
    startup.StartupInfo.hStdInput = input.value;
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes))
        return error(winError("Initialize command handles"));

    struct Attributes {
        LPPROC_THREAD_ATTRIBUTE_LIST value;

        ~Attributes() {
            DeleteProcThreadAttributeList(value);
        }
    } attributes{startup.lpAttributeList};

    HANDLE inherited[]{input.value, outWrite.value, errWrite.value};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                   sizeof(inherited), nullptr, nullptr))
        return error(winError("Restrict command handles"));
    PROCESS_INFORMATION info{};
    auto native = line.toStdWString();
    const auto app = executable.toStdWString(),
               cwd = QDir::toNativeSeparators(QDir::homePath()).toStdWString();
    if (!CreateProcessW(app.c_str(), native.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                        cwd.c_str(), &startup.StartupInfo, &info))
        return error(winError("Start shell"));
    process.value = info.hProcess;
    thread.value = info.hThread;
    if (!AssignProcessToJobObject(job.value, process.value)) {
        const auto message = winError("Contain command process");
        TerminateProcess(process.value, 1);
        return error(message);
    }
    CloseHandle(outWrite.value);
    outWrite.value = nullptr;
    CloseHandle(errWrite.value);
    errWrite.value = nullptr;
    if (cancelled()) {
        TerminateJobObject(job.value, 1);
        return error("Command cancelled before execution");
    }
    if (ResumeThread(thread.value) == DWORD(-1))
        return error(winError("Resume shell"));
    QStringDecoder outDecoder(QStringDecoder::Utf8), errDecoder(QStringDecoder::Utf8);
    auto outStream = [&](QByteArray bytes) {
        if (stream)
            stream(false, outDecoder(bytes));
    };
    auto errStream = [&](QByteArray bytes) {
        if (stream)
            stream(true, errDecoder(bytes));
    };
    QElapsedTimer clock;
    clock.start();
    QByteArray stdoutData, stderrData;
    bool truncated = false, timedOut = false, wasCancelled = false;
    while (true) {
        drain(outRead.value, stdoutData, truncated, outStream);
        drain(errRead.value, stderrData, truncated, errStream);
        if (WaitForSingleObject(process.value, 0) == WAIT_OBJECT_0)
            break;
        wasCancelled = cancelled();
        timedOut = clock.elapsed() >= (timeout + (shell == "wsl" ? 3 : 0)) * 1000;
        if (wasCancelled || timedOut) {
            TerminateJobObject(job.value, 1);
            WaitForSingleObject(process.value, 1000);
            break;
        }
        QThread::msleep(10);
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process.value, &exitCode);
    wasCancelled = wasCancelled || cancelled();
    if (args["leave_running"].toBool() && exitCode == 0 && !timedOut && !wasCancelled) {
        // Only a successful command may release its children. Until this point,
        // timeout, permission revocation and cancellation still kill the whole job.
        limits.BasicLimitInformation.LimitFlags = 0;
        if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            return error(winError("Leave launched applications running"));
    } else {
        TerminateJobObject(job.value, 1);
    }
    drain(outRead.value, stdoutData, truncated, outStream);
    drain(errRead.value, stderrData, truncated, errStream);
    if (shell == "wsl" && exitCode == 124)
        timedOut = true;
    return {{"stdout", QString::fromUtf8(stdoutData)},
            {"stderr", QString::fromUtf8(stderrData)},
            {"exit_code", double(exitCode)},
            {"timed_out", timedOut},
            {"cancelled", wasCancelled},
            {"truncated", truncated}};
}
