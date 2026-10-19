#pragma once
#include "CrashDiagnostics.h"
#include "DiagnosticReport.h"
#include "core/BackgroundWorker.h"
#include <QTimer>
#include <utility>
#include <windows.h>
#include <atomic>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <openssl/crypto.h>

// Activity is redacted and written off the GUI thread; crash capture is native.
class LocalDiagnostics {
public:
    explicit LocalDiagnostics(QString directory) : folder(std::move(directory)) {
        CrashDiagnostics::initialize(reinterpret_cast<LPCWSTR>(folder.utf16()), "WHAT",
                                     QCoreApplication::applicationVersion().toUtf8().constData());
        timer.setInterval(200);
        QObject::connect(&timer, &QTimer::timeout, &timer, [this] { flushActivity(); });
        timer.start();
    }

    void event(QString module, QString message) {
        if (!active.load() || pending.size() >= 256)
            return;
        pending.append({module, message, GetCurrentThreadId()});
        if (message.contains("fail", Qt::CaseInsensitive) || message.contains("error", Qt::CaseInsensitive))
            flushActivity();
    }

    bool setDumpsEnabled(bool enabled) {
        const bool requested = enabled && active.load();
        if (dumpMode == int(requested))
            return true;
        if (!flag("dumps.enabled", requested))
            return false;
        dumpMode = int(requested);
        return true;
    }

    ~LocalDiagnostics() {
        flushActivity();
        if (active.load())
            append("session_closed_cleanly\n");
        active.store(false);
        timer.stop();
        CrashDiagnostics::restore();
        session.reset(); // Queued writes retain and close the handle off the GUI.
    }

    bool setEnabled(bool enabled) {
        if (enabled == active.load())
            return true;
        if (!enabled) {
            append("logging_disabled\n");
            active.store(false);
            pending.clear();
            flag("dumps.enabled", false);
            dumpMode = 0;
            return flag("logging.enabled", false);
        }
        if (!session) {
            QDir().mkpath(folder);
            QDir dir(folder);
            const auto files = dir.entryList({"session-*.log"}, QDir::Files, QDir::Name);
            for (int i = 0; i < files.size() - 4; ++i)
                dir.remove(files[i]);
            const auto path = folder + "/session-" +
                              QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") + ".log";
            const auto file =
                CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), FILE_APPEND_DATA, FILE_SHARE_READ,
                            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;
            session = std::make_shared<Session>(file);
            const auto crashFiles = dir.entryList({"crash-*.txt"}, QDir::Files, QDir::Name);
            for (int i = 0; i < crashFiles.size() - 10; ++i) {
                dir.remove(crashFiles[i]);
                dir.remove(QString(crashFiles[i]).replace(".txt", ".dmp"));
            }
            if (QFileInfo(folder + "/activity.log").size() >= 4 * 1024 * 1024) {
                QFile::remove(folder + "/activity.previous.log");
                QFile::rename(folder + "/activity.log", folder + "/activity.previous.log");
            }
        }
        if (!flag("logging.enabled", true))
            return false;
        active.store(true);
        QJsonObject versions{{"WHAT", QCoreApplication::applicationVersion()},
                             {"Qt", qVersion()},
                             {"Windows", QSysInfo::kernelVersion()},
                             {"architecture", QSysInfo::currentCpuArchitecture()},
                             {"OpenSSL", OpenSSL_version(OPENSSL_VERSION)}};
        append("logging_enabled\n" + QJsonDocument(versions).toJson(QJsonDocument::Compact) + "\n");
        return true;
    }

    void snapshot(const QJsonObject &safeState) {
        if (!active.load())
            return;
        append(QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toUtf8() + " " +
               QJsonDocument(safeState).toJson(QJsonDocument::Compact) + "\n");
    }

private:
    struct Session {
        explicit Session(HANDLE value) : file(value) {}

        ~Session() {
            CloseHandle(file);
        }

        HANDLE file;
    };

    void append(const QByteArray &bytes) {
        constexpr quint64 limit = 256 * 1024;
        if (!session || written >= limit)
            return;
        const auto bounded = bytes.left(qsizetype(limit - written));
        written += quint64(bounded.size()); // Bound queued bytes as well as disk usage.
        sessionWorker.post([state = session, bounded] {
            DWORD count = 0;
            WriteFile(state->file, bounded.constData(), DWORD(bounded.size()), &count, nullptr);
            FlushFileBuffers(state->file);
        });
    }

    bool flag(const QString &name, bool enabled) {
        const auto path = folder + "/" + name;
        if (!enabled)
            return !QFile::exists(path) || QFile::remove(path);
        if (QFile::exists(path))
            return true;
        QFile marker(path);
        return marker.open(QIODevice::WriteOnly);
    }

    void flushActivity() {
        if (activityWorker.isRunning() || pending.isEmpty())
            return;
        const auto batch = std::exchange(pending, {});
        const auto directory = folder.toStdWString();
        activityWorker.submit(
            [batch, directory] {
                for (const auto &entry : batch) {
                    const auto safe = DiagnosticReport::activity(entry.message).toUtf8();
                    const auto level = entry.message.contains("fail", Qt::CaseInsensitive) ||
                                               entry.message.contains("error", Qt::CaseInsensitive) ||
                                               entry.message.contains("reject", Qt::CaseInsensitive)
                                           ? "ERROR"
                                           : "INFO";
                    CrashDiagnostics::appendAt(directory.c_str(), entry.module.toUtf8().constData(), level,
                                               safe.constData(), entry.thread);
                }
                return true;
            },
            [](bool) {});
    }

    QTimer timer;
    BackgroundWorker activityWorker{"Diagnostic activity writer"};

    struct Entry {
        QString module, message;
        DWORD thread;
    };

    QList<Entry> pending;
    int dumpMode = -1;
    QString folder;
    quint64 written = 0;
    std::shared_ptr<Session> session;
    BackgroundWorker sessionWorker{"Diagnostic session writer"};
    inline static std::atomic_bool active{false};
};
