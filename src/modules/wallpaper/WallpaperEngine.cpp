#include "WallpaperEngine.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QProcess>
#include <QSet>
#include <windows.h>
#include <tlhelp32.h>
#include <cmath>

namespace {
bool validExe(const QString &path) {
    const QFileInfo f(path);
    return f.isFile() && QStringList{"wallpaper32.exe", "wallpaper64.exe"}.contains(f.fileName().toLower());
}

QStringList processes() {
    QStringList paths;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return paths;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    if (Process32FirstW(snap, &e))
        do {
            if (!QStringList{"wallpaper32.exe", "wallpaper64.exe"}.contains(
                    QString::fromWCharArray(e.szExeFile).toLower()))
                continue;
            HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e.th32ProcessID);
            if (proc) {
                wchar_t buffer[32768];
                DWORD size = 32768;
                if (QueryFullProcessImageNameW(proc, 0, buffer, &size))
                    paths << QDir::fromNativeSeparators(QString::fromWCharArray(buffer, int(size)));
                CloseHandle(proc);
            }
        } while (Process32NextW(snap, &e));
    CloseHandle(snap);
    return paths;
}
} // namespace

QString WallpaperEngine::discover(const QString &overridePath) {
    if (!overridePath.trimmed().isEmpty())
        return validExe(overridePath) ? QFileInfo(overridePath).absoluteFilePath() : QString();
    for (const auto &path : processes())
        if (validExe(path))
            return path;
    QStringList roots;
    for (const auto &key : {QString("HKEY_CURRENT_USER\\Software\\Valve\\Steam"),
                            QString("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Valve\\Steam"),
                            QString("HKEY_LOCAL_MACHINE\\SOFTWARE\\Valve\\Steam")}) {
        QSettings registry(key, QSettings::NativeFormat);
        for (const auto &value : {"SteamPath", "InstallPath"}) {
            auto path = registry.value(value).toString();
            if (!path.isEmpty())
                roots << QDir::fromNativeSeparators(path);
        }
    }
    roots << qEnvironmentVariable("ProgramFiles(x86)") + "/Steam"
          << qEnvironmentVariable("ProgramFiles") + "/Steam";
    for (const auto &drive : QDir::drives())
        roots << drive.absoluteFilePath() + "Steam" << drive.absoluteFilePath() + "SteamLibrary";
    const auto initial = roots;
    for (const auto &root : initial) {
        QFile f(root + "/steamapps/libraryfolders.vdf");
        if (!f.open(QIODevice::ReadOnly) || f.size() > 1024 * 1024)
            continue;
        const auto text = QString::fromUtf8(f.readAll());
        QRegularExpression re("\\\"path\\\"\\s*\\\"([^\\\"]+)\\\"");
        auto it = re.globalMatch(text);
        while (it.hasNext()) {
            auto path = it.next().captured(1);
            path.replace("\\\\", "\\");
            roots << QDir::fromNativeSeparators(path);
        }
    }
    roots.removeDuplicates();
    for (const auto &root : roots)
        for (const auto &exe : {"wallpaper64.exe", "wallpaper32.exe"}) {
            auto path = root + "/steamapps/common/wallpaper_engine/" + exe;
            if (validExe(path))
                return path;
        }
    return {};
}

bool WallpaperEngine::running(const QString &exe) {
    for (const auto &path : processes())
        if (QFileInfo(path).absolutePath().compare(QFileInfo(exe).absolutePath(), Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

QString WallpaperEngine::validate(const QJsonObject &a, bool checkFiles) {
    const auto op = a["operation"].toString();
    const QSet<QString> basic{"pause", "stop", "play", "mute", "unmute", "hideIcons", "showIcons"};
    const QSet<QString> targeted{"openWallpaper", "nextWallpaper",   "closeWallpaper",
                                 "openPlaylist",  "applyProperties", "getWallpaper"};
    if (!basic.contains(op) && !targeted.contains(op) && op != "openProfile")
        return "Unknown Wallpaper Engine operation";
    QSet<QString> keys{"operation"};
    if (targeted.contains(op))
        keys.unite({"monitor", "location"});
    if (op == "openWallpaper")
        keys.unite({"file", "preset", "playInWindow", "width", "height", "x", "y", "activate", "borderless"});
    if (op == "openPlaylist")
        keys.insert("playlist");
    if (op == "openProfile")
        keys.insert("profile");
    if (op == "applyProperties")
        keys.insert("properties");
    for (auto it = a.begin(); it != a.end(); ++it) {
        if (!keys.contains(it.key()))
            return "Argument is not supported for this operation";
        if (it.value().isString() &&
            (it.value().toString().size() > 4096 || it.value().toString().contains(QChar(0)) ||
             it.value().toString().contains('\n') || it.value().toString().contains('\r')))
            return "Invalid text argument";
    }
    if (a.contains("monitor") && a.contains("location"))
        return "Choose a monitor index or location, not both";
    for (const auto &k : {"monitor", "width", "height", "x", "y"})
        if (a.contains(k)) {
            auto v = a[k];
            if (!v.isDouble() || !std::isfinite(v.toDouble()) || v.toDouble() != v.toInt())
                return "Expected a whole-number monitor or window coordinate";
            const int n = v.toInt();
            if (QString(k) == "monitor"                             ? (n < 0 || n > 255)
                : (QString(k) == "width" || QString(k) == "height") ? (n < 1 || n > 32768)
                                                                    : (n < -65536 || n > 65536))
                return "Monitor or window coordinate outside supported range";
        }
    for (const auto &k : {"activate", "borderless"})
        if (a.contains(k) && !a[k].isBool())
            return "Expected a boolean window option";
    for (const auto &k : {"location", "file", "playlist", "profile", "preset", "playInWindow"})
        if (a.contains(k) && (!a[k].isString() || a[k].toString().trimmed().isEmpty()))
            return "Expected a non-empty text argument";
    if (op == "openWallpaper" && (!a.contains("file") || !QFileInfo(a["file"].toString()).isAbsolute() ||
                                  (checkFiles && !QFileInfo(a["file"].toString()).isFile())))
        return "Choose an existing absolute wallpaper file on this PC";
    if (op == "openPlaylist" && !a.contains("playlist"))
        return "Playlist name required";
    if (op == "openProfile" && !a.contains("profile"))
        return "Profile name required";
    for (const auto &k : {"width", "height", "x", "y", "activate", "borderless"})
        if (a.contains(k) && !a.contains("playInWindow"))
            return "Window options require a window name";
    if (op == "applyProperties") {
        if (!a["properties"].isObject() || a["properties"].toObject().isEmpty())
            return "Properties must be a non-empty JSON object";
        auto json = QJsonDocument(a["properties"].toObject()).toJson(QJsonDocument::Compact);
        if (json.size() > 8192 || json.contains(")~END"))
            return "Properties too large or contain a reserved delimiter";
    }
    return {};
}

QStringList WallpaperEngine::arguments(const QJsonObject &a) {
    QStringList result{"-control", a["operation"].toString()};
    for (const auto &key : {"file", "playlist", "profile", "location", "monitor", "preset", "playInWindow",
                            "width", "height", "x", "y", "activate", "borderless"})
        if (a.contains(key)) {
            auto v = a[key];
            result << "-" + QString(key)
                   << (v.isBool()     ? (v.toBool() ? "1" : "0")
                       : v.isDouble() ? QString::number(v.toInt())
                                      : v.toString());
        }
    return result;
}

QJsonObject WallpaperEngine::execute(const QString &exe, const QJsonObject &args) {
    auto error = validate(args);
    if (!error.isEmpty())
        return {{"error", error}};
    if (!validExe(exe))
        return {{"error", "Wallpaper Engine executable not found"}};
    if (!running(exe))
        return {{"error", "Start Wallpaper Engine first"}};
    QProcess process;
    process.setWorkingDirectory(QFileInfo(exe).absolutePath());
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
        // Wallpaper Engine is a GUI executable: CREATE_NO_WINDOW alone is ignored.
        // Hide the short-lived CLI process without hiding the already-running wallpaper.
        args->flags |= CREATE_NO_WINDOW;
        args->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
        args->startupInfo->wShowWindow = SW_HIDE;
    });
    // Wallpaper Engine's parser explicitly requires raw JSON delimiters, not argv quote escaping.
    if (args.contains("properties"))
        process.setNativeArguments(
            "-properties RAW~(" +
            QString::fromUtf8(QJsonDocument(args["properties"].toObject()).toJson(QJsonDocument::Compact)) +
            ")~END");
    process.start(exe, arguments(args));
    if (!process.waitForStarted(1500))
        return {{"error", "Could not start Wallpaper Engine CLI"}};
    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(1000);
        return {{"error", "Wallpaper Engine CLI timed out; outcome unknown, not retried"}};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return {{"error", "Wallpaper Engine CLI returned an error"}};
    auto output = QString::fromUtf8(process.readAllStandardOutput().left(8192)).trimmed();
    return {
        {"output", output},
        {"message", args["operation"] == "getWallpaper"
                        ? "Wallpaper queried"
                        : "Command dispatched; Wallpaper Engine does not acknowledge the resulting state"}};
}

QJsonObject WallpaperEngine::inspect(const QString &overridePath, bool queryWallpaper) {
    const auto exe = discover(overridePath);
    const bool active = !exe.isEmpty() && running(exe);
    QJsonObject state{{"executable", exe},
                      {"installed", !exe.isEmpty()},
                      {"running", active},
                      {"status", exe.isEmpty() ? "Choose Wallpaper Engine executable"
                                 : active      ? "Running"
                                               : "Start Wallpaper Engine"}};
    if (active && queryWallpaper) {
        auto result = execute(exe, {{"operation", "getWallpaper"}});
        if (result.contains("error")) {
            state["error"] = result["error"];
            state["status"] = result["error"];
        } else
            state["current_wallpaper"] = result["output"];
    }
    return state;
}
